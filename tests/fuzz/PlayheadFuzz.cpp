// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/Playhead.hpp"

#include "FuzzInput.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

using tapeloop::Nanoseconds;
using tapeloop::Playhead;
using tapeloop::PlayheadConfig;
using tapeloop::ScrubDirection;
using tapeloop::fuzz::FuzzInput;
using tapeloop::fuzz::require;

namespace {

__extension__ typedef __int128 Int128;

// The playhead keeps its position in nanoseconds plus a remainder in 2000ths of one.
constexpr Int128 kUnitsPerNanosecond = 2000;

Int128 floorUnits(Int128 units)
{
	Int128 nanoseconds = units / kUnitsPerNanosecond;
	if (units % kUnitsPerNanosecond < 0)
		--nanoseconds;
	return nanoseconds;
}

// Frame times for load(): mostly increasing, as a clip gives them, but unsorted and
// repeated now and then.
std::vector<Nanoseconds> readFrames(FuzzInput &input)
{
	std::vector<Nanoseconds> frames(input.below(64));
	Nanoseconds time{input.i64()};
	for (Nanoseconds &frame : frames) {
		if (input.byte() % 8 == 0)
			time = Nanoseconds{input.i64()};
		else
			time = tapeloop::saturatingAdd(time,
						       Nanoseconds{static_cast<int64_t>(input.below(40'000'000))});
		frame = time;
	}
	return frames;
}

// The limits as the playhead brings them into range.
struct Limits {
	int32_t minRate;
	int32_t maxRate;
	int32_t scrubStartRate;

	explicit Limits(const PlayheadConfig &config)
		: minRate(std::max(config.minRate, 1)),
		  maxRate(std::max(config.maxRate, minRate)),
		  scrubStartRate(std::max(config.scrubStartRate, 1))
	{
	}

	int32_t rateFor(int32_t requested) const
	{
		const int64_t magnitude =
			std::clamp<int64_t>(requested < 0 ? -int64_t{requested} : requested, minRate, maxRate);
		return static_cast<int32_t>(requested < 0 ? -magnitude : magnitude);
	}
};

struct State {
	Nanoseconds position;
	int32_t rate;
	bool playing;
	bool scrubbing;
	size_t shown;

