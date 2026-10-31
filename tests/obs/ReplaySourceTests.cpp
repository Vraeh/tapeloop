// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "AllocationCounter.hpp"
#include "ObsFixture.hpp"
#include "PlayerClips.hpp"
#include "TestPattern.hpp"

#include "core/Clip.hpp"
#include "core/FrameDecoder.hpp"
#include "core/GopSource.hpp"
#include "core/MediaTime.hpp"
#include "core/Playhead.hpp"
#include "decode/FFmpegDecoder.hpp"
#include "decode/Picture.hpp"
#include "decode/PictureDecoder.hpp"
#include "obs/ReplayPlayer.hpp"
#include "obs/ReplaySource.hpp"

#include <catch2/catch_test_macros.hpp>
#include <obs.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using tapeloop::Clip;
using tapeloop::ClipGops;
using tapeloop::DecodedFrame;
using tapeloop::DecodeStatus;
using tapeloop::Nanoseconds;
using tapeloop::ScrubDirection;
using tapeloop::VideoCodec;
using tapeloop::decode::FFmpegDecoder;
using tapeloop::decode::Picture;
using tapeloop::decode::PictureDecoder;
using tapeloop::obs::createReplaySource;
using tapeloop::obs::kReplaySourceId;
using tapeloop::obs::kSpeedPresets;
using tapeloop::obs::PlayerState;
using tapeloop::obs::registerReplaySource;
using tapeloop::obs::ReplayPlayer;
using tapeloop::obs::replayPlayer;
using tapeloop::test::AllocationCounter;
using tapeloop::test::captureClip;
using tapeloop::test::expectedFrames;
using tapeloop::test::ExpectedFrame;
using tapeloop::test::gopStarts;
using tapeloop::test::kClipHeight;
using tapeloop::test::kClipWidth;
using tapeloop::test::ObsFixture;

namespace {

std::unique_ptr<PictureDecoder> makeDecoder()
{
	return std::make_unique<FFmpegDecoder>();
}

std::unique_ptr<ClipGops> gopsOf(const Clip &clip)
{
	return std::make_unique<ClipGops>(clip);
}

// Holds OBS's graphics thread at the start of each frame, before the sources tick, and
// lets it through one frame at a time, so that the test thread can draw a source and read
// its player between two of its ticks. Optionally counts what the graphics thread
// allocates from the sources' tick to the main render of each frame let through.
class TickGate {
public:
	TickGate()
	{
		obs_add_main_render_callback(onRender, this);
		obs_add_tick_callback(onTick, this);
	}

	~TickGate()
	{
		{
			const std::lock_guard lock(mutex_);
			open_ = true;
		}
		changed_.notify_all();
		obs_remove_tick_callback(onTick, this);
		// The counter of a frame let through ends on the graphics thread, in its render.
		bool counting = false;
		{
			std::unique_lock lock(mutex_);
			counting = !changed_.wait_for(lock, 10s, [this] { return !counter_; });
		}
		obs_remove_main_render_callback(onRender, this);
		CHECK_FALSE(counting);
	}

	TickGate(const TickGate &) = delete;
	TickGate &operator=(const TickGate &) = delete;

	// Waits until the graphics thread is held.
	void hold()
	{
		std::unique_lock lock(mutex_);
		REQUIRE(changed_.wait_for(lock, 10s, [this] { return arrivals_ > grants_; }));
	}

	// Lets one frame through and returns the time OBS ticked the sources by in it, once
	// the graphics thread is held at the start of the next.
	float frame()
	{
		std::unique_lock lock(mutex_);
		REQUIRE(changed_.wait_for(lock, 10s, [this] { return arrivals_ > grants_; }));
		const float seconds = seconds_;
		++grants_;
		changed_.notify_all();
		REQUIRE(changed_.wait_for(lock, 10s, [this] { return arrivals_ > grants_; }));
		return seconds;
	}

