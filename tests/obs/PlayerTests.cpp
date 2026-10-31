// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "AllocationCounter.hpp"
#include "ObsFixture.hpp"
#include "PlayerClips.hpp"
#include "TestPattern.hpp"

#include "core/Clip.hpp"
#include "core/GopSource.hpp"
#include "core/MediaTime.hpp"
#include "core/Playhead.hpp"
#include "decode/FFmpegDecoder.hpp"
#include "obs/PictureRenderer.hpp"
#include "obs/ReplayPlayer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <obs.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

using namespace std::chrono_literals;
using tapeloop::Clip;
using tapeloop::ClipGops;
using tapeloop::Nanoseconds;
using tapeloop::ScrubDirection;
using tapeloop::decode::FFmpegDecoder;
using tapeloop::decode::PictureDecoder;
using tapeloop::obs::PictureRenderer;
using tapeloop::obs::PlayerState;
using tapeloop::obs::ReplayPlayer;
using tapeloop::test::AllocationCounter;
using tapeloop::test::AllocationFailure;
using tapeloop::test::captureClip;
using tapeloop::test::expectedFrames;
using tapeloop::test::ExpectedFrame;
using tapeloop::test::gopStarts;
using tapeloop::test::kClipHeight;
using tapeloop::test::kClipWidth;
using tapeloop::test::ObsFixture;
using tapeloop::test::waitFor;

namespace {

// One frame of a 60 Hz canvas.
constexpr Nanoseconds kTick{16'666'667};

std::unique_ptr<PictureDecoder> makeDecoder()
{
	return std::make_unique<FFmpegDecoder>();
}

std::unique_ptr<ClipGops> gopsOf(const Clip &clip)
{
	return std::make_unique<ClipGops>(clip);
}

struct Shown {
	bool drawn = false;
	std::optional<uint32_t> number;
};

// Stands in for the source: a renderer and a target to draw into, made and destroyed
// inside the graphics context, and the test thread in place of the player's own.
class Screen {
public:
	Screen()
	{
		obs_enter_graphics();
		renderer_.emplace();
		target_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		stage_ = gs_stagesurface_create(kClipWidth, kClipHeight, GS_RGBA);
		obs_leave_graphics();
		REQUIRE(target_);
		REQUIRE(stage_);
	}

	~Screen()
	{
		obs_enter_graphics();
		renderer_.reset();
		gs_stagesurface_destroy(stage_);
		gs_texrender_destroy(target_);
		obs_leave_graphics();
	}

	Screen(const Screen &) = delete;
	Screen &operator=(const Screen &) = delete;

	// What render() draws, read back unless asked not to. Counts the allocations of
	// render() alone into allocations when given.
	Shown draw(ReplayPlayer &player, bool read = true, size_t *allocations = nullptr)
	{
		Shown shown;
		obs_enter_graphics();
		gs_texrender_reset(target_);
		const bool began = gs_texrender_begin(target_, kClipWidth, kClipHeight);
		if (began) {
			vec4 clear;
			vec4_zero(&clear);
			gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
			gs_ortho(0.0f, static_cast<float>(kClipWidth), 0.0f, static_cast<float>(kClipHeight), -100.0f,
				 100.0f);
			// The device culls back faces until OBS's video thread first renders.
			gs_set_cull_mode(GS_NEITHER);
			gs_enable_depth_test(false);
			if (allocations) {
				const AllocationCounter counter;
				shown.drawn = player.render(*renderer_, kClipWidth, kClipHeight);
				*allocations += counter.count();
			} else {
				shown.drawn = player.render(*renderer_, kClipWidth, kClipHeight);
			}
			gs_texrender_end(target_);
		}
		uint8_t *data = nullptr;
		uint32_t stride = 0;
		bool mapped = false;
		if (shown.drawn && read) {
			gs_stage_texture(stage_, gs_texrender_get_texture(target_));
			mapped = gs_stagesurface_map(stage_, &data, &stride);
			if (mapped) {
				for (size_t i = 0; i < reds_.size(); ++i) {
					reds_[i] = data[(i / kClipWidth) * stride + (i % kClipWidth) * 4];
				}
				gs_stagesurface_unmap(stage_);
			}
		}
		obs_leave_graphics();
		REQUIRE(began);
		if (shown.drawn && read) {
			REQUIRE(mapped);
			shown.number = tapeloop::test::readFrameNumber(
				reds_.data(), kClipWidth, static_cast<int>(kClipWidth), static_cast<int>(kClipHeight));
		}
		return shown;
	}

