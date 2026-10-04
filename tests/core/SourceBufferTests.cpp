// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/SourceBuffer.hpp"

#include "AllocationCounter.hpp"
#include "SyntheticEncoder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using tapeloop::Clip;
using tapeloop::CodecConfig;
using tapeloop::EncodedPacket;
using tapeloop::Gop;
using tapeloop::Nanoseconds;
using tapeloop::SourceBuffer;
using tapeloop::SourceBufferConfig;
using tapeloop::SourceBufferStats;
using tapeloop::test::AllocationCounter;
using tapeloop::test::SyntheticEncoder;

namespace {

constexpr size_t kUnlimited = std::numeric_limits<size_t>::max();
// Default SyntheticEncoder GOP: one 4096-byte keyframe and 29 frames of 1024 bytes.
constexpr size_t kGopBytes = 4096 + 29 * 1024;

SourceBufferConfig makeConfig(const SyntheticEncoder &encoder, Nanoseconds window, size_t maxBytes)
{
	SourceBufferConfig config;
	config.window = window;
	config.maxBytes = maxBytes;
	config.frameDuration = encoder.frameDuration();
	return config;
}

void pushFrames(SourceBuffer &buffer, SyntheticEncoder &encoder, int count)
{
	for (int i = 0; i < count; ++i)
		buffer.push(encoder.next());
}

std::vector<std::shared_ptr<const Gop>> heldGops(const SourceBuffer &buffer)
{
	const Clip all = buffer.clip(Nanoseconds::min(), Nanoseconds::max());
	return {all.gops().begin(), all.gops().end()};
}

EncodedPacket packetAt(Nanoseconds time, int64_t dts, bool keyframe)
{
	return {{}, dts, dts, time, keyframe};
}

bool strictlyIncreasing(const std::vector<Nanoseconds> &times)
{
	for (size_t i = 1; i < times.size(); ++i) {
		if (times[i] <= times[i - 1])
			return false;
	}
	return true;
}

// Every GOP held starts with its keyframe and has no frame missing, every byte is
// what the encoder wrote, and the byte count matches what is held.
bool consistent(const SourceBuffer &buffer)
{
	size_t bytes = 0;
	for (const auto &gop : heldGops(buffer)) {
		const auto packets = gop->packets();
		if (!packets.front().keyframe || !tapeloop::test::hasExpectedBytes(*gop))
			return false;
		for (size_t i = 1; i < packets.size(); ++i) {
			if (packets[i].keyframe || packets[i].pts != packets[i - 1].pts + 1)
				return false;
		}
		bytes += gop->byteSize();
	}
	return bytes == buffer.stats().bytes;
}

// A codec configuration that names the first frame of its run, so a reader can tell
// which run a GOP belongs to.
CodecConfig runConfig(int64_t firstFrame)
{
	CodecConfig config(8);
	for (size_t i = 0; i < config.size(); ++i)
		config[i] = static_cast<uint8_t>(static_cast<uint64_t>(firstFrame) >> (8 * i));
	return config;
}

int64_t runOf(const CodecConfig &config)
{
	uint64_t frame = 0;
	for (size_t i = 0; i < config.size(); ++i)
		frame |= uint64_t{config[i]} << (8 * i);
	return static_cast<int64_t>(frame);
}

std::vector<const CodecConfig *> configsOf(const std::vector<std::shared_ptr<const Gop>> &gops)
{
	std::vector<const CodecConfig *> configs;
	for (const auto &gop : gops)
		configs.push_back(gop->codecConfig());
	return configs;
}

} // namespace

TEST_CASE("SourceBuffer starts empty")
{
	SyntheticEncoder encoder({});
	const SourceBuffer buffer(makeConfig(encoder, 10s, kUnlimited));

	const SourceBufferStats stats = buffer.stats();
	CHECK(stats.bytes == 0);
	CHECK(stats.gopCount == 0);
	CHECK(stats.oldestTime == Nanoseconds{0});
	CHECK(stats.newestTime == Nanoseconds{0});
	CHECK(buffer.clip(10s).empty());
	CHECK(buffer.clip(Nanoseconds::min(), Nanoseconds::max()).empty());
}