	// From the next frame on, counts the graphics thread's allocations from zero.
	void countAllocations()
	{
		const std::lock_guard lock(mutex_);
		counting_ = true;
		allocations_ = 0;
	}

	size_t allocations()
	{
		const std::lock_guard lock(mutex_);
		return allocations_;
	}

private:
	static void onTick(void *param, float seconds) noexcept
	{
		auto &gate = *static_cast<TickGate *>(param);
		std::unique_lock lock(gate.mutex_);
		gate.seconds_ = seconds;
		const uint64_t arrival = ++gate.arrivals_;
		gate.changed_.notify_all();
		gate.changed_.wait(lock, [&] { return gate.open_ || gate.grants_ >= arrival; });
		if (gate.counting_ && !gate.open_) {
			gate.counter_.emplace();
		}
	}

	static void onRender(void *param, uint32_t, uint32_t) noexcept
	{
		auto &gate = *static_cast<TickGate *>(param);
		const std::lock_guard lock(gate.mutex_);
		if (gate.counter_) {
			gate.allocations_ += gate.counter_->count();
			gate.counter_.reset();
			gate.changed_.notify_all();
		}
	}

	std::mutex mutex_;
	std::condition_variable changed_;
	uint64_t arrivals_ = 0;
	uint64_t grants_ = 0;
	float seconds_ = 0.0f;
	bool open_ = false;
	bool counting_ = false;
	// Made and destroyed on the graphics thread.
	std::optional<AllocationCounter> counter_;
	size_t allocations_ = 0;
};

// A target to draw a source into from the test thread, as a scene would, and read back
// the frame number it shows. Made and destroyed inside the graphics context.
class SourceScreen {
public:
	explicit SourceScreen(obs_source_t *source) : source_(source)
	{
		obs_enter_graphics();
		target_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		stage_ = gs_stagesurface_create(kClipWidth, kClipHeight, GS_RGBA);
		obs_leave_graphics();
		REQUIRE(target_);
		REQUIRE(stage_);
	}

	~SourceScreen()
	{
		obs_enter_graphics();
		gs_stagesurface_destroy(stage_);
		gs_texrender_destroy(target_);
		obs_leave_graphics();
	}

	SourceScreen(const SourceScreen &) = delete;
	SourceScreen &operator=(const SourceScreen &) = delete;

	// Draws the source and, when asked, reads back the frame number it shows. Counts the
	// allocations of the source's render alone into allocations when given.
	std::optional<uint32_t> draw(bool read, size_t *allocations = nullptr)
	{
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
				obs_source_video_render(source_);
				*allocations += counter.count();
			} else {
				obs_source_video_render(source_);
			}
			gs_texrender_end(target_);
		}
		bool mapped = false;
		if (began && read) {
			uint8_t *data = nullptr;
			uint32_t stride = 0;
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
		if (!read) {
			return std::nullopt;
		}
		REQUIRE(mapped);
		return tapeloop::test::readFrameNumber(reds_.data(), kClipWidth, static_cast<int>(kClipWidth),
						       static_cast<int>(kClipHeight));
	}

private:
	obs_source_t *source_;
	gs_texrender_t *target_ = nullptr;
	gs_stagesurf_t *stage_ = nullptr;
	std::vector<uint8_t> reds_ = std::vector<uint8_t>(size_t{kClipWidth} * kClipHeight);
};

// Releases a source and waits for OBS's destroy thread, so that whatever its player
// decodes with outlives it.
struct ReleaseAndWait {
	void operator()(obs_source_t *source) const noexcept
	{
		obs_source_release(source);
		obs_wait_for_destroy_queue();
	}
};

// A replay source made the way Tapeloop makes them, ticked by OBS's graphics thread one
// frame at a time and drawn by the test thread in between.
class SourceRun {
public:
	explicit SourceRun(const std::vector<ExpectedFrame> &expected) : expected_(expected)
	{
		gate_.hold();
		source_.reset(createReplaySource("replay"));
		REQUIRE(source_);
		player_ = replayPlayer(source_.get());
		REQUIRE(player_);
		screen_.emplace(source_.get());
	}

