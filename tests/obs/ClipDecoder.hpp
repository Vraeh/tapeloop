// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/Clip.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace tapeloop::test {

struct DecodedFrame {
	int64_t pts = 0;
	int width = 0;
	int height = 0;
	// The number TestPattern drew into the frame, if it could be read.
	std::optional<uint32_t> frameNumber;
};

// Decodes every H.264 GOP of the clip on a fresh libavcodec decoder, opened with the
// codec configuration that GOP carries and nothing else, as a player starting at that
// GOP would: parameter sets the encoder repeats in the stream are taken out first. One
// list of frames per GOP, in output order. Throws std::runtime_error when libavcodec
// reports an error or hands back a frame it had to conceal damage in.
std::vector<std::vector<DecodedFrame>> decodeGops(const Clip &clip);

} // namespace tapeloop::test
