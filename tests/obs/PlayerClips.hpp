// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/Clip.hpp"
#include "core/MediaTime.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tapeloop::test {

// The size of the test pattern the clips are captured from.
inline constexpr uint32_t kClipWidth = 320;
inline constexpr uint32_t kClipHeight = 180;

// A frame of a clip: when it shows, and the number the test pattern drew into it.
struct ExpectedFrame {
	Nanoseconds time{0};
	uint32_t number = 0;
};

// The test pattern as a capture keeps it, once it holds at least gops GOPs.
Clip captureClip(size_t gops);

// What an independent decode of every GOP reads in each frame, in time order, which is
// the order of a player's frame indexes.
std::vector<ExpectedFrame> expectedFrames(const Clip &clip);

// The first frame of each GOP after the first, as frame indexes.
std::vector<size_t> gopStarts(const Clip &clip, const std::vector<ExpectedFrame> &expected);

} // namespace tapeloop::test
