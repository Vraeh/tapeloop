// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/MediaTime.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace tapeloop {

// Rates are in thousandths of real time: 250 is quarter speed, -1000 is real time
// backwards. Integers keep the accumulated position exact and replayable.
struct PlayheadConfig {
	// Limits for the magnitude of the playback rate.
	int32_t minRate = 50;
	int32_t maxRate = 4000;

	// Hold-to-scrub runs at scrubStartRate until the key has been held for
	// scrubRampStart, speeds up linearly to scrubEndRate at scrubRampEnd, then stays
	// there.
	int32_t scrubStartRate = 500;
	int32_t scrubEndRate = 4000;
	Nanoseconds scrubRampStart = std::chrono::milliseconds(250);
	Nanoseconds scrubRampEnd = std::chrono::milliseconds(1500);
};

enum class ScrubDirection { Forward, Backward };

// Moves through the frames of one clip. It never reads a clock: the caller passes the
// wall time elapsed since the previous call, so the same calls always give the same
// positions.
class Playhead {
public:
	// Out of range values are brought into range: rates to at least 1 and the limits
	// in order, the ramp to start at or after zero and end at or after its start.
	explicit Playhead(PlayheadConfig config = {});

	// Loads the frame times of a clip and waits paused on the first frame, with any
	// scrub ended. Times are sorted and duplicates dropped if needed. The speed is kept
	// from the previous clip, but a new clip always plays forward.
	void load(std::vector<Nanoseconds> frameTimes);
	bool empty() const noexcept { return frames_.empty(); }

	void advance(Nanoseconds elapsed);

	// Does nothing on the last frame when the rate is forward, or on the first when it
	// is reverse: there is nowhere to go.
	void play() noexcept;
	void pause() noexcept { playing_ = false; }
	void togglePause() noexcept;

	// Sets speed and direction from the argument, clamping the magnitude to the
	// configured limits. Zero is not a rate: it pauses and keeps the current one.
	void setRate(int32_t rate) noexcept;
	void reverse() noexcept { rate_ = -rate_; }

	// Pauses and moves by whole frames from the one on screen.
	void step(int frames) noexcept;
	// Moves to a time within the clip without changing the play state.
	void seek(Nanoseconds time) noexcept;

	// While scrubbing the position follows the scrub curve instead of the rate. Play
	// state and rate are left alone, so endScrub() resumes exactly what was going on.
	// Beginning again, in either direction, restarts the curve.
	void beginScrub(ScrubDirection direction) noexcept;
	void endScrub() noexcept { scrubbing_ = false; }

	Nanoseconds position() const noexcept { return position_; }
	int32_t rate() const noexcept { return rate_; }
	// The rate advance() would move at right now: zero when paused, the scrub curve
	// while scrubbing.
	int32_t effectiveRate() const noexcept;
	bool playing() const noexcept { return playing_; }
	bool scrubbing() const noexcept { return scrubbing_; }
	bool atStart() const noexcept;
	bool atEnd() const noexcept;

	// Index into the loaded frame times of the frame to show. Zero when empty.
	size_t displayedFrame() const noexcept;

private:
	int32_t scrubRate(Nanoseconds held) const noexcept;
	void advanceScrub(int64_t elapsed) noexcept;
	void move(int64_t elapsed, int64_t doubledRate) noexcept;
	void moveTo(Nanoseconds time) noexcept;

	PlayheadConfig config_;
	std::vector<Nanoseconds> frames_;
	Nanoseconds position_{0};
	// Half-thousandths of a nanosecond not yet added to position_, always in
	// [0, 2000). Halves let a scrub piece move at the exact average of two rates.
	int64_t remainder_ = 0;
	int32_t rate_ = 1000;
	bool playing_ = false;
	bool scrubbing_ = false;
	ScrubDirection scrubDirection_ = ScrubDirection::Forward;
	Nanoseconds scrubHeld_{0};
};

} // namespace tapeloop
