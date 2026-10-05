// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include <array>
#include <cstdint>

namespace tapeloop::decode {

// 8-bit 4:2:0, the only formats the capture produces.
enum class PixelLayout { I420, Nv12 };

enum class ColorMatrix { Unspecified, Bt601, Bt709 };

// Where a decoded picture is and how to read it, without the decoder's own types. A
// picture is either in memory, with its planes, or on the GPU, as an NV12 texture.
struct Picture {
	uint32_t width = 0;
	uint32_t height = 0;
	bool fullRange = false;
	ColorMatrix matrix = ColorMatrix::Unspecified;

	PixelLayout layout = PixelLayout::I420;
	// In memory: Y, U and V for I420; Y and the interleaved UV for NV12.
	std::array<const uint8_t *, 3> planes{};
	std::array<uint32_t, 3> strides{};

	// On the GPU (Windows): an ID3D11Texture2D of the decoder's device, in NV12, and the
	// subresource that holds the picture. Null for a picture in memory.
	void *texture = nullptr;
	uint32_t subresource = 0;
};

} // namespace tapeloop::decode
