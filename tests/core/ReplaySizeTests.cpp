// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/ReplaySize.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>

using tapeloop::FrameSize;
using tapeloop::replayOutputSize;
using tapeloop::ReplayResolution;
using tapeloop::ResolutionMode;

TEST_CASE("the target height follows the resolution setting")
{
	const FrameSize canvas{1920, 1080};
	const FrameSize output{1280, 720};
	CHECK(tapeloop::targetHeight({}, canvas, output) == 1080);
	CHECK(tapeloop::targetHeight({ResolutionMode::Output, 0}, canvas, output) == 720);
	CHECK(tapeloop::targetHeight({ResolutionMode::Fixed, 480}, canvas, output) == 480);
	CHECK(tapeloop::targetHeight({ResolutionMode::Fixed, 2160}, canvas, output) == 2160);
}

TEST_CASE("replay output size scales down to the target and keeps the aspect ratio")
{
	CHECK(replayOutputSize({1920, 1080}, 1080) == FrameSize{1920, 1080});
	CHECK(replayOutputSize({1920, 1080}, 720) == FrameSize{1280, 720});
	CHECK(replayOutputSize({1920, 1080}, 360) == FrameSize{640, 360});
	CHECK(replayOutputSize({3840, 2160}, 1080) == FrameSize{1920, 1080});
	CHECK(replayOutputSize({1080, 1920}, 1080) == FrameSize{608, 1080});
	CHECK(replayOutputSize({1440, 1080}, 480) == FrameSize{640, 480});
}

TEST_CASE("replay output size never upscales")
{
	CHECK(replayOutputSize({1280, 720}, 1080) == FrameSize{1280, 720});
	CHECK(replayOutputSize({640, 480}, 2160) == FrameSize{640, 480});
}

TEST_CASE("replay output size is always even")
{
	CHECK(replayOutputSize({1919, 1079}, 1080) == FrameSize{1916, 1078});
	CHECK(replayOutputSize({1921, 1081}, 1080) == FrameSize{1918, 1080});
	CHECK(replayOutputSize({1920, 1080}, 721) == FrameSize{1280, 720});
	CHECK(replayOutputSize({333, 333}, 1080) == FrameSize{332, 332});

	for (uint32_t width = 2; width < 80; ++width) {
		for (uint32_t height = 2; height < 80; ++height) {
			for (const uint32_t target : {1u, 2u, 3u, 37u, 1080u}) {
				CAPTURE(width, height, target);
				const std::optional<FrameSize> size = replayOutputSize({width, height}, target);
				if (!size) {
					continue;
				}
				CHECK(size->width % 2 == 0);
				CHECK(size->height % 2 == 0);
				CHECK(size->width >= 2);
				CHECK(size->height >= 2);
				CHECK(size->width <= width);
				CHECK(size->height <= height);
				CHECK(size->height <= target);
			}
		}
	}
}

TEST_CASE("replay output size refuses what cannot give 2x2")
{
	CHECK_FALSE(replayOutputSize({0, 0}, 1080));
	CHECK_FALSE(replayOutputSize({1920, 0}, 1080));
	CHECK_FALSE(replayOutputSize({0, 1080}, 1080));
	CHECK_FALSE(replayOutputSize({1, 1}, 1080));
	CHECK_FALSE(replayOutputSize({1920, 1080}, 1));
	CHECK_FALSE(replayOutputSize({1920, 1080}, 0));
	CHECK_FALSE(replayOutputSize({2, 1080}, 360));
	CHECK(replayOutputSize({4000, 2}, 1080) == FrameSize{4000, 2});
}

TEST_CASE("replay output size handles the largest sizes")
{
	const uint32_t largest = uint32_t{1} << 20;
	CHECK(replayOutputSize({largest, largest}, UINT32_MAX) == FrameSize{largest, largest});
	CHECK(replayOutputSize({largest, 3}, 3) == FrameSize{699050, 2});
	CHECK_FALSE(replayOutputSize({largest + 1, 1080}, 1080));
	CHECK_FALSE(replayOutputSize({1920, largest + 1}, 1080));
	CHECK_FALSE(replayOutputSize({UINT32_MAX, UINT32_MAX}, UINT32_MAX));
}
