// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/Playhead.hpp"

#include "core/Clip.hpp"
#include "core/Gop.hpp"
#include "SyntheticEncoder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <random>
#include <vector>

using namespace std::chrono_literals;
using tapeloop::Nanoseconds;
using tapeloop::Playhead;
using tapeloop::ScrubDirection;

namespace {

// Frames at 60 fps starting five seconds into the shared clock.
constexpr Nanoseconds kOrigin = 5s;

Nanoseconds frameTime(int64_t frame)
{
	return kOrigin + Nanoseconds{tapeloop::rescale(frame, {1, 60}, tapeloop::kNanosecondTimebase)};
}

std::vector<Nanoseconds> frames(int64_t count)
{
	std::vector<Nanoseconds> times;
	for (int64_t frame = 0; frame < count; ++frame)
		times.push_back(frameTime(frame));
	return times;
}

Playhead loaded(int64_t count)
{
	Playhead playhead;
	playhead.load(frames(count));
	return playhead;
}

} // namespace

TEST_CASE("Playhead starts paused on the first frame")
{
	const Playhead playhead = loaded(120);
	CHECK_FALSE(playhead.playing());
	CHECK(playhead.position() == frameTime(0));
	CHECK(playhead.displayedFrame() == 0);
	CHECK(playhead.atStart());
	CHECK_FALSE(playhead.atEnd());
	CHECK(playhead.rate() == 1000);
	CHECK(playhead.effectiveRate() == 0);
}

TEST_CASE("Playhead plays forward and stops paused on the last frame")
{
	Playhead playhead = loaded(120);
	playhead.play();
	CHECK(playhead.effectiveRate() == 1000);

	playhead.advance(1s);
	CHECK(playhead.position() == frameTime(60));
	CHECK(playhead.displayedFrame() == 60);
	CHECK(playhead.playing());

	playhead.advance(1s);
	CHECK(playhead.position() == frameTime(119));
	CHECK(playhead.displayedFrame() == 119);
	CHECK(playhead.atEnd());
	CHECK_FALSE(playhead.playing());

	playhead.advance(1s);
	CHECK(playhead.position() == frameTime(119));
}

TEST_CASE("Playhead plays in reverse and stops paused on the first frame")
{
	Playhead playhead = loaded(120);
	playhead.seek(frameTime(119));
	playhead.reverse();
	CHECK(playhead.rate() == -1000);
	playhead.play();

	playhead.advance(500ms);
	CHECK(playhead.position() == frameTime(89));
	CHECK(playhead.displayedFrame() == 89);

	playhead.advance(3s);
	CHECK(playhead.position() == frameTime(0));
	CHECK(playhead.atStart());
	CHECK_FALSE(playhead.playing());
}

TEST_CASE("Playhead changes rate mid-play")
{
	Playhead playhead = loaded(600);
	playhead.play();
	playhead.advance(500ms);
	CHECK(playhead.position() == kOrigin + 500ms);

	playhead.setRate(250);
	playhead.advance(400ms);
	CHECK(playhead.position() == kOrigin + 600ms);

	playhead.setRate(-2000);
	playhead.advance(100ms);
	CHECK(playhead.position() == kOrigin + 400ms);
	CHECK(playhead.playing());
}

