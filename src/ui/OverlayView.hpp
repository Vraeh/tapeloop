// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/OverlayLayout.hpp"

#include <cstddef>
#include <optional>

class QRectF;
class QWidget;

namespace tapeloop::ui {

// A live picture of the overlays being edited, in a widget the editor lays out and does
// not paint over: the overlays as they would go on air, a placeholder where the replay
// goes, and the selected overlay's box with its handles. The plugin draws it with OBS;
// tests use a plain widget. It takes no input: the editor handles the mouse and keys.
class OverlayView {
public:
	OverlayView() = default;
	virtual ~OverlayView() = default;

	OverlayView(const OverlayView &) = delete;
	OverlayView &operator=(const OverlayView &) = delete;
	OverlayView(OverlayView &&) = delete;
	OverlayView &operator=(OverlayView &&) = delete;

	// Owned by its Qt parent.
	virtual QWidget *widget() = 0;
	// `selected` indexes the overlays.
	virtual void setOverlays(const OverlaySettings &overlays, std::optional<size_t> selected) = 0;
	// Where the canvas goes in the widget and the size of the handles, in the widget's
	// coordinates.
	virtual void setFrame(const QRectF &canvas, double handle) = 0;
	// Draws only while active, which the editor is while it shows.
	virtual void setActive(bool active) = 0;
};

} // namespace tapeloop::ui
