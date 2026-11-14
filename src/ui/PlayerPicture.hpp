// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "ui/PictureSurface.hpp"

#include <QPointer>
#include <QWidget>
#include <QWindow>

#include <memory>

namespace tapeloop::ui {

// The replay player's picture: a native window the surface draws into while the widget
// is shown and has a size. Hidden, minimized or empty, the surface is disabled, so the
// picture costs nothing; it is attached again when the native window is replaced, and
// detached before the window goes. Without a surface it stays blank.
class PlayerPicture : public QWidget {
public:
	explicit PlayerPicture(std::unique_ptr<PictureSurface> surface, QWidget *parent = nullptr);
	~PlayerPicture() override;

	PlayerPicture(const PlayerPicture &) = delete;
	PlayerPicture &operator=(const PlayerPicture &) = delete;
	PlayerPicture(PlayerPicture &&) = delete;
	PlayerPicture &operator=(PlayerPicture &&) = delete;

	// The surface paints the window, not Qt.
	QPaintEngine *paintEngine() const override { return nullptr; }

protected:
	bool event(QEvent *event) override;
	bool eventFilter(QObject *watched, QEvent *event) override;
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	void paintEvent(QPaintEvent *event) override;

private:
	// Brings the surface in line with the widget: attached once it is shown with a size,
	// then resized with it and enabled only while it can be seen.
	void sync();
	void detach() noexcept;
	void watchWindow();

	std::unique_ptr<PictureSurface> surface_;
	QPointer<QWindow> window_;
	// Qt keeps a minimized widget visible, so what was last shown or hidden is kept here.
	bool shown_ = false;
	bool attached_ = false;
	// A surface that refused a window is asked again only when the widget is next shown.
	bool refused_ = false;
};

} // namespace tapeloop::ui
