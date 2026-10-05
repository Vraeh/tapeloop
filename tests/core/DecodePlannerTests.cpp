// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/DecodePlanner.hpp"

#include "AllocationCounter.hpp"
#include "FakeDecoder.hpp"
#include "SyntheticEncoder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <vector>

using tapeloop::Clip;
using tapeloop::CodecConfig;
using tapeloop::DecodePlanner;
using tapeloop::DecodePlannerConfig;
using tapeloop::DecodeResult;
using tapeloop::DecodeStatus;
using tapeloop::DecodedFrame;
using tapeloop::GopBuilder;
using tapeloop::Nanoseconds;
using tapeloop::PlayDirection;
using tapeloop::VideoCodec;
using tapeloop::test::AllocationCounter;
using tapeloop::test::FakeDecoder;
using tapeloop::test::SyntheticEncoder;

namespace {

constexpr int64_t kGopLength = 30;

struct Run {
	VideoCodec codec = VideoCodec::H264;
	CodecConfig config;
	int gops = 1;
};

SyntheticEncoder::Config encoderConfig()
{
	SyntheticEncoder::Config config;
	config.gopLength = kGopLength;
	return config;
}

Nanoseconds timeOf(int64_t frame)
{
	return SyntheticEncoder(encoderConfig()).timeOf(frame);
}

// Whole GOPs of kGopLength frames, run after run, frame numbers counting from zero.
Clip makeClip(const std::vector<Run> &runs)
{
	SyntheticEncoder encoder(encoderConfig());
	GopBuilder builder(encoder.frameDuration());
	std::vector<std::shared_ptr<const tapeloop::Gop>> gops;
	for (const Run &run : runs) {
		builder.setCodecConfig(run.codec,
				       run.config.empty() ? nullptr : std::make_shared<const CodecConfig>(run.config));
		for (int gop = 0; gop < run.gops; ++gop) {
			for (int64_t i = 0; i < kGopLength; ++i) {
				builder.append(encoder.next());
			}
			gops.push_back(builder.seal());
		}
	}
	return Clip(gops, gops.front()->startTime(), gops.back()->lastTime());
}

FakeDecoder makeDecoder(size_t delay = 0)
{
	return FakeDecoder([](int64_t pts) { return pts % kGopLength == 0; }, delay);
}

// The frame shown at frame number `frame` is that frame, decoded once, and the decoder
// was used as it should be.
bool shows(DecodePlanner &planner, const FakeDecoder &decoder, int64_t frame)
{
	const DecodeResult result = planner.frameAt(timeOf(frame));
	return result.status == DecodeStatus::Ok && result.frame.pts == frame &&
	       decoder.made.at(result.frame.id).pts == frame && decoder.violations == 0;
}

// Hands every frame back at once and allocates nothing, so that only the planner's own
// allocations are counted.
class PassThroughDecoder : public tapeloop::FrameDecoder {
public:
	DecodeStatus open(VideoCodec, std::span<const uint8_t>) noexcept override { return restart(); }
	DecodeStatus send(std::span<const uint8_t>, int64_t pts, int64_t) noexcept override
	{
		if (pending_) {
			return DecodeStatus::InvalidData;
		}
		pending_ = pts;
		return DecodeStatus::Ok;
	}
	DecodeStatus receive(DecodedFrame &frame) noexcept override
	{
		if (!pending_) {
			return flushed_ ? DecodeStatus::Drained : DecodeStatus::NeedMore;
		}
		frame = {nextId_++, *pending_, Nanoseconds{0}};
		pending_.reset();
		return DecodeStatus::Ok;
	}
	DecodeStatus flush() noexcept override
	{
		flushed_ = true;
		return DecodeStatus::Ok;
	}
	void reset() noexcept override { restart(); }
	void release(const DecodedFrame &) noexcept override {}
	void close() noexcept override { restart(); }

private:
	DecodeStatus restart() noexcept
	{
		pending_.reset();
		flushed_ = false;
		return DecodeStatus::Ok;
	}

	std::optional<int64_t> pending_;
	bool flushed_ = false;
	uint64_t nextId_ = 1;
};

} // namespace

