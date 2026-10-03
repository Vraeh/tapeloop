// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace tapeloop::test {

// A video source drawn on the GPU: a white bar that moves one step per frame over a
// grey background, and under it the number of the frame in binary, one black or white
// cell per bit, most significant first, so a decoded frame can be traced back to the
// frame rendered. Settings: "width" and "height".
inline constexpr const char *kTestPatternId = "tapeloop_test_pattern";
inline constexpr int kFrameNumberBits = 16;

void registerTestPattern();

// The frame number drawn into a decoded frame, read from its luma plane. Empty when a
// cell is neither clearly black nor clearly white.
std::optional<uint32_t> readFrameNumber(const uint8_t *luma, ptrdiff_t stride, int width, int height);

} // namespace tapeloop::test