TEST_CASE("Playhead keeps the rate within its limits")
{
	Playhead playhead = loaded(10);

	playhead.setRate(10);
	CHECK(playhead.rate() == 50);
	playhead.setRate(10'000);
	CHECK(playhead.rate() == 4000);
	playhead.setRate(-10);
	CHECK(playhead.rate() == -50);
	playhead.setRate(std::numeric_limits<int32_t>::min());
	CHECK(playhead.rate() == -4000);
	playhead.setRate(std::numeric_limits<int32_t>::max());
	CHECK(playhead.rate() == 4000);

	playhead.setRate(750);
	playhead.play();
	playhead.setRate(0);
	CHECK_FALSE(playhead.playing());
	CHECK(playhead.rate() == 750);
}

TEST_CASE("Playhead steps whole frames across GOP boundaries")
{
	tapeloop::test::SyntheticEncoder::Config config;
	config.frameTimebase = {1001, 60000};
	tapeloop::test::SyntheticEncoder encoder(config);
	std::vector<std::shared_ptr<const tapeloop::Gop>> gops;
	tapeloop::GopBuilder builder(encoder.frameDuration());
	for (int frame = 0; frame < 90; ++frame) {
		builder.append(encoder.next());
		if (encoder.isKeyframe(encoder.nextFrame()))
			gops.push_back(builder.seal());
	}
	const tapeloop::Clip clip(gops, encoder.timeOf(5), encoder.timeOf(85));

	Playhead playhead;
	playhead.load(clip.frameTimes());
	playhead.seek(encoder.timeOf(28));
	playhead.play();

	playhead.step(3);
	CHECK_FALSE(playhead.playing());
	CHECK(playhead.position() == encoder.timeOf(31));
	CHECK(playhead.displayedFrame() == 26);

	playhead.step(-5);
	CHECK(playhead.position() == encoder.timeOf(26));

	playhead.step(-1000);
	CHECK(playhead.position() == encoder.timeOf(5));
	CHECK(playhead.atStart());

	playhead.step(1000);
	CHECK(playhead.position() == encoder.timeOf(85));
	CHECK(playhead.atEnd());

	// From a position between two frames, stepping counts from the one on screen.
	playhead.seek(encoder.timeOf(59) + 1ms);
	playhead.step(1);
	CHECK(playhead.position() == encoder.timeOf(60));
	playhead.seek(encoder.timeOf(59) + 1ms);
	playhead.step(-1);
	CHECK(playhead.position() == encoder.timeOf(58));
}

TEST_CASE("Playhead seeks within the clip")
{
	Playhead playhead = loaded(120);

	playhead.seek(frameTime(30) + 1ms);
	CHECK(playhead.position() == frameTime(30) + 1ms);
	CHECK(playhead.displayedFrame() == 30);

	playhead.seek(frameTime(-10));
	CHECK(playhead.position() == frameTime(0));
	playhead.seek(Nanoseconds::min());
	CHECK(playhead.position() == frameTime(0));

	playhead.seek(frameTime(500));
	CHECK(playhead.position() == frameTime(119));
	playhead.seek(Nanoseconds::max());
	CHECK(playhead.atEnd());
}

TEST_CASE("Playhead scrubs faster the longer it is held")
{
	Playhead playhead = loaded(600);
	playhead.seek(frameTime(300));
	playhead.beginScrub(ScrubDirection::Forward);

	CHECK(playhead.effectiveRate() == 500);
	playhead.advance(250ms);
	CHECK(playhead.effectiveRate() == 500);
	playhead.advance(625ms);
	CHECK(playhead.effectiveRate() == 2250);
	playhead.advance(625ms);
	CHECK(playhead.effectiveRate() == 4000);
	playhead.advance(1s);
	CHECK(playhead.effectiveRate() == 4000);

	playhead.beginScrub(ScrubDirection::Backward);
	CHECK(playhead.effectiveRate() == -500);
	playhead.advance(875ms);
	CHECK(playhead.effectiveRate() == -2250);
}

TEST_CASE("Playhead scrub moves by the area under the curve")
{
	Playhead playhead = loaded(600);
	playhead.beginScrub(ScrubDirection::Forward);

	// 250 ms at 0.5x, then 1250 ms ramping from 0.5x to 4x (2.25x on average).
	playhead.advance(1500ms);
	CHECK(playhead.position() == kOrigin + 125ms + 2812500us);

	// Slicing the same hold into display ticks only adds the rounding of the rate at
	// each cut. The 90 ticks are 30 ns longer than 1.5 s, at 4x by then.
	Playhead ticked = loaded(600);
	ticked.beginScrub(ScrubDirection::Forward);
	for (int tick = 0; tick < 90; ++tick)
		ticked.advance(16'666'667ns);
	const Nanoseconds difference = ticked.position() - playhead.position() - 120ns;
	CHECK(difference < 1us);
	CHECK(difference > -1us);
}

TEST_CASE("Playhead scrubbing stops at the ends of the clip")
{
	Playhead playhead = loaded(120);
	playhead.beginScrub(ScrubDirection::Forward);
	playhead.advance(10s);
	CHECK(playhead.position() == frameTime(119));

	playhead.beginScrub(ScrubDirection::Backward);
	playhead.advance(10s);
	CHECK(playhead.position() == frameTime(0));
}

TEST_CASE("Playhead resumes what it was doing after a scrub")
{
	SECTION("playing")
	{
		Playhead playhead = loaded(600);
		playhead.setRate(250);
		playhead.play();
		playhead.advance(400ms);

		playhead.beginScrub(ScrubDirection::Backward);
		CHECK(playhead.scrubbing());
		playhead.advance(100ms);
		CHECK(playhead.position() == kOrigin + 50ms);
		playhead.endScrub();

		CHECK_FALSE(playhead.scrubbing());
		CHECK(playhead.playing());
		CHECK(playhead.rate() == 250);
		CHECK(playhead.effectiveRate() == 250);
		playhead.advance(400ms);
		CHECK(playhead.position() == kOrigin + 150ms);
	}

	SECTION("paused")
	{
		Playhead playhead = loaded(600);
		playhead.beginScrub(ScrubDirection::Forward);
		playhead.advance(200ms);
		playhead.endScrub();

		CHECK_FALSE(playhead.playing());
		CHECK(playhead.position() == kOrigin + 100ms);
		playhead.advance(1s);
		CHECK(playhead.position() == kOrigin + 100ms);
	}
}

TEST_CASE("Playhead gives the same positions for the same calls")
{
	auto run = [](uint64_t seed) {
		std::mt19937_64 random(seed);
		std::uniform_int_distribution<int> action(0, 9);
		std::uniform_int_distribution<int64_t> elapsed(0, 50'000'000);
		std::uniform_int_distribution<int32_t> rate(-5000, 5000);

		Playhead playhead = loaded(3000);
		std::vector<Nanoseconds> positions;
		for (int i = 0; i < 20'000; ++i) {
			switch (action(random)) {
			case 0:
				playhead.togglePause();
				break;
			case 1:
				playhead.setRate(rate(random));
				break;
			case 2:
				playhead.reverse();
				break;
			case 3:
				playhead.step(action(random) - 5);
				break;
			case 4:
				playhead.beginScrub(action(random) < 5 ? ScrubDirection::Forward
								       : ScrubDirection::Backward);
				break;
			case 5:
				playhead.endScrub();
				break;
			default:
				playhead.advance(Nanoseconds{elapsed(random)});
				break;
			}
			positions.push_back(playhead.position());
		}
		return positions;
	};

	CHECK(run(7) == run(7));
}

TEST_CASE("Playhead does not drift over an hour of display ticks")
{
	// A 60 Hz display clock in whole nanoseconds: 16666666, 16666667, 16666667.
	const Nanoseconds ticks[] = {16'666'666ns, 16'666'667ns, 16'666'667ns};
	const int tickCount = 3599 * 60;

	Playhead realTime = loaded(3600 * 60);
	realTime.play();
	Playhead slow = loaded(3600 * 60);
	slow.setRate(333);
	slow.play();

	for (int tick = 0; tick < tickCount; ++tick) {
		realTime.advance(ticks[tick % 3]);
		slow.advance(ticks[tick % 3]);
	}

	CHECK(realTime.position() == kOrigin + 3599s);
	CHECK(realTime.displayedFrame() == static_cast<size_t>(tickCount));
	CHECK(slow.position() == kOrigin + 1'198'467ms);
}

TEST_CASE("Playhead survives extreme input")
{
	Playhead playhead = loaded(120);
	playhead.play();
	playhead.advance(Nanoseconds::max());
	CHECK(playhead.atEnd());

	playhead.setRate(-4000);
	playhead.play();
	playhead.advance(Nanoseconds::max());
	CHECK(playhead.atStart());

	playhead.advance(Nanoseconds::min());
	playhead.advance(-1s);
	CHECK(playhead.atStart());

	playhead.beginScrub(ScrubDirection::Forward);
	playhead.advance(Nanoseconds::max());
	playhead.advance(Nanoseconds::max());
	CHECK(playhead.atEnd());
	CHECK(playhead.effectiveRate() == 4000);
}

TEST_CASE("Playhead without frames does nothing")
{
	Playhead playhead;
	CHECK(playhead.empty());
	playhead.play();
	playhead.advance(1s);
	playhead.step(5);
	playhead.seek(1s);
	playhead.beginScrub(ScrubDirection::Forward);
	playhead.advance(1s);
	CHECK(playhead.displayedFrame() == 0);
	CHECK(playhead.atStart());
	CHECK(playhead.atEnd());

	playhead.load({});
	CHECK(playhead.empty());
	CHECK(playhead.position() == Nanoseconds{0});
}

TEST_CASE("Playhead stops paused when it lands exactly on a bound")
{
	Playhead playhead = loaded(120);
	playhead.play();
	playhead.advance(frameTime(119) - frameTime(0));
	CHECK(playhead.position() == frameTime(119));
	CHECK_FALSE(playhead.playing());

	playhead.setRate(-1000);
	playhead.play();
	playhead.advance(frameTime(119) - frameTime(0));
	CHECK(playhead.position() == frameTime(0));
	CHECK_FALSE(playhead.playing());
}

TEST_CASE("Playhead does not start playing at the bound it is heading for")
{
	Playhead playhead = loaded(120);
	playhead.seek(frameTime(119));
	playhead.play();
	CHECK_FALSE(playhead.playing());
	playhead.togglePause();
	CHECK_FALSE(playhead.playing());

	playhead.reverse();
	playhead.play();
	CHECK(playhead.playing());

	playhead.seek(frameTime(0));
	playhead.pause();
	playhead.togglePause();
	CHECK_FALSE(playhead.playing());
}

TEST_CASE("Playhead load stops playback and scrubbing")
{
	Playhead playhead = loaded(120);
	playhead.setRate(-500);
	playhead.seek(frameTime(60));
	playhead.play();
	playhead.beginScrub(ScrubDirection::Forward);

	playhead.load(frames(30));
	CHECK_FALSE(playhead.playing());
	CHECK_FALSE(playhead.scrubbing());
	CHECK(playhead.position() == frameTime(0));
	CHECK(playhead.rate() == -500);
}

TEST_CASE("Playhead sorts and deduplicates the frame times it loads")
{
	Playhead playhead;
	playhead.load({9s, 3s, 5s, 3s, 9s});
	CHECK(playhead.position() == 3s);
	playhead.step(1);
	CHECK(playhead.position() == 5s);
	playhead.step(5);
	CHECK(playhead.position() == 9s);
	CHECK(playhead.displayedFrame() == 2);
}

TEST_CASE("Playhead brings its configuration into range")
{
	tapeloop::PlayheadConfig config;
	config.minRate = 100;
	config.maxRate = 500;
	CHECK(Playhead(config).rate() == 500);

	config.minRate = 5000;
	config.maxRate = 10;
	config.scrubStartRate = 0;
	config.scrubEndRate = -5;
	config.scrubRampStart = -1s;
	config.scrubRampEnd = -2s;
	Playhead playhead(config);
	playhead.load(frames(120));

	playhead.setRate(100);
	CHECK(playhead.rate() == 5000);
	playhead.setRate(std::numeric_limits<int32_t>::min());
	CHECK(playhead.rate() == -5000);

	playhead.beginScrub(ScrubDirection::Forward);
	CHECK(playhead.effectiveRate() == 1);
	playhead.advance(1s);
	CHECK(playhead.position() == frameTime(0) + 1ms);
}

TEST_CASE("Playhead scrub ramp of zero length jumps straight to the end rate")
{
	tapeloop::PlayheadConfig config;
	config.scrubRampStart = 250ms;
	config.scrubRampEnd = 250ms;

	Playhead whole(config);
	whole.load(frames(600));
	whole.beginScrub(ScrubDirection::Forward);
	whole.advance(1250ms);
	CHECK(whole.position() == kOrigin + 4125ms);

	Playhead ticked(config);
	ticked.load(frames(600));
	ticked.beginScrub(ScrubDirection::Forward);
	for (int tick = 0; tick < 125; ++tick)
		ticked.advance(10ms);
	CHECK(ticked.position() == kOrigin + 4125ms);
}

TEST_CASE("Playhead scrub ramps longer than two seconds")
{
	tapeloop::PlayheadConfig config;
	config.scrubRampEnd = 5s;
	Playhead playhead(config);
	playhead.load(frames(3600));
	playhead.beginScrub(ScrubDirection::Forward);

	playhead.advance(250ms + 2375ms);
	CHECK(playhead.effectiveRate() == 2250);
	playhead.advance(2375ms);
	CHECK(playhead.effectiveRate() == 4000);
	// 250 ms at 0.5x, then 4750 ms ramping from 0.5x to 4x.
	CHECK(playhead.position() == kOrigin + 125ms + 10'687'500us);

	// A ramp so long that offset times rate change needs more than 64 bits.
	config.scrubStartRate = 1;
	config.scrubEndRate = std::numeric_limits<int32_t>::max();
	config.scrubRampStart = 0s;
	config.scrubRampEnd = Nanoseconds{int64_t{1} << 62};
	Playhead huge(config);
	huge.load(frames(10));
	huge.beginScrub(ScrubDirection::Forward);
	huge.advance(Nanoseconds{int64_t{1} << 61});
	CHECK(huge.effectiveRate() > (1 << 30) - 2);
	CHECK(huge.effectiveRate() < (1 << 30) + 2);
}

TEST_CASE("Playhead does not drift in reverse at a fractional rate")
{
	const Nanoseconds ticks[] = {16'666'666ns, 16'666'667ns, 16'666'667ns};
	const int tickCount = 3599 * 60;

	Playhead playhead = loaded(3600 * 60);
	playhead.seek(frameTime(3600 * 60 - 1));
	playhead.setRate(-333);
	playhead.play();
	for (int tick = 0; tick < tickCount; ++tick)
		playhead.advance(ticks[tick % 3]);

	CHECK(playhead.position() == frameTime(3600 * 60 - 1) - 1'198'467ms);
}

#if defined(__SIZEOF_INT128__)

TEST_CASE("Playhead position is exact against 128-bit arithmetic")
{
	__extension__ typedef __int128 Int128;

	// One frame per second for a day, starting in the middle, so no sequence below
	// reaches a bound.
	std::vector<Nanoseconds> times;
	for (int64_t second = 0; second < 86'400; ++second)
		times.push_back(Nanoseconds{second * 1'000'000'000});
	Playhead playhead;
	playhead.load(times);
	playhead.seek(43'200s);

	std::mt19937_64 random(20261003);
	std::uniform_int_distribution<int> action(0, 9);
	std::uniform_int_distribution<int64_t> elapsed(0, 50'000'000);
	std::uniform_int_distribution<int32_t> rate(-4000, 4000);

	// position * 2000 plus the remainder, in the same units the playhead uses.
	Int128 exact = Int128{43'200'000'000'000} * 2000;
	for (int i = 0; i < 20'000; ++i) {
		switch (action(random)) {
		case 0:
			playhead.togglePause();
			break;
		case 1:
			playhead.setRate(rate(random));
			break;
		case 2:
			playhead.reverse();
			break;
		default: {
			const int64_t step = elapsed(random);
			if (playhead.playing())
				exact += Int128{step} * 2 * playhead.rate();
			playhead.advance(Nanoseconds{step});
			break;
		}
		}
		const Int128 floor = exact >= 0 ? exact / 2000 : (exact - 1999) / 2000;
		REQUIRE(playhead.position().count() == static_cast<int64_t>(floor));
	}
}

#endif

TEST_CASE("Playhead reaches the end of a clip that spans most of the clock")
{
	// Found by the fuzzer: a step whose length times the rate overflows used to move by
	// the saturated product from a negative position and stop short of the end.
	tapeloop::PlayheadConfig config;
	config.maxRate = 50'000;
	Playhead playhead(config);
	playhead.load({Nanoseconds{-500'000'000'000'000'000}, Nanoseconds{9'000'000'000'000'000'000}});
	playhead.setRate(45'362);
	playhead.play();

	Playhead twoSteps = playhead;
	playhead.advance(Nanoseconds{5'280'832'617'179'597'129});
	CHECK(playhead.atEnd());
	CHECK_FALSE(playhead.playing());

	twoSteps.advance(Nanoseconds{1'000'000'000'000'000'000});
	twoSteps.advance(Nanoseconds{4'280'832'617'179'597'129});
	CHECK(twoSteps.position() == playhead.position());
}

TEST_CASE("Playhead moves exactly over steps whose product needs more than 64 bits")
{
	tapeloop::PlayheadConfig config;
	config.maxRate = 50'000;
	Playhead playhead(config);
	playhead.load({Nanoseconds::min() + 1s, Nanoseconds::max() - 1s});
	playhead.setRate(4'500);
	playhead.play();

	// 2.2e18 ns at 4.5x is 9.9e18 ns, just more than the range from the start to zero,
	// and 2.2e18 * 9000 does not fit in 64 bits.
	const int64_t elapsed = 2'200'000'000'000'000'000;
	playhead.advance(Nanoseconds{elapsed});
	CHECK(playhead.position() ==
	      Nanoseconds::min() + 1s + Nanoseconds{elapsed / 2 * 4} + Nanoseconds{elapsed / 2 * 5});
}

TEST_CASE("Playhead moves away from the edges of the clock")
{
	// Found by the fuzzer: a clip starting at the earliest representable time did not
	// play forward, nor one ending at the latest play in reverse.
	Playhead first;
	first.load({Nanoseconds::min(), Nanoseconds::min() + 1s});
	first.play();
	first.advance(500ms);
	CHECK(first.position() == Nanoseconds::min() + 500ms);

	Playhead last;
	last.load({Nanoseconds::max() - 1s, Nanoseconds::max()});
	last.seek(Nanoseconds::max());
	last.reverse();
	last.play();
	last.advance(500ms);
	CHECK(last.position() == Nanoseconds::max() - 500ms);
}
