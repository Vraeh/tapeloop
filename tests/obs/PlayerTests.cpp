// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "AllocationCounter.hpp"
#include "LogCounter.hpp"
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
#include "obs/GpuPictures.hpp"
#include "obs/PictureRenderer.hpp"
#include "obs/ReplayPlayer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <obs.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
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
using tapeloop::decode::PixelLayout;
using tapeloop::obs::GpuPictures;
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
using tapeloop::test::LogCounter;
using tapeloop::test::waitFor;

namespace {

// One frame of a 60 Hz canvas.
constexpr Nanoseconds kTick{16'666'667};

std::unique_ptr<PictureDecoder> makeDecoder(bool)
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

// Shows the first frame of the clip loaded last. A picture on the GPU of a size the
// GpuPictures is not prepared for takes one render more: that render prepares it, and
// the decoder hands the picture over again.
void showFirst(Screen &screen, ReplayPlayer &player, const std::vector<ExpectedFrame> &expected, bool preparing)
{
	player.tick(0ns);
	Screen::decodeAll(player);
	if (preparing) {
		REQUIRE_FALSE(screen.draw(player).drawn);
		Screen::decodeAll(player);
	}
	const Shown shown = screen.draw(player);
	REQUIRE(shown.drawn);
	CHECK(shown.number == expected.front().number);
	CHECK(player.state().drawn == size_t{0});
}

// Stands in for SharedPictures where there is no Direct3D 11: the decoding side writes
// two NV12 pictures in memory, and latch() uploads the newest one into a pair of
// textures. rebuild() does to it what a device rebuild does to SharedPictures.
class FakeGpuPictures final : public GpuPictures {
public:
	FakeGpuPictures() = default;

	~FakeGpuPictures() override
	{
		obs_enter_graphics();
		destroyTextures();
		obs_leave_graphics();
	}

	FakeGpuPictures(const FakeGpuPictures &) = delete;
	FakeGpuPictures &operator=(const FakeGpuPictures &) = delete;
	FakeGpuPictures(FakeGpuPictures &&) = delete;
	FakeGpuPictures &operator=(FakeGpuPictures &&) = delete;

	bool prepare(uint32_t width, uint32_t height) noexcept override
	{
		++prepares;
		if (refuse) {
			return false;
		}
		if (luma_ && width == width_ && height == height_) {
			return true;
		}
		destroyTextures();
		luma_ = gs_texture_create(width, height, GS_R8, 1, nullptr, GS_DYNAMIC);
		chroma_ = gs_texture_create(width / 2, height / 2, GS_R8G8, 1, nullptr, GS_DYNAMIC);
		if (!luma_ || !chroma_) {
			destroyTextures();
			return false;
		}
		const std::lock_guard lock(mutex_);
		try {
			for (Slot &slot : slots_) {
				slot.luma.resize(size_t{width} * height);
				slot.chroma.resize(size_t{width} * (height / 2));
			}
		} catch (...) {
			return false;
		}
		width_ = width;
		height_ = height;
		++generation_;
		return true;
	}

	bool publish(const Picture &picture, Tag tag) noexcept override
	{
		const std::lock_guard lock(mutex_);
		if (width_ == 0 || picture.width != width_ || picture.height != height_) {
			return false;
		}
		const int target = drawing_ == 0 ? 1 : 0;
		Slot &slot = slots_[static_cast<size_t>(target)];
		for (uint32_t row = 0; row < height_; ++row) {
			std::memcpy(slot.luma.data() + size_t{row} * width_,
				    picture.planes[0] + size_t{row} * picture.strides[0], width_);
		}
		for (uint32_t row = 0; row < height_ / 2; ++row) {
			uint8_t *to = slot.chroma.data() + size_t{row} * width_;
			const uint8_t *u = picture.planes[1] + size_t{row} * picture.strides[1];
			if (picture.layout == PixelLayout::Nv12) {
				std::memcpy(to, u, width_);
				continue;
			}
			const uint8_t *v = picture.planes[2] + size_t{row} * picture.strides[2];
			for (uint32_t x = 0; x < width_ / 2; ++x) {
				to[size_t{x} * 2] = u[x];
				to[size_t{x} * 2 + 1] = v[x];
			}
		}
		slot.colors = picture;
		slot.colors.planes = {};
		slot.colors.strides = {};
		slot.colors.texture = nullptr;
		slot.tag = tag;
		ready_ = target;
		++publishes;
		return true;
	}

