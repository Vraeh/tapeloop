// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/Gop.hpp"

#include "AllocationCounter.hpp"
#include "SyntheticEncoder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <new>
#include <vector>

using tapeloop::CodecConfig;
using tapeloop::EncodedPacket;
using tapeloop::GopBuilder;
using tapeloop::Nanoseconds;
using tapeloop::VideoCodec;
using tapeloop::test::AllocationCounter;
using tapeloop::test::SyntheticEncoder;

TEST_CASE("GopBuilder copies packets into one block")
{
	SyntheticEncoder::Config config;
	config.gopLength = 4;
	config.keyframeSize = 100;
	config.frameSize = 10;
	SyntheticEncoder encoder(config);
	GopBuilder builder(encoder.frameDuration());
	CHECK(builder.empty());

	for (int i = 0; i < 4; ++i) {
		builder.append(encoder.next());
	}
	CHECK(builder.byteSize() == 130);

	const auto gop = builder.seal();
	CHECK(builder.empty());
	CHECK(builder.byteSize() == 0);

	REQUIRE(gop->packets().size() == 4);
	CHECK(gop->byteSize() == 130);
	CHECK(gop->packets()[0].keyframe);
	CHECK_FALSE(gop->packets()[1].keyframe);
	CHECK(gop->packets()[0].offset == 0);
	CHECK(gop->packets()[1].offset == 100);
	CHECK(gop->packets()[3].offset == 120);
	CHECK(gop->packets()[3].size == 10);
	CHECK(gop->packets()[2].pts == 2);
	CHECK(gop->packets()[2].dts == 2);
	CHECK(gop->startTime() == encoder.timeOf(0));
	CHECK(gop->lastTime() == encoder.timeOf(3));
	CHECK(gop->endTime() == encoder.timeOf(3) + encoder.frameDuration());
	CHECK(gop->packetData(1).size() == 10);
	CHECK(tapeloop::test::hasExpectedBytes(*gop));
}

TEST_CASE("GopBuilder does not keep a reference to the packet data")
{
	std::vector<uint8_t> data(16, 0xab);
	GopBuilder builder(Nanoseconds{1});
	builder.append({data, 0, 0, Nanoseconds{0}, true});
	std::fill(data.begin(), data.end(), uint8_t{0});

	const auto gop = builder.seal();
	const auto stored = gop->packetData(0);
	CHECK(std::all_of(stored.begin(), stored.end(), [](uint8_t byte) { return byte == 0xab; }));
}

TEST_CASE("GopBuilder accepts empty packets")
{
	GopBuilder builder(Nanoseconds{1});
	builder.append({{}, 0, 0, Nanoseconds{0}, true});
	builder.append({{}, 1, 1, Nanoseconds{1}, false});

	const auto gop = builder.seal();
	CHECK(gop->byteSize() == 0);
	CHECK(gop->packets().size() == 2);
	CHECK(gop->packetData(1).empty());
}

TEST_CASE("GopBuilder snapshots are not affected by later packets")
{
	SyntheticEncoder::Config config;
	config.gopLength = 10;
	SyntheticEncoder encoder(config);
	GopBuilder builder(encoder.frameDuration());
	for (int i = 0; i < 3; ++i) {
		builder.append(encoder.next());
	}

	const auto snapshot = builder.snapshot();
	for (int i = 0; i < 5; ++i) {
		builder.append(encoder.next());
	}
	const auto sealed = builder.seal();

	CHECK(snapshot->packets().size() == 3);
	CHECK(snapshot->lastTime() == encoder.timeOf(2));
	CHECK(tapeloop::test::hasExpectedBytes(*snapshot));
	CHECK(sealed->packets().size() == 8);
	CHECK(sealed.get() != snapshot.get());
}

TEST_CASE("GopBuilder gives every GOP the codec configuration it holds")
{
	SyntheticEncoder::Config config;
	config.gopLength = 3;
	SyntheticEncoder encoder(config);
	GopBuilder builder(encoder.frameDuration());

	for (int i = 0; i < 3; ++i) {
		builder.append(encoder.next());
	}
	CHECK(builder.seal()->codecConfig() == nullptr);

	auto first = std::make_shared<const CodecConfig>(CodecConfig{0, 0, 0, 1, 0x67});
	const CodecConfig *firstAddress = first.get();
	builder.setCodecConfig(VideoCodec::H264, first);
	std::vector<std::shared_ptr<const tapeloop::Gop>> gops;
	for (int gop = 0; gop < 2; ++gop) {
		for (int i = 0; i < 3; ++i) {
			builder.append(encoder.next());
		}
		gops.push_back(builder.seal());
	}
	builder.append(encoder.next());
	gops.push_back(builder.snapshot());
	builder.append(encoder.next());
	builder.append(encoder.next());
	gops.push_back(builder.seal());
	for (const auto &gop : gops) {
		CHECK(gop->codecConfig() == firstAddress);
	}

	builder.setCodecConfig(VideoCodec::H264, std::make_shared<const CodecConfig>(CodecConfig{0, 0, 0, 1, 0x40}));
	builder.append(encoder.next());
	const auto next = builder.seal();
	REQUIRE(next->codecConfig() != nullptr);
	CHECK(*next->codecConfig() == CodecConfig{0, 0, 0, 1, 0x40});

	// The GOPs keep the earlier configuration alive.
	first.reset();
	CHECK(gops.front()->codecConfig() == firstAddress);
	CHECK(*gops.front()->codecConfig() == CodecConfig{0, 0, 0, 1, 0x67});
}