TEST_CASE("SourceBuffer drops packets until the first keyframe")
{
	SyntheticEncoder::Config encoderConfig;
	encoderConfig.firstFrame = 27;
	SyntheticEncoder encoder(encoderConfig);
	SourceBuffer buffer(makeConfig(encoder, 10s, kUnlimited));

	pushFrames(buffer, encoder, 3);
	CHECK(buffer.stats().droppedBeforeKeyframe == 3);
	CHECK(buffer.stats().gopCount == 0);

	pushFrames(buffer, encoder, 2);
	const SourceBufferStats stats = buffer.stats();
	CHECK(stats.droppedBeforeKeyframe == 3);
	CHECK(stats.gopCount == 1);
	CHECK(stats.bytes == 4096 + 1024);
	CHECK(stats.oldestTime == encoder.timeOf(30));
	CHECK(stats.newestTime == encoder.timeOf(31));
}

TEST_CASE("SourceBuffer evicts old GOPs but always keeps the window")
{
	SyntheticEncoder encoder({});
	const Nanoseconds window = 2s;
	SourceBuffer buffer(makeConfig(encoder, window, kUnlimited));

	bool evicted = false;
	for (int frame = 0; frame < 1200; ++frame) {
		buffer.push(encoder.next());
		if (!encoder.isKeyframe(encoder.nextFrame()))
			continue;

		// The last GOP returned is the copy of the one still being encoded.
		std::vector<std::shared_ptr<const Gop>> sealed = heldGops(buffer);
		sealed.pop_back();
		if (sealed.empty())
			continue;

		CAPTURE(frame, sealed.size());
		if (sealed.size() > 1)
			CHECK(sealed.back()->endTime() - sealed[1]->startTime() < window);
		if (sealed.front()->startTime() > encoder.timeOf(0)) {
			evicted = true;
			CHECK(sealed.back()->endTime() - sealed.front()->startTime() >= window);
		}
	}
	CHECK(evicted);
}

TEST_CASE("SourceBuffer evicts old GOPs to stay within its byte budget")
{
	SyntheticEncoder encoder({});

	SECTION("budget for three and a half GOPs")
	{
		SourceBuffer buffer(makeConfig(encoder, 1h, kGopBytes * 7 / 2));
		pushFrames(buffer, encoder, 301);

		const SourceBufferStats stats = buffer.stats();
		CHECK(stats.gopCount == 4);
		CHECK(stats.bytes == 3 * kGopBytes + 4096);
		CHECK(stats.oldestTime == encoder.timeOf(210));
		CHECK(stats.newestTime == encoder.timeOf(300));
	}

	SECTION("budget smaller than one GOP keeps the newest sealed one")
	{
		SourceBuffer buffer(makeConfig(encoder, 1h, 1));
		pushFrames(buffer, encoder, 301);

		const SourceBufferStats stats = buffer.stats();
		CHECK(stats.gopCount == 2);
		CHECK(stats.oldestTime == encoder.timeOf(270));
	}
}

TEST_CASE("SourceBuffer takes a new byte budget at the next keyframe")
{
	SyntheticEncoder encoder({});
	SourceBuffer buffer(makeConfig(encoder, 1h, kUnlimited));
	pushFrames(buffer, encoder, 150);
	REQUIRE(buffer.stats().gopCount == 5);

	buffer.setByteBudget(2 * kGopBytes);
	CHECK(buffer.stats().gopCount == 5);
	pushFrames(buffer, encoder, 1);
	CHECK(buffer.stats().gopCount == 3);
	CHECK(buffer.stats().bytes == 2 * kGopBytes + 4096);

	// Nothing more leaves once the budget is lifted: two GOPs kept, two sealed since, and
	// the open one.
	buffer.setByteBudget(kUnlimited);
	pushFrames(buffer, encoder, 89);
	CHECK(buffer.stats().gopCount == 5);
	CHECK(consistent(buffer));
}

