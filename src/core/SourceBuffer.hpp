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
#include <vector>

namespace tapeloop {

struct SourceBufferConfig {
	// How much history to keep. Eviction by age never leaves less than this; the byte
	// budget and discontinuities can.
	Nanoseconds window{0};
	// Upper bound for the sealed GOPs; the newest one is kept even if it alone is larger.
	size_t maxBytes = 0;
	// Used for the end time of the last frame of each GOP. Must be positive.
	Nanoseconds frameDuration{0};
};

struct SourceBufferStats {
	// Bytes and GOPs held, counting the GOP still being encoded.
	size_t bytes = 0;
	size_t gopCount = 0;
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

	// The last duration up to the newest frame.
	Clip clip(Nanoseconds duration) const;
	// The frames held with times in [from, to].
	Clip clip(Nanoseconds from, Nanoseconds to) const;

	SourceBufferStats stats() const;

	// Drops everything, counters included, and waits for a keyframe again.
	void clear();

private:
	std::vector<std::shared_ptr<const Gop>> collectLocked(Nanoseconds from, Nanoseconds to) const;
	void sealLocked();
	void evictLocked();
	void dropFromLocked(Nanoseconds time);
	Nanoseconds newestTimeLocked() const noexcept;

	const SourceBufferConfig config_;

	mutable std::mutex mutex_;
	GopBuilder builder_;
	std::deque<std::shared_ptr<const Gop>> gops_;
	size_t sealedBytes_ = 0;
	// False until a keyframe starts a run of packets, and again after a discontinuity.
	bool synced_ = false;
	Nanoseconds lastTime_{0};
	int64_t lastDts_ = 0;
	uint64_t droppedBeforeKeyframe_ = 0;
	uint64_t discontinuities_ = 0;
};

} // namespace tapeloop
