// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "BufferOutput.hpp"
#include "ClipDecoder.hpp"
#include "ObsFixture.hpp"
#include "TestPattern.hpp"

#include "core/EncoderPolicy.hpp"
#include "core/SourceBuffer.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <catch2/catch_test_macros.hpp>
#include <obs.hpp>

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;
using tapeloop::Clip;
using tapeloop::Nanoseconds;
using tapeloop::SourceBuffer;
using tapeloop::test::ObsFixture;
using tapeloop::test::waitFor;

namespace {

// A source rendered on its own view and encoded with obs-x264 into a buffer, torn down
// in the order libobs supports: output stopped and inactive, output and encoder
// released, view removed, then destroyed.
class EncodedView {
public:
	EncodedView(obs_source_t *source, SourceBuffer &buffer) : view_(obs_view_create())
	{
		obs_view_set_source(view_, 0, source);

		obs_video_info video = {};
		obs_get_video_info(&video);
		video.base_width = obs_source_get_width(source);
		video.base_height = obs_source_get_height(source);
		video.output_width = video.base_width;
		video.output_height = video.base_height;
		video.output_format = VIDEO_FORMAT_NV12;
		video.gpu_conversion = true;
		video_t *output = obs_view_add2(view_, &video);
		if (!output) {
			obs_view_destroy(view_);
			throw std::runtime_error("obs_view_add2 failed");
		}

		tapeloop::ReplayEncoderParams params;
		params.width = video.output_width;
		params.height = video.output_height;
		params.frameDuration = {static_cast<int32_t>(video.fps_den), static_cast<int32_t>(video.fps_num)};
		const tapeloop::EncoderInfo x264{"obs_x264", "h264", tapeloop::Vendor::Software};
		OBSDataAutoRelease settings = tapeloop::test::toObsData(tapeloop::buildReplaySettings(x264, params));

		encoder_ = obs_video_encoder_create("obs_x264", "tapeloop-test", settings, nullptr);
		obs_encoder_set_video(encoder_, output);
		output_ = tapeloop::test::createBufferOutput("tapeloop-test", buffer);
		obs_output_set_video_encoder(output_, encoder_);
	}

	~EncodedView()
	{
		stop();
		output_ = nullptr;
		encoder_ = nullptr;
		obs_view_remove(view_);
		obs_view_destroy(view_);
	}

	EncodedView(const EncodedView &) = delete;
	EncodedView &operator=(const EncodedView &) = delete;

	bool start() { return obs_output_start(output_); }

	// True once the encoder has stopped.
	bool stop()
	{
		obs_output_stop(output_);
		return waitFor([this] { return !obs_output_active(output_); }, 10s);
	}

private:
	obs_view_t *view_;
	OBSEncoderAutoRelease encoder_;
	OBSOutputAutoRelease output_;
};

} // namespace

TEST_CASE_METHOD(ObsFixture, "libobs starts and shuts down headless", "[obs]")
{
	obs_video_info video = {};
	REQUIRE(obs_get_video_info(&video));
	CHECK(video.base_width == 640);
	CHECK(video.fps_num == 30);
	CHECK(obs_get_encoder_codec("obs_x264") != nullptr);
}

#ifdef _WIN32
TEST_CASE_METHOD(ObsFixture, "libobs sets up WinRT on its video thread as OBS does", "[obs]")
{
	// The video thread loads it as it starts; without it libobs runs in a state OBS
	// itself never runs in.
	CHECK(waitFor([] { return GetModuleHandleW(L"libobs-winrt.dll") != nullptr; }, 10s));
}
#endif

TEST_CASE_METHOD(ObsFixture, "x264 encodes a view of a source into a buffer of decodable GOPs", "[obs]")
{
	OBSSourceAutoRelease pattern = tapeloop::test::createTestPattern(640, 360);
	REQUIRE(pattern);

	tapeloop::SourceBufferConfig config;
	config.window = 1h;
	config.maxBytes = size_t{256} << 20;
	config.frameDuration = Nanoseconds{33'333'333};
	SourceBuffer buffer(config);
	{
		EncodedView encoded(pattern, buffer);
		REQUIRE(encoded.start());
		REQUIRE(waitFor([&] { return buffer.stats().gopCount >= 5; }, 60s));
		REQUIRE(encoded.stop());
	}

	const tapeloop::SourceBufferStats stats = buffer.stats();
	CHECK(stats.droppedBeforeKeyframe == 0);
	CHECK(stats.discontinuities == 0);
	CHECK(stats.configBytes > 0);

	const Clip clip = buffer.clip(Nanoseconds::min(), Nanoseconds::max());
	REQUIRE(clip.gops().size() >= 5);

	const auto decoded = tapeloop::test::decodeGops(clip);
	REQUIRE(decoded.size() == clip.gops().size());
	uint32_t previous = 0;
	bool first = true;
	for (size_t gop = 0; gop < decoded.size(); ++gop) {
		CAPTURE(gop);
		const auto packets = clip.gops()[gop]->packets();
		REQUIRE(decoded[gop].size() == packets.size());
		for (size_t i = 0; i < packets.size(); ++i) {
			CAPTURE(i);
			const tapeloop::test::DecodedFrame &frame = decoded[gop][i];
			CHECK(frame.pts == packets[i].pts);
			CHECK(frame.width == 640);
			CHECK(frame.height == 360);
			REQUIRE(frame.frameNumber);
			// A frame the renderer could not produce in time is sent again, so a
			// number can repeat but never go back.
			CHECK((first || *frame.frameNumber >= previous));
			previous = *frame.frameNumber;
			first = false;
		}
	}
	CHECK(decoded.back().back().frameNumber > decoded.front().front().frameNumber);

	// Without its configuration the first GOP cannot be decoded: the helper takes the
	// parameter sets x264 repeats in the stream out.
	tapeloop::GopBuilder builder(config.frameDuration);
	const tapeloop::Gop &firstGop = *clip.gops().front();
	for (size_t i = 0; i < firstGop.packets().size(); ++i) {
		const tapeloop::PacketRecord &record = firstGop.packets()[i];
		builder.append({firstGop.packetData(i), record.pts, record.dts, record.time, record.keyframe});
	}
	const Clip unconfigured({builder.snapshot()}, Nanoseconds::min(), Nanoseconds::max());
	REQUIRE(unconfigured.gops().front()->codecConfig() == nullptr);
	CHECK_THROWS_AS(tapeloop::test::decodeGops(unconfigured), std::runtime_error);
}
