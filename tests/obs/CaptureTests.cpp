// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "ClipDecoder.hpp"
#include "ObsFixture.hpp"
#include "TestEncoders.hpp"
#include "TestPattern.hpp"

#include "obs/SourceCapture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <obs.hpp>
#include <util/platform.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using tapeloop::Clip;
using tapeloop::EncoderInfo;
using tapeloop::FrameSize;
using tapeloop::Nanoseconds;
using tapeloop::Vendor;
using tapeloop::obs::CaptureSettings;
using tapeloop::obs::CaptureState;
using tapeloop::obs::SourceCapture;
using tapeloop::obs::StartResult;
using tapeloop::test::createTestPattern;
using tapeloop::test::ObsFixture;
using tapeloop::test::waitFor;

namespace {

// The canvas of ObsFixture runs at 30 fps.
constexpr Nanoseconds kFrameInterval{33'333'333};

EncoderInfo testEncoder(const char *id)
{
	return {id, "h264", Vendor::Software};
}

Clip everything(const SourceCapture &capture)
{
	return capture.buffer()->clip(Nanoseconds::min(), Nanoseconds::max());
}

bool hasGops(const SourceCapture &capture, size_t count)
{
	return capture.buffer() && capture.buffer()->stats().gopCount >= count;
}

// The time of each frame number in the clip, as the decoder reads it back.
std::map<uint32_t, Nanoseconds> frameTimes(const Clip &clip)
{
	std::map<uint32_t, Nanoseconds> times;
	const auto decoded = tapeloop::test::decodeGops(clip);
	for (size_t gop = 0; gop < decoded.size(); ++gop) {
		const auto packets = clip.gops()[gop]->packets();
		for (size_t i = 0; i < decoded[gop].size() && i < packets.size(); ++i) {
			if (decoded[gop][i].frameNumber) {
				times.emplace(*decoded[gop][i].frameNumber, packets[i].time);
			}
		}
	}
	return times;
}

} // namespace

TEST_CASE_METHOD(ObsFixture, "a capture encodes its source into GOPs that decode alone", "[obs][capture]")
{
	OBSSourceAutoRelease pattern = createTestPattern(640, 360);
	SourceCapture capture;
	REQUIRE(capture.start(pattern, {}) == StartResult::Started);
	CHECK(capture.start(pattern, {}) == StartResult::AlreadyRunning);
	REQUIRE(waitFor([&] { return hasGops(capture, 4); }, 60s));

	const tapeloop::obs::CaptureStats running = capture.stats();
	CHECK(running.state == CaptureState::Running);
	CHECK(running.encoderId == "obs_x264");
	CHECK(running.outputSize == FrameSize{640, 360});
	CHECK(running.buffer.bytes > 0);
	CHECK(running.buffer.configBytes > 0);
	CHECK(running.bufferedDuration > Nanoseconds{0});

	capture.stop();
	CHECK_FALSE(capture.active());
	const tapeloop::obs::CaptureStats stopped = capture.stats();
	CHECK(stopped.state == CaptureState::Stopped);
	CHECK(stopped.buffer.gopCount >= 4);
	CHECK(stopped.buffer.droppedBeforeKeyframe == 0);
	CHECK(stopped.buffer.discontinuities == 0);

	const Clip clip = everything(capture);
	const auto decoded = tapeloop::test::decodeGops(clip);
	REQUIRE(decoded.size() == clip.gops().size());
	for (size_t gop = 0; gop < decoded.size(); ++gop) {
		CAPTURE(gop);
		REQUIRE(decoded[gop].size() == clip.gops()[gop]->packets().size());
		for (const tapeloop::test::DecodedFrame &frame : decoded[gop]) {
			CHECK(frame.width == 640);
			CHECK(frame.height == 360);
			CHECK(frame.frameNumber);
		}
	}
}

