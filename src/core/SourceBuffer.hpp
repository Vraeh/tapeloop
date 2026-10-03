// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/Clip.hpp"
#include "core/Gop.hpp"
#include "core/MediaTime.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace tapeloop {

struct SourceBufferConfig {
	// How much history to keep. Eviction by age never leaves less than this; the byte
	// budget and discontinuities can.
	Nanoseconds window{0};
	// Upper bound for the packet data of the sealed GOPs; the newest one is kept even if
	// it alone is larger. Codec configurations do not count.
	size_t maxBytes = 0;
	// Used for the end time of the last frame of each GOP. Must be positive.
	Nanoseconds frameDuration{0};
};

struct SourceBufferStats {
	// Packet bytes and GOPs held, counting the GOP still being encoded.
	size_t bytes = 0;
	size_t gopCount = 0;
	// Bytes of the codec configurations those GOPs point at, each counted once.
	size_t configBytes = 0;
	// Times of the oldest and newest frame held. Zero when the buffer is empty.
	Nanoseconds oldestTime{0};
	Nanoseconds newestTime{0};
	uint64_t droppedBeforeKeyframe = 0;
	uint64_t discontinuities = 0;
};

// The ring buffer of one replay source. The encoder thread pushes packets while any
// other thread cuts clips; a clip shares the GOPs it covers, so eviction and clear()
// never invalidate it.
class SourceBuffer {
public:
	explicit SourceBuffer(SourceBufferConfig config);

	~SourceBuffer() = default;
	SourceBuffer(const SourceBuffer &) = delete;
	SourceBuffer &operator=(const SourceBuffer &) = delete;
	SourceBuffer(SourceBuffer &&) = delete;
	SourceBuffer &operator=(SourceBuffer &&) = delete;

	void push(const EncodedPacket &packet);

	// Starts a new run with a copy of the codec configuration the encoder reports, empty
	// when it reports none. Call it between runs: after the last packet of the previous
	// encoder and before the first of the next one. The GOP being built is sealed with
	// the previous configuration, the next packet kept is a keyframe, and a run cut short
	// this way counts as a discontinuity.
	void setCodecConfig(std::span<const uint8_t> codecConfig);

	// The last duration up to the newest frame.
	Clip clip(Nanoseconds duration) const;
	// The frames held with times in [from, to].
	Clip clip(Nanoseconds from, Nanoseconds to) const;

	SourceBufferStats stats() const;

	// Drops every packet and the counters, keeps the codec configuration, and waits for a
	// keyframe again.
	void clear();

	// Replaces the configured byte budget; eviction applies it at the next keyframe.
	void setByteBudget(size_t maxBytes);

private:
	std::vector<std::shared_ptr<const Gop>> collectLocked(Nanoseconds from, Nanoseconds to) const;
	void sealLocked();
	void evictLocked();
	void dropFromLocked(Nanoseconds time);
	Nanoseconds newestTimeLocked() const noexcept;

	const SourceBufferConfig config_;
	size_t maxBytes_;

	mutable std::mutex mutex_;
	GopBuilder builder_;
	std::deque<std::shared_ptr<const Gop>> gops_;
	size_t sealedBytes_ = 0;
	// False until a keyframe starts a run of packets, and again after a discontinuity or a
	// new codec configuration.
	bool synced_ = false;
	Nanoseconds lastTime_{0};
	int64_t lastDts_ = 0;
	uint64_t droppedBeforeKeyframe_ = 0;
	uint64_t discontinuities_ = 0;
};

} // namespace tapeloop
