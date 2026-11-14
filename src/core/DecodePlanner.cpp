// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/DecodePlanner.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>

namespace tapeloop {
namespace {

constexpr size_t kWholeGop = std::numeric_limits<size_t>::max();
// The index of no GOP, for a kept slot between two GOPs.
constexpr size_t kNoGop = std::numeric_limits<size_t>::max();
constexpr size_t kNoPacket = std::numeric_limits<size_t>::max();

size_t distance(size_t a, size_t b) noexcept
{
	return a > b ? a - b : b - a;
}

std::span<const uint8_t> configOf(const Gop &gop) noexcept
{
	const CodecConfig *config = gop.codecConfig();
	return config ? std::span<const uint8_t>(*config) : std::span<const uint8_t>();
}

// Whether a decoder opened for one GOP takes the other after a reset: open() is given
// nothing but the codec and the configuration's bytes.
bool opensAlike(const Gop &a, const Gop &b) noexcept
{
	const std::span<const uint8_t> left = configOf(a);
	const std::span<const uint8_t> right = configOf(b);
	return a.codec() == b.codec() && std::equal(left.begin(), left.end(), right.begin(), right.end());
}

} // namespace

// The bounds reach 4 GiB, more than a 32-bit size holds.
static_assert(sizeof(size_t) >= 8, "the decoded cache needs 64-bit sizes");

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

void DecodePlanner::load(std::unique_ptr<GopSource> gops)
{
	releaseAll();
	if (started_) {
		decoder_.close();
		started_.reset();
	}
	fresh_ = true;
	gops_ = std::move(gops);
	streamGop_.reset();
	lastGop_.reset();
}

DecodeResult DecodePlanner::frameAt(Nanoseconds t)
{
	if (!gops_ || gops_->gopCount() == 0) {
		return {DecodeStatus::NoFrame, {}};
	}
	const FrameLocation at = gops_->locate(t);
	// A source describes a file that may be damaged, so a place it does not hold is
	// refused rather than looked up.
	if (at.gop >= gops_->gopCount() || at.packet >= gops_->packetCount(at.gop)) {
		return {DecodeStatus::InvalidData, {}};
	}
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
	const size_t count = gops_->packetCount(gop);
	slot->frames.assign(count, std::nullopt);
	slot->dropped.assign(count, char{0});
	slot->low = count;
	slot->high = 0;
	slot->spared = kNoPacket;
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
	for (KeptGop &other : kept_) {
		if (&other == &kept || other.gop == lastGop_) {
			continue;
		}
		const size_t end = std::min(other.high, other.frames.size());
		if (other.gop < kept.gop) {
			dropSpared(other, bytes, kNoPacket);
			other.low = dropUp(other, other.low, end, bytes, kNoPacket);
		} else {
			other.high = dropDown(other, end, other.low, bytes);
			dropSpared(other, bytes, kNoPacket);
		}
	}
	// Reverse play keeps what it needs next: the frames just before the one asked for.
	const size_t count = kept.frames.size();
	if (wanted < count) {
		// Only frames this pass has not come to yet: one it just brought out may be the next
		// one shown.
		const size_t stop = std::max(wanted + 1, passNext_);
		kept.high = dropDown(kept, std::min(kept.high, count), std::max(stop, kept.low), bytes);
		if (kept.spared >= stop) {
			dropSpared(kept, bytes, wanted);
		}
	}
	dropSpared(kept, bytes, wanted);
	// Then the earliest, in two parts: the pass brings frames out right behind passNext_,
	// and none past it.
	const size_t behind = dropUp(kept, kept.low, passNext_, bytes, wanted);
	if (kept.low <= wanted && wanted < behind && kept.frames[wanted]) {
		kept.spared = wanted;
	}
	kept.low = behind;
	const size_t from = std::max(passNext_, aheadFrom_);
	const size_t ahead = dropUp(kept, from, std::min(kept.high, count), bytes, wanted);
	aheadFrom_ = from <= wanted && wanted < ahead && kept.frames[wanted] ? wanted : ahead;
	return fits(bytes);
}

bool DecodePlanner::fits(size_t bytes) const noexcept
{
	return heldBytes_ <= config_.maxBytes && bytes <= config_.maxBytes - heldBytes_;
}

size_t DecodePlanner::dropUp(KeptGop &kept, size_t from, size_t to, size_t bytes, size_t wanted) noexcept
{
	size_t packet = from;
	for (; packet < to && !fits(bytes); ++packet) {
		if (packet != wanted) {
			drop(kept, packet);
		}
	}
	return packet;
}

size_t DecodePlanner::dropDown(KeptGop &kept, size_t from, size_t to, size_t bytes) noexcept
{
	size_t packet = from;
	for (; packet > to && !fits(bytes); --packet) {
		drop(kept, packet - 1);
	}
	return packet;
}

void DecodePlanner::dropSpared(KeptGop &kept, size_t bytes, size_t wanted) noexcept
{
	if (kept.spared < kept.low && kept.spared != wanted && !fits(bytes)) {
		drop(kept, kept.spared);
		kept.spared = kNoPacket;
	}
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
	const size_t count = gops_->packetCount(gop);
	// Frames come out in presentation order, so within one uninterrupted pass over a GOP
	// a frame not out yet comes after every frame already out; one given back for room
	// that the pass already went past needs a pass from the keyframe. The decoder is
	// opened before a kept GOP makes room, so a run that cannot be opened costs nothing
	// kept.
	const KeptGop *before = find(gop);
	const bool passed = streamGop_ == gop && packet < count && packet < passNext_;
	if (streamGop_ != gop || flushed_ || (passed && before && before->dropped[packet])) {
		std::shared_ptr<const Gop> read = streamGop_ == gop ? started_ : gops_->read(gop);
		// A GOP the source cannot give, or one that is not what its index says, has no
		// frames to show. It is kept with nothing more to come, so that it is not read again
		// while it is kept; the slot it takes may be the one of the pass under way, which
		// therefore ends.
		if (!read || read->packets().size() != count) {
			KeptGop &kept = keep(gop);
			std::fill(kept.dropped.begin(), kept.dropped.end(), char{0});
			kept.complete = true;
			return fail(DecodeStatus::InvalidData);
		}
		const DecodeStatus status = startAt(gop, std::move(read));
		if (status != DecodeStatus::Ok) {
			return fail(status);
		}
	}
	const Gop &source = *started_;
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
		started_.reset();
		fresh_ = true;
	}
	return status;
}

