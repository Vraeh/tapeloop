// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "ui/OverlayPreview.hpp"

#include "ui/Guarded.hpp"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace tapeloop::ui {
namespace {

// In the widget's own pixels: the room around the canvas, the size of a handle, how near
// a handle a press takes it, and how near an edge goes before it snaps.
constexpr double kMargin = 8.0;
constexpr double kHandleSize = 8.0;
constexpr double kHandleReach = 6.0;
constexpr double kSnapReach = 10.0;
constexpr int kMinimumWidth = 320;
constexpr int kMinimumHeight = 180;
constexpr int32_t kNudge = 1;
constexpr int32_t kLongNudge = 10;

int32_t canvasLength(double length, double scale)
{
	return static_cast<int32_t>(std::max(1.0, std::round(length / scale)));
}

// Some platforms report a modifier key's own press or release in the event's modifiers
// and others do not.
Qt::KeyboardModifiers afterKey(Qt::KeyboardModifiers modifiers, int key, bool pressed)
{
	const Qt::KeyboardModifier modifier = key == Qt::Key_Shift ? Qt::ShiftModifier : Qt::ControlModifier;
	return pressed ? modifiers | modifier : modifiers & ~modifier;
}

} // namespace

OverlayPreview::OverlayPreview(DockBackend &backend, OverlayLayout &layout, std::function<void()> changed,
			       QWidget *parent)
	: QWidget(parent),
	  layout_(layout),
	  changed_(std::move(changed)),
	  view_(backend.createOverlayView(this))
{
	setFocusPolicy(Qt::StrongFocus);
	setMinimumSize(kMinimumWidth, kMinimumHeight);
	auto *box = new QVBoxLayout(this);
	box->setContentsMargins(0, 0, 0, 0);
	if (view_) {
		view_->widget()->setFocusPolicy(Qt::NoFocus);
		box->addWidget(view_->widget());
	}
	refresh();
}

void OverlayPreview::refresh()
{
	if (view_) {
		view_->setOverlays(layout_.settings(), layout_.selected());
	}
	showFrame();
}

QRectF OverlayPreview::canvasArea() const
{
	const CanvasSize canvas = layout_.canvas();
	const double roomWidth = width() - 2.0 * kMargin;
	const double roomHeight = height() - 2.0 * kMargin;
	if (canvas.width <= 0 || canvas.height <= 0 || roomWidth < 1.0 || roomHeight < 1.0) {
		return {};
	}
	const double scale = std::min(roomWidth / canvas.width, roomHeight / canvas.height);
	const double areaWidth = canvas.width * scale;
	const double areaHeight = canvas.height * scale;
	return {(width() - areaWidth) / 2.0, (height() - areaHeight) / 2.0, areaWidth, areaHeight};
}

CanvasPoint OverlayPreview::canvasPoint(const QPointF &position) const
{
	const QRectF area = canvasArea();
	const double factor = scale();
	if (area.isEmpty() || factor <= 0.0) {
		return {};
	}
	const auto toCanvas = [factor](double offset) {
		return static_cast<int32_t>(std::clamp(std::floor(offset / factor), -1e9, 1e9));
	};
	return {toCanvas(position.x() - area.left()), toCanvas(position.y() - area.top())};
}

void OverlayPreview::mousePressEvent(QMouseEvent *event)
{
	if (event->button() != Qt::LeftButton || canvasArea().isEmpty()) {
		QWidget::mousePressEvent(event);
		return;
	}
	event->accept();
	guarded([&] {
		pointer_ = event->position();
		const CanvasPoint point = canvasPoint(pointer_);
		if (const std::optional<ResizeHandle> handle =
			    layout_.handleAt(point, canvasLength(kHandleReach, scale()))) {
			layout_.beginResize(*handle, point);
		} else if (const std::optional<size_t> index = layout_.itemAt(point)) {
			layout_.select(index);
			layout_.beginMove(point);
		} else {
			layout_.select(std::nullopt);
		}
		changed_();
	});
}