	// A tick of the source: the playhead moves, whatever it asked for is decoded, and
	// the frame is drawn.
	Shown frame(ReplayPlayer &player, Nanoseconds elapsed = kTick)
	{
		player.tick(elapsed);
		decodeAll(player);
		return draw(player);
	}

	static void decodeAll(ReplayPlayer &player)
	{
		// One tick asks for one frame at most, so a third pass means the player spins.
		size_t passes = 0;
		while (player.decodeNext()) {
			REQUIRE(++passes < 3);
		}
	}

private:
	std::optional<PictureRenderer> renderer_;
	gs_texrender_t *target_ = nullptr;
	gs_stagesurf_t *stage_ = nullptr;
	std::vector<uint8_t> reds_ = std::vector<uint8_t>(size_t{kClipWidth} * kClipHeight);
};

// A tick, then the frame on screen must be the one the playhead is on.
PlayerState checkTick(Screen &screen, ReplayPlayer &player, const std::vector<ExpectedFrame> &expected,
		      Nanoseconds elapsed = kTick)
{
	const Shown shown = screen.frame(player, elapsed);
	const PlayerState state = player.state();
	CAPTURE(state.frame, state.position.count(), state.rate);
	REQUIRE(state.loaded);
	REQUIRE(state.frames == expected.size());
	REQUIRE(state.frame < expected.size());
	// The frame on screen is the last one starting at or before the position.
	CHECK(expected[state.frame].time <= state.position);
	if (state.frame + 1 < expected.size()) {
		CHECK(state.position < expected[state.frame + 1].time);
	}
	REQUIRE(shown.drawn);
	REQUIRE(state.drawn == state.frame);
	CHECK(state.width == kClipWidth);
	CHECK(state.height == kClipHeight);
	CHECK(shown.number == expected[state.frame].number);
	return state;
}

} // namespace

TEST_CASE_METHOD(ObsFixture, "the replay player shows the frame its playhead is on at every speed, either way",
		 "[obs][player]")
{
	const Clip clip = captureClip(4);
	REQUIRE(clip.gops().size() >= 4);
	const std::vector<ExpectedFrame> expected = expectedFrames(clip);
	const std::vector<size_t> starts = gopStarts(clip, expected);
	// A boundary with a GOP on each side, which every run crosses.
	const size_t boundary = starts[1];
	REQUIRE(boundary >= 8);

	Screen screen;
	ReplayPlayer player(makeDecoder);
	REQUIRE(player.load(gopsOf(clip)));
	PlayerState state = checkTick(screen, player, expected, 0ns);
	CHECK(state.frame == 0);
	CHECK_FALSE(state.playing);

	for (const int32_t rate : {250, 500, 750, 1000, 2000, 4000}) {
		for (const int32_t sign : {1, -1}) {
			CAPTURE(rate * sign);
			player.setRate(rate * sign);
			player.seek(expected[sign > 0 ? boundary - 4 : boundary + 3].time);
			player.play();
			state = checkTick(screen, player, expected);
			CHECK(state.playing);
			CHECK(state.rate == rate * sign);
			size_t previous = state.frame;
			bool crossed = false;
			for (int tick = 0; tick < 120 && state.playing; ++tick) {
				state = checkTick(screen, player, expected);
				CHECK((sign > 0 ? state.frame >= previous : state.frame <= previous));
				previous = state.frame;
				crossed = crossed || (sign > 0 ? state.frame >= boundary : state.frame < boundary);
			}
			CHECK(crossed);
		}
	}
}