	explicit State(const Playhead &playhead)
		: position(playhead.position()),
		  rate(playhead.rate()),
		  playing(playhead.playing()),
		  scrubbing(playhead.scrubbing()),
		  shown(playhead.displayedFrame())
	{
	}
};

// Whether play() can start: there is somewhere to go in the direction of the rate.
bool canPlay(const std::vector<Nanoseconds> &frames, const State &state)
{
	return !frames.empty() && (state.rate > 0 ? state.position < frames.back() : state.position > frames.front());
}

void checkAlways(const Playhead &playhead, const std::vector<Nanoseconds> &frames, const Limits &limits)
{
	const int32_t rate = playhead.rate();
	require(rate != 0);
	const int32_t magnitude = rate < 0 ? -rate : rate;
	require(magnitude >= limits.minRate && magnitude <= limits.maxRate);
	require((playhead.effectiveRate() == 0) == (!playhead.playing() && !playhead.scrubbing()));

	if (frames.empty()) {
		require(playhead.empty() && playhead.displayedFrame() == 0 && playhead.atStart() && playhead.atEnd());
		return;
	}

	const Nanoseconds position = playhead.position();
	require(position >= frames.front() && position <= frames.back());
	require(playhead.atStart() == (position == frames.front()));
	require(playhead.atEnd() == (position == frames.back()));

	const size_t shown = playhead.displayedFrame();
	require(shown < frames.size());
	require(frames[shown] <= position);
	require(shown + 1 == frames.size() || frames[shown + 1] > position);
}

// What advance() promises: it moves only while playing or scrubbing, in the direction
// of the rate or the scrub, stops paused when playback reaches a bound, and while
// playing, moves by exactly elapsed times the rate, so one call goes as far as the same
// time in two calls. `exact` is position times 2000 plus the remainder, when known.
void checkAdvance(Playhead &playhead, const std::vector<Nanoseconds> &frames, Nanoseconds elapsed,
		  std::optional<Int128> &exact)
{
	const State before(playhead);
	const int32_t scrubRate = playhead.effectiveRate();

	Playhead split = playhead;
	const Nanoseconds firstPart{elapsed.count() / 3};
	split.advance(firstPart);
	split.advance(elapsed - firstPart);

	playhead.advance(elapsed);
	const State after(playhead);
	require(after.rate == before.rate && after.scrubbing == before.scrubbing);

	if (frames.empty() || elapsed <= Nanoseconds{0} || (!before.playing && !before.scrubbing)) {
		require(after.position == before.position && after.playing == before.playing);
		return;
	}

	const int32_t direction = before.scrubbing ? scrubRate : before.rate;
	if (direction > 0)
		require(after.position >= before.position);
	else
		require(after.position <= before.position);

	if (before.scrubbing) {
		require(after.playing == before.playing);
		// The model does not follow the scrub curve.
		exact.reset();
		return;
	}
	const bool atBound = before.rate > 0 ? after.position == frames.back() : after.position == frames.front();
	require(after.playing == !atBound);

	const std::optional<Int128> target =
		exact ? std::optional<Int128>(*exact + Int128{elapsed.count()} * 2 * before.rate) : std::nullopt;
	if (atBound) {
		if (target)
			require(before.rate > 0 ? floorUnits(*target) >= frames.back().count()
						: floorUnits(*target) <= frames.front().count());
		exact = Int128{after.position.count()} * kUnitsPerNanosecond;
		return;
	}
	require(split.position() == after.position && split.playing());
	if (target)
		require(floorUnits(*target) == after.position.count());
	exact = target;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	FuzzInput input(data, size);

	PlayheadConfig config;
	config.minRate = input.i32();
	config.maxRate = input.i32();
	config.scrubStartRate = input.i32();
	config.scrubEndRate = input.i32();
	config.scrubRampStart = Nanoseconds{input.i64()};
	config.scrubRampEnd = Nanoseconds{input.i64()};
	Playhead playhead(config);
	const Limits limits(config);
	require(playhead.rate() == limits.rateFor(1000));

	// The frames as the playhead keeps them: sorted, without duplicates.
	std::vector<Nanoseconds> frames;
	std::optional<Int128> exact;
	while (!input.empty()) {
		const State before(playhead);
		switch (input.byte() % 12) {
		case 0: {
			frames = readFrames(input);
			playhead.load(frames);
			std::sort(frames.begin(), frames.end());
			frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
			require(!playhead.playing() && !playhead.scrubbing() && playhead.rate() == before.rate);
			require(playhead.position() == (frames.empty() ? Nanoseconds{0} : frames.front()));
			exact = Int128{playhead.position().count()} * kUnitsPerNanosecond;
			break;
		}
		case 1:
		case 2: {
			const Nanoseconds elapsed{input.flag() ? input.i64()
							       : static_cast<int64_t>(input.below(100'000'000))};
			checkAdvance(playhead, frames, elapsed, exact);
			break;
		}
		case 3:
			playhead.play();
			require(playhead.playing() == (frames.empty() ? before.playing : canPlay(frames, before)));
			break;
		case 4:
			playhead.pause();
			require(!playhead.playing());
			break;
		case 5:
			playhead.togglePause();
			require(playhead.playing() == (!before.playing && canPlay(frames, before)));
			break;
		case 6: {
			const int32_t rate = input.i32();
			playhead.setRate(rate);
			if (rate == 0)
				require(!playhead.playing() && playhead.rate() == before.rate);
			else
				require(playhead.rate() == limits.rateFor(rate) &&
					playhead.playing() == before.playing);
			break;
		}
		case 7:
			playhead.reverse();
			require(playhead.rate() == -before.rate);
			break;
		case 8: {
			const int32_t frameCount = input.i32();
			playhead.step(frameCount);
			require(!playhead.playing());
			if (!frames.empty()) {
				const int64_t target =
					std::clamp<int64_t>(static_cast<int64_t>(before.shown) + frameCount, 0,
							    static_cast<int64_t>(frames.size()) - 1);
				require(playhead.position() == frames[static_cast<size_t>(target)]);
				exact = Int128{playhead.position().count()} * kUnitsPerNanosecond;
			}
			break;
		}
		case 9: {
			const Nanoseconds time{input.i64()};
			playhead.seek(time);
			if (!frames.empty()) {
				require(playhead.position() == std::clamp(time, frames.front(), frames.back()));
				exact = Int128{playhead.position().count()} * kUnitsPerNanosecond;
			}
			require(playhead.playing() == before.playing);
			break;
		}
		case 10: {
			const bool forward = input.flag();
			playhead.beginScrub(forward ? ScrubDirection::Forward : ScrubDirection::Backward);
			require(playhead.scrubbing() && playhead.playing() == before.playing);
			require(playhead.effectiveRate() == (forward ? limits.scrubStartRate : -limits.scrubStartRate));
			break;
		}
		default:
			playhead.endScrub();
			require(!playhead.scrubbing() && playhead.playing() == before.playing);
			break;
		}
		checkAlways(playhead, frames, limits);
	}
	return 0;
}
