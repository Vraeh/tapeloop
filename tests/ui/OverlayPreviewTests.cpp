// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "FakeBackend.hpp"
#include "LocaleText.hpp"

#include "ui/OverlayDialog.hpp"
#include "ui/OverlayPreview.hpp"

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QDialogButtonBox>
#include <QKeyEvent>
#include <QListWidget>
#include <QMouseEvent>
#include <QPushButton>
#include <QSpinBox>

#include <iterator>
#include <optional>
#include <utility>

using tapeloop::CanvasPoint;
using tapeloop::OverlayBox;
using tapeloop::OverlayItem;
using tapeloop::OverlayLayout;
using tapeloop::test::FakeBackend;
using tapeloop::test::localeText;
using tapeloop::ui::OverlayDialog;
using tapeloop::ui::OverlayPreview;

namespace {

template<typename Widget> Widget *child(const QWidget &parent, const char *name)
{
	auto *widget = parent.findChild<Widget *>(name);
	REQUIRE(widget);
	return widget;
}

// Score along the bottom, Logo on the right and Clock on top at the top left, apart and
// far enough from every line they snap to that only a drag toward one snaps.
void placeOverlays(FakeBackend &backend)
{
	backend.overlayList = {{"uuid-clock", "Clock", false, 300, 100},
			       {"uuid-logo", "Logo", false, 400, 200},
			       {"uuid-score", "Score", false, 800, 120}};
	const OverlayBox boxes[] = {{100, 900, 800, 120}, {1300, 250, 400, 200}, {100, 100, 300, 100}};
	backend.current.overlays.items.clear();
	const OverlayBox *box = std::begin(boxes);
	for (auto it = backend.overlayList.rbegin(); it != backend.overlayList.rend(); ++it, ++box) {
		OverlayItem item = tapeloop::newOverlay(it->uuid, it->name, it->width, it->height, backend.canvasSize);
		tapeloop::placeBox(item, *box, backend.canvasSize);
		backend.current.overlays.items.push_back(std::move(item));
	}
}

constexpr size_t kLogo = 1;

OverlayBox boxOf(const OverlayDialog &dialog, size_t index)
{
	return tapeloop::boxOf(dialog.editing().items()[index], dialog.editing().canvas());
}

OverlayPreview *shownPreview(OverlayDialog &dialog)
{
	dialog.show();
	QApplication::processEvents();
	OverlayPreview *preview = dialog.preview();
	REQUIRE(preview);
	REQUIRE_FALSE(preview->canvasArea().isEmpty());
	return preview;
}

// The preview's pixel over a canvas point. A mouse event the picture passes on to the
// preview keeps only whole pixels, so at the scale of the editor the preview sees a
// canvas point a few pixels away: seen() says which.
QPoint at(const OverlayDialog &dialog, int x, int y)
{
	const QRectF area = dialog.preview()->canvasArea();
	const double scale = area.width() / dialog.editing().canvas().width;
	return (area.topLeft() + QPointF((x + 0.5) * scale, (y + 0.5) * scale)).toPoint();
}

CanvasPoint seen(const OverlayDialog &dialog, QPoint position)
{
	return dialog.preview()->canvasPoint(position);
}

// How far the canvas moves under a drag between two of the preview's pixels.
CanvasPoint shift(const OverlayDialog &dialog, QPoint from, QPoint to)
{
	const CanvasPoint start = seen(dialog, from);
	const CanvasPoint end = seen(dialog, to);
	return {end.x - start.x, end.y - start.y};
}

// Sent to the picture when there is one, as a real click is, so it reaches the preview
// only through the picture passing it on.
void mouse(OverlayPreview &preview, QEvent::Type type, QPoint position,
	   Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
	QWidget *target = preview.view() ? preview.view()->widget() : &preview;
	const Qt::MouseButton button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
	const Qt::MouseButtons buttons = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
	QMouseEvent event(type, position, target->mapToGlobal(position), button, buttons, modifiers);
	QApplication::sendEvent(target, &event);
}

void click(OverlayPreview &preview, QPoint position)
{
	mouse(preview, QEvent::MouseButtonPress, position);
	mouse(preview, QEvent::MouseButtonRelease, position);
}

void drag(OverlayPreview &preview, QPoint from, QPoint to, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
	mouse(preview, QEvent::MouseButtonPress, from, modifiers);
	mouse(preview, QEvent::MouseMove, to, modifiers);
	mouse(preview, QEvent::MouseButtonRelease, to, modifiers);
}

void key(QWidget *widget, QEvent::Type type, int code, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
	QKeyEvent event(type, code, modifiers);
	QApplication::sendEvent(widget, &event);
}

void checkFields(const OverlayDialog &dialog, const OverlayBox &box)
{
	CHECK(child<QSpinBox>(dialog, "overlayX")->value() == box.left);
	CHECK(child<QSpinBox>(dialog, "overlayY")->value() == box.top);
	CHECK(child<QSpinBox>(dialog, "overlayWidth")->value() == box.width);
	CHECK(child<QSpinBox>(dialog, "overlayHeight")->value() == box.height);
}

bool canApply(const OverlayDialog &dialog)
{
	return child<QDialogButtonBox>(dialog, "overlayButtons")->button(QDialogButtonBox::Apply)->isEnabled();
}

} // namespace

