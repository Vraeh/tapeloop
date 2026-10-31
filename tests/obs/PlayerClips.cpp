// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "PlayerClips.hpp"

#include "ClipDecoder.hpp"
#include "ObsFixture.hpp"
#include "TestPattern.hpp"

#include "obs/SourceCapture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <obs.hpp>

#include <algorithm>
#include <chrono>

using namespace std::chrono_literals;

namespace tapeloop::test {

Clip captureClip(size_t gops)
{
	OBSSourceAutoRelease pattern = createTestPattern(kClipWidth, kClipHeight);
	tapeloop::obs::SourceCapture capture;
	REQUIRE(capture.start(pattern, {}) == tapeloop::obs::StartResult::Started);
	REQUIRE(waitFor([&] { return capture.buffer() && capture.buffer()->stats().gopCount >= gops; }, 60s));
	capture.stop();
	return capture.buffer()->clip(Nanoseconds::min(), Nanoseconds::max());
}

std::vector<ExpectedFrame> expectedFrames(const Clip &clip)
{
	const auto decoded = decodeGops(clip);
	REQUIRE(decoded.size() == clip.gops().size());
	std::vector<ExpectedFrame> frames;
	for (size_t gop = 0; gop < decoded.size(); ++gop) {
		const auto packets = clip.gops()[gop]->packets();
		REQUIRE(decoded[gop].size() == packets.size());
		for (size_t i = 0; i < packets.size(); ++i) {
			REQUIRE(decoded[gop][i].frameNumber);
			frames.push_back({packets[i].time, *decoded[gop][i].frameNumber});
		}
	}
	std::sort(frames.begin(), frames.end(),
		  [](const ExpectedFrame &a, const ExpectedFrame &b) { return a.time < b.time; });
	REQUIRE(frames.size() == clip.frameTimes().size());
	return frames;
}

std::vector<size_t> gopStarts(const Clip &clip, const std::vector<ExpectedFrame> &expected)
{
	std::vector<size_t> starts;
	for (size_t gop = 1; gop < clip.gops().size(); ++gop) {
		const Nanoseconds first = clip.gops()[gop]->packets().front().time;
		const auto at =
			std::lower_bound(expected.begin(), expected.end(), first,
					 [](const ExpectedFrame &frame, Nanoseconds t) { return frame.time < t; });
		starts.push_back(static_cast<size_t>(at - expected.begin()));
	}
	return starts;
}

} // namespace tapeloop::test
