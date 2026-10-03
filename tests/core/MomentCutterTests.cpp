// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/MomentCutter.hpp"

#include "core/MomentList.hpp"
#include "core/SourceBuffer.hpp"
#include "SyntheticEncoder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using tapeloop::Clip;
using tapeloop::cutMoment;
using tapeloop::Gop;
using tapeloop::Moment;
using tapeloop::MomentCut;
using tapeloop::MomentList;
using tapeloop::MomentSource;
using tapeloop::Nanoseconds;
using tapeloop::SourceBuffer;
using tapeloop::test::SyntheticEncoder;

namespace {

// A camera whose clock starts at origin, so frames of different cameras do not line
// up with each other.
struct Camera {
	explicit Camera(Nanoseconds origin, Nanoseconds window = 1h)
		: encoder(configFor(origin)),
		  buffer(configFor(encoder, window))
	{
	}

	static SyntheticEncoder::Config configFor(Nanoseconds origin)
	{
		SyntheticEncoder::Config config;
		config.origin = origin;
		return config;
	}

	static tapeloop::SourceBufferConfig configFor(const SyntheticEncoder &encoder, Nanoseconds window)
	{
		tapeloop::SourceBufferConfig config;
		config.window = window;
		config.maxBytes = std::numeric_limits<size_t>::max();
		config.frameDuration = encoder.frameDuration();
		return config;
	}

	void pushUntil(Nanoseconds time)
	{
		while (encoder.timeOf(encoder.nextFrame()) <= time)
			buffer.push(encoder.next());
	}

	SyntheticEncoder encoder;
	SourceBuffer buffer;
};

const Clip *clipOf(const Moment &moment, const std::string &key)
{
	for (const auto &entry : moment.clips) {
		if (entry.sourceKey == key)
			return &entry.clip;
	}
	return nullptr;
}

// The first frame at or after t and the last at or before it.
Nanoseconds firstFrameFrom(const Camera &camera, Nanoseconds t)
{
	int64_t frame = 0;
	while (camera.encoder.timeOf(frame) < t)
		++frame;
	return camera.encoder.timeOf(frame);
}

Nanoseconds lastFrameUntil(const Camera &camera, Nanoseconds t)
{
	int64_t frame = 0;
	while (camera.encoder.timeOf(frame + 1) <= t)
		++frame;
	return camera.encoder.timeOf(frame);
}

} // namespace

TEST_CASE("cutMoment cuts the same range from every source")
{
	Camera wide(0ms);
	Camera close(1s + 5ms);
	Camera late(7s + 11ms);
	Camera idle(0ms);
	for (Camera *camera : {&wide, &close, &late})
		camera->pushUntil(10s);

	const std::vector<MomentSource> sources = {{"wide", wide.buffer},
						   {"close", close.buffer},
						   {"late", late.buffer},
						   {"idle", idle.buffer}};
	const MomentCut cut = cutMoment(sources, 9s, 3s);

	CHECK(cut.moment.id == 0);
	CHECK(cut.moment.start == 6s);
	CHECK(cut.moment.end == 9s);
	CHECK(cut.skipped == std::vector<std::string>{"idle"});
	REQUIRE(cut.moment.clips.size() == 3);

	SECTION("sources that hold the whole range")
	{
		for (const Camera *camera : {&wide, &close}) {
			const Clip *clip = clipOf(cut.moment, camera == &wide ? "wide" : "close");
			REQUIRE(clip != nullptr);
			CHECK(clip->in() == firstFrameFrom(*camera, 6s));
			CHECK(clip->out() == lastFrameUntil(*camera, 9s));
		}
	}

	SECTION("a source that started inside the range")
	{
		const Clip *clip = clipOf(cut.moment, "late");
		REQUIRE(clip != nullptr);
		CHECK(clip->in() == late.encoder.timeOf(0));
		CHECK(clip->out() == lastFrameUntil(late, 9s));
	}
}

TEST_CASE("cutMoment skips a source whose history does not reach the range")
{
	Camera current(0ms, 2s);
	Camera stale(0ms);
	current.pushUntil(20s);
	stale.pushUntil(3s);

	const std::vector<MomentSource> sources = {{"current", current.buffer}, {"stale", stale.buffer}};

	const MomentCut recent = cutMoment(sources, 20s, 1s);
	CHECK(recent.skipped == std::vector<std::string>{"stale"});
	CHECK(recent.moment.clips.size() == 1);

	// The current camera evicted these seconds long ago.
	const MomentCut old = cutMoment(sources, 2s, 1s);
	CHECK(old.skipped == std::vector<std::string>{"current"});
	CHECK(old.moment.clips.size() == 1);
}

TEST_CASE("cutMoment keeps the range defined at the edges of the clock")
{
	Camera camera(0ms);
	camera.pushUntil(1s);
	const std::vector<MomentSource> sources = {{"camera", camera.buffer}};

	const MomentCut everything = cutMoment(sources, 1s, Nanoseconds::max());
	CHECK(everything.moment.start == 1s - Nanoseconds::max());
	CHECK(everything.moment.clips.size() == 1);

	const MomentCut single = cutMoment(sources, 1s, -5s);
	CHECK(single.moment.start == 1s);
	CHECK(single.moment.clips.size() == 1);
	CHECK(single.moment.clips[0].clip.frameTimes().size() == 1);

	const MomentCut earliest = cutMoment(sources, Nanoseconds::min() + 1s, 2s);
	CHECK(earliest.moment.start == Nanoseconds::min());
	CHECK(earliest.skipped == std::vector<std::string>{"camera"});
}

