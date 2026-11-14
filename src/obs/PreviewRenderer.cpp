// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "obs/PreviewRenderer.hpp"

#include <graphics/graphics.h>
#include <graphics/vec4.h>

#include <cmath>

namespace tapeloop::obs {
namespace {

constexpr const char *kPlaceholderName = "Tapeloop preview replay";
// Colors as 0xAABBGGRR. Around the canvas, where the replay goes, and the selection.
constexpr uint32_t kBackgroundColor = 0xFF202020;
constexpr uint32_t kPlaceholderColor = 0xFF5A5A5A;
constexpr uint32_t kSelectionColor = 0xFF0000FF;

void fill(gs_effect_t *solid, gs_eparam_t *color, uint32_t rgba, float left, float top, float width, float height)
{
	vec4 value = {};
	vec4_from_rgba(&value, rgba);
	gs_effect_set_vec4(color, &value);
	gs_matrix_push();
	gs_matrix_translate3f(left, top, 0.0f);
	gs_matrix_scale3f(width, height, 1.0f);
	while (gs_effect_loop(solid, "Solid")) {
		gs_draw_sprite(nullptr, 0, 1, 1);
	}
	gs_matrix_pop();
}

} // namespace

PreviewRenderer::PreviewRenderer()
	: placeholder_(obs_scene_create_private(kPlaceholderName)),
	  composition_(std::make_unique<Composition>(placeholder_ ? obs_scene_get_source(placeholder_) : nullptr))
{
}

PreviewRenderer::~PreviewRenderer()
{
	destroyDisplay();
	composition_.reset();
}

bool PreviewRenderer::createDisplay(const gs_init_data &window)
{
	if (display_) {
		return true;
	}
	display_ = obs_display_create(&window, kBackgroundColor);
	if (!display_) {
		return false;
	}
	obs_display_set_enabled(display_, enabled_);
	obs_display_add_draw_callback(display_, draw, this);
	updateShowing();
	return true;
}

void PreviewRenderer::destroyDisplay()
{
	if (!display_) {
		return;
	}
	// Once removed, the callback is not running and does not run again.
	obs_display_remove_draw_callback(display_, draw, this);
	obs_display_destroy(display_);
	display_ = nullptr;
	updateShowing();
}

void PreviewRenderer::resize(uint32_t width, uint32_t height)
{
	if (display_) {
		obs_display_resize(display_, width, height);
	}
}

void PreviewRenderer::updateColorSpace()
{
	if (display_) {
		obs_display_update_color_space(display_);
	}
}

void PreviewRenderer::setEnabled(bool enabled)
{
	enabled_ = enabled;
	if (display_) {
		obs_display_set_enabled(display_, enabled);
	}
	updateShowing();
}

void PreviewRenderer::setOverlays(const OverlaySettings &overlays, std::optional<size_t> selected)
{
	composition_->setOverlays(overlays);
	composition_->apply();

	const bool boxed = selected && *selected < overlays.items.size() && overlays.items[*selected].visible;
	const OverlayBox box = boxed ? boxOf(overlays.items[*selected], overlays.canvas) : OverlayBox{};
	const CanvasSize scene = mainCanvas();
	const std::lock_guard lock(shotMutex_);
	shot_.canvas = overlays.canvas;
	shot_.scene = scene;
	shot_.selected = boxed;
	shot_.box = box;
}

void PreviewRenderer::setFrame(const PreviewFrame &frame)
{
	const std::lock_guard lock(shotMutex_);
	shot_.frame = frame;
}

obs_source_t *PreviewRenderer::composition() const noexcept
{
	return composition_->source();
}

void PreviewRenderer::updateShowing()
{
	obs_source_t *source = composition_->source();
	const bool show = enabled_ && display_ && source;
	if (show == showing_) {
		return;
	}
	if (show) {
		obs_source_inc_showing(source);
	} else {
		obs_source_dec_showing(source);
	}
	showing_ = show;
}

void PreviewRenderer::draw(void *data, uint32_t, uint32_t) noexcept
{
	auto &self = *static_cast<PreviewRenderer *>(data);
	Shot shot;
	{
		const std::lock_guard lock(self.shotMutex_);
		shot = self.shot_;
	}
	const PreviewFrame &frame = shot.frame;
	if (frame.width < 1.0f || frame.height < 1.0f || shot.canvas.width <= 0 || shot.canvas.height <= 0 ||
	    shot.scene.width <= 0 || shot.scene.height <= 0) {
		return;
	}
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color = gs_effect_get_param_by_name(solid, "color");
	const auto sceneWidth = static_cast<float>(shot.scene.width);
	const auto sceneHeight = static_cast<float>(shot.scene.height);

	gs_viewport_push();
	gs_projection_push();
	gs_set_viewport(static_cast<int>(std::lround(frame.left)), static_cast<int>(std::lround(frame.top)),
			static_cast<int>(std::lround(frame.width)), static_cast<int>(std::lround(frame.height)));
	gs_ortho(0.0f, sceneWidth, 0.0f, sceneHeight, -100.0f, 100.0f);
	fill(solid, color, kPlaceholderColor, 0.0f, 0.0f, sceneWidth, sceneHeight);
	if (obs_source_t *scene = self.composition_->source()) {
		obs_source_video_render(scene);
	}
	gs_projection_pop();
	gs_viewport_pop();

	if (!shot.selected) {
		return;
	}
	// Drawn over the picture in the display's pixels, so lines and handles keep their
	// size whatever the scale.
	const float scaleX = frame.width / static_cast<float>(shot.canvas.width);
	const float scaleY = frame.height / static_cast<float>(shot.canvas.height);
	const float left = frame.left + static_cast<float>(shot.box.left) * scaleX;
	const float top = frame.top + static_cast<float>(shot.box.top) * scaleY;
	const float width = static_cast<float>(shot.box.width) * scaleX;
	const float height = static_cast<float>(shot.box.height) * scaleY;
	const float line = frame.line;
	fill(solid, color, kSelectionColor, left, top, width, line);
	fill(solid, color, kSelectionColor, left, top + height - line, width, line);
	fill(solid, color, kSelectionColor, left, top, line, height);
	fill(solid, color, kSelectionColor, left + width - line, top, line, height);
	const float half = frame.handle / 2.0f;
	for (int column = 0; column < 3; ++column) {
		for (int row = 0; row < 3; ++row) {
			if (column == 1 && row == 1) {
				continue;
			}
			const float x = left + width * static_cast<float>(column) / 2.0f;
			const float y = top + height * static_cast<float>(row) / 2.0f;
			fill(solid, color, kSelectionColor, x - half, y - half, frame.handle, frame.handle);
		}
	}
}

} // namespace tapeloop::obs
