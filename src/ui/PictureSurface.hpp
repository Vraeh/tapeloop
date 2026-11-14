// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include <cstdint>

namespace tapeloop::ui {

// Draws the replay player's picture into a native window, scaled to fit with black bars,
// and nothing while the player has no clip. The plugin implements it on libobs; tests
// replace it, so the widget that shows it runs without OBS. Every call comes from the UI
// thread.
class PictureSurface {
public:
	PictureSurface() = default;
	virtual ~PictureSurface() = default;

	PictureSurface(const PictureSurface &) = delete;
	PictureSurface &operator=(const PictureSurface &) = delete;
	PictureSurface(PictureSurface &&) = delete;
	PictureSurface &operator=(PictureSurface &&) = delete;

	// Starts drawing into the window, a native window id as Qt's winId() gives it, whose
	// size is in device pixels. False when it cannot.
	virtual bool attach(uintptr_t window, uint32_t width, uint32_t height) = 0;
	virtual void resize(uint32_t width, uint32_t height) = 0;
	// Disabled, it draws nothing and costs nothing.
	virtual void setEnabled(bool enabled) = 0;
	// Stops drawing into the window, which may then go.
	virtual void detach() noexcept = 0;
};

} // namespace tapeloop::ui
