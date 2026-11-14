// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "obs/ManagerDockBackend.hpp"
#include "obs/PreviewRenderer.hpp"
#include "ui/OverlayView.hpp"

#include <QRectF>
#include <QWidget>

namespace tapeloop::obs {

// The overlay editor's live picture: a native window OBS draws into. The display is made
// once the window is on screen, at its size in physical pixels, draws only while the
// editor says it is active and the window shows, and goes with the window.
class PreviewDisplay : public QWidget, public ui::OverlayView {
public:
	explicit PreviewDisplay(QWidget *parent);
	~PreviewDisplay() override;

	PreviewDisplay(const PreviewDisplay &) = delete;
	PreviewDisplay &operator=(const PreviewDisplay &) = delete;
	PreviewDisplay(PreviewDisplay &&) = delete;
	PreviewDisplay &operator=(PreviewDisplay &&) = delete;

	QWidget *widget() override { return this; }
	void setOverlays(const OverlaySettings &overlays, std::optional<size_t> selected) override;
	void setFrame(const QRectF &canvas, double handle) override;
	void setActive(bool active) override;

	// OBS draws the window, Qt does not.
	QPaintEngine *paintEngine() const override { return nullptr; }

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;
	void paintEvent(QPaintEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	void moveEvent(QMoveEvent *event) override;
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;
#ifdef _WIN32
	bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;
#endif

private:
	void watchWindow();
	void createDisplay();
	void resizeDisplay();
	void updateFrame();
	void updateEnabled();

	PreviewRenderer renderer_;
	QRectF canvas_;
	double handle_ = 0.0;
	bool active_ = false;
	bool watching_ = false;
	bool failed_ = false;
};

class PreviewDisplayFactory final : public OverlayViewFactory {
public:
	ui::OverlayView *createOverlayView(QWidget *parent) override;
};

} // namespace tapeloop::obs
