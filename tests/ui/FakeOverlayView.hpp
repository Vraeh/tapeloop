// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "ui/OverlayView.hpp"

#include <QColor>
#include <QPainter>
#include <QPen>
#include <QRectF>
#include <QString>
#include <QWidget>

#include <cstddef>
#include <optional>

namespace tapeloop::test {

// Records what the overlay editor hands its live picture, and sketches that picture so
// the screenshots show it: the canvas, the visible overlays by name, and the selection.
class FakeOverlayView : public QWidget, public tapeloop::ui::OverlayView {
public:
	explicit FakeOverlayView(QWidget *parent) : QWidget(parent) { setObjectName("fakeOverlayView"); }

	OverlaySettings overlays;
	std::optional<size_t> selected;
	QRectF canvas;
	double handle = 0.0;
	bool active = false;
	int changes = 0;

	QWidget *widget() override { return this; }
	void setOverlays(const OverlaySettings &shown, std::optional<size_t> index) override
	{
		overlays = shown;
		selected = index;
		++changes;
		update();
	}
	void setFrame(const QRectF &area, double size) override
	{
		canvas = area;
		handle = size;
		update();
	}
	void setActive(bool on) override { active = on; }

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter painter(this);
		painter.fillRect(rect(), QColor(0x20, 0x20, 0x20));
		if (canvas.isEmpty() || overlays.canvas.width <= 0) {
			return;
		}
		painter.fillRect(canvas, QColor(0x5a, 0x5a, 0x5a));
		const double scale = canvas.width() / overlays.canvas.width;
		const auto place = [&](const OverlayItem &item) {
			const OverlayBox box = boxOf(item, overlays.canvas);
			return QRectF(canvas.left() + box.left * scale, canvas.top() + box.top * scale,
				      box.width * scale, box.height * scale);
		};
		painter.save();
		painter.setClipRect(canvas);
		painter.setPen(Qt::white);
		for (const OverlayItem &item : overlays.items) {
			if (item.visible) {
				const QRectF box = place(item);
				painter.fillRect(box, QColor(0x2a, 0x6e, 0xaa));
				painter.drawText(box, Qt::AlignCenter, QString::fromStdString(item.name));
			}
		}
		painter.restore();
		if (!selected || *selected >= overlays.items.size() || !overlays.items[*selected].visible) {
			return;
		}
		const QRectF box = place(overlays.items[*selected]);
		painter.setPen(QPen(Qt::red, 1.0));
		painter.setBrush(Qt::NoBrush);
		painter.drawRect(box);
		for (int column = 0; column < 3; ++column) {
			for (int row = 0; row < 3; ++row) {
				if (column != 1 || row != 1) {
					const double x = box.left() + box.width() * column / 2.0;
					const double y = box.top() + box.height() * row / 2.0;
					painter.fillRect(QRectF(x - handle / 2.0, y - handle / 2.0, handle, handle),
							 Qt::red);
				}
			}
		}
	}
};

} // namespace tapeloop::test