TEST_CASE_METHOD(ObsFixture, "capture times increase on the shared clock", "[obs][capture]")
{
	OBSSourceAutoRelease pattern = createTestPattern(640, 360);
	SourceCapture capture;
	REQUIRE(capture.start(pattern, {}) == StartResult::Started);
	REQUIRE(waitFor([&] { return hasGops(capture, 3); }, 60s));
	const Nanoseconds now{static_cast<int64_t>(os_gettime_ns())};
	capture.stop();

	std::vector<Nanoseconds> times;
	const Clip clip = everything(capture);
	for (const auto &gop : clip.gops()) {
		for (const tapeloop::PacketRecord &packet : gop->packets()) {
			times.push_back(packet.time);
		}
	}
	REQUIRE(times.size() > 2);
	CHECK(times.back() <= now + 1s);
	CHECK(times.back() >= now - 5s);
	for (size_t i = 1; i < times.size(); ++i) {
		CAPTURE(i);
		const Nanoseconds step = times[i] - times[i - 1];
		REQUIRE(step > Nanoseconds{0});
		// Frames the renderer skipped leave whole intervals out; packet times carry
		// microseconds.
		const int64_t frames = (step + kFrameInterval / 2) / kFrameInterval;
		CHECK(std::llabs((step - frames * kFrameInterval).count()) <= 2'000);
	}
}

TEST_CASE_METHOD(ObsFixture, "two captures of one source line up on the same frames", "[obs][capture]")
{
	OBSSourceAutoRelease pattern = createTestPattern(640, 360);
	SourceCapture first;
	SourceCapture second;
	REQUIRE(first.start(pattern, {}) == StartResult::Started);
	std::this_thread::sleep_for(500ms);
	REQUIRE(second.start(pattern, {}) == StartResult::Started);
	REQUIRE(waitFor([&] { return hasGops(second, 3); }, 60s));
	first.stop();
	second.stop();

	const auto firstTimes = frameTimes(everything(first));
	const auto secondTimes = frameTimes(everything(second));
	size_t common = 0;
	for (const auto &[frame, time] : secondTimes) {
		const auto found = firstTimes.find(frame);
		if (found == firstTimes.end()) {
			continue;
		}
		++common;
		CAPTURE(frame);
		CHECK(std::llabs((found->second - time).count()) <= kFrameInterval.count());
	}
	CHECK(common >= 10);
	CHECK(firstTimes.begin()->first < secondTimes.begin()->first);
}

TEST_CASE_METHOD(ObsFixture, "a hundred start and stop cycles leave nothing behind", "[obs][capture]")
{
	OBSSourceAutoRelease pattern = createTestPattern(320, 180);
	SourceCapture capture;
	for (int cycle = 0; cycle < 100; ++cycle) {
		CAPTURE(cycle);
		REQUIRE(capture.start(pattern, {}) == StartResult::Started);
		REQUIRE(waitFor([&] { return capture.buffer()->stats().bytes > 0; }, 30s));
		capture.stop();
		REQUIRE_FALSE(capture.active());
	}
}

TEST_CASE_METHOD(ObsFixture, "a capture stops after its source was removed", "[obs][capture]")
{
	OBSSourceAutoRelease pattern = createTestPattern(640, 360);
	SourceCapture capture;
	REQUIRE(capture.start(pattern, {}) == StartResult::Started);
	REQUIRE(waitFor([&] { return hasGops(capture, 1); }, 60s));
	obs_source_remove(pattern);
	std::this_thread::sleep_for(200ms);
	capture.stop();
	CHECK(capture.stats().state == CaptureState::Stopped);
}

TEST_CASE_METHOD(ObsFixture, "a capture still running at shutdown stops from the module unload", "[obs][capture]")
{
	obs_module_t *module = nullptr;
	REQUIRE(obs_open_module(&module, TAPELOOP_TEST_UNLOAD_MODULE, "") == MODULE_SUCCESS);
	REQUIRE(obs_init_module(module));
	using SetHook = void (*)(void (*)(void *), void *);
	const auto setHook =
		reinterpret_cast<SetHook>(os_dlsym(obs_get_module_lib(module), "tapeloop_test_set_unload_hook"));
	REQUIRE(setHook);

	OBSSourceAutoRelease pattern = createTestPattern(640, 360);
	auto capture = std::make_unique<SourceCapture>();
	REQUIRE(capture->start(pattern, {}) == StartResult::Started);
	REQUIRE(waitFor([&] { return hasGops(*capture, 1); }, 60s));

	// obs_shutdown, when the fixture ends, unloads the module after the graphics thread
	// has stopped, as it does with the plugin.
	setHook([](void *param) { delete static_cast<SourceCapture *>(param); }, capture.release());
}

TEST_CASE_METHOD(ObsFixture, "OBS refuses a video reset while a capture runs", "[obs][capture]")
{
	OBSSourceAutoRelease pattern = createTestPattern(640, 360);
	SourceCapture capture;
	REQUIRE(capture.start(pattern, {}) == StartResult::Started);
	REQUIRE(waitFor([&] { return hasGops(capture, 1); }, 60s));
	CHECK(ObsFixture::resetCanvas({1280, 720, 30}) == OBS_VIDEO_CURRENTLY_ACTIVE);

	capture.stop();
	CHECK(ObsFixture::resetCanvas({1280, 720, 30}) == OBS_VIDEO_SUCCESS);
	REQUIRE(capture.start(pattern, {}) == StartResult::Started);
	REQUIRE(waitFor([&] { return hasGops(capture, 1); }, 60s));
	CHECK(capture.stats().outputSize == FrameSize{640, 360});
	capture.stop();
}

TEST_CASE_METHOD(ObsFixture, "an odd source size is encoded at an even size", "[obs][capture]")
{
	OBSSourceAutoRelease pattern = createTestPattern(641, 359);
	SourceCapture capture;
	REQUIRE(capture.start(pattern, {}) == StartResult::Started);
	CHECK(capture.stats().outputSize == FrameSize{638, 358});
	REQUIRE(waitFor([&] { return hasGops(capture, 2); }, 60s));
	capture.stop();

	for (const auto &frames : tapeloop::test::decodeGops(everything(capture))) {
		for (const tapeloop::test::DecodedFrame &frame : frames) {
			CHECK(frame.width == 638);
			CHECK(frame.height == 358);
		}
	}
}

TEST_CASE_METHOD(ObsFixture, "a capture falls through to the next encoder candidate", "[obs][capture]")
{
	OBSSourceAutoRelease pattern = createTestPattern(640, 360);
	SourceCapture capture;

	CaptureSettings onlyFailing;
	onlyFailing.candidates = {testEncoder(tapeloop::test::kFailingEncoderId)};
	CHECK(capture.start(pattern, onlyFailing) == StartResult::NoEncoder);
	CHECK_FALSE(capture.active());

	CaptureSettings settings;
	settings.candidates = {testEncoder(tapeloop::test::kFailingEncoderId), testEncoder("obs_x264")};
	REQUIRE(capture.start(pattern, settings) == StartResult::Started);
	CHECK(capture.stats().encoderId == "obs_x264");
	REQUIRE(waitFor([&] { return hasGops(capture, 1); }, 60s));
	capture.stop();
}

TEST_CASE_METHOD(ObsFixture, "a capture reports an encoder that fails while running", "[obs][capture]")
{
	OBSSourceAutoRelease pattern = createTestPattern(640, 360);
	SourceCapture capture;
	CaptureSettings settings;
	settings.candidates = {testEncoder(tapeloop::test::kBrokenEncoderId)};
	REQUIRE(capture.start(pattern, settings) == StartResult::Started);
	CHECK(waitFor([&] { return capture.stats().state == CaptureState::Failed; }, 30s));
	CHECK(capture.active());
	capture.stop();
	CHECK(capture.stats().state == CaptureState::Stopped);
}

TEST_CASE_METHOD(ObsFixture, "a capture notices when its source changes size", "[obs][capture]")
{
	OBSSourceAutoRelease pattern = createTestPattern(640, 360);
	SourceCapture capture;
	REQUIRE(capture.start(pattern, {}) == StartResult::Started);
	CHECK(capture.sourceSizeMatches());

	OBSDataAutoRelease larger = obs_data_create();
	obs_data_set_int(larger, "width", 800);
	obs_data_set_int(larger, "height", 360);
	// Video sources apply settings on the next tick.
	obs_source_update(pattern, larger);
	CHECK(waitFor([&] { return !capture.sourceSizeMatches(); }, 5s));
	capture.stop();
	CHECK(capture.sourceSizeMatches());
}

TEST_CASE_METHOD(ObsFixture, "a source without a size is shown and waited for", "[obs][capture]")
{
	OBSSourceAutoRelease pattern = createTestPattern(0, 0);
	SourceCapture capture;
	CHECK(capture.start(pattern, {}) == StartResult::NoSourceSize);
	CHECK_FALSE(capture.active());
	CHECK(capture.waiting());
	CHECK(capture.stats().state == CaptureState::Waiting);
	CHECK(obs_source_showing(pattern));
	CHECK(capture.buffer() == nullptr);

	// A second try while waiting holds the source once, not twice.
	CHECK(capture.start(pattern, {}) == StartResult::NoSourceSize);
	capture.stop();
	CHECK_FALSE(obs_source_showing(pattern));
	CHECK(capture.stats().state == CaptureState::Stopped);
}

TEST_CASE_METHOD(ObsFixture, "a capture waiting for a size starts on the view it already has", "[obs][capture]")
{
	OBSSourceAutoRelease pattern = createTestPattern(0, 0);
	SourceCapture capture;
	REQUIRE(capture.start(pattern, {}) == StartResult::NoSourceSize);

	OBSDataAutoRelease sized = obs_data_create();
	obs_data_set_int(sized, "width", 640);
	obs_data_set_int(sized, "height", 360);
	obs_source_update(pattern, sized);
	REQUIRE(waitFor([&] { return obs_source_get_width(pattern) == 640; }, 5s));
	REQUIRE(capture.start(pattern, {}) == StartResult::Started);
	CHECK_FALSE(capture.waiting());
	CHECK(waitFor([&] { return hasGops(capture, 1); }, 60s));
	capture.stop();
	CHECK_FALSE(obs_source_showing(pattern));
}

TEST_CASE_METHOD(ObsFixture, "a waiting capture lets go of its source when it cannot start", "[obs][capture]")
{
	OBSSourceAutoRelease pattern = createTestPattern(0, 0);
	SourceCapture capture;
	REQUIRE(capture.start(pattern, {}) == StartResult::NoSourceSize);

	OBSDataAutoRelease huge = obs_data_create();
	obs_data_set_int(huge, "width", 20000);
	obs_data_set_int(huge, "height", 360);
	obs_source_update(pattern, huge);
	REQUIRE(waitFor([&] { return obs_source_get_width(pattern) == 20000; }, 5s));
	CHECK(capture.start(pattern, {}) == StartResult::SourceTooLarge);
	CHECK_FALSE(capture.waiting());
	CHECK_FALSE(obs_source_showing(pattern));
}

TEST_CASE_METHOD(ObsFixture, "a restart keeps the buffer only when asked", "[obs][capture]")
{
	OBSSourceAutoRelease pattern = createTestPattern(640, 360);
	SourceCapture capture;
	REQUIRE(capture.start(pattern, {}) == StartResult::Started);
	REQUIRE(waitFor([&] { return hasGops(capture, 2); }, 60s));
	capture.stop();
	const tapeloop::SourceBufferStats before = capture.stats().buffer;
	const tapeloop::SourceBuffer *buffer = capture.buffer();

	// A new size changes the output size and the byte budget; the buffer stays.
	OBSDataAutoRelease smaller = obs_data_create();
	obs_data_set_int(smaller, "width", 320);
	obs_data_set_int(smaller, "height", 180);
	obs_source_update(pattern, smaller);
	REQUIRE(waitFor([&] { return obs_source_get_width(pattern) == 320; }, 5s));

	REQUIRE(capture.start(pattern, {}, true) == StartResult::Started);
	CHECK(capture.buffer() == buffer);
	CHECK(capture.stats().outputSize == FrameSize{320, 180});
	REQUIRE(waitFor([&] { return capture.stats().buffer.newestTime > before.newestTime + 1s; }, 60s));
	capture.stop();
	const tapeloop::SourceBufferStats kept = capture.stats().buffer;
	CHECK(kept.oldestTime == before.oldestTime);
	CHECK(kept.discontinuities == 1);

	REQUIRE(capture.start(pattern, {}) == StartResult::Started);
	REQUIRE(waitFor([&] { return hasGops(capture, 1); }, 60s));
	capture.stop();
	CHECK(capture.stats().buffer.oldestTime > kept.newestTime);
	CHECK(capture.stats().buffer.discontinuities == 0);
}

TEST_CASE_METHOD(ObsFixture, "a source larger than a view allows is refused and OBS keeps working", "[obs][capture]")
{
	OBSSourceAutoRelease huge = createTestPattern(20000, 360);
	SourceCapture refused;
	CHECK(refused.start(huge, {}) == StartResult::SourceTooLarge);
	CHECK_FALSE(refused.active());

	OBSSourceAutoRelease pattern = createTestPattern(320, 180);
	SourceCapture capture;
	REQUIRE(capture.start(pattern, {}) == StartResult::Started);
	CHECK(waitFor([&] { return hasGops(capture, 1); }, 30s));
	capture.stop();
}

TEST_CASE_METHOD(ObsFixture, "a start that fails leaves the buffer as it was", "[obs][capture]")
{
	OBSSourceAutoRelease pattern = createTestPattern(640, 360);
	SourceCapture capture;
	REQUIRE(capture.start(pattern, {}) == StartResult::Started);
	REQUIRE(waitFor([&] { return hasGops(capture, 2); }, 60s));
	capture.stop();
	const tapeloop::SourceBufferStats before = capture.stats().buffer;

	CaptureSettings failing;
	failing.candidates = {testEncoder(tapeloop::test::kFailingEncoderId)};
	CHECK(capture.start(pattern, failing) == StartResult::NoEncoder);
	CHECK(capture.stats().buffer.gopCount == before.gopCount);
	CHECK(capture.stats().buffer.newestTime == before.newestTime);

	CaptureSettings longer;
	longer.bufferLength = 120s;
	longer.candidates = failing.candidates;
	const tapeloop::SourceBuffer *buffer = capture.buffer();
	CHECK(capture.start(pattern, longer) == StartResult::NoEncoder);
	CHECK(capture.buffer() == buffer);
	CHECK(capture.stats().buffer.gopCount == before.gopCount);
}

TEST_CASE_METHOD(ObsFixture, "a new buffer length needs a new buffer even when keeping it", "[obs][capture]")
{
	OBSSourceAutoRelease pattern = createTestPattern(640, 360);
	SourceCapture capture;
	REQUIRE(capture.start(pattern, {}) == StartResult::Started);
	REQUIRE(waitFor([&] { return hasGops(capture, 1); }, 60s));
	capture.stop();
	const tapeloop::SourceBuffer *buffer = capture.buffer();

	CaptureSettings longer;
	longer.bufferLength = 120s;
	REQUIRE(capture.start(pattern, longer, true) == StartResult::Started);
	CHECK(capture.buffer() != buffer);
	CHECK(capture.stats().buffer.gopCount <= 1);
	capture.stop();
}

TEST_CASE_METHOD(ObsFixture, "a capture outlives the reference to a removed source", "[obs][capture]")
{
	OBSSourceAutoRelease pattern = createTestPattern(640, 360);
	OBSWeakSourceAutoRelease weak = obs_source_get_weak_source(pattern);
	SourceCapture capture;
	REQUIRE(capture.start(pattern, {}) == StartResult::Started);
	pattern = nullptr;

	// The view lets go of a removed source when it next renders.
	{
		OBSSourceAutoRelease strong = obs_weak_source_get_source(weak);
		obs_source_remove(strong);
	}
	REQUIRE(waitFor([&] { return obs_weak_source_expired(weak); }, 5s));
	CHECK(capture.sourceSizeMatches());
	capture.stop();
}

TEST_CASE_METHOD(ObsFixture, "a waiting capture lets go of its source when it is too small to encode", "[obs][capture]")
{
	OBSSourceAutoRelease pattern = createTestPattern(0, 0);
	SourceCapture capture;
	REQUIRE(capture.start(pattern, {}) == StartResult::NoSourceSize);

	OBSDataAutoRelease tiny = obs_data_create();
	obs_data_set_int(tiny, "width", 1);
	obs_data_set_int(tiny, "height", 1);
	obs_source_update(pattern, tiny);
	REQUIRE(waitFor([&] { return obs_source_get_width(pattern) == 1; }, 5s));
	CHECK(capture.start(pattern, {}) == StartResult::NoOutputSize);
	CHECK_FALSE(capture.waiting());
	CHECK_FALSE(obs_source_showing(pattern));
}