TEST_CASE("GopBuilder does not allocate per packet in steady state")
{
	if (!tapeloop::test::kAllocationHooks) {
		SKIP("operator new cannot be replaced under this sanitizer");
	}

	SyntheticEncoder::Config config;
	config.keyframeSize = 20'000;
	config.frameSize = 5'000;
	SyntheticEncoder encoder(config);
	GopBuilder builder(encoder.frameDuration());
	builder.setCodecConfig(VideoCodec::H264, std::make_shared<const CodecConfig>(CodecConfig(40, 0x42)));

	// The first GOPs size the buffers.
	for (int i = 0; i < 90; ++i) {
		const EncodedPacket packet = encoder.next();
		if (packet.keyframe && !builder.empty()) {
			builder.seal();
		}
		builder.append(packet);
	}

	for (int gop = 0; gop < 10; ++gop) {
		const EncodedPacket keyframe = encoder.next();
		REQUIRE(keyframe.keyframe);
		{
			AllocationCounter allocations;
			builder.seal();
			// The Gop, its bytes and its packet table; the codec configuration is
			// shared, not copied.
			if (tapeloop::test::kExactAllocationCounts) {
				CHECK(allocations.count() <= 3);
			}
		}

		AllocationCounter allocations;
		builder.append(keyframe);
		for (int i = 1; i < 30; ++i) {
			builder.append(encoder.next());
		}
		CHECK(allocations.count() == 0);
	}
}

TEST_CASE("GopBuilder loses nothing when sealing fails")
{
	if (!tapeloop::test::kAllocationFailures) {
		SKIP("allocation failures cannot be injected in this configuration");
	}

	SyntheticEncoder::Config config;
	config.gopLength = 10;

	// Fail each allocation of seal() in turn, then let it through.
	for (size_t skip = 0; skip < 12; ++skip) {
		CAPTURE(skip);
		SyntheticEncoder encoder(config);
		GopBuilder builder(encoder.frameDuration());
		for (int i = 0; i < 5; ++i) {
			builder.append(encoder.next());
		}

		std::shared_ptr<const tapeloop::Gop> gop;
		bool failed = false;
		{
			tapeloop::test::AllocationFailure failure(skip);
			try {
				gop = builder.seal();
			} catch (const std::bad_alloc &) {
				failed = true;
			}
		}

		if (failed) {
			CHECK(builder.byteSize() == 4096 + 4 * 1024);
			gop = builder.seal();
		}
		CHECK(builder.empty());
		REQUIRE(gop->packets().size() == 5);
		CHECK(tapeloop::test::hasExpectedBytes(*gop));
	}
}

TEST_CASE("GopBuilder gives every GOP the codec of its run")
{
	SyntheticEncoder::Config config;
	config.gopLength = 2;
	SyntheticEncoder encoder(config);
	GopBuilder builder(encoder.frameDuration());
	CHECK(builder.codec() == VideoCodec::H264);

	builder.append(encoder.next());
	builder.append(encoder.next());
	CHECK(builder.seal()->codec() == VideoCodec::H264);

	builder.setCodecConfig(VideoCodec::Hevc, nullptr);
	builder.append(encoder.next());
	CHECK(builder.snapshot()->codec() == VideoCodec::Hevc);
	builder.append(encoder.next());
	const auto hevc = builder.seal();
	CHECK(hevc->codec() == VideoCodec::Hevc);
	CHECK(hevc->codecConfig() == nullptr);
}

TEST_CASE("codec names map to the codecs a replay holds")
{
	CHECK(tapeloop::videoCodecFromName("h264") == VideoCodec::H264);
	CHECK(tapeloop::videoCodecFromName("hevc") == VideoCodec::Hevc);
	CHECK_FALSE(tapeloop::videoCodecFromName("av1"));
	CHECK_FALSE(tapeloop::videoCodecFromName("H264"));
	CHECK_FALSE(tapeloop::videoCodecFromName(""));
}