TEST_CASE("the overlay preview fits the canvas at its own aspect and maps points onto it")
{
	FakeBackend backend;
	OverlayLayout layout(backend.current.overlays);
	OverlayPreview preview(backend, layout, [] {});
	preview.resize(976, 656);
	preview.show();
	QApplication::processEvents();
	REQUIRE(backend.overlayView);
	CHECK(backend.overlayView->geometry() == preview.rect());

	CHECK(preview.canvasArea() == QRectF(8, 58, 960, 540));
	CHECK(preview.canvasPoint({8, 58}) == CanvasPoint{0, 0});
	CHECK(preview.canvasPoint({488, 328}) == CanvasPoint{960, 540});
	CHECK(preview.canvasPoint({967.9, 597.9}) == CanvasPoint{1919, 1079});
	// Past the canvas the points go on, so a drag may take an overlay off its edge.
	CHECK(preview.canvasPoint({0, 0}) == CanvasPoint{-16, -116});
	CHECK(backend.overlayView->canvas == preview.canvasArea());
	CHECK(backend.overlayView->handle > 0.0);

	preview.resize(1000, 400);
	QApplication::processEvents();
	const double width = 1920.0 * 384.0 / 1080.0;
	CHECK(preview.canvasArea() == QRectF((1000 - width) / 2, 8, width, 384));
	CHECK(backend.overlayView->canvas == preview.canvasArea());
}

TEST_CASE("a click in the overlay preview selects the overlay under it")
{
	FakeBackend backend;
	placeOverlays(backend);
	OverlayDialog dialog(backend, localeText());
	OverlayPreview *preview = shownPreview(dialog);
	auto *list = child<QListWidget>(dialog, "overlays");
	REQUIRE(backend.overlayView);
	CHECK(backend.overlayView->selected == 2);

	click(*preview, at(dialog, 1500, 350));
	CHECK(dialog.editing().selected() == kLogo);
	CHECK(list->currentRow() == 1);
	CHECK(backend.overlayView->selected == kLogo);
	checkFields(dialog, {1300, 250, 400, 200});
	// A click alone changes nothing.
	CHECK_FALSE(canApply(dialog));

	click(*preview, at(dialog, 960, 700));
	CHECK_FALSE(dialog.editing().selected());
	CHECK(list->currentRow() == -1);
	CHECK_FALSE(child<QWidget>(dialog, "overlayFields")->isEnabled());
	CHECK_FALSE(backend.overlayView->selected);
}

TEST_CASE("a drag in the overlay preview moves the overlay, snapping unless Ctrl is held")
{
	FakeBackend backend;
	placeOverlays(backend);
	OverlayDialog dialog(backend, localeText());
	OverlayPreview *preview = shownPreview(dialog);

	const QPoint from = at(dialog, 1500, 350);
	const QPoint to = at(dialog, 1520, 320);
	drag(*preview, from, to);
	const CanvasPoint moves = shift(dialog, from, to);
	const OverlayBox moved{1300 + moves.x, 250 + moves.y, 400, 200};
	CHECK(boxOf(dialog, kLogo) == moved);
	CHECK_FALSE(dialog.editing().dragging());
	checkFields(dialog, moved);
	CHECK(tapeloop::boxOf(backend.overlayView->overlays.items[kLogo], backend.overlayView->overlays.canvas) ==
	      moved);
	CHECK(canApply(dialog));

	// Five canvas pixels short of the right edge is within reach of it at any size.
	const int toEdge = 1915 - (moved.left + moved.width);
	drag(*preview, at(dialog, 1600, 320), at(dialog, 1600 + toEdge, 320));
	CHECK(boxOf(dialog, kLogo).left == 1520);

	// A pixel of the preview is always nearer than the reach of the snapping.
	const QPoint edge = at(dialog, 1700, 320);
	const QPoint inside = edge - QPoint(1, 0);
	const int back = shift(dialog, edge, inside).x;
	REQUIRE(back < 0);
	drag(*preview, edge, inside);
	CHECK(boxOf(dialog, kLogo).left == 1520);
	drag(*preview, edge, inside, Qt::ControlModifier);
	CHECK(boxOf(dialog, kLogo).left == 1520 + back);
	CHECK(child<QSpinBox>(dialog, "overlayX")->value() == 1520 + back);

	// Ctrl pressed or let go during a drag counts at once, without the mouse moving.
	mouse(*preview, QEvent::MouseButtonPress, edge);
	mouse(*preview, QEvent::MouseMove, inside);
	CHECK(boxOf(dialog, kLogo).left == 1520);
	key(preview, QEvent::KeyPress, Qt::Key_Control, Qt::ControlModifier);
	CHECK(boxOf(dialog, kLogo).left == 1520 + 2 * back);
	key(preview, QEvent::KeyRelease, Qt::Key_Control);
	CHECK(boxOf(dialog, kLogo).left == 1520);
	mouse(*preview, QEvent::MouseButtonRelease, inside);
	CHECK_FALSE(dialog.editing().dragging());
}

