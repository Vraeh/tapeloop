// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "ui/PictureSurface.hpp"

#include <obs.h>

namespace tapeloop::obs {

// Draws a source into a window of the UI through an obs_display of its own: once per
// frame on the graphics thread while enabled, scaled to fit with black bars, and nothing
// that shows while the source has no size. It holds the source weakly, so it draws
// nothing once the source is gone. Not on Wayland, where a window id is not enough to
// draw into.
class SourceDisplay final : public ui::PictureSurface {
public:
	explicit SourceDisplay(obs_source_t *source);
	~SourceDisplay() override;

	SourceDisplay(const SourceDisplay &) = delete;
	SourceDisplay &operator=(const SourceDisplay &) = delete;
	SourceDisplay(SourceDisplay &&) = delete;
	SourceDisplay &operator=(SourceDisplay &&) = delete;

	bool attach(uintptr_t window, uint32_t width, uint32_t height) override;
	void resize(uint32_t width, uint32_t height) override;
	void setEnabled(bool enabled) override;
	void detach() noexcept override;

private:
	static void draw(void *data, uint32_t width, uint32_t height) noexcept;

	obs_weak_source_t *source_;
	obs_display_t *display_ = nullptr;
};

} // namespace tapeloop::obs
