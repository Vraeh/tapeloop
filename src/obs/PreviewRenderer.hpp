// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/OverlayLayout.hpp"
#include "obs/Composition.hpp"

#include <obs.hpp>

#include <memory>
#include <mutex>
#include <optional>

namespace tapeloop::obs {

// Where the canvas goes in a display, and how large the selection's handles are, in the
// display's own pixels.
struct PreviewFrame {
	float left = 0.0f;
	float top = 0.0f;
	float width = 0.0f;
	float height = 0.0f;
	float handle = 0.0f;
	float line = 1.0f;
};

// A live picture of overlays that are being edited, drawn by OBS into a window: the
// overlays composed as they would go on air, a neutral placeholder where the replay goes,
// and the selected overlay's box with its eight handles. The UI thread only; the drawing
// runs on OBS's graphics thread and reads a copy of what it needs.
class PreviewRenderer {
public:
	PreviewRenderer();
	~PreviewRenderer();

	PreviewRenderer(const PreviewRenderer &) = delete;
	PreviewRenderer &operator=(const PreviewRenderer &) = delete;
	PreviewRenderer(PreviewRenderer &&) = delete;
	PreviewRenderer &operator=(PreviewRenderer &&) = delete;

	// Starts drawing into the window, at its size in pixels, when enabled. False when OBS
	// could not make the display; true when there already is one.
	bool createDisplay(const gs_init_data &window);
	bool hasDisplay() const noexcept { return display_ != nullptr; }
	void destroyDisplay();
	void resize(uint32_t width, uint32_t height);
	// After the window moved to a screen with other colors.
	void updateColorSpace();

	// Draws only while enabled. While it draws, the overlays' sources count as shown,
	// as they do in OBS's own previews.
	void setEnabled(bool enabled);
	bool enabled() const noexcept { return enabled_; }
	bool showing() const noexcept { return showing_; }

	// `selected` indexes the overlays.
	void setOverlays(const OverlaySettings &overlays, std::optional<size_t> selected);
	void setFrame(const PreviewFrame &frame);

	// The composition the display draws, or null.
	obs_source_t *composition() const noexcept;

private:
	struct Shot {
		CanvasSize canvas{0, 0};
		CanvasSize scene{0, 0};
		PreviewFrame frame;
		bool selected = false;
		OverlayBox box;
	};

	void updateShowing();
	static void draw(void *data, uint32_t width, uint32_t height) noexcept;

	OBSSceneAutoRelease placeholder_;
	std::unique_ptr<Composition> composition_;
	obs_display_t *display_ = nullptr;
	bool enabled_ = false;
	bool showing_ = false;
	std::mutex shotMutex_;
	Shot shot_;
};

} // namespace tapeloop::obs