TEST_CASE("SourceBuffer treats time going backwards as a discontinuity")
{
	SyntheticEncoder encoder({});
	SourceBuffer buffer(makeConfig(encoder, 1h, kUnlimited));
	pushFrames(buffer, encoder, 65);

	buffer.push(packetAt(encoder.timeOf(64), 65, false));
	SourceBufferStats stats = buffer.stats();
	CHECK(stats.discontinuities == 1);
	CHECK(stats.droppedBeforeKeyframe == 1);
	CHECK(stats.gopCount == 3);
	CHECK(stats.newestTime == encoder.timeOf(64));

	buffer.push(packetAt(encoder.timeOf(70), 70, false));
	CHECK(buffer.stats().droppedBeforeKeyframe == 2);

	buffer.push(packetAt(encoder.timeOf(80), 80, true));
	buffer.push(packetAt(encoder.timeOf(81), 81, false));
	stats = buffer.stats();
	CHECK(stats.discontinuities == 1);
	CHECK(stats.gopCount == 4);
	CHECK(stats.newestTime == encoder.timeOf(81));
	CHECK(strictlyIncreasing(buffer.clip(1h).frameTimes()));
}

TEST_CASE("SourceBuffer treats dts going backwards as a discontinuity")
{
	SyntheticEncoder encoder({});
	SourceBuffer buffer(makeConfig(encoder, 1h, kUnlimited));
	pushFrames(buffer, encoder, 40);

	// An encoder restart: the clock keeps going, dts starts over with a keyframe.
	buffer.push(packetAt(encoder.timeOf(40), 0, true));
	buffer.push(packetAt(encoder.timeOf(41), 1, false));

	const SourceBufferStats stats = buffer.stats();
	CHECK(stats.discontinuities == 1);
	CHECK(stats.droppedBeforeKeyframe == 0);
	CHECK(stats.gopCount == 3);
	CHECK(buffer.clip(1h).frameTimes().size() == 42);
}

TEST_CASE("SourceBuffer makes way for a run that restarts earlier")
{
	SyntheticEncoder encoder({});
	SourceBuffer buffer(makeConfig(encoder, 1h, kUnlimited));
	pushFrames(buffer, encoder, 90);

	buffer.push(packetAt(encoder.timeOf(45), 0, true));
	buffer.push(packetAt(encoder.timeOf(46), 1, false));

	const SourceBufferStats stats = buffer.stats();
	CHECK(stats.discontinuities == 1);
	CHECK(stats.gopCount == 2);
	CHECK(stats.bytes == kGopBytes);

	const std::vector<Nanoseconds> times = buffer.clip(1h).frameTimes();
	CHECK(times.size() == 32);
	CHECK(times[29] == encoder.timeOf(29));
	CHECK(times[30] == encoder.timeOf(45));
	CHECK(strictlyIncreasing(times));
}

TEST_CASE("SourceBuffer measures the window after making way for a restart")
{
	SyntheticEncoder encoder({});
	SourceBuffer buffer(makeConfig(encoder, 10s, kUnlimited));
	pushFrames(buffer, encoder, 630);

	// Only five seconds remain after the restart, so nothing old has to go.
	buffer.push(packetAt(encoder.timeOf(300), 0, true));
	const SourceBufferStats stats = buffer.stats();
	CHECK(stats.oldestTime == encoder.timeOf(0));
	CHECK(stats.newestTime == encoder.timeOf(300));
	CHECK(stats.gopCount == 11);
}

