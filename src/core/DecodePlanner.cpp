// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/DecodePlanner.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

namespace tapeloop {
namespace {

constexpr size_t kWholeGop = std::numeric_limits<size_t>::max();
// The index of no GOP, for a kept slot between two GOPs.
constexpr size_t kNoGop = std::numeric_limits<size_t>::max();

std::optional<size_t> packetOfPts(const Gop &gop, int64_t pts) noexcept
{
	const auto packets = gop.packets();
	for (size_t i = 0; i < packets.size(); ++i) {
		if (packets[i].pts == pts) {
			return i;
		}
	}
	return std::nullopt;
}

size_t distance(size_t a, size_t b) noexcept
{
	return a > b ? a - b : b - a;
}

} // namespace

DecodedCacheBounds decodedCacheBounds(FrameMemory where, MemorySizes sizes) noexcept
{
	constexpr uint64_t kLeastMiB = 128;
	constexpr uint64_t kMostMiB = 4096;
	const uint64_t memory = where == FrameMemory::Video ? sizes.dedicatedVideo : sizes.system;
	return {kLeastMiB, std::clamp((memory / 4) >> 20, kLeastMiB, kMostMiB)};
}

size_t decodedCacheBytes(int64_t settingMiB, FrameMemory where, MemorySizes sizes) noexcept
{
	const DecodedCacheBounds bounds = decodedCacheBounds(where, sizes);
	const uint64_t mib = settingMiB < 0 ? 0 : static_cast<uint64_t>(settingMiB);
	return static_cast<size_t>(std::clamp(mib, bounds.leastMiB, bounds.mostMiB) << 20);
}

DecodePlanner::DecodePlanner(FrameDecoder &decoder, DecodePlannerConfig config) : decoder_(decoder), config_(config)
{
	config_.keptGops = std::max<size_t>(config_.keptGops, 1);
	kept_.reserve(config_.keptGops);
}

DecodePlanner::~DecodePlanner()
{
	releaseAll();
	decoder_.close();
}

void DecodePlanner::load(Clip clip)
{
	releaseAll();
	if (open_) {
		decoder_.close();
		open_ = false;
	}
	fresh_ = true;
	clip_ = std::move(clip);
	openConfig_ = nullptr;
	streamGop_.reset();
	lastGop_.reset();
}

DecodeResult DecodePlanner::frameAt(Nanoseconds t)
{
	if (clip_.empty()) {
		return {DecodeStatus::NoFrame, {}};
	}
	const FrameLocation at = clip_.locate(t);
	lastGop_ = at.gop;

	KeptGop *kept = find(at.gop);
	// A frame no pass has reached in a GOP the decoder has not given all it will give
	// needs decoding, and so does one given back to stay within maxBytes; any other frame
	// not held is one the decoder does not give.
	if (!kept || (!kept->frames[at.packet] &&
		      ((!kept->complete && at.packet >= kept->reached) || kept->dropped[at.packet]))) {
		const DecodeStatus status = decode(at.gop, at.packet);
		if (status != DecodeStatus::Ok) {
			return {status, {}};
		}
		kept = find(at.gop);
	}
	if (kept) {
		if (const std::optional<DecodedFrame> &frame = kept->frames[at.packet]) {
			return {DecodeStatus::Ok, *frame};
		}
	}
	// The decoder went past this frame, or gave every frame it had, without giving it.
	return {DecodeStatus::InvalidData, {}};
}

DecodeStatus DecodePlanner::prefetch(PlayDirection direction)
{
	const size_t current = lastGop_.value_or(0);
	if (direction == PlayDirection::Forward || config_.keptGops < 2 || current == 0) {
		return DecodeStatus::Ok;
	}
	const size_t previous = current - 1;
	const KeptGop *kept = find(previous);
	if (kept && kept->complete) {
		return DecodeStatus::Ok;
	}
	// With the GOP on screen filling the cap, frames decoded ahead would only be given
	// back.
	if (const KeptGop *shown = find(current)) {
		size_t bytes = 0;
		size_t frame = 0;
		for (const std::optional<DecodedFrame> &held : shown->frames) {
			if (held) {
				bytes += held->bytes;
				frame = held->bytes;
			}
		}
		if (bytes > config_.maxBytes || frame > config_.maxBytes - bytes) {
			return DecodeStatus::Ok;
		}
	}
	return decode(previous, kWholeGop);
}

size_t DecodePlanner::heldFrames() const noexcept
{
	size_t held = 0;
	for (const KeptGop &kept : kept_) {
		held += kept.received;
	}
	return held;
}

DecodePlanner::KeptGop &DecodePlanner::keep(size_t gop)
{
	if (KeptGop *kept = find(gop)) {
		return *kept;
	}
	KeptGop *slot = nullptr;
	if (kept_.size() < config_.keptGops) {
		slot = &kept_.emplace_back();
	} else {
		// The farthest from the GOP asked for goes, never the one on screen while another
		// can. Its vector is reused, so a GOP no longer than the ones before allocates
		// nothing.
		for (KeptGop &kept : kept_) {
			const bool shown = kept.gop == lastGop_ && kept_.size() > 1;
			if (!shown && (!slot || distance(kept.gop, gop) > distance(slot->gop, gop))) {
				slot = &kept;
			}
		}
		release(*slot);
	}
	// Named only once its frames are sized, so that a throw leaves a slot no GOP finds.
	slot->gop = kNoGop;
	slot->received = 0;
	slot->reached = 0;
	slot->complete = false;
	const size_t count = clip_.gops()[gop]->packets().size();
	slot->frames.assign(count, std::nullopt);
	slot->dropped.assign(count, char{0});
	slot->gop = gop;
	return *slot;
}

DecodePlanner::KeptGop *DecodePlanner::find(size_t gop) noexcept
{
	const auto found =
		std::find_if(kept_.begin(), kept_.end(), [gop](const KeptGop &kept) { return kept.gop == gop; });
	return found != kept_.end() ? &*found : nullptr;
}

void DecodePlanner::release(KeptGop &kept) noexcept
{
	for (std::optional<DecodedFrame> &frame : kept.frames) {
		if (frame) {
			heldBytes_ -= frame->bytes;
			decoder_.release(*frame);
			frame.reset();
		}
	}
	std::fill(kept.dropped.begin(), kept.dropped.end(), char{0});
	kept.received = 0;
	kept.reached = 0;
	kept.complete = false;
}

void DecodePlanner::drop(KeptGop &kept, size_t packet) noexcept
{
	if (std::optional<DecodedFrame> &frame = kept.frames[packet]) {
		heldBytes_ -= frame->bytes;
		decoder_.release(*frame);
		frame.reset();
		kept.dropped[packet] = char{1};
		--kept.received;
	}
}

bool DecodePlanner::makeRoom(KeptGop &kept, size_t bytes, size_t wanted) noexcept
{
	const auto fits = [&] {
		return heldBytes_ <= config_.maxBytes && bytes <= config_.maxBytes - heldBytes_;
	};
	for (KeptGop &other : kept_) {
		if (&other == &kept || other.gop == lastGop_) {
			continue;
		}
		const size_t count = other.frames.size();
		for (size_t i = 0; i < count && !fits(); ++i) {
			drop(other, other.gop < kept.gop ? i : count - 1 - i);
		}
	}
	// Reverse play keeps what it needs next: the frames just before the one asked for.
	const size_t count = kept.frames.size();
	// Only frames this pass has not come to yet: one it just brought out may be the next
	// one shown.
	for (size_t packet = count; wanted < count && packet > std::max(wanted + 1, passNext_) && !fits(); --packet) {
		drop(kept, packet - 1);
	}
	for (size_t packet = 0; packet < count && !fits(); ++packet) {
		if (packet != wanted) {
			drop(kept, packet);
		}
	}
	return fits();
}

void DecodePlanner::releaseAll() noexcept
{
	for (KeptGop &kept : kept_) {
		release(kept);
	}
	kept_.clear();
}

DecodeStatus DecodePlanner::decode(size_t gop, size_t packet)
{
	const Gop &source = *clip_.gops()[gop];
	const size_t count = source.packets().size();
	// Frames come out in presentation order, so within one uninterrupted pass over a GOP
	// a frame not out yet comes after every frame already out; one given back for room
	// that the pass already went past needs a pass from the keyframe. The decoder is
	// opened before a kept GOP makes room, so a run that cannot be opened costs nothing
	// kept.
	const KeptGop *before = find(gop);
	const bool passed = streamGop_ == gop && packet < count && packet < passNext_;
	if (streamGop_ != gop || flushed_ || (passed && before && before->dropped[packet])) {
		const DecodeStatus status = startAt(gop);
		if (status != DecodeStatus::Ok) {
			return fail(status);
		}
	}
	KeptGop &kept = keep(gop);

	// A frame behind the pass that is not held is one the decoder skipped: one given back
	// for room started a new pass above, and the frame asked for is never given back.
	const auto done = [&] {
		return packet < count ? kept.frames[packet].has_value() || packet < passNext_ : kept.complete;
	};
	while (!done()) {
		DecodeStatus status = DecodeStatus::Ok;
		if (nextPacket_ < count) {
			const PacketRecord &record = source.packets()[nextPacket_];
			status = decoder_.send(source.packetData(nextPacket_), record.pts, record.dts);
			++work_.packetsSent;
			++nextPacket_;
			fresh_ = false;
		} else if (!flushed_) {
			status = decoder_.flush();
			++work_.flushes;
			flushed_ = true;
			fresh_ = false;
		} else {
			return DecodeStatus::Ok;
		}
		if (status == DecodeStatus::Ok) {
			status = receiveAll(kept, packet);
		}
		if (status != DecodeStatus::Ok) {
			return fail(status);
		}
		if (flushed_) {
			// What the flush did not bring out the decoder never gives.
			std::fill(kept.dropped.begin() + static_cast<ptrdiff_t>(passNext_), kept.dropped.end(),
				  char{0});
			kept.complete = true;
		}
	}
	return DecodeStatus::Ok;
}

DecodeStatus DecodePlanner::fail(DecodeStatus status) noexcept
{
	streamGop_.reset();
	if (status == DecodeStatus::DeviceLost) {
		// The kept frames lived on the lost device, and the decoder has to be opened
		// again before it decodes anything.
		releaseAll();
		decoder_.close();
		open_ = false;
		fresh_ = true;
	}
	return status;
}

DecodeStatus DecodePlanner::startAt(size_t gop)
{
	const Gop &source = *clip_.gops()[gop];
	if (!open_ || source.codecConfig() != openConfig_ || source.codec() != openCodec_) {
		const std::span<const uint8_t> config = source.codecConfig()
								? std::span<const uint8_t>(*source.codecConfig())
								: std::span<const uint8_t>();
		++work_.opens;
		const DecodeStatus status = decoder_.open(source.codec(), config);
		open_ = status == DecodeStatus::Ok;
		if (!open_) {
			return status;
		}
		openConfig_ = source.codecConfig();
		openCodec_ = source.codec();
	} else if (!fresh_) {
		decoder_.reset();
		++work_.resets;
	}
	fresh_ = true;
	streamGop_ = gop;
	nextPacket_ = 0;
	passNext_ = 0;
	flushed_ = false;
	return DecodeStatus::Ok;
}

DecodeStatus DecodePlanner::receiveAll(KeptGop &kept, size_t wanted)
{
	const Gop &source = *clip_.gops()[kept.gop];
	for (;;) {
		DecodedFrame frame;
		const DecodeStatus status = decoder_.receive(frame);
		if (status == DecodeStatus::NeedMore || status == DecodeStatus::Drained) {
			return DecodeStatus::Ok;
		}
		if (status != DecodeStatus::Ok) {
			return status;
		}
		++work_.framesReceived;
		// A frame of no packet, or from before where the pass is, is not one to keep.
		const std::optional<size_t> index = packetOfPts(source, frame.pts);
		if (!index || *index < passNext_) {
			decoder_.release(frame);
			continue;
		}
		// Frames the pass went past without them the decoder never gives, so they no
		// longer count as given back.
		std::fill(kept.dropped.begin() + static_cast<ptrdiff_t>(passNext_),
			  kept.dropped.begin() + static_cast<ptrdiff_t>(*index), char{0});
		passNext_ = *index + 1;
		kept.reached = std::max(kept.reached, passNext_);
		if (passNext_ == kept.frames.size()) {
			kept.complete = true;
		}
		// A frame held already came again in a pass from the keyframe, and one that
		// finds no room is given back, unless it is the one asked for.
		if (kept.frames[*index] || (!makeRoom(kept, frame.bytes, wanted) && *index != wanted)) {
			if (!kept.frames[*index]) {
				kept.dropped[*index] = char{1};
			}
			decoder_.release(frame);
			continue;
		}
		frame.time = source.packets()[*index].time;
		kept.frames[*index] = frame;
		kept.dropped[*index] = char{0};
		++kept.received;
		heldBytes_ += frame.bytes;
	}
}

} // namespace tapeloop
