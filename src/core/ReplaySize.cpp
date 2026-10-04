// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/ReplaySize.hpp"

#include <algorithm>

namespace tapeloop {
namespace {

// Far above any texture OBS can create, so only the arithmetic below needs it: it keeps
// the products within 64 bits.
constexpr uint32_t kMaxDimension = uint32_t{1} << 20;

uint32_t evenFloor(uint64_t value)
{
	return static_cast<uint32_t>(value & ~uint64_t{1});
}

} // namespace

uint32_t targetHeight(ReplayResolution resolution, FrameSize canvas, FrameSize output)
{
	switch (resolution.mode) {
	case ResolutionMode::Output:
		return output.height;
	case ResolutionMode::Fixed:
		return resolution.height;
	case ResolutionMode::Canvas:
		break;
	}
	return canvas.height;
}

std::optional<FrameSize> replayOutputSize(FrameSize source, uint32_t targetHeight)
{
	if (source.width == 0 || source.height == 0 || source.width > kMaxDimension || source.height > kMaxDimension) {
		return std::nullopt;
	}

	const uint32_t height = evenFloor(std::min(source.height, targetHeight));
	// Rounded to the nearest pixel before rounding down to even.
	const uint64_t scaled = (uint64_t{source.width} * height * 2 + source.height) / (uint64_t{source.height} * 2);
	const uint32_t width = evenFloor(std::min<uint64_t>(scaled, source.width));
	if (width < 2 || height < 2) {
		return std::nullopt;
	}
	return FrameSize{width, height};
}

} // namespace tapeloop