	SourceRun(const SourceRun &) = delete;
	SourceRun &operator=(const SourceRun &) = delete;

	obs_source_t *source() const { return source_.get(); }
	TickGate &gate() { return gate_; }
	// Seconds of the frames let through, in total.
	double played() const { return played_; }

	// The player, to give it controls, which the next frame applies.
	ReplayPlayer &control()
	{
		controlled_ = true;
		return *player_;
	}

	// A frame of OBS, drawn without waiting for any picture.
	PlayerState tick()
	{
		played_ += gate_.frame();
		screen_->draw(false);
		return player_->state();
	}

	// A frame of OBS, and then the frame on screen, once decoded, must be the one the
	// playhead is on, and the playhead must have moved by the time OBS ticked it by.
	// Counts the allocations of the source's renders into allocations when given.
	PlayerState frame(size_t *allocations = nullptr)
	{
		const float seconds = gate_.frame();
		played_ += seconds;
		PlayerState state;
		const auto deadline = std::chrono::steady_clock::now() + 10s;
		while (true) {
			screen_->draw(false, allocations);
			state = player_->state();
			if (state.drawn == state.frame || std::chrono::steady_clock::now() > deadline) {
				break;
			}
			std::this_thread::sleep_for(1ms);
		}
		CAPTURE(state.frame, state.position.count(), state.rate, seconds);
		REQUIRE(state.loaded);
		REQUIRE(state.frames == expected_.size());
		REQUIRE(state.frame < expected_.size());
		REQUIRE(state.drawn == state.frame);
		// The frame on screen is the last one starting at or before the position.
		CHECK(expected_[state.frame].time <= state.position);
		if (state.frame + 1 < expected_.size()) {
			CHECK(state.position < expected_[state.frame + 1].time);
		}
		CHECK(obs_source_get_width(source_.get()) == kClipWidth);
		CHECK(obs_source_get_height(source_.get()) == kClipHeight);
		CHECK(screen_->draw(true, allocations) == expected_[state.frame].number);

		if (!controlled_ && last_ && last_->playing && state.playing && !last_->scrubbing && !state.scrubbing) {
			const auto elapsed = static_cast<double>(std::llround(static_cast<double>(seconds) * 1e9));
			const double wanted = elapsed * state.rate / 1000.0;
			CHECK(std::abs(static_cast<double>((state.position - last_->position).count()) - wanted) <=
			      1.0);
		}
		last_ = state;
		controlled_ = false;
		return state;
	}

private:
	const std::vector<ExpectedFrame> &expected_;
	// Released after the gate opens, and the screen goes before both.
	std::unique_ptr<obs_source_t, ReleaseAndWait> source_;
	TickGate gate_;
	std::optional<SourceScreen> screen_;
	ReplayPlayer *player_ = nullptr;
	std::optional<PlayerState> last_;
	bool controlled_ = false;
	double played_ = 0.0;
};

// What a MeteredDecoder saw, read by the test thread while the decoding thread writes it.
struct DecodeMeter {
	std::atomic<int64_t> busy{0};
	std::atomic<size_t> frames{0};
	std::atomic<size_t> allocations{0};
};

// An FFmpegDecoder that times its calls and counts every allocation of the thread that
// made it, the player's decoding thread, until it is destroyed there.
class MeteredDecoder final : public PictureDecoder {
public:
	explicit MeteredDecoder(DecodeMeter &meter) : meter_(meter) {}

	DecodeStatus open(VideoCodec codec, std::span<const uint8_t> config) noexcept override
	{
		const Timed timed(*this);
		return decoder_.open(codec, config);
	}

	DecodeStatus send(std::span<const uint8_t> data, int64_t pts, int64_t dts) noexcept override
	{
		const Timed timed(*this);
		return decoder_.send(data, pts, dts);
	}