TEST_CASE("DecodePlanner plays forward sending every packet once")
{
	FakeDecoder decoder = makeDecoder();
	DecodePlanner planner(decoder);
	planner.load(makeClip({{VideoCodec::H264, {1, 2, 3}, 3}}));

	for (int64_t frame = 0; frame < 3 * kGopLength; ++frame) {
		REQUIRE(shows(planner, decoder, frame));
	}
	CHECK(planner.work().packetsSent == 3 * kGopLength);
	CHECK(planner.work().opens == 1);
	CHECK(planner.work().resets == 2);
	CHECK(planner.work().flushes == 0);
	CHECK(planner.keptGopCount() == 2);
	CHECK(planner.heldFrames() == 2 * kGopLength);
	CHECK(decoder.outstanding.size() == planner.heldFrames());
}

TEST_CASE("DecodePlanner flushes a decoder that holds frames back at the end of a GOP")
{
	FakeDecoder decoder = makeDecoder(3);
	DecodePlanner planner(decoder);
	planner.load(makeClip({{VideoCodec::H264, {}, 2}}));

	for (int64_t frame = 0; frame < 2 * kGopLength; ++frame) {
		REQUIRE(shows(planner, decoder, frame));
	}
	CHECK(planner.work().packetsSent == 2 * kGopLength);
	CHECK(planner.work().flushes == 2);
}

TEST_CASE("DecodePlanner steps back within a GOP without decoding")
{
	FakeDecoder decoder = makeDecoder();
	DecodePlanner planner(decoder);
	planner.load(makeClip({{VideoCodec::H264, {}, 2}}));

	REQUIRE(shows(planner, decoder, 40));
	const uint64_t sent = planner.work().packetsSent;
	CHECK(sent == 11);
	for (int64_t frame = 39; frame >= 30; --frame) {
		REQUIRE(shows(planner, decoder, frame));
	}
	CHECK(planner.work().packetsSent == sent);
	// Stepping forward again carries on where the decoder stopped.
	REQUIRE(shows(planner, decoder, 41));
	CHECK(planner.work().packetsSent == sent + 1);
	CHECK(planner.work().resets == 0);
}

TEST_CASE("DecodePlanner decodes the previous GOP ahead when playing in reverse")
{
	FakeDecoder decoder = makeDecoder(2);
	DecodePlanner planner(decoder);
	planner.load(makeClip({{VideoCodec::H264, {9}, 4}}));

	REQUIRE(shows(planner, decoder, 4 * kGopLength - 1));
	REQUIRE(planner.prefetch(PlayDirection::Backward) == DecodeStatus::Ok);
	for (int64_t frame = 4 * kGopLength - 2; frame >= 0; --frame) {
		const uint64_t before = planner.work().packetsSent;
		REQUIRE(shows(planner, decoder, frame));
		CHECK(planner.work().packetsSent == before);
		REQUIRE(planner.prefetch(PlayDirection::Backward) == DecodeStatus::Ok);
		CHECK(planner.keptGopCount() <= 2);
	}
	CHECK(planner.work().packetsSent == 4 * kGopLength);
	CHECK(planner.heldFrames() <= 2 * kGopLength);
}

TEST_CASE("DecodePlanner opens the decoder again for each run")
{
	FakeDecoder decoder = makeDecoder();
	DecodePlanner planner(decoder);
	planner.load(makeClip({{VideoCodec::H264, {1}, 2}, {VideoCodec::Hevc, {2}, 2}}));

	REQUIRE(shows(planner, decoder, 100));
	DecodeResult result = planner.frameAt(timeOf(100));
	CHECK(decoder.sessionOf(result.frame.id).codec == VideoCodec::Hevc);
	CHECK(decoder.sessionOf(result.frame.id).config == CodecConfig{2});

	REQUIRE(shows(planner, decoder, 10));
	result = planner.frameAt(timeOf(10));
	CHECK(decoder.sessionOf(result.frame.id).codec == VideoCodec::H264);
	CHECK(decoder.sessionOf(result.frame.id).config == CodecConfig{1});
	CHECK(planner.work().opens == 2);

	// Back to the HEVC run: the frames kept from it are still valid.
	result = planner.frameAt(timeOf(100));
	CHECK(result.status == DecodeStatus::Ok);
	CHECK(decoder.outstanding.contains(result.frame.id));
	CHECK(planner.work().opens == 2);
}