	std::optional<Tag> latch() noexcept override
	{
		const std::lock_guard lock(mutex_);
		if (ready_ >= 0) {
			drawing_ = std::exchange(ready_, -1);
			const Slot &slot = slots_[static_cast<size_t>(drawing_)];
			// Rows of interleaved chroma are as long as rows of luma.
			gs_texture_set_image(luma_, slot.luma.data(), width_, false);
			gs_texture_set_image(chroma_, slot.chroma.data(), width_, false);
		}
		if (drawing_ < 0) {
			return std::nullopt;
		}
		return slots_[static_cast<size_t>(drawing_)].tag;
	}

	bool draw(PictureRenderer &renderer, uint32_t width, uint32_t height) noexcept override
	{
		Picture colors;
		{
			const std::lock_guard lock(mutex_);
			if (drawing_ < 0) {
				return false;
			}
			colors = slots_[static_cast<size_t>(drawing_)].colors;
		}
		return renderer.drawNv12(luma_, chroma_, colors, width, height);
	}

	uint64_t generation() const noexcept override
	{
		const std::lock_guard lock(mutex_);
		return generation_;
	}

	// The textures are still there, with nothing in them. SharedPictures counts the
	// release and the rebuild.
	void rebuild() noexcept
	{
		const std::lock_guard lock(mutex_);
		ready_ = -1;
		drawing_ = -1;
		generation_ += 2;
	}

	// The graphics thread's.
	bool refuse = false;
	int prepares = 0;
	// The decoding thread's.
	std::atomic<size_t> publishes{0};

private:
	struct Slot {
		std::vector<uint8_t> luma;
		std::vector<uint8_t> chroma;
		Picture colors;
		Tag tag;
	};

	void destroyTextures() noexcept
	{
		{
			const std::lock_guard lock(mutex_);
			width_ = 0;
			height_ = 0;
			ready_ = -1;
			drawing_ = -1;
			++generation_;
		}
		gs_texture_destroy(luma_);
		gs_texture_destroy(chroma_);
		luma_ = nullptr;
		chroma_ = nullptr;
	}

	gs_texture_t *luma_ = nullptr;
	gs_texture_t *chroma_ = nullptr;
	mutable std::mutex mutex_;
	std::array<Slot, 2> slots_;
	uint32_t width_ = 0;
	uint32_t height_ = 0;
	int ready_ = -1;
	int drawing_ = -1;
	uint64_t generation_ = 0;
};

// What a picture said to be on the GPU points at, for a decoder that has none.
int gpuTexture = 0;

// What the decoders of a test's player share with the test: whether their pictures are
// on the GPU, how much smaller than the clip they say they are, how many were asked for,
// and how many decoders were made for each kind of picture.
struct GpuMeter {
	std::atomic<bool> onGpu{true};
	std::atomic<uint32_t> crop{0};
	std::atomic<size_t> pictures{0};
	std::atomic<size_t> gpuDecoders{0};
	std::atomic<size_t> memoryDecoders{0};
};

// An FFmpegDecoder whose pictures say they are on the GPU while the meter says so, as a
// hardware decoder's are, and keep their planes for FakeGpuPictures to read. One asked
// for pictures in memory gives them in memory.
class GpuDecoder final : public PictureDecoder {
public:
	GpuDecoder(GpuMeter &meter, bool onGpu) : meter_(meter), onGpu_(onGpu) {}

	DecodeStatus open(VideoCodec codec, std::span<const uint8_t> config) noexcept override
	{
		return decoder_.open(codec, config);
	}

	DecodeStatus send(std::span<const uint8_t> data, int64_t pts, int64_t dts) noexcept override
	{
		return decoder_.send(data, pts, dts);
	}

	DecodeStatus receive(DecodedFrame &frame) noexcept override { return decoder_.receive(frame); }
	DecodeStatus flush() noexcept override { return decoder_.flush(); }
	void reset() noexcept override { decoder_.reset(); }
	void release(const DecodedFrame &frame) noexcept override { decoder_.release(frame); }
	void close() noexcept override { decoder_.close(); }