	DecodeStatus receive(DecodedFrame &frame) noexcept override
	{
		const Timed timed(*this);
		const DecodeStatus status = decoder_.receive(frame);
		if (status == DecodeStatus::Ok) {
			++meter_.frames;
		}
		return status;
	}

	DecodeStatus flush() noexcept override
	{
		const Timed timed(*this);
		return decoder_.flush();
	}

	void reset() noexcept override
	{
		const Timed timed(*this);
		decoder_.reset();
	}

	void release(const DecodedFrame &frame) noexcept override
	{
		const Timed timed(*this);
		decoder_.release(frame);
	}

	void close() noexcept override
	{
		const Timed timed(*this);
		decoder_.close();
	}

	std::optional<Picture> picture(const DecodedFrame &frame) const noexcept override
	{
		return decoder_.picture(frame);
	}

private:
	class Timed {
	public:
		explicit Timed(MeteredDecoder &decoder) : decoder_(decoder) {}
		~Timed()
		{
			const auto spent = std::chrono::steady_clock::now() - started_;
			decoder_.meter_.busy += std::chrono::duration_cast<std::chrono::nanoseconds>(spent).count();
			decoder_.meter_.allocations = decoder_.counter_.count();
		}

		Timed(const Timed &) = delete;
		Timed &operator=(const Timed &) = delete;

	private:
		MeteredDecoder &decoder_;
		std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
	};

	DecodeMeter &meter_;
	AllocationCounter counter_;
	FFmpegDecoder decoder_;
};

constexpr size_t kNormalPreset =
	static_cast<size_t>(std::find(kSpeedPresets.begin(), kSpeedPresets.end(), 1000) - kSpeedPresets.begin());

} // namespace

TEST_CASE_METHOD(ObsFixture, "a replay source shows the frame its playhead is on at every preset speed, either way",
		 "[obs][player][source]")
{
	const Clip clip = captureClip(4);
	REQUIRE(clip.gops().size() >= 4);
	const std::vector<ExpectedFrame> expected = expectedFrames(clip);
	const std::vector<size_t> starts = gopStarts(clip, expected);
	// A boundary with a GOP on each side, which every run crosses.
	const size_t boundary = starts[1];
	REQUIRE(boundary >= 8);

	registerReplaySource(makeDecoder);
	// Out of OBS's list of sources to add.
	CHECK((obs_get_source_output_flags(kReplaySourceId) & OBS_SOURCE_CAP_DISABLED) != 0);
	CHECK(replayPlayer(nullptr) == nullptr);
	{
		OBSSourceAutoRelease pattern = tapeloop::test::createTestPattern(kClipWidth, kClipHeight);
		CHECK(replayPlayer(pattern) == nullptr);
	}

	SourceRun run(expected);
	{
		// Private, so out of OBS's own lists.
		OBSSourceAutoRelease named = obs_get_source_by_name("replay");
		CHECK_FALSE(named);
	}
	CHECK(obs_source_get_width(run.source()) == 0);
	REQUIRE(run.control().load(gopsOf(clip)));
	PlayerState state = run.frame();
	CHECK(state.frame == 0);
	CHECK_FALSE(state.playing);

	for (size_t preset = 0; preset < kSpeedPresets.size(); ++preset) {
		for (const int32_t sign : {1, -1}) {
			const int32_t rate = sign * kSpeedPresets[preset];
			CAPTURE(rate);
			// To the preset the way the hotkeys go, from real time.
			ReplayPlayer &player = run.control();
			player.setRate(sign * 4000);
			player.normalSpeed();
			for (size_t at = kNormalPreset; at > preset; --at) {
				player.slower();
			}
			for (size_t at = kNormalPreset; at < preset; ++at) {
				player.faster();
			}
			player.seek(expected[sign > 0 ? boundary - 4 : boundary + 3].time);
			player.play();
			state = run.frame();
			CHECK(state.playing);
			REQUIRE(state.rate == rate);
			size_t previous = state.frame;
			bool crossed = false;
			for (int tick = 0; tick < 60 && state.playing && !crossed; ++tick) {
				state = run.frame();
				CHECK((sign > 0 ? state.frame >= previous : state.frame <= previous));
				previous = state.frame;
				crossed = sign > 0 ? state.frame >= boundary : state.frame < boundary;
			}
			CHECK(crossed);
		}
	}
}

