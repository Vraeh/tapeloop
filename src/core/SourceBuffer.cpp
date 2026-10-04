// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/SourceBuffer.hpp"

#include <algorithm>
#include <cassert>
#include <utility>

namespace tapeloop {

SourceBuffer::SourceBuffer(SourceBufferConfig config)
	: config_(config),
	  maxBytes_(config.maxBytes),
	  builder_(config.frameDuration)
{
	assert(config.frameDuration > Nanoseconds{0});
}

void SourceBuffer::push(const EncodedPacket &packet)
{
	std::lock_guard lock(mutex_);

	if (synced_ && (packet.time <= lastTime_ || packet.dts < lastDts_)) {
		++discontinuities_;
		synced_ = false;
		sealLocked();
	}

	if (!synced_ && !packet.keyframe) {
		++droppedBeforeKeyframe_;
		return;
	}

	// Until the packet is stored, the buffer counts as waiting for a keyframe, so a
	// failed allocation never leaves a GOP with a frame missing.
	const bool restarting = !synced_;
	synced_ = false;

	if (packet.keyframe) {
		// After a failed seal the builder can still hold the previous GOP.
		sealLocked();
		// After a discontinuity the new run can start earlier than what is held.
		// Times in the buffer must keep increasing for clips to find frames, so the
		// GOPs that do not end before the new keyframe make way for it, before
		// eviction measures what is left.
		if (restarting) {
			dropFromLocked(packet.time);
		}
		evictLocked();
	}

	builder_.append(packet);
	lastTime_ = packet.time;
	lastDts_ = packet.dts;
	synced_ = true;
}

void SourceBuffer::setCodecConfig(std::span<const uint8_t> codecConfig)
{
	// The copy is made before locking, so the encoder thread never waits for it.
	std::shared_ptr<const CodecConfig> shared;
	if (!codecConfig.empty()) {
		shared = std::make_shared<const CodecConfig>(codecConfig.begin(), codecConfig.end());
	}

	std::lock_guard lock(mutex_);
	sealLocked();
	builder_.setCodecConfig(std::move(shared));
	if (synced_) {
		++discontinuities_;
	}
	synced_ = false;
}

Clip SourceBuffer::clip(Nanoseconds duration) const
{
	std::vector<std::shared_ptr<const Gop>> gops;
	Nanoseconds from{0};
	Nanoseconds to{0};
	{
		std::lock_guard lock(mutex_);
		if (gops_.empty() && builder_.empty()) {
			return {};
		}
		to = newestTimeLocked();
		from = saturatingSub(to, std::max(duration, Nanoseconds{0}));
		gops = collectLocked(from, to);
	}
	return Clip(std::move(gops), from, to);
}

Clip SourceBuffer::clip(Nanoseconds from, Nanoseconds to) const
{
	std::vector<std::shared_ptr<const Gop>> gops;
	{
		std::lock_guard lock(mutex_);
		gops = collectLocked(from, to);
	}
	return Clip(std::move(gops), from, to);
}

SourceBufferStats SourceBuffer::stats() const
{
	std::lock_guard lock(mutex_);

	SourceBufferStats stats;
	stats.bytes = sealedBytes_ + builder_.byteSize();
	stats.gopCount = gops_.size() + (builder_.empty() ? 0 : 1);

	// GOPs that share a configuration are adjacent, because the buffer never goes back
	// to an earlier one, so counting at each change of pointer counts each one once.
	const CodecConfig *previous = nullptr;
	const auto countConfig = [&](const CodecConfig *codecConfig) {
		if (codecConfig && codecConfig != previous) {
			stats.configBytes += codecConfig->size();
		}
		previous = codecConfig;
	};
	for (const auto &gop : gops_) {
		countConfig(gop->codecConfig());
	}
	if (!builder_.empty()) {
		countConfig(builder_.codecConfig());
	}

	if (!gops_.empty()) {
		stats.oldestTime = gops_.front()->startTime();
	} else if (!builder_.empty()) {
		stats.oldestTime = builder_.startTime();
	}
	stats.newestTime = newestTimeLocked();
	stats.droppedBeforeKeyframe = droppedBeforeKeyframe_;
	stats.discontinuities = discontinuities_;
	return stats;
}

void SourceBuffer::clear()
{
	// Released after unlocking, so freeing the memory does not hold up the encoder.
	std::deque<std::shared_ptr<const Gop>> released;
	{
		std::lock_guard lock(mutex_);
		released.swap(gops_);
		builder_.clear();
		sealedBytes_ = 0;
		synced_ = false;
		lastTime_ = Nanoseconds{0};
		lastDts_ = 0;
		droppedBeforeKeyframe_ = 0;
		discontinuities_ = 0;
	}
}

void SourceBuffer::setByteBudget(size_t maxBytes)
{
	std::lock_guard lock(mutex_);
	maxBytes_ = maxBytes;
}

std::vector<std::shared_ptr<const Gop>> SourceBuffer::collectLocked(Nanoseconds from, Nanoseconds to) const
{
	std::vector<std::shared_ptr<const Gop>> gops;
	if (from > to) {
		return gops;
	}

	const auto first = std::partition_point(gops_.begin(), gops_.end(),
						[from](const auto &gop) { return gop->lastTime() < from; });
	const auto last =
		std::partition_point(first, gops_.end(), [to](const auto &gop) { return gop->startTime() <= to; });
	gops.reserve(static_cast<size_t>(last - first) + 1);
	gops.insert(gops.end(), first, last);

	if (!builder_.empty() && builder_.startTime() <= to && builder_.lastTime() >= from) {
		gops.push_back(builder_.snapshot());
	}

	return gops;
}

void SourceBuffer::sealLocked()
{
	if (builder_.empty()) {
		return;
	}

	// Stored before the builder lets go of the packets, so a failed allocation loses
	// nothing.
	gops_.push_back(builder_.snapshot());
	builder_.clear();
	sealedBytes_ += gops_.back()->byteSize();
}

void SourceBuffer::evictLocked()
{
	const auto dropOldest = [this] {
		sealedBytes_ -= gops_.front()->byteSize();
		gops_.pop_front();
	};

	while (gops_.size() > 1 && saturatingSub(gops_.back()->endTime(), gops_[1]->startTime()) >= config_.window) {
		dropOldest();
	}

	while (gops_.size() > 1 && sealedBytes_ > maxBytes_) {
		dropOldest();
	}
}

void SourceBuffer::dropFromLocked(Nanoseconds time)
{
	while (!gops_.empty() && gops_.back()->lastTime() >= time) {
		sealedBytes_ -= gops_.back()->byteSize();
		gops_.pop_back();
	}
}

Nanoseconds SourceBuffer::newestTimeLocked() const noexcept
{
	if (!builder_.empty()) {
		return builder_.lastTime();
	}
	if (!gops_.empty()) {
		return gops_.back()->lastTime();
	}
	return Nanoseconds{0};
}

} // namespace tapeloop
