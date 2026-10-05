// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/DecodePlanner.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace tapeloop {
namespace {

constexpr size_t kWholeGop = std::numeric_limits<size_t>::max();

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

DecodePlanner::DecodePlanner(FrameDecoder &decoder, DecodePlannerConfig config) : decoder_(decoder), config_(config)
{
	config_.keptGops = std::max<size_t>(config_.keptGops, 1);
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
	if (!kept || !kept->frames[at.packet]) {
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
	// The decoder gave every frame it had and this one was not among them.
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
	if (kept && kept->received == kept->frames.size()) {
		return DecodeStatus::Ok;
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
	while (kept_.size() >= config_.keptGops) {
		const auto farthest =
			std::max_element(kept_.begin(), kept_.end(), [gop](const KeptGop &a, const KeptGop &b) {
				return distance(a.gop, gop) < distance(b.gop, gop);
			});
		release(*farthest);
		kept_.erase(farthest);
	}
	KeptGop kept;
	kept.gop = gop;
	kept.frames.resize(clip_.gops()[gop]->packets().size());
	kept_.push_back(std::move(kept));
	return kept_.back();
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
			decoder_.release(*frame);
			frame.reset();
		}
	}
	kept.received = 0;
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
	KeptGop &kept = keep(gop);
	const Gop &source = *clip_.gops()[gop];
	const size_t count = source.packets().size();
	// Frames come out in presentation order, so within one uninterrupted pass over a GOP
	// a frame not out yet comes after every frame already out.
	if (streamGop_ != gop || flushed_) {
		const DecodeStatus status = startAt(gop);
		if (status != DecodeStatus::Ok) {
			return fail(status);
		}
	}

	const auto done = [&] {
		return packet < count ? kept.frames[packet].has_value() : kept.received == count;
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
			status = receiveAll(kept);
		}
		if (status != DecodeStatus::Ok) {
			return fail(status);
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
	flushed_ = false;
	return DecodeStatus::Ok;
}

DecodeStatus DecodePlanner::receiveAll(KeptGop &kept)
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
		const std::optional<size_t> index = packetOfPts(source, frame.pts);
		if (!index || kept.frames[*index]) {
			decoder_.release(frame);
			continue;
		}
		kept.frames[*index] = frame;
		++kept.received;
	}
}

} // namespace tapeloop
