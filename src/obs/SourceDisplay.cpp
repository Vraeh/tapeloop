// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "obs/SourceDisplay.hpp"

#if !defined(_WIN32) && !defined(__APPLE__)
#include <obs-nix-platform.h>
#endif

namespace tapeloop::obs {

namespace {

constexpr uint32_t kBlack = 0xFF000000;

} // namespace

SourceDisplay::SourceDisplay(obs_source_t *source) : source_(obs_source_get_weak_source(source)) {}

SourceDisplay::~SourceDisplay()
{
	detach();
	obs_weak_source_release(source_);
}

bool SourceDisplay::attach(uintptr_t window, uint32_t width, uint32_t height)
{
	detach();
	gs_init_data info = {};
	info.cx = width;
	info.cy = height;
	info.format = GS_BGRA;
	info.zsformat = GS_ZS_NONE;
#if defined(_WIN32)
	info.window.hwnd = reinterpret_cast<void *>(window);
#elif defined(__APPLE__)
	info.window.view = reinterpret_cast<id>(window);
#else
	if (obs_get_nix_platform() == OBS_NIX_PLATFORM_WAYLAND) {
		return false;
	}
	info.window.id = static_cast<uint32_t>(window);
	info.window.display = obs_get_nix_platform_display();
#endif
	display_ = obs_display_create(&info, kBlack);
	if (!display_) {
		return false;
	}
	obs_display_add_draw_callback(display_, draw, this);
	return true;
}

void SourceDisplay::resize(uint32_t width, uint32_t height)
{
	obs_display_resize(display_, width, height);
}

void SourceDisplay::setEnabled(bool enabled)
{
	obs_display_set_enabled(display_, enabled);
}

// Once the callback is removed, which waits for a draw in progress, nothing reaches this
// object from the graphics thread.
void SourceDisplay::detach() noexcept
{
	if (!display_) {
		return;
	}
	obs_display_remove_draw_callback(display_, draw, this);
	obs_display_destroy(display_);
	display_ = nullptr;
}

void SourceDisplay::draw(void *data, uint32_t width, uint32_t height) noexcept
{
	obs_source_t *source = obs_weak_source_get_source(static_cast<SourceDisplay *>(data)->source_);
	if (!source) {
		return;
	}
	const uint32_t sourceWidth = obs_source_get_width(source);
	const uint32_t sourceHeight = obs_source_get_height(source);
	// As OBS's own previews do, so that sources which can draw in linear light do.
	const bool linear = gs_set_linear_srgb(true);
	if (sourceWidth == 0 || sourceHeight == 0) {
		// A replay source reports the size of the picture it drew last, so it has none
		// until it draws one. Drawn with every pixel clipped, it takes its picture in
		// unseen and is fitted from the next frame.
		const gs_rect none = {};
		gs_set_scissor_rect(&none);
		obs_source_video_render(source);
		gs_set_scissor_rect(nullptr);
	} else {
		uint32_t fitWidth = width;
		uint32_t fitHeight = height;
		if (uint64_t{width} * sourceHeight < uint64_t{height} * sourceWidth) {
			fitHeight = static_cast<uint32_t>(uint64_t{width} * sourceHeight / sourceWidth);
		} else {
			fitWidth = static_cast<uint32_t>(uint64_t{height} * sourceWidth / sourceHeight);
		}
		if (fitWidth != 0 && fitHeight != 0) {
			gs_viewport_push();
			gs_projection_push();
			gs_ortho(0.0f, static_cast<float>(sourceWidth), 0.0f, static_cast<float>(sourceHeight), -100.0f,
				 100.0f);
			gs_set_viewport(static_cast<int>((width - fitWidth) / 2),
					static_cast<int>((height - fitHeight) / 2), static_cast<int>(fitWidth),
					static_cast<int>(fitHeight));
			obs_source_video_render(source);
			gs_projection_pop();
			gs_viewport_pop();
		}
	}
	gs_set_linear_srgb(linear);
	// A release that destroys the source hands the destruction to libobs's own thread.
	obs_source_release(source);
}

} // namespace tapeloop::obs