TEST_CASE("DecodePlanner returns each frame asked for at any rate")
{
	FakeDecoder decoder = makeDecoder(1);
	DecodePlanner planner(decoder);
	planner.load(makeClip({{VideoCodec::H264, {}, 3}}));

	for (int64_t frame = 0; frame < 3 * kGopLength; frame += 7) {
		REQUIRE(shows(planner, decoder, frame));
	}
	for (int64_t frame = 3 * kGopLength - 1; frame >= 0; frame -= 11) {
		REQUIRE(shows(planner, decoder, frame));
	}
	// Times between frames show the frame before; times outside the clip its ends.
	CHECK(planner.frameAt(timeOf(5) + Nanoseconds{1}).frame.pts == 5);
	CHECK(planner.frameAt(Nanoseconds{-1'000'000'000}).frame.pts == 0);
	CHECK(planner.frameAt(timeOf(10'000)).frame.pts == 3 * kGopLength - 1);
}

TEST_CASE("DecodePlanner keeps no more GOPs than configured")
{
	FakeDecoder decoder = makeDecoder();
	DecodePlannerConfig config;
	config.keptGops = 1;
	DecodePlanner planner(decoder, config);
	planner.load(makeClip({{VideoCodec::H264, {}, 3}}));

	for (int64_t frame = 0; frame < 3 * kGopLength; ++frame) {
		REQUIRE(shows(planner, decoder, frame));
		CHECK(planner.keptGopCount() == 1);
		CHECK(planner.heldFrames() <= static_cast<size_t>(kGopLength));
	}
	const uint64_t sent = planner.work().packetsSent;
	CHECK(planner.prefetch(PlayDirection::Backward) == DecodeStatus::Ok);
	CHECK(planner.work().packetsSent == sent);
}

TEST_CASE("DecodePlanner reports a decoder that cannot open")
{
	FakeDecoder decoder = makeDecoder();
	decoder.openStatus = DecodeStatus::Unsupported;
	DecodePlanner planner(decoder);
	planner.load(makeClip({{VideoCodec::Hevc, {}, 1}}));
	CHECK(planner.frameAt(timeOf(3)).status == DecodeStatus::Unsupported);
	CHECK(planner.heldFrames() == 0);

	decoder.openStatus = DecodeStatus::Ok;
	CHECK(shows(planner, decoder, 3));
}

TEST_CASE("DecodePlanner has nothing to show in an empty clip")
{
	FakeDecoder decoder = makeDecoder();
	DecodePlanner planner(decoder);
	CHECK(planner.frameAt(Nanoseconds{0}).status == DecodeStatus::NoFrame);
	CHECK(planner.prefetch(PlayDirection::Backward) == DecodeStatus::Ok);
	CHECK(planner.work().opens == 0);
}

TEST_CASE("DecodePlanner gives every frame back")
{
	FakeDecoder decoder = makeDecoder(2);
	{
		DecodePlanner planner(decoder);
		planner.load(makeClip({{VideoCodec::H264, {}, 2}}));
		REQUIRE(shows(planner, decoder, 50));
		REQUIRE(planner.prefetch(PlayDirection::Backward) == DecodeStatus::Ok);
		CHECK_FALSE(decoder.outstanding.empty());

		planner.load(makeClip({{VideoCodec::H264, {}, 1}}));
		CHECK(decoder.outstanding.empty());
		REQUIRE(shows(planner, decoder, 5));
	}
	CHECK(decoder.outstanding.empty());
	CHECK(decoder.violations == 0);
}

TEST_CASE("DecodePlanner reports a packet the decoder refuses and starts over after it")
{
	FakeDecoder decoder = makeDecoder();
	DecodePlanner planner(decoder);
	planner.load(makeClip({{VideoCodec::H264, {}, 2}}));
	REQUIRE(shows(planner, decoder, 3));

	decoder.failOnce = 5;
	CHECK(planner.frameAt(timeOf(8)).status == DecodeStatus::InvalidData);
	// The next request starts again at the keyframe, with a reset.
	const uint64_t resets = planner.work().resets;
	CHECK(shows(planner, decoder, 8));
	CHECK(planner.work().resets == resets + 1);
	CHECK(shows(planner, decoder, 2));
}

TEST_CASE("DecodePlanner opens the decoder again after its device is lost")
{
	FakeDecoder decoder = makeDecoder();
	DecodePlanner planner(decoder);
	planner.load(makeClip({{VideoCodec::H264, {1}, 2}}));
	REQUIRE(shows(planner, decoder, 5));
	REQUIRE(decoder.closes == 0);

	decoder.receiveFailure = DecodeStatus::DeviceLost;
	CHECK(planner.frameAt(timeOf(6)).status == DecodeStatus::DeviceLost);
	CHECK(planner.heldFrames() == 0);
	CHECK(planner.keptGopCount() == 0);
	CHECK(decoder.outstanding.empty());
	CHECK(decoder.closes == 1);

	// The frames kept before were on the lost device, so they are decoded again too.
	const uint64_t opens = planner.work().opens;
	const uint64_t sent = planner.work().packetsSent;
	CHECK(shows(planner, decoder, 5));
	CHECK(planner.work().opens == opens + 1);
	CHECK(planner.work().packetsSent == sent + 6);
}

TEST_CASE("DecodePlanner reports a frame the decoder never gives without decoding again")
{
	FakeDecoder decoder = makeDecoder();
	decoder.dropped = {kGopLength + 10, 10};
	DecodePlanner planner(decoder);
	planner.load(makeClip({{VideoCodec::H264, {1}, 3}}));

	CHECK(planner.frameAt(timeOf(kGopLength + 10)).status == DecodeStatus::InvalidData);
	const uint64_t sent = planner.work().packetsSent;
	CHECK(planner.frameAt(timeOf(kGopLength + 10)).status == DecodeStatus::InvalidData);
	CHECK(shows(planner, decoder, kGopLength + 11));
	CHECK(planner.work().packetsSent == sent);

	// Ahead of reverse play, the GOP with the missing frame is decoded once however
	// often it is asked for.
	REQUIRE(planner.prefetch(PlayDirection::Backward) == DecodeStatus::Ok);
	const tapeloop::DecodeWork once = planner.work();
	REQUIRE(planner.prefetch(PlayDirection::Backward) == DecodeStatus::Ok);
	CHECK(planner.work().packetsSent == once.packetsSent);
	CHECK(planner.work().resets == once.resets);
	CHECK(planner.work().flushes == once.flushes);
	CHECK(planner.frameAt(timeOf(10)).status == DecodeStatus::InvalidData);
	CHECK(planner.work().packetsSent == once.packetsSent);
}

TEST_CASE("DecodePlanner gives back a frame no packet of the GOP has")
{
	FakeDecoder decoder = makeDecoder();
	decoder.strayAfter = 2;
	DecodePlanner planner(decoder);
	planner.load(makeClip({{VideoCodec::H264, {1}, 1}}));
	CHECK(shows(planner, decoder, 5));
	CHECK(decoder.outstanding.size() == planner.heldFrames());
	CHECK(planner.heldFrames() == 6);
}

TEST_CASE("DecodePlanner never lets go of the GOP on screen to decode ahead")
{
	FakeDecoder decoder = makeDecoder();
	DecodePlanner planner(decoder);
	planner.load(makeClip({{VideoCodec::H264, {1}, 6}}));
	REQUIRE(shows(planner, decoder, 4 * kGopLength + 5));
	REQUIRE(shows(planner, decoder, 2 * kGopLength + 5));
	REQUIRE(shows(planner, decoder, 4 * kGopLength + 5));

	// GOPs 2 and 4 are equally far from GOP 3; GOP 4 is on screen.
	REQUIRE(planner.prefetch(PlayDirection::Backward) == DecodeStatus::Ok);
	const uint64_t sent = planner.work().packetsSent;
	CHECK(shows(planner, decoder, 4 * kGopLength + 4));
	CHECK(shows(planner, decoder, 3 * kGopLength + 20));
	CHECK(planner.work().packetsSent == sent);
}

TEST_CASE("DecodePlanner lets the farthest GOP go first")
{
	FakeDecoder decoder = makeDecoder();
	DecodePlanner planner(decoder);
	planner.load(makeClip({{VideoCodec::H264, {1}, 5}}));
	REQUIRE(shows(planner, decoder, 3 * kGopLength + 1));
	REQUIRE(shows(planner, decoder, 1));
	// GOP 0 came last but is farther from GOP 4 than GOP 3 is.
	REQUIRE(shows(planner, decoder, 4 * kGopLength + 1));
	const uint64_t sent = planner.work().packetsSent;
	CHECK(shows(planner, decoder, 3 * kGopLength + 1));
	CHECK(planner.work().packetsSent == sent);
}

TEST_CASE("DecodePlanner keeps what it holds when a run cannot be opened")
{
	FakeDecoder decoder = makeDecoder();
	DecodePlanner planner(decoder, DecodePlannerConfig{1});
	planner.load(makeClip({{VideoCodec::H264, {1}, 1}, {VideoCodec::H264, {2}, 1}}));
	REQUIRE(shows(planner, decoder, 10));
	const size_t held = planner.heldFrames();

	decoder.openStatus = DecodeStatus::Unsupported;
	CHECK(planner.frameAt(timeOf(kGopLength + 3)).status == DecodeStatus::Unsupported);
	CHECK(planner.heldFrames() == held);
	decoder.openStatus = DecodeStatus::Ok;
	const uint64_t sent = planner.work().packetsSent;
	CHECK(shows(planner, decoder, 9));
	CHECK(planner.work().packetsSent == sent);
}

TEST_CASE("DecodePlanner keeps at least one GOP")
{
	FakeDecoder decoder = makeDecoder();
	DecodePlanner planner(decoder, DecodePlannerConfig{0});
	planner.load(makeClip({{VideoCodec::H264, {1}, 3}}));
	REQUIRE(shows(planner, decoder, 1));
	REQUIRE(shows(planner, decoder, 2 * kGopLength + 1));
	CHECK(planner.keptGopCount() == 1);
	CHECK(planner.prefetch(PlayDirection::Backward) == DecodeStatus::Ok);
	CHECK(planner.keptGopCount() == 1);
}

TEST_CASE("DecodePlanner allocates nothing per frame or GOP once warm")
{
	PassThroughDecoder decoder;
	DecodePlanner planner(decoder);
	constexpr int64_t kGops = 8;
	planner.load(makeClip({{VideoCodec::H264, {1}, kGops}}));
	for (int64_t frame = 0; frame < 2 * kGopLength; ++frame) {
		REQUIRE(planner.frameAt(timeOf(frame)).status == DecodeStatus::Ok);
	}

	// Assertions allocate, so the answers are counted and checked afterwards.
	int wrong = 0;
	size_t allocations = 0;
	{
		AllocationCounter counter;
		for (int64_t frame = 2 * kGopLength; frame < kGops * kGopLength; ++frame) {
			const DecodeResult result = planner.frameAt(timeOf(frame));
			wrong += result.status != DecodeStatus::Ok || result.frame.pts != frame;
		}
		for (int64_t frame = kGops * kGopLength - 1; frame >= 0; --frame) {
			const DecodeResult result = planner.frameAt(timeOf(frame));
			wrong += result.status != DecodeStatus::Ok || result.frame.pts != frame;
			wrong += planner.prefetch(PlayDirection::Backward) != DecodeStatus::Ok;
		}
		allocations = counter.count();
	}
	CHECK(wrong == 0);
	if (tapeloop::test::kExactAllocationCounts) {
		CHECK(allocations == 0);
	}
}

TEST_CASE("DecodePlanner gives each frame the time of its packet")
{
	FakeDecoder decoder = makeDecoder();
	DecodePlanner planner(decoder);
	planner.load(makeClip({{VideoCodec::H264, {1}, 2}}));
	for (const int64_t frame : {int64_t{0}, int64_t{17}, kGopLength + 3}) {
		const DecodeResult result = planner.frameAt(timeOf(frame));
		REQUIRE(result.status == DecodeStatus::Ok);
		CHECK(result.frame.time == timeOf(frame));
	}
}

TEST_CASE("DecodePlanner decodes the whole previous GOP once ahead of reverse play")
{
	FakeDecoder decoder = makeDecoder();
	DecodePlanner planner(decoder);
	planner.load(makeClip({{VideoCodec::H264, {1}, 3}}));
	REQUIRE(shows(planner, decoder, 2 * kGopLength + 5));
	REQUIRE(planner.prefetch(PlayDirection::Backward) == DecodeStatus::Ok);
	const tapeloop::DecodeWork ahead = planner.work();
	REQUIRE(planner.prefetch(PlayDirection::Backward) == DecodeStatus::Ok);
	CHECK(planner.work().packetsSent == ahead.packetsSent);
	CHECK(planner.work().resets == ahead.resets);

	for (int64_t frame = 2 * kGopLength - 1; frame >= kGopLength; --frame) {
		REQUIRE(shows(planner, decoder, frame));
	}
	CHECK(planner.work().packetsSent == ahead.packetsSent);
}

TEST_CASE("DecodePlanner reports a frame it could not receive and starts over after it")
{
	FakeDecoder decoder = makeDecoder();
	DecodePlanner planner(decoder);
	planner.load(makeClip({{VideoCodec::H264, {1}, 1}}));
	decoder.receiveFailure = DecodeStatus::InvalidData;
	CHECK(planner.frameAt(timeOf(3)).status == DecodeStatus::InvalidData);
	CHECK(shows(planner, decoder, 3));
	CHECK(shows(planner, decoder, 4));
}

TEST_CASE("DecodePlanner closes the decoder when it loads another clip")
{
	FakeDecoder decoder = makeDecoder();
	DecodePlanner planner(decoder);
	const Clip clip = makeClip({{VideoCodec::H264, {1}, 1}});
	planner.load(clip);
	REQUIRE(shows(planner, decoder, 5));
	planner.load(clip);
	CHECK(decoder.closes == 1);
	const uint64_t opens = planner.work().opens;
	CHECK(shows(planner, decoder, 5));
	CHECK(planner.work().opens == opens + 1);
}

TEST_CASE("DecodePlanner recovers from running out of memory for a longer GOP")
{
	if (!tapeloop::test::kAllocationFailures) {
		SKIP("allocation failures cannot be injected in this configuration");
	}

	// GOPs of 10, 50 and 10 frames: the second needs more room than either kept slot.
	SyntheticEncoder::Config config;
	config.gopLength = 10;
	SyntheticEncoder encoder(config);
	GopBuilder builder(encoder.frameDuration());
	std::vector<std::shared_ptr<const tapeloop::Gop>> gops;
	for (const int length : {10, 50, 10}) {
		for (int i = 0; i < length; ++i) {
			builder.append(encoder.next());
		}
		gops.push_back(builder.seal());
	}
	const Clip clip(gops, gops.front()->startTime(), gops.back()->lastTime());
	const auto timeAt = [&encoder](int64_t frame) {
		return encoder.timeOf(frame);
	};
	FakeDecoder decoder([](int64_t pts) { return pts == 0 || pts == 10 || pts == 60; }, 0);
	DecodePlanner planner(decoder);
	planner.load(clip);
	REQUIRE(planner.frameAt(timeAt(5)).status == DecodeStatus::Ok);
	REQUIRE(planner.frameAt(timeAt(65)).status == DecodeStatus::Ok);

	bool threw = false;
	{
		tapeloop::test::AllocationFailure failure;
		try {
			planner.frameAt(timeAt(40));
		} catch (const std::bad_alloc &) {
			threw = true;
		}
	}
	REQUIRE(threw);

	// Nothing may take the slot that ran out of room for a GOP it never held.
	REQUIRE(planner.frameAt(timeAt(65)).status == DecodeStatus::Ok);
	REQUIRE(planner.prefetch(PlayDirection::Backward) == DecodeStatus::Ok);
	for (int64_t frame = 59; frame >= 10; --frame) {
		const DecodeResult result = planner.frameAt(timeAt(frame));
		REQUIRE(result.status == DecodeStatus::Ok);
		CHECK(result.frame.pts == frame);
	}
	CHECK(decoder.violations == 0);
}

TEST_CASE("DecodePlanner keeps its frames within the byte cap and decodes again what it gave back")
{
	FakeDecoder decoder = makeDecoder(2);
	decoder.frameBytes = 100;
	DecodePlannerConfig config;
	config.maxBytes = 350;
	DecodePlanner planner(decoder, config);
	planner.load(makeClip({{VideoCodec::H264, {}, 2}}));

	for (int64_t frame = 0; frame < 2 * kGopLength; ++frame) {
		REQUIRE(shows(planner, decoder, frame));
		CHECK(planner.heldBytes() <= 300);
		CHECK(planner.heldBytes() == planner.heldFrames() * 100);
		CHECK(decoder.outstanding.size() == planner.heldFrames());
	}
	// Each packet went once, forward.
	CHECK(planner.work().packetsSent == 2 * kGopLength);

	// Three frames are kept: the last ones show without decoding, an earlier one decodes
	// its GOP again from the keyframe.
	const uint64_t sent = planner.work().packetsSent;
	REQUIRE(shows(planner, decoder, 2 * kGopLength - 2));
	CHECK(planner.work().packetsSent == sent);
	REQUIRE(shows(planner, decoder, kGopLength + 5));
	CHECK(planner.work().packetsSent > sent);
	CHECK(planner.heldBytes() <= 300);
	for (int64_t frame = kGopLength + 4; frame >= 0; --frame) {
		REQUIRE(shows(planner, decoder, frame));
		CHECK(planner.heldBytes() <= 300);
	}
}

TEST_CASE("DecodePlanner shows a frame larger than the byte cap and keeps only it")
{
	FakeDecoder decoder = makeDecoder();
	decoder.frameBytes = 100;
	DecodePlannerConfig config;
	config.maxBytes = 50;
	DecodePlanner planner(decoder, config);
	planner.load(makeClip({{VideoCodec::H264, {}, 2}}));

	REQUIRE(shows(planner, decoder, 10));
	CHECK(planner.heldFrames() == 1);
	REQUIRE(shows(planner, decoder, 9));
	CHECK(planner.heldFrames() == 1);
	REQUIRE(planner.prefetch(PlayDirection::Backward) == DecodeStatus::Ok);
	REQUIRE(shows(planner, decoder, kGopLength + 3));
	REQUIRE(planner.prefetch(PlayDirection::Backward) == DecodeStatus::Ok);
	CHECK(planner.heldFrames() == 1);
	REQUIRE(shows(planner, decoder, kGopLength - 1));
	CHECK(decoder.outstanding.size() == 1);
}

TEST_CASE("DecodePlanner decodes ahead within the byte cap without letting go of the GOP on screen")
{
	FakeDecoder decoder = makeDecoder(1);
	decoder.frameBytes = 10;
	DecodePlannerConfig config;
	config.maxBytes = 10 * (kGopLength + 5);
	DecodePlanner planner(decoder, config);
	planner.load(makeClip({{VideoCodec::H264, {}, 3}}));

	REQUIRE(shows(planner, decoder, 3 * kGopLength - 1));
	for (int64_t frame = 3 * kGopLength - 2; frame >= 2 * kGopLength; --frame) {
		REQUIRE(shows(planner, decoder, frame));
	}
	// The GOP on screen stays whole; of the one before, its last frames fit.
	REQUIRE(planner.prefetch(PlayDirection::Backward) == DecodeStatus::Ok);
	CHECK(planner.heldBytes() <= config.maxBytes);
	CHECK(planner.heldFrames() == kGopLength + 5);
	const uint64_t sent = planner.work().packetsSent;
	REQUIRE(shows(planner, decoder, 2 * kGopLength + 7));
	for (int64_t frame = 2 * kGopLength - 1; frame >= 2 * kGopLength - 5; --frame) {
		REQUIRE(shows(planner, decoder, frame));
	}
	CHECK(planner.work().packetsSent == sent);
	// Further back than what fit, the GOP is decoded again.
	REQUIRE(shows(planner, decoder, kGopLength + 2));
	CHECK(planner.work().packetsSent > sent);
	CHECK(planner.heldBytes() <= config.maxBytes);
}

TEST_CASE("The decoded-frame cache is 512 MiB unless set within its bounds")
{
	constexpr size_t kMiB = size_t{1} << 20;
	CHECK(DecodePlannerConfig{}.maxBytes == 512 * kMiB);
	CHECK(tapeloop::kDefaultDecodedCacheBytes == 512 * kMiB);
	// 128 MiB to a quarter of the dedicated video memory, at most 4 GiB.
	CHECK(tapeloop::clampDecodedCacheBytes(512 * kMiB, 8192 * kMiB) == 512 * kMiB);
	CHECK(tapeloop::clampDecodedCacheBytes(64 * kMiB, 8192 * kMiB) == 128 * kMiB);
	CHECK(tapeloop::clampDecodedCacheBytes(4096 * kMiB, 8192 * kMiB) == 2048 * kMiB);
	CHECK(tapeloop::clampDecodedCacheBytes(8192 * kMiB, 32768 * kMiB) == 4096 * kMiB);
	// An adapter with little or no memory of its own still allows the least.
	CHECK(tapeloop::clampDecodedCacheBytes(512 * kMiB, 256 * kMiB) == 128 * kMiB);
	CHECK(tapeloop::clampDecodedCacheBytes(512 * kMiB, 0) == 128 * kMiB);
}

TEST_CASE("DecodePlanner carries a pass on past frames it gave back instead of starting over")
{
	FakeDecoder decoder = makeDecoder();
	decoder.frameBytes = 100;
	DecodePlannerConfig config;
	config.maxBytes = 1000;
	DecodePlanner planner(decoder, config);
	planner.load(makeClip({{VideoCodec::H264, {}, 1}}));

	for (int64_t frame = 0; frame < kGopLength; ++frame) {
		REQUIRE(shows(planner, decoder, frame));
	}
	REQUIRE(shows(planner, decoder, 10));
	const uint64_t sent = planner.work().packetsSent;
	const uint64_t resets = planner.work().resets;
	for (int64_t frame = 11; frame < kGopLength; ++frame) {
		REQUIRE(shows(planner, decoder, frame));
	}
	CHECK(planner.work().packetsSent == sent + 19);
	CHECK(planner.work().resets == resets);
}

TEST_CASE("DecodePlanner plays a GOP larger than the cap in reverse with a pass per cap's worth")
{
	FakeDecoder decoder = makeDecoder();
	decoder.frameBytes = 100;
	DecodePlannerConfig config;
	config.maxBytes = 500;
	DecodePlanner planner(decoder, config);
	planner.load(makeClip({{VideoCodec::H264, {}, 1}}));

	for (int64_t frame = kGopLength - 1; frame >= 0; --frame) {
		REQUIRE(shows(planner, decoder, frame));
	}
	// Each pass from the keyframe leaves the five frames before the one asked for:
	// 30, then 25, 20, 15, 10 and 5 packets.
	CHECK(planner.work().packetsSent == 105);
	CHECK(planner.work().resets == 5);
}

TEST_CASE("DecodePlanner does not decode ahead what the cap would only give back")
{
	FakeDecoder decoder = makeDecoder();
	decoder.frameBytes = 100;
	DecodePlannerConfig config;
	config.maxBytes = 2000;
	DecodePlanner planner(decoder, config);
	planner.load(makeClip({{VideoCodec::H264, {}, 3}}));

	for (int64_t frame = 3 * kGopLength - 1; frame >= 0; --frame) {
		REQUIRE(shows(planner, decoder, frame));
		REQUIRE(planner.prefetch(PlayDirection::Backward) == DecodeStatus::Ok);
		CHECK(planner.heldBytes() <= config.maxBytes);
	}
	// Without decoding ahead the same walk sends 3 * (30 + 10) packets.
	CHECK(planner.work().packetsSent <= 3 * (kGopLength + 10) + kGopLength);
}

TEST_CASE("DecodePlanner lets go of another GOP a frame at a time, from its far end")
{
	FakeDecoder decoder = makeDecoder();
	decoder.frameBytes = 100;
	DecodePlannerConfig config;
	config.maxBytes = 4000;
	DecodePlanner planner(decoder, config);
	planner.load(makeClip({{VideoCodec::H264, {}, 2}}));

	for (int64_t frame = 0; frame <= kGopLength + 10; ++frame) {
		REQUIRE(shows(planner, decoder, frame));
	}
	const uint64_t sent = planner.work().packetsSent;
	for (int64_t frame = kGopLength - 1; frame >= kGopLength - 29; --frame) {
		REQUIRE(shows(planner, decoder, frame));
	}
	CHECK(planner.work().packetsSent == sent);
}

TEST_CASE("DecodePlanner counts a frame the decoder gives twice once")
{
	FakeDecoder decoder = makeDecoder();
	decoder.twice = 3;
	DecodePlanner planner(decoder);
	planner.load(makeClip({{VideoCodec::H264, {}, 1}}));
	REQUIRE(shows(planner, decoder, kGopLength - 1));
	REQUIRE(shows(planner, decoder, 3));
	CHECK(decoder.outstanding.size() == planner.heldFrames());
}

TEST_CASE("DecodePlanner reports a frame the decoder never gives once, under the cap too")
{
	FakeDecoder decoder = makeDecoder();
	decoder.frameBytes = 100;
	decoder.dropped = {5};
	DecodePlannerConfig config;
	config.maxBytes = 300;
	DecodePlanner planner(decoder, config);
	planner.load(makeClip({{VideoCodec::H264, {}, 1}}));

	REQUIRE(shows(planner, decoder, kGopLength - 1));
	CHECK(planner.frameAt(timeOf(5)).status == DecodeStatus::InvalidData);
	const uint64_t sent = planner.work().packetsSent;
	CHECK(planner.frameAt(timeOf(5)).status == DecodeStatus::InvalidData);
	CHECK(planner.work().packetsSent == sent);
}