DecodeStatus DecodePlanner::startAt(size_t gop, std::shared_ptr<const Gop> read)
{
	// Sized first, so that nothing after the decoder opens can throw.
	const std::span<const PacketRecord> packets = read->packets();
	packetsByPts_.reserve(packets.size());
	if (!started_ || !opensAlike(*started_, *read)) {
		++work_.opens;
		const DecodeStatus status = decoder_.open(read->codec(), configOf(*read));
		if (status != DecodeStatus::Ok) {
			started_.reset();
			return status;
		}
	} else if (!fresh_) {
		decoder_.reset();
		++work_.resets;
	}
	fresh_ = true;
	streamGop_ = gop;
	started_ = std::move(read);
	nextPacket_ = 0;
	passNext_ = 0;
	aheadFrom_ = 0;
	flushed_ = false;
	packetsByPts_.clear();
	for (size_t i = 0; i < packets.size(); ++i) {
		packetsByPts_.emplace_back(packets[i].pts, i);
	}
	// Packets in decode order are in presentation order too unless frames are reordered.
	if (!std::is_sorted(packetsByPts_.begin(), packetsByPts_.end())) {
		std::sort(packetsByPts_.begin(), packetsByPts_.end());
	}
	return DecodeStatus::Ok;
}

std::optional<size_t> DecodePlanner::packetOf(int64_t pts) const noexcept
{
	// The first packet of that time, should two share it.
	const auto found =
		std::lower_bound(packetsByPts_.begin(), packetsByPts_.end(), std::pair<int64_t, size_t>(pts, 0));
	if (found == packetsByPts_.end() || found->first != pts) {
		return std::nullopt;
	}
	return found->second;
}

DecodeStatus DecodePlanner::receiveAll(KeptGop &kept, size_t wanted)
{
	// Frames come only from the GOP the decoder is being given.
	const Gop &source = *started_;
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
		const std::optional<size_t> index = packetOf(frame.pts);
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
		kept.high = std::max(kept.high, *index + 1);
		if (*index < kept.low) {
			kept.low = *index;
			if (kept.spared > *index) {
				kept.spared = kNoPacket;
			}
		}
	}
}

} // namespace tapeloop
