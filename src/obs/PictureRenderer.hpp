// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "decode/Picture.hpp"

#include <graphics/vec3.h>
#include <graphics/vec4.h>
#include <obs.h>

#include <array>
#include <cstdint>

namespace tapeloop::obs {

// Draws decoded pictures with OBS. A picture in memory is copied into textures of the
// renderer's own, reused while the size and layout stay the same, and converted to RGB
// by an effect of the plugin's own while it is drawn. Everything here runs on the
// graphics thread, inside the graphics context, destruction included.
class PictureRenderer {
public:
	PictureRenderer() = default;
	~PictureRenderer();

	PictureRenderer(const PictureRenderer &) = delete;
	PictureRenderer &operator=(const PictureRenderer &) = delete;
	PictureRenderer(PictureRenderer &&) = delete;
	PictureRenderer &operator=(PictureRenderer &&) = delete;

	// Copies a picture in memory; the picture can be released once this returns. False
	// for a picture on the GPU, or when the effect or a texture cannot be created, which
	// leaves nothing to draw.
	bool upload(const decode::Picture &picture) noexcept;
	// Draws the last picture uploaded, scaled to width by height at the origin of the
	// current matrix. False when there is none.
	bool draw(uint32_t width, uint32_t height) noexcept;
	// Draws NV12 planes held elsewhere, such as the textures of SharedPictures, with the
	// colors of picture, whose planes are not read. False when the effect cannot be made.
	bool drawNv12(gs_texture_t *luma, gs_texture_t *chroma, const decode::Picture &colors, uint32_t width,
		      uint32_t height) noexcept;
	bool hasPicture() const noexcept { return ready_; }
	// Lets go of the picture and every texture; the next upload makes them again.
	void clear() noexcept;

private:
	bool makeEffect() noexcept;
	bool makeTextures(const decode::Picture &picture) noexcept;
	void setColors(const decode::Picture &picture) noexcept;
	void drawPlanes(decode::PixelLayout layout, const std::array<gs_texture_t *, 3> &planes, uint32_t lumaWidth,
			uint32_t width, uint32_t height) noexcept;

	gs_effect_t *effect_ = nullptr;
	// The effect's parameters, looked up once when it is made.
	struct Params {
		gs_eparam_t *luma = nullptr;
		gs_eparam_t *chroma = nullptr;
		gs_eparam_t *chromaV = nullptr;
		std::array<gs_eparam_t *, 3> toRgb{};
		gs_eparam_t *rangeMin = nullptr;
		gs_eparam_t *rangeMax = nullptr;
		gs_eparam_t *chromaShift = nullptr;
	} params_;
	std::array<gs_texture_t *, 3> planes_{};
	decode::PixelLayout layout_ = decode::PixelLayout::I420;
	uint32_t width_ = 0;
	uint32_t height_ = 0;
	decode::Picture colors_;
	// The rows of the YUV to RGB matrix, and the range the YUV values are clamped to.
	std::array<vec4, 3> toRgb_{};
	vec3 rangeMin_{};
	vec3 rangeMax_{};
	bool ready_ = false;
};

} // namespace tapeloop::obs
