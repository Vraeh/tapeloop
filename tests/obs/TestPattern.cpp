// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "TestPattern.hpp"

#include <obs-module.h>

#include <algorithm>
#include <atomic>
#include <new>

namespace tapeloop::test {
namespace {

constexpr uint32_t kGrey = 0xff808080;
constexpr uint32_t kWhite = 0xffffffff;
constexpr uint32_t kBlack = 0xff000000;
constexpr uint32_t kBarWidth = 16;
constexpr uint32_t kBarStep = 8;

// The size changes on the thread that updates the settings while the graphics thread
// draws.
struct TestPattern {
	std::atomic<uint32_t> width{0};
	std::atomic<uint32_t> height{0};
	uint32_t frame = 0;
};

// The bar fills the top three quarters, the frame number the bottom quarter.
uint32_t bitsTop(uint32_t height)
{
	return height - height / 4;
}

void fill(gs_eparam_t *color, uint32_t argb, uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
	gs_effect_set_color(color, argb);
	gs_matrix_push();
	gs_matrix_translate3f(static_cast<float>(x), static_cast<float>(y), 0.0f);
	gs_draw_sprite(nullptr, 0, width, height);
	gs_matrix_pop();
}

const char *name(void *) noexcept
{
	return "Tapeloop test pattern";
}

void update(void *data, obs_data_t *settings) noexcept
{
	auto *pattern = static_cast<TestPattern *>(data);
	pattern->width = static_cast<uint32_t>(std::max<long long>(obs_data_get_int(settings, "width"), 0));
	pattern->height = static_cast<uint32_t>(std::max<long long>(obs_data_get_int(settings, "height"), 0));
}

void *create(obs_data_t *settings, obs_source_t *) noexcept
{
	auto *pattern = new (std::nothrow) TestPattern;
	if (pattern)
		update(pattern, settings);
	return pattern;
}

void destroy(void *data) noexcept
{
	delete static_cast<TestPattern *>(data);
}

uint32_t width(void *data) noexcept
{
	return static_cast<TestPattern *>(data)->width;
}

uint32_t height(void *data) noexcept
{
	return static_cast<TestPattern *>(data)->height;
}

// Ticks run once per frame on the graphics thread, before any view renders the frame.
void tick(void *data, float) noexcept
{
	++static_cast<TestPattern *>(data)->frame;
}

void render(void *data, gs_effect_t *) noexcept
{
	const TestPattern &pattern = *static_cast<TestPattern *>(data);
	const uint32_t patternWidth = pattern.width;
	const uint32_t patternHeight = pattern.height;
	const uint32_t top = bitsTop(patternHeight);
	const uint32_t travel = patternWidth > kBarWidth ? patternWidth - kBarWidth : 1;
	const uint32_t barX = pattern.frame * kBarStep % travel;
	const uint32_t cellWidth = patternWidth / kFrameNumberBits;

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color = gs_effect_get_param_by_name(solid, "color");
	while (gs_effect_loop(solid, "Solid")) {
		fill(color, kGrey, 0, 0, patternWidth, patternHeight);
		fill(color, kWhite, barX, 0, std::min(kBarWidth, patternWidth), top);
		for (int bit = 0; bit < kFrameNumberBits; ++bit) {
			const bool set = (pattern.frame >> (kFrameNumberBits - 1 - bit)) & 1;
			fill(color, set ? kWhite : kBlack, static_cast<uint32_t>(bit) * cellWidth, top, cellWidth,
			     patternHeight - top);
		}
	}
}

} // namespace

void registerTestPattern()
{
	obs_source_info info = {};
	info.id = kTestPatternId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	// Custom draw: the pattern runs its own effect instead of being wrapped in the default one.
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	info.get_name = name;
	info.create = create;
	info.destroy = destroy;
	info.update = update;
	info.get_width = width;
	info.get_height = height;
	info.video_tick = tick;
	info.video_render = render;
	obs_register_source(&info);
}

OBSSourceAutoRelease createTestPattern(uint32_t width, uint32_t height)
{
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_int(settings, "width", width);
	obs_data_set_int(settings, "height", height);
	return obs_source_create(kTestPatternId, "pattern", settings, nullptr);
}

std::optional<uint32_t> readFrameNumber(const uint8_t *luma, ptrdiff_t stride, int width, int height)
{
	// Limited range puts black at 16 and white at 235; anything far from both means the
	// cell was not read where it was drawn.
	constexpr uint8_t kBlackBelow = 80;
	constexpr uint8_t kWhiteAbove = 176;

	const int top = static_cast<int>(bitsTop(static_cast<uint32_t>(height)));
	const int row = top + (height - top) / 2;
	const int cellWidth = width / kFrameNumberBits;
	uint32_t number = 0;
	for (int bit = 0; bit < kFrameNumberBits; ++bit) {
		const uint8_t value = luma[row * stride + bit * cellWidth + cellWidth / 2];
		if (value > kWhiteAbove)
			number = (number << 1) | 1;
		else if (value < kBlackBelow)
			number <<= 1;
		else
			return std::nullopt;
	}
	return number;
}

} // namespace tapeloop::test
