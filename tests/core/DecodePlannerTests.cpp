// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/DecodePlanner.hpp"

#include "FakeDecoder.hpp"
#include "SyntheticEncoder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <vector>

using tapeloop::Clip;
using tapeloop::CodecConfig;
using tapeloop::DecodePlanner;
using tapeloop::DecodePlannerConfig;
using tapeloop::DecodeResult;
using tapeloop::DecodeStatus;
using tapeloop::GopBuilder;
using tapeloop::Nanoseconds;
using tapeloop::PlayDirection;
using tapeloop::VideoCodec;
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