TEST_CASE("a handle in the overlay preview resizes the selected overlay")
{
	FakeBackend backend;
	placeOverlays(backend);
	OverlayDialog dialog(backend, localeText());
	OverlayPreview *preview = shownPreview(dialog);
	click(*preview, at(dialog, 1500, 350));

	// Shift keeps the proportions the overlay had when the drag began, here 2:1.
	drag(*preview, at(dialog, 1700, 450), at(dialog, 1600, 440), Qt::ControlModifier | Qt::ShiftModifier);
	const OverlayBox kept = boxOf(dialog, kLogo);
	CHECK(kept.left == 1300);
	CHECK(kept.top == 250);
	CHECK(kept.height < 200);
	CHECK(kept.width == 2 * kept.height);
	checkFields(dialog, kept);

	const QPoint corner = at(dialog, kept.left + kept.width, kept.top + kept.height);
	const QPoint to = at(dialog, kept.left + kept.width - 100, kept.top + kept.height - 20);
	drag(*preview, corner, to, Qt::ControlModifier);
	const CanvasPoint grows = shift(dialog, corner, to);
	const OverlayBox smaller{1300, 250, kept.width + grows.x, kept.height + grows.y};
	CHECK(boxOf(dialog, kLogo) == smaller);
	checkFields(dialog, smaller);
}

TEST_CASE("Escape in the overlay preview puts back what a drag changed")
{
	FakeBackend backend;
	placeOverlays(backend);
	OverlayDialog dialog(backend, localeText());
	OverlayPreview *preview = shownPreview(dialog);

	const QPoint from = at(dialog, 1500, 350);
	mouse(*preview, QEvent::MouseButtonPress, from);
	mouse(*preview, QEvent::MouseMove, at(dialog, 1550, 380), Qt::ControlModifier);
	CHECK(boxOf(dialog, kLogo).left == 1300 + shift(dialog, from, at(dialog, 1550, 380)).x);
	key(preview, QEvent::KeyPress, Qt::Key_Escape);
	const OverlayBox start{1300, 250, 400, 200};
	CHECK(boxOf(dialog, kLogo) == start);
	CHECK_FALSE(dialog.editing().dragging());
	checkFields(dialog, start);
	CHECK_FALSE(canApply(dialog));
	CHECK(dialog.isVisible());
	mouse(*preview, QEvent::MouseMove, at(dialog, 1600, 400), Qt::ControlModifier);
	mouse(*preview, QEvent::MouseButtonRelease, at(dialog, 1600, 400));
	CHECK(boxOf(dialog, kLogo) == start);

	// Without a drag, Escape is the dialog's.
	key(preview, QEvent::KeyPress, Qt::Key_Escape);
	CHECK_FALSE(dialog.isVisible());
}

TEST_CASE("the arrows move the overlay selected in the preview")
{
	FakeBackend backend;
	placeOverlays(backend);
	OverlayDialog dialog(backend, localeText());
	OverlayPreview *preview = shownPreview(dialog);
	dialog.activateWindow();
	CHECK(preview->focusPolicy() == Qt::StrongFocus);
	click(*preview, at(dialog, 1500, 350));
	preview->setFocus();
	QApplication::processEvents();
	REQUIRE(preview->hasFocus());

	key(preview, QEvent::KeyPress, Qt::Key_Right);
	key(preview, QEvent::KeyPress, Qt::Key_Down, Qt::ShiftModifier);
	key(preview, QEvent::KeyPress, Qt::Key_Left, Qt::ControlModifier);
	const OverlayBox nudged{1301, 260, 400, 200};
	CHECK(boxOf(dialog, kLogo) == nudged);
	checkFields(dialog, nudged);
	CHECK(preview->hasFocus());

	click(*preview, at(dialog, 960, 700));
	const tapeloop::OverlaySettings before = dialog.editing().settings();
	key(preview, QEvent::KeyPress, Qt::Key_Up);
	CHECK(dialog.editing().settings() == before);
}

TEST_CASE("the overlay preview draws only while the dialog shows")
{
	FakeBackend backend;
	placeOverlays(backend);
	OverlayDialog dialog(backend, localeText());
	REQUIRE(backend.overlayView);
	CHECK_FALSE(backend.overlayView->active);
	dialog.show();
	QApplication::processEvents();
	CHECK(backend.overlayView->active);
	dialog.hide();
	CHECK_FALSE(backend.overlayView->active);
	dialog.show();
	CHECK(backend.overlayView->active);
}

TEST_CASE("the overlay preview edits the copy without a picture to show")
{
	FakeBackend backend;
	placeOverlays(backend);
	backend.overlayViews = false;
	OverlayDialog dialog(backend, localeText());
	OverlayPreview *preview = shownPreview(dialog);
	CHECK_FALSE(preview->view());

	const QPoint from = at(dialog, 1500, 350);
	const QPoint to = at(dialog, 1520, 320);
	drag(*preview, from, to, Qt::ControlModifier);
	const CanvasPoint moves = shift(dialog, from, to);
	checkFields(dialog, {1300 + moves.x, 250 + moves.y, 400, 200});
}