TEST_CASE_METHOD(ObsFixture, "the replay player steps, seeks, scrubs, reverses and pauses on the frame shown",
		 "[obs][player]")
{
	// The last GOP may have just begun when the capture stops.
	const Clip clip = captureClip(5);
	REQUIRE(clip.gops().size() >= 5);
	const std::vector<ExpectedFrame> expected = expectedFrames(clip);
	const std::vector<size_t> starts = gopStarts(clip, expected);

	Screen screen;
	ReplayPlayer player(makeDecoder);
	REQUIRE(player.load(gopsOf(clip)));
	checkTick(screen, player, expected, 0ns);

	SECTION("stepping goes frame by frame across GOPs, both ways")
	{
		const size_t last = starts[1] + 2;
		for (size_t frame = 1; frame <= last; ++frame) {
			player.step(1);
			const PlayerState state = checkTick(screen, player, expected);
			CHECK(state.frame == frame);
			CHECK_FALSE(state.playing);
		}
		for (size_t frame = last; frame-- > 0;) {
			player.step(-1);
			CHECK(checkTick(screen, player, expected).frame == frame);
		}
		player.step(static_cast<int>(starts[2]) + 1);
		CHECK(checkTick(screen, player, expected).frame == starts[2] + 1);
		player.step(-static_cast<int>(starts[1]));
		CHECK(checkTick(screen, player, expected).frame == starts[2] + 1 - starts[1]);
		// Stepping stops at either end.
		player.step(-100'000);
		CHECK(checkTick(screen, player, expected).frame == 0);
		player.step(100'000);
		CHECK(checkTick(screen, player, expected).frame == expected.size() - 1);
	}

	SECTION("seeking lands on the frame at that time, in any order")
	{
		// A stride that shares no factor with the frame count visits every frame once.
		size_t stride = 37;
		while (expected.size() % stride == 0) {
			++stride;
		}
		for (size_t i = 0; i < expected.size(); ++i) {
			const size_t frame = i * stride % expected.size();
			// Between two frames is the earlier one.
			const Nanoseconds within = frame + 1 < expected.size() ? 1ms : 0ms;
			player.seek(expected[frame].time + within);
			CHECK(checkTick(screen, player, expected).frame == frame);
		}
		player.seek(Nanoseconds::min());
		CHECK(checkTick(screen, player, expected).frame == 0);
		player.seek(Nanoseconds::max());
		CHECK(checkTick(screen, player, expected).frame == expected.size() - 1);
	}

	SECTION("scrubbing goes either way and then resumes what was going on")
	{
		player.setRate(500);
		player.seek(expected[starts[1] + 2].time);
		player.play();
		PlayerState state = checkTick(screen, player, expected);
		player.beginScrub(ScrubDirection::Backward);
		size_t previous = state.frame;
		for (int tick = 0; tick < 30; ++tick) {
			state = checkTick(screen, player, expected);
			CHECK(state.scrubbing);
			CHECK(state.frame <= previous);
			previous = state.frame;
		}
		CHECK(state.frame < starts[1]);

		player.beginScrub(ScrubDirection::Forward);
		for (int tick = 0; tick < 30; ++tick) {
			state = checkTick(screen, player, expected);
			CHECK(state.frame >= previous);
			previous = state.frame;
		}
		CHECK(state.frame > starts[1]);

		player.endScrub();
		state = checkTick(screen, player, expected);
		CHECK_FALSE(state.scrubbing);
		CHECK(state.playing);
		CHECK(state.rate == 500);
		for (int tick = 0; tick < 10; ++tick) {
			state = checkTick(screen, player, expected);
			CHECK(state.frame >= previous);
			previous = state.frame;
		}
	}

	SECTION("reversing turns play around where it is")
	{
		player.seek(expected[starts[1] + 5].time);
		player.play();
		PlayerState state = checkTick(screen, player, expected);
		for (int tick = 0; tick < 6; ++tick) {
			state = checkTick(screen, player, expected);
		}
		const size_t turned = state.frame;
		CHECK(turned > starts[1] + 5);
		player.reverse();
		state = checkTick(screen, player, expected);
		CHECK(state.rate == -1000);
		CHECK(state.playing);
		size_t previous = state.frame;
		for (int tick = 0; tick < 30; ++tick) {
			state = checkTick(screen, player, expected);
			CHECK(state.frame <= previous);
			previous = state.frame;
		}
		CHECK(state.frame < starts[1]);
	}

	SECTION("pausing holds the frame, and toggling resumes it")
	{
		player.play();
		PlayerState state = checkTick(screen, player, expected);
		for (int tick = 0; tick < 5; ++tick) {
			state = checkTick(screen, player, expected);
		}
		player.pause();
		const PlayerState held = checkTick(screen, player, expected);
		for (int tick = 0; tick < 10; ++tick) {
			state = checkTick(screen, player, expected);
			CHECK(state.position == held.position);
			CHECK_FALSE(state.playing);
		}
		// Play resumes from the tick after the one that applies it.
		player.togglePause();
		state = checkTick(screen, player, expected);
		CHECK(state.playing);
		CHECK(state.position == held.position);
		for (int tick = 0; tick < 3; ++tick) {
			state = checkTick(screen, player, expected);
		}
		CHECK(state.position > held.position);
		CHECK(state.frame > held.frame);
		player.togglePause();
		state = checkTick(screen, player, expected);
		CHECK_FALSE(state.playing);
		// Zero is not a speed: it pauses and keeps the one there was.
		player.play();
		player.setRate(0);
		state = checkTick(screen, player, expected);
		CHECK_FALSE(state.playing);
		CHECK(state.rate == 1000);
	}
}

TEST_CASE_METHOD(ObsFixture, "the replay player's speed presets keep the direction and the play state", "[obs][player]")
{
	const Clip clip = captureClip(3);
	REQUIRE(clip.gops().size() >= 3);
	const std::vector<ExpectedFrame> expected = expectedFrames(clip);

	Screen screen;
	ReplayPlayer player(makeDecoder);
	REQUIRE(player.load(gopsOf(clip)));
	checkTick(screen, player, expected, 0ns);

	// Each control on its own tick, while playing forward from the start.
	player.play();
	auto rateAfter = [&](void (ReplayPlayer::*control)()) {
		(player.*control)();
		const PlayerState state = checkTick(screen, player, expected);
		CHECK(state.playing);
		return state.rate;
	};
	CHECK(rateAfter(&ReplayPlayer::faster) == 2000);
	CHECK(rateAfter(&ReplayPlayer::faster) == 2000);
	player.seek(expected.front().time);
	for (const int32_t rate : {1000, 750, 500, 250, 250}) {
		CHECK(rateAfter(&ReplayPlayer::slower) == rate);
	}
	CHECK(rateAfter(&ReplayPlayer::normalSpeed) == 1000);

	// Backwards from the end, the same ladder with the sign kept.
	player.seek(expected.back().time);
	player.reverse();
	for (const int32_t rate : {-2000, -2000}) {
		CHECK(rateAfter(&ReplayPlayer::faster) == rate);
	}
	player.seek(expected.back().time);
	for (const int32_t rate : {-1000, -750, -500, -250, -250}) {
		CHECK(rateAfter(&ReplayPlayer::slower) == rate);
	}
	CHECK(rateAfter(&ReplayPlayer::normalSpeed) == -1000);

	// Paused, the speed changes and the frame stays.
	player.pause();
	const PlayerState held = checkTick(screen, player, expected);
	player.faster();
	PlayerState state = checkTick(screen, player, expected);
	CHECK_FALSE(state.playing);
	CHECK(state.rate == -2000);
	CHECK(state.position == held.position);

	// From speeds set off the ladder, and several controls in one tick, in order.
	struct Case {
		int32_t from;
		void (ReplayPlayer::*control)();
		int32_t to;
	};
	for (const Case &step :
	     {Case{600, &ReplayPlayer::faster, 750}, Case{600, &ReplayPlayer::slower, 500},
	      Case{-600, &ReplayPlayer::faster, -750}, Case{4000, &ReplayPlayer::faster, 4000},
	      Case{4000, &ReplayPlayer::slower, 2000}, Case{100, &ReplayPlayer::slower, 100},
	      Case{100, &ReplayPlayer::faster, 250}, Case{-3000, &ReplayPlayer::normalSpeed, -1000}}) {
		CAPTURE(step.from);
		player.setRate(step.from);
		(player.*step.control)();
		CHECK(checkTick(screen, player, expected).rate == step.to);
	}
	player.setRate(250);
	player.faster();
	player.faster();
	player.slower();
	player.faster();
	CHECK(checkTick(screen, player, expected).rate == 750);
}

TEST_CASE_METHOD(ObsFixture, "a replay player shows nothing of the clip before at the speed it had", "[obs][player]")
{
	const Clip clip = captureClip(4);
	REQUIRE(clip.gops().size() >= 4);
	const std::vector<ExpectedFrame> expected = expectedFrames(clip);
	const Clip later({clip.gops().begin() + 2, clip.gops().end()}, Nanoseconds::min(), Nanoseconds::max());
	const std::vector<ExpectedFrame> laterExpected = expectedFrames(later);
	REQUIRE(laterExpected.front().number != expected.front().number);

	Screen screen;
	ReplayPlayer player(makeDecoder);
	REQUIRE(player.load(gopsOf(clip)));
	checkTick(screen, player, expected, 0ns);
	player.setRate(-2000);
	player.play();
	checkTick(screen, player, expected);
	player.step(3);
	checkTick(screen, player, expected);

	// A picture of the clip before is decoded and waiting when the next clip loads.
	// Controls before the load apply to the clip before; the speed stays, forward.
	player.step(1);
	player.tick(kTick);
	Screen::decodeAll(player);
	player.step(5);
	player.setRate(-500);
	REQUIRE(player.load(gopsOf(later)));
	player.tick(kTick);
	PlayerState state = player.state();
	CHECK(state.frame == 0);
	CHECK(state.frames == laterExpected.size());
	CHECK(state.rate == 500);
	CHECK_FALSE(state.playing);
	CHECK_FALSE(state.drawn);
	CHECK_FALSE(screen.draw(player).drawn);
	CHECK_FALSE(player.state().drawn);

	Screen::decodeAll(player);
	const Shown shown = screen.draw(player);
	CHECK(shown.drawn);
	CHECK(shown.number == laterExpected.front().number);
	CHECK(player.state().drawn == size_t{0});

	// Controls after a load apply to the new clip.
	REQUIRE(player.load(gopsOf(clip)));
	player.step(2);
	state = checkTick(screen, player, expected);
	CHECK(state.frame == 2);

	// Nothing to show: no frames, nothing drawn, and nothing asked of the decoder.
	REQUIRE(player.load(nullptr));
	player.play();
	player.tick(kTick);
	Screen::decodeAll(player);
	state = player.state();
	CHECK_FALSE(state.loaded);
	CHECK(state.frames == 0);
	CHECK_FALSE(screen.draw(player).drawn);
}

TEST_CASE_METHOD(ObsFixture, "a replay player without a decoder keeps its playhead and tries again at the next load",
		 "[obs][player]")
{
	const Clip clip = captureClip(2);
	const std::vector<ExpectedFrame> expected = expectedFrames(clip);

	Screen screen;
	for (const bool throws : {false, true}) {
		CAPTURE(throws);
		int calls = 0;
		ReplayPlayer player([&]() -> std::unique_ptr<PictureDecoder> {
			if (++calls == 1) {
				if (throws) {
					throw std::runtime_error("no decoder");
				}
				return nullptr;
			}
			return makeDecoder();
		});
		REQUIRE(player.load(gopsOf(clip)));
		player.step(3);
		Shown shown = screen.frame(player);
		const PlayerState state = player.state();
		CHECK(calls == 1);
		CHECK(state.loaded);
		CHECK(state.frame == 3);
		CHECK_FALSE(shown.drawn);
		CHECK_FALSE(state.drawn);
		// A request that failed is not tried again on every tick.
		shown = screen.frame(player);
		CHECK_FALSE(shown.drawn);
		CHECK(calls == 1);

		REQUIRE(player.load(gopsOf(clip)));
		checkTick(screen, player, expected, 0ns);
		CHECK(calls == 2);
		player.step(3);
		checkTick(screen, player, expected);
	}
}

TEST_CASE_METHOD(ObsFixture, "a replay player out of memory shows nothing wrong and recovers at the next load",
		 "[obs][player]")
{
	if (!tapeloop::test::kAllocationFailures) {
		SKIP("allocations cannot be made to fail in this build");
	}
	const Clip clip = captureClip(2);
	const std::vector<ExpectedFrame> expected = expectedFrames(clip);
	Screen screen;

	for (size_t skip = 0;; ++skip) {
		CAPTURE(skip);
		ReplayPlayer player(makeDecoder);
		REQUIRE(player.load(gopsOf(clip)));
		player.tick(0ns);
		bool triggered = false;
		{
			// Nothing of the test's own may allocate in here.
			const AllocationFailure failure(skip);
			for (int pass = 0; pass < 3 && player.decodeNext(); ++pass) {
			}
			triggered = failure.triggered();
		}
		CHECK_FALSE(player.decodeNext());
		const Shown shown = screen.draw(player);
		if (shown.drawn) {
			CHECK(shown.number == expected.front().number);
		}

		REQUIRE(player.load(gopsOf(clip)));
		checkTick(screen, player, expected, 0ns);
		player.step(1);
		checkTick(screen, player, expected);
		if (!triggered) {
			break;
		}
	}
}

TEST_CASE_METHOD(ObsFixture, "steady replay playback allocates nothing per frame", "[obs][player]")
{
	if (!tapeloop::test::kAllocationHooks) {
		SKIP("allocations cannot be counted in this build");
	}
	const Clip clip = captureClip(4);
	REQUIRE(clip.gops().size() >= 4);
	const std::vector<ExpectedFrame> expected = expectedFrames(clip);
	const std::vector<size_t> starts = gopStarts(clip, expected);

	Screen screen;
	ReplayPlayer player(makeDecoder);
	REQUIRE(player.load(gopsOf(clip)));

	// Ticks the way the source does, counting what the player allocates and timing
	// what it decodes.
	size_t allocations = 0;
	std::chrono::steady_clock::duration decoding{};
	size_t decodes = 0;
	auto run = [&](int count) {
		for (int tick = 0; tick < count; ++tick) {
			{
				const AllocationCounter counter;
				player.tick(kTick);
				const auto started = std::chrono::steady_clock::now();
				while (player.decodeNext()) {
					++decodes;
				}
				decoding += std::chrono::steady_clock::now() - started;
				allocations += counter.count();
			}
			REQUIRE(screen.draw(player, false, &allocations).drawn);
			REQUIRE(player.state().drawn == player.state().frame);
		}
	};
	auto workout = [&] {
		player.setRate(1000);
		player.seek(expected.front().time);
		player.play();
		run(static_cast<int>(expected.size()) * 2);
		player.setRate(-1000);
		player.play();
		run(static_cast<int>(expected.size()) * 2);
		player.setRate(2000);
		player.play();
		run(static_cast<int>(expected.size()));
		player.seek(expected[starts[2]].time);
		player.beginScrub(ScrubDirection::Backward);
		run(60);
		player.beginScrub(ScrubDirection::Forward);
		run(60);
		player.endScrub();
		player.pause();
		for (int step = 0; step < 10; ++step) {
			player.step(step % 2 == 0 ? -3 : 1);
			run(1);
		}
	};

	// The first pass sizes the planner's and the player's buffers and the textures.
	workout();
	allocations = 0;
	decoding = {};
	decodes = 0;
	workout();
	CHECK(allocations == 0);
	// A baseline for the CPU path; the planner's cache serves some of these frames.
	const double milliseconds = std::chrono::duration<double, std::milli>(decoding).count();
	std::printf("[player] %zu frames of 320x180 H.264 decoded and copied in %.1f ms, %.3f ms each\n", decodes,
		    milliseconds, milliseconds / static_cast<double>(decodes));
}

TEST_CASE_METHOD(ObsFixture, "a replay player decodes on its own thread once started", "[obs][player]")
{
	const Clip clip = captureClip(4);
	REQUIRE(clip.gops().size() >= 4);
	const std::vector<ExpectedFrame> expected = expectedFrames(clip);
	const std::vector<size_t> starts = gopStarts(clip, expected);

	Screen screen;
	ReplayPlayer player(makeDecoder);
	REQUIRE(player.start());
	REQUIRE(player.start());
	REQUIRE(player.load(gopsOf(clip)));

	// The picture follows the playhead when it is ready; each tick waits for it here.
	auto settle = [&](Nanoseconds elapsed) {
		player.tick(elapsed);
		REQUIRE(waitFor(
			[&] {
				const bool drawn = screen.draw(player, false).drawn;
				const PlayerState state = player.state();
				return drawn && state.drawn == state.frame;
			},
			10s));
		const PlayerState state = player.state();
		CAPTURE(state.frame);
		CHECK(screen.draw(player).number == expected[state.frame].number);
		return state;
	};
	settle(0ns);
	player.seek(expected[starts[1] - 3].time);
	player.play();
	PlayerState state = settle(kTick);
	for (int tick = 0; tick < 20; ++tick) {
		state = settle(kTick);
	}
	CHECK(state.frame > starts[1]);
	player.reverse();
	for (int tick = 0; tick < 30; ++tick) {
		state = settle(kTick);
	}
	CHECK(state.frame < starts[1]);

	// Ticks that do not wait leave the decoder behind; the player keeps up with the
	// latest frame and stops cleanly in the middle of it.
	player.setRate(4000);
	for (int tick = 0; tick < 30; ++tick) {
		player.tick(kTick);
		screen.draw(player, false);
	}
	player.stop();
	CHECK_FALSE(player.decodeNext());
	CHECK_FALSE(player.start());
	// The playhead still answers, with nothing decoded for it.
	const size_t at = player.state().frame;
	REQUIRE(at > 0);
	player.step(-1);
	player.tick(kTick);
	CHECK(player.state().frame == at - 1);
}