TEST_CASE("SourceBuffer accepts a repeated dts")
{
	SyntheticEncoder encoder({});
	SourceBuffer buffer(makeConfig(encoder, 1h, kUnlimited));
	buffer.push(packetAt(encoder.timeOf(0), 0, true));
	buffer.push(packetAt(encoder.timeOf(1), 0, false));
	buffer.push(packetAt(encoder.timeOf(2), 1, false));

	CHECK(buffer.stats().discontinuities == 0);
	CHECK(buffer.clip(1h).frameTimes().size() == 3);
}

TEST_CASE("SourceBuffer stays consistent when an allocation fails while pushing")
{
	if (!tapeloop::test::kAllocationFailures)
		SKIP("allocation failures cannot be injected in this configuration");

	// Fail each allocation a push makes in turn: on the keyframe that seals a GOP, and
	// on a frame that makes the first GOP's buffers grow.
	struct Scenario {
		int framesBefore;
		size_t frameSize;
	};
	const Scenario scenarios[] = {{30, 1024}, {1, 100'000}};

	for (const Scenario scenario : scenarios) {
		for (size_t skip = 0; skip < 16; ++skip) {
			CAPTURE(scenario.framesBefore, skip);
			SyntheticEncoder::Config encoderConfig;
			encoderConfig.frameSize = scenario.frameSize;
			SyntheticEncoder encoder(encoderConfig);
			SourceBuffer buffer(makeConfig(encoder, 1h, kUnlimited));
			pushFrames(buffer, encoder, scenario.framesBefore);

			const EncodedPacket packet = encoder.next();
			bool failed = false;
			{
				tapeloop::test::AllocationFailure failure(skip);
				try {
					buffer.push(packet);
				} catch (const std::bad_alloc &) {
					failed = true;
				}
			}
			pushFrames(buffer, encoder, 70);

			CHECK(consistent(buffer));
			if (failed)
				CHECK(buffer.stats().droppedBeforeKeyframe > 0);
		}
	}
}

TEST_CASE("SourceBuffer clamps clips to what it holds")
{
	SyntheticEncoder encoder({});
	SourceBuffer buffer(makeConfig(encoder, 1h, kUnlimited));
	pushFrames(buffer, encoder, 100);

	const Clip everything = buffer.clip(1h);
	CHECK(everything.in() == encoder.timeOf(0));
	CHECK(everything.out() == encoder.timeOf(99));
	CHECK(everything.frameTimes().size() == 100);

	const Clip absolute = buffer.clip(encoder.timeOf(-500), encoder.timeOf(500));
	CHECK(absolute.in() == encoder.timeOf(0));
	CHECK(absolute.out() == encoder.timeOf(99));

	const Clip newest = buffer.clip(Nanoseconds{0});
	CHECK(newest.in() == encoder.timeOf(99));
	CHECK(newest.out() == encoder.timeOf(99));
	CHECK(buffer.clip(Nanoseconds{-5}).frameTimes().size() == 1);
	CHECK(buffer.clip(Nanoseconds::max()).frameTimes().size() == 100);

	CHECK(buffer.clip(encoder.timeOf(200), encoder.timeOf(300)).empty());
	CHECK(buffer.clip(encoder.timeOf(50), encoder.timeOf(40)).empty());
}

TEST_CASE("SourceBuffer clips start at the GOP that holds their in point")
{
	SyntheticEncoder encoder({});
	SourceBuffer buffer(makeConfig(encoder, 1h, kUnlimited));
	pushFrames(buffer, encoder, 100);

	const Clip clip = buffer.clip(encoder.timeOf(45), encoder.timeOf(80));
	CHECK(clip.in() == encoder.timeOf(45));
	CHECK(clip.out() == encoder.timeOf(80));
	REQUIRE(clip.gops().size() == 2);
	CHECK(clip.gops()[0]->startTime() == encoder.timeOf(30));
	CHECK(clip.gops()[1]->startTime() == encoder.timeOf(60));
}

TEST_CASE("SourceBuffer clips reach into the GOP being encoded")
{
	SyntheticEncoder encoder({});
	SourceBuffer buffer(makeConfig(encoder, 1h, kUnlimited));
	pushFrames(buffer, encoder, 75);

	const Clip clip = buffer.clip(encoder.timeOf(10) - encoder.timeOf(0));
	CHECK(clip.out() == encoder.timeOf(74));
	CHECK(clip.in() == encoder.timeOf(64));
	REQUIRE(clip.gops().size() == 1);
	CHECK(clip.gops()[0]->packets().size() == 15);

	pushFrames(buffer, encoder, 40);
	CHECK(clip.gops()[0]->packets().size() == 15);
	CHECK(clip.out() == encoder.timeOf(74));
	CHECK(tapeloop::test::hasExpectedBytes(*clip.gops()[0]));

	const Clip sealedOnly = buffer.clip(encoder.timeOf(40), encoder.timeOf(50));
	REQUIRE(sealedOnly.gops().size() == 1);
	CHECK(sealedOnly.gops()[0]->packets().size() == 30);
}

TEST_CASE("SourceBuffer clips stay readable after clear")
{
	SyntheticEncoder encoder({});
	SourceBuffer buffer(makeConfig(encoder, 1s, kUnlimited));
	pushFrames(buffer, encoder, 3);
	buffer.push(packetAt(encoder.timeOf(1), 0, false));
	pushFrames(buffer, encoder, 117);

	const Clip clip = buffer.clip(1s);
	REQUIRE_FALSE(clip.empty());
	const std::vector<Nanoseconds> times = clip.frameTimes();

	buffer.clear();
	const SourceBufferStats cleared = buffer.stats();
	CHECK(cleared.bytes == 0);
	CHECK(cleared.gopCount == 0);
	CHECK(cleared.discontinuities == 0);
	CHECK(cleared.droppedBeforeKeyframe == 0);
	CHECK(buffer.clip(1h).empty());

	CHECK(clip.frameTimes() == times);
	for (const auto &gop : clip.gops())
		CHECK(tapeloop::test::hasExpectedBytes(*gop));
}

TEST_CASE("SourceBuffer clips stay readable after their GOPs are evicted")
{
	SyntheticEncoder encoder({});
	SourceBuffer buffer(makeConfig(encoder, 1s, kUnlimited));
	pushFrames(buffer, encoder, 120);

	const Clip clip = buffer.clip(1s);
	REQUIRE_FALSE(clip.empty());
	const std::vector<Nanoseconds> times = clip.frameTimes();

	pushFrames(buffer, encoder, 600);
	REQUIRE(buffer.stats().oldestTime > clip.out());

	CHECK(clip.frameTimes() == times);
	for (const auto &gop : clip.gops())
		CHECK(tapeloop::test::hasExpectedBytes(*gop));
}

TEST_CASE("SourceBuffer does not allocate per packet in steady state")
{
	if (!tapeloop::test::kAllocationHooks)
		SKIP("operator new cannot be replaced under this sanitizer");

	SyntheticEncoder encoder({});
	SourceBuffer buffer(makeConfig(encoder, 2s, kUnlimited));
	buffer.setCodecConfig(runConfig(0));
	pushFrames(buffer, encoder, 300);

	for (int frame = 0; frame < 600; ++frame) {
		const EncodedPacket packet = encoder.next();
		size_t allocations = 0;
		{
			AllocationCounter counter;
			buffer.push(packet);
			allocations = counter.count();
		}

		CAPTURE(frame);
		if (packet.keyframe) {
			// Sealing: the Gop, its bytes, its packet table, and now and then a new
			// block and map for the deque of GOPs. The codec configuration is
			// shared, not copied.
			if (tapeloop::test::kExactAllocationCounts)
				CHECK(allocations <= 5);
		} else {
			CHECK(allocations == 0);
		}
	}
}

TEST_CASE("SourceBuffer can be read while the encoder pushes")
{
	SyntheticEncoder::Config encoderConfig;
	encoderConfig.keyframeSize = 2048;
	encoderConfig.frameSize = 256;
	SyntheticEncoder encoder(encoderConfig);
	SourceBuffer buffer(makeConfig(encoder, 2s, kUnlimited));

	std::atomic<bool> done{false};
	std::atomic<int> failures{0};
	std::atomic<int> clipsRead{0};

	const auto check = [&](const Clip &clip) {
		if (clip.empty())
			return;
		const std::vector<Nanoseconds> times = clip.frameTimes();
		if (times.empty() || times.front() != clip.in() || times.back() != clip.out() ||
		    !strictlyIncreasing(times))
			++failures;
		for (const auto &gop : clip.gops()) {
			const CodecConfig *config = gop->codecConfig();
			if (!tapeloop::test::hasExpectedBytes(*gop) || !config ||
			    runOf(*config) != gop->packets().front().pts / 600 * 600)
				++failures;
		}
		const auto last = clip.locate(clip.out());
		if (clip.gops()[last.gop]->packets()[last.packet].time != clip.out())
			++failures;
		++clipsRead;
	};

	std::thread producer([&] {
		// A new run every 600 frames, each starting on a keyframe.
		for (int frame = 0; frame < 20'000; ++frame) {
			if (frame % 600 == 0)
				buffer.setCodecConfig(runConfig(frame));
			buffer.push(encoder.next());
		}
		done = true;
	});
	std::thread recent([&] {
		while (!done)
			check(buffer.clip(1s));
	});
	std::thread absolute([&] {
		while (!done) {
			const SourceBufferStats stats = buffer.stats();
			check(buffer.clip(stats.oldestTime, stats.newestTime));
		}
	});

	producer.join();
	recent.join();
	absolute.join();

	CHECK(failures == 0);
	CHECK(clipsRead > 0);
	CHECK(buffer.stats().newestTime == encoder.timeOf(19'999));
	CHECK(buffer.stats().droppedBeforeKeyframe == 0);
}

TEST_CASE("SourceBuffer gives every GOP of a run the same codec configuration")
{
	SyntheticEncoder encoder({});
	SourceBuffer buffer(makeConfig(encoder, 1h, kUnlimited));
	pushFrames(buffer, encoder, 30);
	buffer.setCodecConfig(runConfig(30));
	pushFrames(buffer, encoder, 65);

	const auto gops = heldGops(buffer);
	REQUIRE(gops.size() == 4);
	CHECK(gops[0]->codecConfig() == nullptr);
	REQUIRE(gops[1]->codecConfig() != nullptr);
	CHECK(*gops[1]->codecConfig() == runConfig(30));
	CHECK(gops[2]->codecConfig() == gops[1]->codecConfig());
	CHECK(gops[3]->codecConfig() == gops[1]->codecConfig());

	const SourceBufferStats stats = buffer.stats();
	CHECK(stats.configBytes == 8);
	CHECK(stats.bytes == 3 * kGopBytes + 4096 + 4 * 1024);
	CHECK(stats.droppedBeforeKeyframe == 0);
	CHECK(consistent(buffer));
}

TEST_CASE("SourceBuffer starts a new run at a new codec configuration")
{
	SyntheticEncoder encoder({});
	SourceBuffer buffer(makeConfig(encoder, 1h, kUnlimited));
	buffer.setCodecConfig(runConfig(0));
	pushFrames(buffer, encoder, 45);

	// The open GOP is sealed with the configuration it was encoded with, and the
	// frames that follow wait for a keyframe of the new run.
	buffer.setCodecConfig(CodecConfig(100, 0x11));
	CHECK(buffer.stats().gopCount == 2);
	pushFrames(buffer, encoder, 15);
	CHECK(buffer.stats().droppedBeforeKeyframe == 15);
	pushFrames(buffer, encoder, 30);

	const auto gops = heldGops(buffer);
	REQUIRE(gops.size() == 3);
	CHECK(gops[0]->packets().size() == 30);
	CHECK(gops[1]->packets().size() == 15);
	CHECK(gops[2]->packets().front().pts == 60);
	CHECK(gops[1]->codecConfig() == gops[0]->codecConfig());
	REQUIRE(gops[2]->codecConfig() != nullptr);
	CHECK(*gops[2]->codecConfig() == CodecConfig(100, 0x11));

	const SourceBufferStats stats = buffer.stats();
	CHECK(stats.configBytes == 108);
	CHECK(stats.discontinuities == 1);
	CHECK(consistent(buffer));
}

TEST_CASE("SourceBuffer counts a new codec configuration as a discontinuity only mid-run")
{
	SyntheticEncoder encoder({});
	SourceBuffer buffer(makeConfig(encoder, 1h, kUnlimited));

	// Before the first packet, after a clear, or twice in a row, nothing is cut short.
	buffer.setCodecConfig(runConfig(0));
	buffer.setCodecConfig(runConfig(0));
	CHECK(buffer.stats().discontinuities == 0);
	pushFrames(buffer, encoder, 30);
	buffer.clear();
	buffer.setCodecConfig(runConfig(30));
	CHECK(buffer.stats().discontinuities == 0);

	// A restart that keeps the buffer, after the run stopped cleanly at a GOP boundary.
	pushFrames(buffer, encoder, 30);
	buffer.setCodecConfig(runConfig(60));
	CHECK(buffer.stats().discontinuities == 1);
	pushFrames(buffer, encoder, 30);
	CHECK(buffer.stats().droppedBeforeKeyframe == 0);
	CHECK(buffer.stats().gopCount == 2);
}

TEST_CASE("SourceBuffer counts codec configurations in its stats but not its budget")
{
	SyntheticEncoder plainEncoder({});
	SyntheticEncoder configuredEncoder({});
	SourceBuffer plain(makeConfig(plainEncoder, 1h, 2 * kGopBytes));
	SourceBuffer configured(makeConfig(configuredEncoder, 1h, 2 * kGopBytes));
	configured.setCodecConfig(CodecConfig(50'000, 0x22));
	pushFrames(plain, plainEncoder, 150);
	pushFrames(configured, configuredEncoder, 150);

	const SourceBufferStats withoutConfig = plain.stats();
	const SourceBufferStats withConfig = configured.stats();
	CHECK(withConfig.gopCount == withoutConfig.gopCount);
	CHECK(withConfig.bytes == withoutConfig.bytes);
	CHECK(withConfig.oldestTime == withoutConfig.oldestTime);
	CHECK(withoutConfig.configBytes == 0);
	CHECK(withConfig.configBytes == 50'000);
}

TEST_CASE("SourceBuffer stops counting a configuration once its GOPs are gone")
{
	SyntheticEncoder encoder({});
	SourceBuffer buffer(makeConfig(encoder, 1s, kUnlimited));
	buffer.setCodecConfig(runConfig(0));
	pushFrames(buffer, encoder, 90);
	buffer.setCodecConfig(CodecConfig(3, 0x33));
	pushFrames(buffer, encoder, 30);
	CHECK(buffer.stats().configBytes == 11);

	// A one second window lets the GOPs of the first run go once the second run holds
	// more than a second.
	pushFrames(buffer, encoder, 90);
	CHECK(configsOf(heldGops(buffer)) ==
	      std::vector<const CodecConfig *>(3, heldGops(buffer).back()->codecConfig()));
	CHECK(buffer.stats().configBytes == 3);

	buffer.clear();
	CHECK(buffer.stats().configBytes == 0);
}

TEST_CASE("SourceBuffer keeps the codec configuration through clear and discontinuities")
{
	SyntheticEncoder::Config encoderConfig;
	SyntheticEncoder encoder(encoderConfig);
	SourceBuffer buffer(makeConfig(encoder, 1h, kUnlimited));
	buffer.setCodecConfig(runConfig(0));
	pushFrames(buffer, encoder, 40);
	const CodecConfig *config = heldGops(buffer).front()->codecConfig();

	// Frames 40 to 59 wait for the keyframe at 60.
	buffer.clear();
	pushFrames(buffer, encoder, 30);
	REQUIRE(buffer.stats().gopCount == 1);
	CHECK(heldGops(buffer).front()->codecConfig() == config);

	// A keyframe earlier than what is held breaks the run but not the configuration.
	encoderConfig.origin = -1s;
	SyntheticEncoder restarted(encoderConfig);
	pushFrames(buffer, restarted, 30);
	CHECK(buffer.stats().discontinuities == 1);
	CHECK(configsOf(heldGops(buffer)) == std::vector<const CodecConfig *>{config});
	CHECK(buffer.stats().configBytes == 8);
}

TEST_CASE("SourceBuffer takes an empty codec configuration as none")
{
	SyntheticEncoder encoder({});
	SourceBuffer buffer(makeConfig(encoder, 1h, kUnlimited));
	buffer.setCodecConfig(runConfig(0));
	buffer.setCodecConfig({});
	pushFrames(buffer, encoder, 30);
	CHECK(heldGops(buffer).front()->codecConfig() == nullptr);
	CHECK(buffer.stats().configBytes == 0);
}

TEST_CASE("SourceBuffer clips keep the codec configuration after the buffer is gone")
{
	SyntheticEncoder encoder({});
	Clip clip;
	{
		SourceBuffer buffer(makeConfig(encoder, 1h, kUnlimited));
		buffer.setCodecConfig(runConfig(0));
		pushFrames(buffer, encoder, 45);
		clip = buffer.clip(1s);
	}
	REQUIRE(clip.gops().size() == 2);
	for (const auto &gop : clip.gops()) {
		REQUIRE(gop->codecConfig() != nullptr);
		CHECK(*gop->codecConfig() == runConfig(0));
	}
}

TEST_CASE("SourceBuffer changes nothing when setting a codec configuration fails")
{
	if (!tapeloop::test::kAllocationFailures)
		SKIP("allocation failures cannot be injected in this configuration");

	for (size_t skip = 0; skip < 8; ++skip) {
		CAPTURE(skip);
		SyntheticEncoder encoder({});
		SourceBuffer buffer(makeConfig(encoder, 1h, kUnlimited));
		buffer.setCodecConfig(runConfig(0));
		pushFrames(buffer, encoder, 45);
		const CodecConfig *previous = heldGops(buffer).front()->codecConfig();
		const CodecConfig next = runConfig(45);

		bool failed = false;
		{
			tapeloop::test::AllocationFailure failure(skip);
			try {
				buffer.setCodecConfig(next);
			} catch (const std::bad_alloc &) {
				failed = true;
			}
		}
		pushFrames(buffer, encoder, 45);

		CHECK(consistent(buffer));
		const auto gops = heldGops(buffer);
		REQUIRE(gops.size() == 3);
		if (failed) {
			CHECK(buffer.stats().droppedBeforeKeyframe == 0);
			CHECK(buffer.stats().discontinuities == 0);
			CHECK(configsOf(gops) == std::vector<const CodecConfig *>(3, previous));
		} else {
			CHECK(buffer.stats().droppedBeforeKeyframe == 15);
			CHECK(buffer.stats().discontinuities == 1);
			CHECK(gops[1]->codecConfig() == previous);
			REQUIRE(gops[2]->codecConfig() != nullptr);
			CHECK(*gops[2]->codecConfig() == runConfig(45));
		}
	}
}