TEST_CASE_METHOD(ObsFixture, "a replay source steps, seeks and scrubs on the frame shown", "[obs][player][source]")
{
	// The last GOP may have just begun when the capture stops.
	const Clip clip = captureClip(5);
	REQUIRE(clip.gops().size() >= 5);
	const std::vector<ExpectedFrame> expected = expectedFrames(clip);
	const std::vector<size_t> starts = gopStarts(clip, expected);
	REQUIRE(starts[1] >= 3);

	registerReplaySource(makeDecoder);
	SourceRun run(expected);
	REQUIRE(run.control().load(gopsOf(clip)));
	PlayerState state = run.frame();

	// Frame by frame across a GOP boundary, forward and then back.
	run.control().seek(expected[starts[1] - 2].time);
	state = run.frame();
	CHECK(state.frame == starts[1] - 2);
	for (size_t i = 0; i < 4; ++i) {
		run.control().step(1);
		state = run.frame();
		CHECK(state.frame == starts[1] - 1 + i);
	}
	for (size_t i = 0; i < 6; ++i) {
		run.control().step(-1);
		state = run.frame();
		CHECK(state.frame == starts[1] + 1 - i);
	}

	// Seeks that jump back and forth over the clip, and to its ends.
	for (size_t i = 1; i <= 24; ++i) {
		const size_t to = (i * 37) % expected.size();
		run.control().seek(expected[to].time);
		state = run.frame();
		CHECK(state.frame == to);
	}
	run.control().seek(expected.back().time);
	CHECK(run.frame().frame == expected.size() - 1);
	run.control().seek(expected.front().time);
	CHECK(run.frame().frame == 0);

	// Held scrubbing either way, then play resumes at the speed it had.
	ReplayPlayer &player = run.control();
	player.setRate(kSpeedPresets[1]);
	player.seek(expected[starts[2] + 2].time);
	player.play();
	state = run.frame();
	// The tick that applies a control first moves the playhead the way it was going.
	run.control().beginScrub(ScrubDirection::Backward);
	state = run.frame();
	size_t previous = state.frame;
	// Held long enough for the scrub to speed up, and short of the front of the clip.
	const double held = run.played();
	for (int tick = 0; tick < 60 && run.played() - held < 0.6; ++tick) {
		state = run.frame();
		CHECK(state.scrubbing);
		CHECK(state.frame <= previous);
		previous = state.frame;
	}
	CHECK(state.frame < starts[2]);
	run.control().beginScrub(ScrubDirection::Forward);
	state = run.frame();
	previous = state.frame;
	for (int tick = 0; tick < 60 && state.frame <= starts[2]; ++tick) {
		state = run.frame();
		CHECK(state.scrubbing);
		CHECK(state.frame >= previous);
		previous = state.frame;
	}
	CHECK(state.frame > starts[2]);
	run.control().endScrub();
	state = run.frame();
	CHECK_FALSE(state.scrubbing);
	CHECK(state.playing);
	CHECK(state.rate == kSpeedPresets[1]);
	for (int tick = 0; tick < 5; ++tick) {
		state = run.frame();
		CHECK(state.frame >= previous);
		previous = state.frame;
	}

	// Destroyed while it plays.
	run.control().faster();
	run.control().faster();
	run.control().faster();
	state = run.frame();
	CHECK(state.playing);
	CHECK(state.rate == kSpeedPresets.back());
}

