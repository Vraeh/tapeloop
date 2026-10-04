// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include <cstdint>
#include <optional>

namespace tapeloop {

struct FrameSize {
	uint32_t width = 0;
	uint32_t height = 0;

	bool operator==(const FrameSize &) const = default;
};

// Which height a replay is encoded at: the canvas (base) height, the scaled output
// height, or a fixed height.
enum class ResolutionMode { Canvas, Output, Fixed };

struct ReplayResolution {
	ResolutionMode mode = ResolutionMode::Canvas;
	// Only for ResolutionMode::Fixed.
	uint32_t height = 1080;

	bool operator==(const ReplayResolution &) const = default;
};

uint32_t targetHeight(ReplayResolution resolution, FrameSize canvas, FrameSize output);

// The size a source is encoded at in its buffer: the target height, never above the
// source's own, and the width that keeps the source's aspect ratio, both rounded down
// to even numbers as NV12 needs. Empty when that leaves less than 2x2, or when the
// source is larger than any video OBS can render.
std::optional<FrameSize> replayOutputSize(FrameSize source, uint32_t targetHeight);

} // namespace tapeloop
