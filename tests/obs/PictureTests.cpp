// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "ObsFixture.hpp"
#include "TestPattern.hpp"

#include "decode/Picture.hpp"
#include "obs/PictureRenderer.hpp"
#include "obs/RenderAdapter.hpp"

#include <catch2/catch_test_macros.hpp>
#include <obs.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

using tapeloop::decode::ColorMatrix;
using tapeloop::decode::Picture;
using tapeloop::decode::PixelLayout;
using tapeloop::obs::PictureRenderer;
using tapeloop::test::ObsFixture;

namespace {

constexpr uint32_t kWidth = 320;
constexpr uint32_t kHeight = 180;

// The test pattern's frame as a decoder would hand it over: a grey picture, the bar at
// the left edge (frame 0 of its travel) and the number's cells along the bottom quarter.
struct PatternPicture {
	std::vector<uint8_t> luma;
	std::vector<uint8_t> chroma;
	std::vector<uint8_t> chromaV;
	Picture picture;
};

PatternPicture makePattern(uint32_t number, PixelLayout layout, bool fullRange)
{
	const uint8_t grey = 126;
	const uint8_t white = fullRange ? 255 : 235;
	const uint8_t black = fullRange ? 0 : 16;
	PatternPicture pattern;
	pattern.luma.assign(kWidth * kHeight, grey);
	const uint32_t top = kHeight - kHeight / 4;
	const uint32_t cell = kWidth / tapeloop::test::kFrameNumberBits;
	for (uint32_t y = 0; y < kHeight; ++y) {
		uint8_t *row = pattern.luma.data() + y * kWidth;
		if (y < top) {
			std::memset(row, white, 16);
			continue;
		}
		for (uint32_t bit = 0; bit < tapeloop::test::kFrameNumberBits; ++bit) {
			const bool set = (number >> (tapeloop::test::kFrameNumberBits - 1 - bit)) & 1;
			std::memset(row + bit * cell, set ? white : black, cell);
		}
	}

	const uint32_t chromaWidth = kWidth / 2;
	const uint32_t chromaHeight = kHeight / 2;
	Picture &picture = pattern.picture;
	picture.width = kWidth;
	picture.height = kHeight;
	picture.fullRange = fullRange;
	picture.matrix = ColorMatrix::Bt709;
	picture.layout = layout;
	picture.planes[0] = pattern.luma.data();
	picture.strides[0] = kWidth;
	if (layout == PixelLayout::Nv12) {
		pattern.chroma.assign(chromaWidth * 2 * chromaHeight, 128);
		picture.planes[1] = pattern.chroma.data();
		picture.strides[1] = chromaWidth * 2;
	} else {
		pattern.chroma.assign(chromaWidth * chromaHeight, 128);
		pattern.chromaV.assign(chromaWidth * chromaHeight, 128);
		picture.planes[1] = pattern.chroma.data();
		picture.planes[2] = pattern.chromaV.data();
		picture.strides[1] = chromaWidth;
		picture.strides[2] = chromaWidth;
	}
	return pattern;
}

// What the renderer draws at the picture's own size, as RGBA rows without padding.
std::vector<uint8_t> drawAndRead(PictureRenderer &renderer)
{
	gs_texrender_t *target = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	REQUIRE(target);
	REQUIRE(gs_texrender_begin(target, kWidth, kHeight));
	vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, static_cast<float>(kWidth), 0.0f, static_cast<float>(kHeight), -100.0f, 100.0f);
	const bool drawn = renderer.draw(kWidth, kHeight);
	gs_texrender_end(target);

	std::vector<uint8_t> pixels;
	gs_stagesurf_t *stage = gs_stagesurface_create(kWidth, kHeight, GS_RGBA);
	gs_stage_texture(stage, gs_texrender_get_texture(target));
	uint8_t *data = nullptr;
	uint32_t stride = 0;
	if (drawn && gs_stagesurface_map(stage, &data, &stride)) {
		pixels.resize(size_t{kWidth} * kHeight * 4);
		for (uint32_t y = 0; y < kHeight; ++y) {
			std::memcpy(pixels.data() + size_t{y} * kWidth * 4, data + size_t{y} * stride,
				    size_t{kWidth} * 4);
		}
		gs_stagesurface_unmap(stage);
	}
	gs_stagesurface_destroy(stage);
	gs_texrender_destroy(target);
	return pixels;
}

uint8_t red(const std::vector<uint8_t> &pixels, uint32_t x, uint32_t y)
{
	return pixels[(size_t{y} * kWidth + x) * 4];
}

std::optional<uint32_t> numberIn(const std::vector<uint8_t> &pixels)
{
	std::vector<uint8_t> reds(size_t{kWidth} * kHeight);
	for (size_t i = 0; i < reds.size(); ++i) {
		reds[i] = pixels[i * 4];
	}
	return tapeloop::test::readFrameNumber(reds.data(), kWidth, static_cast<int>(kWidth),
					       static_cast<int>(kHeight));
}

} // namespace

