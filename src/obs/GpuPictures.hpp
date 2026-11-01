// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "decode/Picture.hpp"
#include "obs/PictureRenderer.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace tapeloop::obs {

// Where the replay player puts pictures a decoder keeps on the GPU, which it cannot copy
// into memory of its own, for render() to draw: SharedPictures on Direct3D 11. publish()
// runs on the decoding thread; everything else on the graphics thread, inside the
// graphics context.
class GpuPictures {
public:
	// What a picture was published as, given back while it is the one on screen.
	struct Tag {
		uint64_t clip = 0;
		size_t frame = 0;
		uint32_t width = 0;
		uint32_t height = 0;
	};

	GpuPictures() = default;
	virtual ~GpuPictures() = default;

	GpuPictures(const GpuPictures &) = delete;
	GpuPictures &operator=(const GpuPictures &) = delete;
	GpuPictures(GpuPictures &&) = delete;
	GpuPictures &operator=(GpuPictures &&) = delete;

	// Makes room for pictures of this size, if there is none yet. False when pictures of
	// this size cannot be taken.
	virtual bool prepare(uint32_t width, uint32_t height) noexcept = 0;
	// Copies a picture on the GPU, of the prepared size, to where it can be drawn from.
	// False when nothing was copied; the picture stays the decoder's either way.
	virtual bool publish(const decode::Picture &picture, Tag tag) noexcept = 0;
	// Switches to the newest picture published, if any, and gives the tag of the one on
	// screen after that; empty when there is none.
	virtual std::optional<Tag> latch() noexcept = 0;
	// Draws the picture on screen. False when there is none.
	virtual bool draw(PictureRenderer &renderer, uint32_t width, uint32_t height) noexcept = 0;
	// Changes whenever the pictures published before are gone, as when the room for
	// them is made again or the device they were on is rebuilt.
	virtual uint64_t generation() const noexcept = 0;
};

} // namespace tapeloop::obs