void OverlayPreview::mouseMoveEvent(QMouseEvent *event)
{
	if (!layout_.dragging() || !event->buttons().testFlag(Qt::LeftButton)) {
		QWidget::mouseMoveEvent(event);
		return;
	}
	event->accept();
	pointer_ = event->position();
	guarded([&] { dragTo(event->modifiers()); });
}

void OverlayPreview::mouseReleaseEvent(QMouseEvent *event)
{
	if (event->button() != Qt::LeftButton || !layout_.dragging()) {
		QWidget::mouseReleaseEvent(event);
		return;
	}
	event->accept();
	layout_.endDrag();
}

void OverlayPreview::keyPressEvent(QKeyEvent *event)
{
	const int key = event->key();
	const Qt::KeyboardModifiers modifiers = event->modifiers() & ~Qt::KeypadModifier;
	if (layout_.dragging()) {
		if (key == Qt::Key_Escape) {
			event->accept();
			guarded([this] {
				layout_.cancelDrag();
				changed_();
			});
			return;
		}
		if (key == Qt::Key_Shift || key == Qt::Key_Control) {
			event->accept();
			guarded([&] { dragTo(afterKey(modifiers, key, true)); });
			return;
		}
		QWidget::keyPressEvent(event);
		return;
	}

	int32_t dx = 0;
	int32_t dy = 0;
	if (key == Qt::Key_Left) {
		dx = -1;
	} else if (key == Qt::Key_Right) {
		dx = 1;
	} else if (key == Qt::Key_Up) {
		dy = -1;
	} else if (key == Qt::Key_Down) {
		dy = 1;
	}
	const bool plain = modifiers == Qt::NoModifier;
	if ((dx == 0 && dy == 0) || !layout_.selected() || (!plain && modifiers != Qt::ShiftModifier)) {
		QWidget::keyPressEvent(event);
		return;
	}
	event->accept();
	const int32_t step = plain ? kNudge : kLongNudge;
	guarded([&] {
		layout_.nudge(dx * step, dy * step);
		changed_();
	});
}

void OverlayPreview::keyReleaseEvent(QKeyEvent *event)
{
	const int key = event->key();
	if (!layout_.dragging() || event->isAutoRepeat() || (key != Qt::Key_Shift && key != Qt::Key_Control)) {
		QWidget::keyReleaseEvent(event);
		return;
	}
	event->accept();
	guarded([&] { dragTo(afterKey(event->modifiers() & ~Qt::KeypadModifier, key, false)); });
}

void OverlayPreview::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	guarded([this] { showFrame(); });
}

void OverlayPreview::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	guarded([this] {
		showFrame();
		if (view_) {
			view_->setActive(true);
		}
	});
}

void OverlayPreview::hideEvent(QHideEvent *event)
{
	QWidget::hideEvent(event);
	guarded([this] {
		if (view_) {
			view_->setActive(false);
		}
	});
}

double OverlayPreview::scale() const
{
	const QRectF area = canvasArea();
	const CanvasSize canvas = layout_.canvas();
	return area.isEmpty() || canvas.width <= 0 ? 0.0 : area.width() / canvas.width;
}

void OverlayPreview::dragTo(Qt::KeyboardModifiers modifiers)
{
	const double factor = scale();
	if (factor <= 0.0) {
		return;
	}
	DragOptions options;
	options.snapDistance = modifiers.testFlag(Qt::ControlModifier) ? 0 : canvasLength(kSnapReach, factor);
	options.keepAspect = modifiers.testFlag(Qt::ShiftModifier);
	layout_.dragTo(canvasPoint(pointer_), options);
	changed_();
}

void OverlayPreview::showFrame()
{
	if (view_) {
		view_->setFrame(canvasArea(), kHandleSize);
	}
}

} // namespace tapeloop::ui
