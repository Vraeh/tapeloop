// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/OverlayLayout.hpp"
#include "ui/DockBackend.hpp"
#include "ui/OverlayView.hpp"

#include <QPointF>
#include <QRectF>
#include <QWidget>

#include <functional>

namespace tapeloop::ui {

// The overlay editor's live picture of the copy, and the mouse and keys that edit it. A
// click selects the topmost overlay under it, and a drag moves it or, from one of its
// handles, resizes it. A drag snaps to the canvas unless Ctrl is held, and a resize keeps
// the proportions while Shift is held. Escape puts back what a drag changed. The arrows
// move the selected overlay a pixel, or ten with Shift.
class OverlayPreview : public QWidget {
public:
	// `changed` runs after every change made here to the copy or its selection.
	OverlayPreview(DockBackend &backend, OverlayLayout &layout, std::function<void()> changed,
		       QWidget *parent = nullptr);

	// Shows the copy again, after it changed elsewhere.
	void refresh();
	// Null when the backend has no picture to draw.
	OverlayView *view() const noexcept { return view_; }
	// Where the canvas is drawn, at its own aspect and centered, in this widget's
	// coordinates. Empty when there is no room or no canvas.
	QRectF canvasArea() const;
	// The canvas pixel under a point of this widget, which may lie outside the canvas.
	CanvasPoint canvasPoint(const QPointF &position) const;

protected:
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	void keyPressEvent(QKeyEvent *event) override;
	void keyReleaseEvent(QKeyEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;

private:
	double scale() const;
	void dragTo(Qt::KeyboardModifiers modifiers);
	void showFrame();

	OverlayLayout &layout_;
	std::function<void()> changed_;
	OverlayView *view_;
	QPointF pointer_;
};

} // namespace tapeloop::ui