	std::optional<Picture> picture(const DecodedFrame &frame) const noexcept override
	{
		++meter_.pictures;
		std::optional<Picture> picture = decoder_.picture(frame);
		if (picture && onGpu_ && meter_.onGpu) {
			picture->texture = &gpuTexture;
			picture->width -= meter_.crop;
			picture->height -= meter_.crop;
		}
		return picture;
	}

	static ReplayPlayer::DecoderFactory factory(GpuMeter &meter)
	{
		return [&meter](bool onGpu) -> std::unique_ptr<PictureDecoder> {
			++(onGpu ? meter.gpuDecoders : meter.memoryDecoders);
			return std::make_unique<GpuDecoder>(meter, onGpu);
		};
	}

private:
	GpuMeter &meter_;
	bool onGpu_;
	FFmpegDecoder decoder_;
};

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

TEST_CASE_METHOD(ObsFixture, "a replay player says when its clip played forward to the end", "[obs][player]")
{
	const Clip clip = captureClip(2);
	constexpr Nanoseconds kLong = 1min;

	ReplayPlayer player(makeDecoder);
	const uint64_t first = player.load(gopsOf(clip));
	REQUIRE(first != 0);
	player.play();
	player.tick(kTick);
	PlayerState state = player.state();
	CHECK(state.clip == first);
	CHECK(state.playing);
	CHECK_FALSE(state.ended);

	player.tick(kLong);
	state = player.state();
	REQUIRE(state.frames > 2);
	CHECK(state.frame == state.frames - 1);
	CHECK_FALSE(state.playing);
	CHECK(state.ended);

	// A change of speed leaves it there; a step away, or back onto the last frame, is
	// someone at the controls.
	player.faster();
	player.tick(kTick);
	CHECK(player.state().ended);
	player.step(-1);
	player.tick(kTick);
	CHECK_FALSE(player.state().ended);
	player.step(1);
	player.tick(kTick);
	state = player.state();
	CHECK(state.frame == state.frames - 1);
	CHECK_FALSE(state.ended);

	// Played again it starts over, and ends again.
	player.play();
	player.tick(kTick);
	state = player.state();
	CHECK(state.playing);
	CHECK(state.frame < state.frames - 1);
	CHECK_FALSE(state.ended);
	player.tick(kLong);
	CHECK(player.state().ended);

	// Backwards to the first frame, or scrubbing to the last, is not an end. Controls
	// apply after the playhead moves, so a tick of no time applies them first.
	player.reverse();
	player.play();
	player.tick(0ns);
	player.tick(kLong);
	state = player.state();
	CHECK(state.frame == 0);
	CHECK_FALSE(state.playing);
	CHECK_FALSE(state.ended);
	player.beginScrub(ScrubDirection::Forward);
	player.tick(0ns);
	player.tick(kLong);
	player.endScrub();
	player.tick(kTick);
	state = player.state();
	CHECK(state.frame == state.frames - 1);
	CHECK_FALSE(state.ended);

	// Scrubbing while it plays and letting go plays on to the end.
	player.setRate(1000);
	player.play();
	player.beginScrub(ScrubDirection::Backward);
	player.tick(0ns);
	player.tick(kTick);
	player.endScrub();
	player.tick(0ns);
	CHECK_FALSE(player.state().ended);
	player.tick(kLong);
	CHECK(player.state().ended);

	// The next clip has its own number, which the state gives once a tick takes the clip
	// in, and has not ended; neither has a load of nothing.
	const uint64_t second = player.load(gopsOf(clip));
	CHECK(second > first);
	CHECK(player.state().clip == first);
	player.tick(kTick);
	state = player.state();
	CHECK(state.clip == second);
	CHECK_FALSE(state.ended);
	const uint64_t nothing = player.load(nullptr);
	CHECK(nothing > second);
	player.play();
	player.tick(kLong);
	state = player.state();
	CHECK(state.clip == nothing);
	CHECK_FALSE(state.ended);
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
		ReplayPlayer player([&](bool onGpu) -> std::unique_ptr<PictureDecoder> {
			if (++calls == 1) {
				if (throws) {
					throw std::runtime_error("no decoder");
				}
				return nullptr;
			}
			return makeDecoder(onGpu);
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
	for (const bool onGpu : {false, true}) {
		CAPTURE(onGpu);
		FakeGpuPictures gpu;
		GpuMeter meter;
		meter.onGpu = onGpu;
		ReplayPlayer player(GpuDecoder::factory(meter), {}, &gpu);
		REQUIRE(player.load(gopsOf(clip)));
		showFirst(screen, player, expected, onGpu);

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
		CHECK(gpu.prepares == (onGpu ? 1 : 0));
		// A baseline; the planner's cache serves some of these frames.
		const double milliseconds = std::chrono::duration<double, std::milli>(decoding).count();
		std::printf("[player] %zu frames of 320x180 H.264 decoded and %s in %.1f ms, %.3f ms each\n", decodes,
			    onGpu ? "published" : "copied", milliseconds, milliseconds / static_cast<double>(decodes));
	}
}

TEST_CASE_METHOD(ObsFixture, "a replay player decodes on its own thread once started", "[obs][player]")
{
	const Clip clip = captureClip(4);
	REQUIRE(clip.gops().size() >= 4);
	const std::vector<ExpectedFrame> expected = expectedFrames(clip);
	const std::vector<size_t> starts = gopStarts(clip, expected);

	Screen screen;
	for (const auto &[onGpu, refused] : {std::pair{false, false}, std::pair{true, false}, std::pair{true, true}}) {
		CAPTURE(onGpu, refused);
		FakeGpuPictures gpu;
		gpu.refuse = refused;
		GpuMeter meter;
		ReplayPlayer player(onGpu ? GpuDecoder::factory(meter) : ReplayPlayer::DecoderFactory(makeDecoder), {},
				    onGpu ? &gpu : nullptr);
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

		// After a device rebuild the frame comes back with nothing else asking for it.
		player.pause();
		state = settle(kTick);
		gpu.rebuild();
		CHECK(settle(0ns).frame == state.frame);
		player.play();

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
		if (onGpu) {
			CHECK(meter.gpuDecoders == 1);
			CHECK(meter.memoryDecoders == (refused ? 1 : 0));
			CHECK(gpu.prepares == 1);
		}
		// The playhead still answers, with nothing decoded for it.
		const size_t at = player.state().frame;
		REQUIRE(at > 0);
		player.step(-1);
		player.tick(kTick);
		CHECK(player.state().frame == at - 1);
	}
}

TEST_CASE_METHOD(ObsFixture, "a replay player shows pictures decoded on the GPU where its playhead is", "[obs][player]")
{
	const Clip clip = captureClip(4);
	REQUIRE(clip.gops().size() >= 4);
	const std::vector<ExpectedFrame> expected = expectedFrames(clip);
	const std::vector<size_t> starts = gopStarts(clip, expected);

	Screen screen;
	FakeGpuPictures gpu;
	GpuMeter meter;
	ReplayPlayer player(GpuDecoder::factory(meter), {}, &gpu);
	REQUIRE(player.load(gopsOf(clip)));
	showFirst(screen, player, expected, true);

	const size_t last = starts[1] + 2;
	for (size_t frame = 1; frame <= last; ++frame) {
		player.step(1);
		CHECK(checkTick(screen, player, expected).frame == frame);
	}
	for (size_t frame = last; frame-- > 0;) {
		player.step(-1);
		CHECK(checkTick(screen, player, expected).frame == frame);
	}

	size_t stride = 37;
	while (expected.size() % stride == 0) {
		++stride;
	}
	for (size_t i = 0; i < expected.size(); ++i) {
		const size_t frame = i * stride % expected.size();
		player.seek(expected[frame].time);
		CHECK(checkTick(screen, player, expected).frame == frame);
	}

	for (const int32_t rate : {2000, -2000}) {
		CAPTURE(rate);
		player.setRate(rate);
		player.seek(expected[rate > 0 ? starts[1] - 4 : starts[1] + 3].time);
		player.play();
		PlayerState state = checkTick(screen, player, expected);
		bool crossed = false;
		for (int tick = 0; tick < 60 && state.playing; ++tick) {
			state = checkTick(screen, player, expected);
			crossed = crossed || (rate > 0 ? state.frame >= starts[1] : state.frame < starts[1]);
		}
		CHECK(crossed);
	}
	CHECK(gpu.prepares == 1);
	CHECK(gpu.publishes >= expected.size());

	// A picture of another size takes one render more, for the GpuPictures to be
	// prepared for it; the one of the size before is gone with it.
	player.pause();
	player.seek(expected[2].time);
	checkTick(screen, player, expected);
	meter.crop = 2;
	player.step(1);
	player.tick(kTick);
	Screen::decodeAll(player);
	CHECK_FALSE(screen.draw(player, false).drawn);
	CHECK_FALSE(player.state().drawn);
	Screen::decodeAll(player);
	CHECK(screen.draw(player, false).drawn);
	const PlayerState state = player.state();
	CHECK(state.drawn == size_t{3});
	CHECK(state.width == kClipWidth - 2);
	CHECK(state.height == kClipHeight - 2);
	CHECK(gpu.prepares == 2);
}

TEST_CASE_METHOD(ObsFixture, "a replay player switches between pictures in memory and on the GPU", "[obs][player]")
{
	const Clip clip = captureClip(3);
	REQUIRE(clip.gops().size() >= 3);
	const std::vector<ExpectedFrame> expected = expectedFrames(clip);
	const std::vector<size_t> starts = gopStarts(clip, expected);

	Screen screen;
	FakeGpuPictures gpu;
	GpuMeter meter;
	ReplayPlayer player(GpuDecoder::factory(meter), {}, &gpu);
	REQUIRE(player.load(gopsOf(clip)));
	showFirst(screen, player, expected, true);

	// Every third picture in memory, the others on the GPU, either way.
	const size_t last = starts[0] + 4;
	REQUIRE(last < starts[1]);
	for (size_t frame = 1; frame <= last; ++frame) {
		meter.onGpu = frame % 3 != 0;
		player.step(1);
		CHECK(checkTick(screen, player, expected).frame == frame);
	}
	for (size_t frame = last; frame-- > 0;) {
		meter.onGpu = frame % 3 != 1;
		player.step(-1);
		CHECK(checkTick(screen, player, expected).frame == frame);
	}
	CHECK(gpu.prepares == 1);

	// Without GpuPictures the decoder is asked for pictures in memory. One on the GPU is
	// not shown, and the playhead goes on.
	std::optional<bool> asked;
	ReplayPlayer blind([&](bool onGpu) -> std::unique_ptr<PictureDecoder> {
		asked = onGpu;
		return std::make_unique<GpuDecoder>(meter, true);
	});
	meter.onGpu = true;
	REQUIRE(blind.load(gopsOf(clip)));
	for (size_t frame = 1; frame <= 3; ++frame) {
		blind.step(1);
		CHECK_FALSE(screen.frame(blind).drawn);
		CHECK_FALSE(blind.state().drawn);
		CHECK(blind.state().frame == frame);
	}
	CHECK(asked == false);
	meter.onGpu = false;
	blind.step(1);
	CHECK(checkTick(screen, blind, expected).frame == 4);
}

TEST_CASE_METHOD(ObsFixture, "a replay player shows no picture on the GPU of the clip before", "[obs][player]")
{
	const Clip clip = captureClip(4);
	REQUIRE(clip.gops().size() >= 4);
	const std::vector<ExpectedFrame> expected = expectedFrames(clip);
	const Clip later({clip.gops().begin() + 2, clip.gops().end()}, Nanoseconds::min(), Nanoseconds::max());
	const std::vector<ExpectedFrame> laterExpected = expectedFrames(later);
	REQUIRE(laterExpected.front().number != expected.front().number);

	Screen screen;
	FakeGpuPictures gpu;
	GpuMeter meter;
	ReplayPlayer player(GpuDecoder::factory(meter), {}, &gpu);
	REQUIRE(player.load(gopsOf(clip)));
	showFirst(screen, player, expected, true);
	player.step(3);
	checkTick(screen, player, expected);

	// A picture of the clip before is published and waiting when the next clip loads.
	player.step(1);
	player.tick(kTick);
	Screen::decodeAll(player);
	REQUIRE(player.load(gopsOf(later)));
	player.tick(kTick);
	CHECK_FALSE(player.state().drawn);
	CHECK_FALSE(screen.draw(player).drawn);
	CHECK_FALSE(player.state().drawn);

	// The textures are of the right size already, so the new clip shows at once.
	Screen::decodeAll(player);
	const Shown shown = screen.draw(player);
	CHECK(shown.drawn);
	CHECK(shown.number == laterExpected.front().number);
	CHECK(player.state().drawn == size_t{0});
	player.step(2);
	CHECK(checkTick(screen, player, laterExpected).frame == 2);
}

TEST_CASE_METHOD(ObsFixture, "a replay player hands its picture over again after a device rebuild", "[obs][player]")
{
	const Clip clip = captureClip(2);
	const std::vector<ExpectedFrame> expected = expectedFrames(clip);

	Screen screen;
	for (const bool onGpu : {true, false}) {
		CAPTURE(onGpu);
		FakeGpuPictures gpu;
		GpuMeter meter;
		meter.onGpu = onGpu;
		ReplayPlayer player(GpuDecoder::factory(meter), {}, &gpu);
		REQUIRE(player.load(gopsOf(clip)));
		showFirst(screen, player, expected, onGpu);
		player.step(3);
		const PlayerState held = checkTick(screen, player, expected);
		const size_t pictures = meter.pictures;

		// A picture on the GPU is gone at once. One in memory is still drawn, though in
		// OBS its texture may have lost what it held.
		gpu.rebuild();
		CHECK(screen.draw(player).drawn == !onGpu);
		CHECK(player.state().drawn.has_value() == !onGpu);

		// One request brings the frame back.
		REQUIRE(player.decodeNext());
		CHECK_FALSE(player.decodeNext());
		CHECK(meter.pictures == pictures + 1);
		Shown shown = screen.draw(player);
		CHECK(shown.drawn);
		CHECK(shown.number == expected[held.frame].number);
		CHECK(player.state().drawn == held.frame);

		// Nothing more is asked while nothing changes.
		for (int tick = 0; tick < 3; ++tick) {
			player.tick(kTick);
			CHECK_FALSE(player.decodeNext());
			shown = screen.draw(player);
			CHECK(shown.number == expected[held.frame].number);
		}
		CHECK(meter.pictures == pictures + 1);
	}
}

TEST_CASE_METHOD(ObsFixture, "a replay player whose GPU pictures cannot be prepared decodes in memory instead",
		 "[obs][player]")
{
	const Clip clip = captureClip(3);
	REQUIRE(clip.gops().size() >= 3);
	const std::vector<ExpectedFrame> expected = expectedFrames(clip);
	const std::vector<size_t> starts = gopStarts(clip, expected);

	Screen screen;
	FakeGpuPictures gpu;
	gpu.refuse = true;
	GpuMeter meter;
	const LogCounter said("so it decodes them in memory from now on");
	ReplayPlayer player(GpuDecoder::factory(meter), {}, &gpu);
	REQUIRE(player.load(gopsOf(clip)));
	player.step(2);

	// The render that finds the picture cannot be shown on the GPU draws nothing, and
	// the frame the playhead is on comes back in memory.
	player.tick(kTick);
	Screen::decodeAll(player);
	CHECK_FALSE(screen.draw(player).drawn);
	CHECK(gpu.prepares == 1);
	Screen::decodeAll(player);
	const Shown shown = screen.draw(player);
	CHECK(shown.drawn);
	CHECK(shown.number == expected[2].number);
	CHECK(player.state().drawn == size_t{2});
	CHECK(meter.gpuDecoders == 1);
	CHECK(meter.memoryDecoders == 1);

	// The GpuPictures is not prepared again, even once it could be.
	gpu.refuse = false;
	const size_t last = starts[0] + 2;
	for (size_t frame = 3; frame <= last; ++frame) {
		player.step(1);
		CHECK(checkTick(screen, player, expected).frame == frame);
	}
	for (size_t frame = last; frame-- > starts[0] - 3;) {
		player.step(-1);
		CHECK(checkTick(screen, player, expected).frame == frame);
	}
	for (const size_t frame : {expected.size() - 1, starts[0] - 1}) {
		player.seek(expected[frame].time);
		CHECK(checkTick(screen, player, expected).frame == frame);
	}

	// The next clip is decoded in memory from its first frame.
	REQUIRE(player.load(gopsOf(clip)));
	checkTick(screen, player, expected, 0ns);
	player.step(1);
	CHECK(checkTick(screen, player, expected).frame == 1);

	CHECK(gpu.prepares == 1);
	CHECK(gpu.publishes == 0);
	CHECK(meter.gpuDecoders == 1);
	CHECK(meter.memoryDecoders == 1);
	CHECK(said.lines == 1);
}
