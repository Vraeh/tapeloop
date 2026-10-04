// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/Clip.hpp"

#include "SyntheticEncoder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

using tapeloop::Clip;
using tapeloop::FrameLocation;
using tapeloop::Gop;
using tapeloop::GopBuilder;
using tapeloop::Nanoseconds;
using tapeloop::test::SyntheticEncoder;

namespace {

constexpr Nanoseconds kOneNs{1};

// 59.94 fps, so frame times are not whole nanoseconds apart.
SyntheticEncoder::Config ntscConfig()
{
	SyntheticEncoder::Config config;
	config.frameTimebase = {1001, 60000};
	config.keyframeSize = 300;
	config.frameSize = 100;
	return config;
}

std::vector<std::shared_ptr<const Gop>> encodeGops(SyntheticEncoder &encoder, int count)
{
	std::vector<std::shared_ptr<const Gop>> gops;
	GopBuilder builder(encoder.frameDuration());
	while (gops.size() < static_cast<size_t>(count)) {
		builder.append(encoder.next());
		if (encoder.isKeyframe(encoder.nextFrame())) {
			gops.push_back(builder.seal());
		}
	}
	return gops;
}

bool sameLocation(FrameLocation a, FrameLocation b)
{
	return a.gop == b.gop && a.packet == b.packet;
}

} // namespace

TEST_CASE("Clip keeps the frames between in and out at 59.94 fps")
{
	SyntheticEncoder encoder(ntscConfig());
	const auto gops = encodeGops(encoder, 4);

	SECTION("range on frame times")
	{
		const Clip clip(gops, encoder.timeOf(40), encoder.timeOf(100));
		CHECK(clip.in() == encoder.timeOf(40));
		CHECK(clip.out() == encoder.timeOf(100));
		REQUIRE(clip.gops().size() == 3);
		CHECK(clip.gops()[0] == gops[1]);
		CHECK(clip.gops()[2] == gops[3]);

		const std::vector<Nanoseconds> times = clip.frameTimes();
		REQUIRE(times.size() == 61);
		for (size_t i = 0; i < times.size(); ++i) {
			CHECK(times[i] == encoder.timeOf(40 + static_cast<int64_t>(i)));
		}
	}

	SECTION("range between frame times snaps inwards")
	{
		const Clip clip(gops, encoder.timeOf(40) + kOneNs, encoder.timeOf(100) - kOneNs);
		CHECK(clip.in() == encoder.timeOf(41));
		CHECK(clip.out() == encoder.timeOf(99));
		CHECK(clip.frameTimes().size() == 59);
	}

	SECTION("range wider than the GOPs is clamped")
	{
		const Clip clip(gops, Nanoseconds::min(), Nanoseconds::max());
		CHECK(clip.in() == encoder.timeOf(0));
		CHECK(clip.out() == encoder.timeOf(119));
		CHECK(clip.gops().size() == 4);
		CHECK(clip.frameTimes().size() == 120);
	}

	SECTION("in on a keyframe starts at that GOP")
	{
		const Clip clip(gops, encoder.timeOf(60), encoder.timeOf(70));
		REQUIRE(clip.gops().size() == 1);
		CHECK(clip.gops()[0] == gops[2]);
	}

	SECTION("out just before a keyframe leaves the next GOP out")
	{
		const Clip clip(gops, encoder.timeOf(10), encoder.timeOf(60) - kOneNs);
		CHECK(clip.gops().size() == 2);
		CHECK(clip.out() == encoder.timeOf(59));
	}
}