TEST_CASE_METHOD(ObsFixture, "a playing replay source allocates nothing per frame", "[obs][player][source]")
{
	if (!tapeloop::test::kAllocationHooks) {
		SKIP("allocations cannot be counted in this build");
	}
	const Clip clip = captureClip(4);
	REQUIRE(clip.gops().size() >= 4);
	const std::vector<ExpectedFrame> expected = expectedFrames(clip);
	const std::vector<size_t> starts = gopStarts(clip, expected);

	// Declared before the source, whose decoding thread writes it until it is destroyed.
	DecodeMeter meter;
	registerReplaySource([&meter] { return std::make_unique<MeteredDecoder>(meter); });
	SourceRun run(expected);
	REQUIRE(run.control().load(gopsOf(clip)));
	run.frame();

	size_t renders = 0;
	auto play = [&](int32_t rate) {
		run.control().setRate(rate);
		run.control().play();
		PlayerState state = run.frame(&renders);
		for (size_t tick = 0; tick < expected.size() * 2 && state.playing; ++tick) {
			state = run.frame(&renders);
		}
		CHECK_FALSE(state.playing);
	};
	auto workout = [&] {
		run.control().seek(expected.front().time);
		play(1000);
		play(-1000);
		play(2000);
		run.control().seek(expected[starts[2]].time);
		run.control().beginScrub(ScrubDirection::Backward);
		for (int tick = 0; tick < 20; ++tick) {
			run.frame(&renders);
		}
		run.control().beginScrub(ScrubDirection::Forward);
		for (int tick = 0; tick < 20; ++tick) {
			run.frame(&renders);
		}
		run.control().endScrub();
		run.control().pause();
		for (int step = 0; step < 10; ++step) {
			run.control().step(step % 2 == 0 ? -3 : 1);
			run.frame(&renders);
		}
	};

	// The first pass sizes the planner's and the player's buffers and the textures.
	workout();
	renders = 0;
	run.gate().countAllocations();
	const size_t decodingBefore = meter.allocations;
	const int64_t busyBefore = meter.busy;
	const size_t framesBefore = meter.frames;
	const double playedBefore = run.played();
	workout();
	const double busy = static_cast<double>(meter.busy - busyBefore) / 1e6;
	const size_t frames = meter.frames - framesBefore;
	const double played = run.played() - playedBefore;
	// The decoding thread tells what it allocated at its next decoder call.
	run.control().seek(expected[starts[1]].time);
	run.frame(&renders);

	CHECK(run.gate().allocations() == 0);
	CHECK(renders == 0);
	CHECK(meter.allocations == decodingBefore);
	REQUIRE(frames > 0);
	REQUIRE(played > 0.0);
	// The CPU path at the size of the test clips; FFmpeg's own buffers are not counted.
	std::printf("[source] %zu frames of %ux%u H.264 decoded in %.1f ms, %.3f ms each, over %.1f s played: "
		    "%.2f ms of decoding per second\n",
		    frames, kClipWidth, kClipHeight, busy, busy / static_cast<double>(frames), played, busy / played);
}

TEST_CASE_METHOD(ObsFixture, "a replay source without a decoder moves its playhead and draws nothing",
		 "[obs][player][source]")
{
	const Clip clip = captureClip(2);
	REQUIRE(clip.gops().size() >= 2);
	const std::vector<ExpectedFrame> expected = expectedFrames(clip);

	// What a build without FFmpeg registers.
	registerReplaySource({});
	SourceRun run(expected);
	REQUIRE(run.control().load(gopsOf(clip)));
	run.control().play();
	PlayerState state = run.tick();
	for (int tick = 0; tick < 10; ++tick) {
		state = run.tick();
	}
	CHECK(state.loaded);
	CHECK(state.frames == expected.size());
	CHECK(state.frame > 0);
	CHECK_FALSE(state.drawn);
	CHECK(obs_source_get_width(run.source()) == 0);
}