TEST_CASE("cutMoment starts each clip at the keyframe before the range")
{
	Camera wide(0ms);
	Camera close(5ms);
	wide.pushUntil(10s);
	close.pushUntil(10s);
	const std::vector<MomentSource> sources = {{"wide", wide.buffer}, {"close", close.buffer}};

	// 6.2 s is 12 frames into the GOP that starts at 6.0 s.
	const MomentCut cut = cutMoment(sources, 8s, 1800ms);
	for (const Camera *camera : {&wide, &close}) {
		const Clip *clip = clipOf(cut.moment, camera == &wide ? "wide" : "close");
		REQUIRE(clip != nullptr);
		CHECK(clip->in() == firstFrameFrom(*camera, 6200ms));
		CHECK(clip->gops().front()->startTime() < clip->in());
		CHECK(clip->gops().front()->packets().front().keyframe);
	}
}

TEST_CASE("cutMoment keeps what a source holds when it stopped before the anchor")
{
	Camera running(0ms);
	Camera stopped(0ms);
	running.pushUntil(10s);
	stopped.pushUntil(8s);
	const std::vector<MomentSource> sources = {{"running", running.buffer}, {"stopped", stopped.buffer}};

	const MomentCut cut = cutMoment(sources, 10s, 3s);
	CHECK(cut.skipped.empty());
	const Clip *clip = clipOf(cut.moment, "stopped");
	REQUIRE(clip != nullptr);
	CHECK(clip->in() == stopped.encoder.timeOf(420));
	CHECK(clip->out() == stopped.encoder.timeOf(480));
}

TEST_CASE("cutMoment with every source skipped gives a moment the list refuses")
{
	Camera idle(0ms);
	Camera stale(0ms);
	stale.pushUntil(1s);
	const std::vector<MomentSource> sources = {{"idle", idle.buffer}, {"stale", stale.buffer}};

	MomentCut cut = cutMoment(sources, 10s, 1s);
	CHECK(cut.moment.clips.empty());
	CHECK(cut.skipped == std::vector<std::string>{"idle", "stale"});

	tapeloop::MomentListConfig config;
	config.maxMoments = 10;
	config.maxBytes = std::numeric_limits<size_t>::max();
	MomentList list(config);
	CHECK(list.add(std::move(cut.moment)) == 0);
	CHECK(list.size() == 0);
}

TEST_CASE("Moments cut from the same buffers share their GOPs")
{
	Camera wide(0ms);
	Camera close(3ms);
	wide.pushUntil(10s);
	close.pushUntil(10s);
	const std::vector<MomentSource> sources = {{"wide", wide.buffer}, {"close", close.buffer}};

	MomentCut first = cutMoment(sources, 6s, 3s);
	MomentCut second = cutMoment(sources, 8s, 3s);

	std::set<const Gop *> distinct;
	size_t distinctBytes = 0;
	size_t clipBytes = 0;
	for (const MomentCut *cut : {&first, &second}) {
		for (const auto &entry : cut->moment.clips) {
			clipBytes += entry.clip.byteSize();
			for (const auto &gop : entry.clip.gops()) {
				if (distinct.insert(gop.get()).second)
					distinctBytes += gop->byteSize();
			}
		}
	}

	tapeloop::MomentListConfig config;
	config.maxMoments = 10;
	config.maxBytes = std::numeric_limits<size_t>::max();
	MomentList list(config);
	list.add(std::move(first.moment));
	list.add(std::move(second.moment));

	CHECK(list.byteSize() == distinctBytes);
	CHECK(distinctBytes < clipBytes);
}

TEST_CASE("cutMoment can run while the encoders push")
{
	std::vector<std::unique_ptr<Camera>> cameras;
	for (int i = 0; i < 3; ++i)
		cameras.push_back(std::make_unique<Camera>(Nanoseconds{i * 7'000'000}, 2s));

	std::vector<MomentSource> sources;
	for (size_t i = 0; i < cameras.size(); ++i)
		sources.push_back({"camera " + std::to_string(i), cameras[i]->buffer});

	std::atomic<int> running{static_cast<int>(cameras.size())};
	std::vector<std::thread> producers;
	for (auto &camera : cameras) {
		producers.emplace_back([&running, &camera] {
			for (int frame = 0; frame < 6000; ++frame)
				camera->buffer.push(camera->encoder.next());
			--running;
		});
	}

	tapeloop::MomentListConfig config;
	config.maxMoments = 20;
	config.maxBytes = std::numeric_limits<size_t>::max();
	MomentList list(config);
	int failures = 0;
	int cuts = 0;
	// At least one cut happens while the producers run or right after, however the
	// threads get scheduled.
	do {
		const Nanoseconds now = cameras[0]->buffer.stats().newestTime;
		MomentCut cut = cutMoment(sources, now, 1s);
		if (cut.moment.clips.size() + cut.skipped.size() != sources.size())
			++failures;
		for (const auto &entry : cut.moment.clips) {
			const std::vector<Nanoseconds> times = entry.clip.frameTimes();
			if (entry.clip.in() < cut.moment.start || entry.clip.out() > cut.moment.end ||
			    times.front() != entry.clip.in() || times.back() != entry.clip.out())
				++failures;
			for (const auto &gop : entry.clip.gops()) {
				if (!tapeloop::test::hasExpectedBytes(*gop))
					++failures;
			}
		}
		list.add(std::move(cut.moment));
		++cuts;
	} while (running > 0);
	for (auto &producer : producers)
		producer.join();

	CHECK(failures == 0);
	CHECK(cuts > 0);

	// The buffers have long evicted most of what the stored moments hold.
	for (const auto &camera : cameras)
		CHECK(camera->buffer.stats().oldestTime > camera->encoder.timeOf(5000));
	for (const Moment &moment : list.moments()) {
		for (const auto &entry : moment.clips) {
			for (const auto &gop : entry.clip.gops())
				CHECK(tapeloop::test::hasExpectedBytes(*gop));
		}
	}
}