TEST_CASE("Clip locates the frame on screen")
{
	SyntheticEncoder encoder(ntscConfig());
	const auto gops = encodeGops(encoder, 3);
	const Clip clip(gops, encoder.timeOf(10), encoder.timeOf(70));

	// gops()[0] is the first GOP of the stream, so packet indices equal frame indices.
	CHECK(sameLocation(clip.locate(encoder.timeOf(10)), {0, 10}));
	CHECK(sameLocation(clip.locate(encoder.timeOf(10) - kOneNs), {0, 10}));
	CHECK(sameLocation(clip.locate(Nanoseconds::min()), {0, 10}));
	CHECK(sameLocation(clip.locate(encoder.timeOf(11) - kOneNs), {0, 10}));
	CHECK(sameLocation(clip.locate(encoder.timeOf(11)), {0, 11}));
	CHECK(sameLocation(clip.locate(encoder.timeOf(30) - kOneNs), {0, 29}));
	CHECK(sameLocation(clip.locate(encoder.timeOf(30)), {1, 0}));
	CHECK(sameLocation(clip.locate(encoder.timeOf(59)), {1, 29}));
	CHECK(sameLocation(clip.locate(encoder.timeOf(60)), {2, 0}));
	CHECK(sameLocation(clip.locate(encoder.timeOf(70)), {2, 10}));
	CHECK(sameLocation(clip.locate(encoder.timeOf(71)), {2, 10}));
	CHECK(sameLocation(clip.locate(Nanoseconds::max()), {2, 10}));
}

TEST_CASE("Clip locates every frame time back to that frame")
{
	SyntheticEncoder encoder(ntscConfig());
	const auto gops = encodeGops(encoder, 3);
	const Clip clip(gops, Nanoseconds::min(), Nanoseconds::max());

	const std::vector<Nanoseconds> times = clip.frameTimes();
	for (size_t frame = 0; frame < times.size(); ++frame) {
		const FrameLocation location = clip.locate(times[frame]);
		CHECK(location.gop == frame / 30);
		CHECK(location.packet == frame % 30);
	}
}

TEST_CASE("Clip is empty when no frame falls in the range")
{
	SyntheticEncoder encoder(ntscConfig());
	const auto gops = encodeGops(encoder, 2);

	CHECK(Clip().empty());
	CHECK(Clip({}, Nanoseconds::min(), Nanoseconds::max()).empty());
	CHECK(Clip(gops, encoder.timeOf(20), encoder.timeOf(10)).empty());
	CHECK(Clip(gops, encoder.timeOf(10) + kOneNs, encoder.timeOf(11) - kOneNs).empty());
	CHECK(Clip(gops, encoder.timeOf(60), Nanoseconds::max()).empty());
	CHECK(Clip(gops, Nanoseconds::min(), encoder.timeOf(0) - kOneNs).empty());
	CHECK(Clip().frameTimes().empty());
	CHECK(Clip().byteSize() == 0);

	const Clip single(gops, encoder.timeOf(5), encoder.timeOf(5));
	CHECK_FALSE(single.empty());
	CHECK(single.frameTimes().size() == 1);
}

TEST_CASE("Clip counts whole GOPs once and shares them when copied")
{
	SyntheticEncoder encoder(ntscConfig());
	const auto gops = encodeGops(encoder, 3);
	const size_t gopBytes = 300 + 29 * 100;

	const Clip clip(gops, encoder.timeOf(29), encoder.timeOf(30));
	CHECK(clip.byteSize() == 2 * gopBytes);

	const long owners = gops[0].use_count();
	const Clip copy = clip;
	CHECK(gops[0].use_count() == owners + 1);
	CHECK(copy.gops()[0] == clip.gops()[0]);
	CHECK(copy.byteSize() == clip.byteSize());
}

TEST_CASE("Clip locates across a gap between GOPs")
{
	SyntheticEncoder encoder(ntscConfig());
	GopBuilder builder(encoder.frameDuration());
	std::vector<std::shared_ptr<const Gop>> gops;
	for (int frame = 0; frame < 90; ++frame) {
		const auto packet = encoder.next();
		if (frame >= 30 && frame < 60) {
			continue;
		}
		builder.append(packet);
		if (encoder.isKeyframe(encoder.nextFrame())) {
			gops.push_back(builder.seal());
		}
	}
	REQUIRE(gops.size() == 2);

	const Clip clip(gops, Nanoseconds::min(), Nanoseconds::max());
	CHECK(clip.frameTimes().size() == 60);
	CHECK(sameLocation(clip.locate(encoder.timeOf(45)), {0, 29}));
	CHECK(sameLocation(clip.locate(encoder.timeOf(60)), {1, 0}));
}

TEST_CASE("Clip locate on an empty clip answers the first position")
{
	CHECK(sameLocation(Clip().locate(Nanoseconds{0}), {0, 0}));
	CHECK(sameLocation(Clip().locate(Nanoseconds::max()), {0, 0}));
}