TEST_CASE_METHOD(ObsFixture, "the picture renderer draws both layouts and both ranges in RGB", "[obs][picture]")
{
	obs_enter_graphics();
	{
		PictureRenderer renderer;
		CHECK_FALSE(renderer.hasPicture());
		CHECK(drawAndRead(renderer).empty());

		for (const PixelLayout layout : {PixelLayout::I420, PixelLayout::Nv12}) {
			for (const bool fullRange : {false, true}) {
				CAPTURE(layout == PixelLayout::Nv12, fullRange);
				const uint32_t number = fullRange ? 0x5a5a : 0x0f0f;
				const PatternPicture pattern = makePattern(number, layout, fullRange);
				REQUIRE(renderer.upload(pattern.picture));
				CHECK(renderer.hasPicture());
				const std::vector<uint8_t> pixels = drawAndRead(renderer);
				REQUIRE(pixels.size() == size_t{kWidth} * kHeight * 4);
				CHECK(numberIn(pixels) == number);

				// Limited range stretches 16 to 235 over 0 to 255; full range keeps the
				// values. Grey is 126 either way.
				const int grey = red(pixels, kWidth / 2, 10);
				const int bar = red(pixels, 8, 10);
				CHECK(grey >= (fullRange ? 123 : 125));
				CHECK(grey <= (fullRange ? 129 : 131));
				CHECK(bar >= 250);
				// The first bit of both numbers is 0, a black cell.
				const int black = red(pixels, 2, kHeight - 4);
				CHECK(black <= 4);
			}
		}

		renderer.clear();
		CHECK_FALSE(renderer.hasPicture());
		CHECK(drawAndRead(renderer).empty());
	}
	obs_leave_graphics();
}

TEST_CASE_METHOD(ObsFixture, "the picture renderer takes no picture it cannot draw", "[obs][picture]")
{
	obs_enter_graphics();
	{
		PictureRenderer renderer;
		PatternPicture pattern = makePattern(1, PixelLayout::I420, false);
		REQUIRE(renderer.upload(pattern.picture));

		Picture onGpu = pattern.picture;
		int texture = 0;
		onGpu.texture = &texture;
		CHECK_FALSE(renderer.upload(onGpu));
		CHECK_FALSE(renderer.hasPicture());

		Picture empty;
		CHECK_FALSE(renderer.upload(empty));

		// A picture of the other layout after one that could not be taken.
		PatternPicture next = makePattern(2, PixelLayout::Nv12, false);
		REQUIRE(renderer.upload(next.picture));
		CHECK(numberIn(drawAndRead(renderer)) == 2u);
	}
	obs_leave_graphics();
}

TEST_CASE_METHOD(ObsFixture, "the picture renderer draws NV12 planes held elsewhere", "[obs][picture]")
{
	obs_enter_graphics();
	{
		const PatternPicture limited = makePattern(0x1234, PixelLayout::Nv12, false);
		gs_texture_t *luma = gs_texture_create(kWidth, kHeight, GS_R8, 1, nullptr, GS_DYNAMIC);
		gs_texture_t *chroma = gs_texture_create(kWidth / 2, kHeight / 2, GS_R8G8, 1, nullptr, GS_DYNAMIC);
		REQUIRE(luma);
		REQUIRE(chroma);
		gs_texture_set_image(luma, limited.picture.planes[0], limited.picture.strides[0], false);
		gs_texture_set_image(chroma, limited.picture.planes[1], limited.picture.strides[1], false);

		PictureRenderer renderer;
		const PatternPicture full = makePattern(0x0042, PixelLayout::I420, true);
		REQUIRE(renderer.upload(full.picture));

		gs_texrender_t *target = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		REQUIRE(gs_texrender_begin(target, kWidth, kHeight));
		gs_ortho(0.0f, static_cast<float>(kWidth), 0.0f, static_cast<float>(kHeight), -100.0f, 100.0f);
		CHECK(renderer.drawNv12(luma, chroma, limited.picture, kWidth, kHeight));
		gs_texrender_end(target);
		gs_stagesurf_t *stage = gs_stagesurface_create(kWidth, kHeight, GS_RGBA);
		gs_stage_texture(stage, gs_texrender_get_texture(target));
		uint8_t *data = nullptr;
		uint32_t stride = 0;
		REQUIRE(gs_stagesurface_map(stage, &data, &stride));
		std::vector<uint8_t> pixels(size_t{kWidth} * kHeight * 4);
		for (uint32_t y = 0; y < kHeight; ++y) {
			std::memcpy(pixels.data() + size_t{y} * kWidth * 4, data + size_t{y} * stride,
				    size_t{kWidth} * 4);
		}
		gs_stagesurface_unmap(stage);
		gs_stagesurface_destroy(stage);
		gs_texrender_destroy(target);
		CHECK(numberIn(pixels) == 0x1234u);
		CHECK(red(pixels, 8, 10) >= 250);

		// The uploaded picture keeps its own colors: full range leaves grey at 126.
		const std::vector<uint8_t> uploaded = drawAndRead(renderer);
		CHECK(numberIn(uploaded) == 0x0042u);
		CHECK(red(uploaded, kWidth / 2, 10) <= 129);

		CHECK_FALSE(renderer.drawNv12(nullptr, chroma, limited.picture, kWidth, kHeight));
		gs_texture_destroy(chroma);
		gs_texture_destroy(luma);
	}
	obs_leave_graphics();
}

TEST_CASE_METHOD(ObsFixture, "only Direct3D 11 names an adapter for the decoder", "[obs][picture]")
{
	obs_enter_graphics();
	CHECK_FALSE(tapeloop::obs::renderAdapterLuid());
	obs_leave_graphics();
}
