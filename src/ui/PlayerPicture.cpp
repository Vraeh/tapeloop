// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "ui/PlayerPicture.hpp"

#include <QEvent>
#include <QPlatformSurfaceEvent>
#include <QScreen>

#include <cmath>
#include <utility>

namespace tapeloop::ui {

PlayerPicture::PlayerPicture(std::unique_ptr<PictureSurface> surface, QWidget *parent)
	: QWidget(parent),
	  surface_(std::move(surface))
{
	setAttribute(Qt::WA_NativeWindow);
	setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_NoSystemBackground);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setAttribute(Qt::WA_DontCreateNativeAncestors);
}

PlayerPicture::~PlayerPicture()
{
	detach();
	if (window_) {
		window_->removeEventFilter(this);
	}
}

bool PlayerPicture::event(QEvent *event)
{
	switch (event->type()) {
	case QEvent::WinIdChange:
		// The surface draws into the window it was given, which may be gone.
		detach();
		sync();
		break;
	default:
		break;
	}
	return QWidget::event(event);
}

bool PlayerPicture::eventFilter(QObject *watched, QEvent *event)
{
	if (watched == window_ && event->type() == QEvent::PlatformSurface &&
	    static_cast<QPlatformSurfaceEvent *>(event)->surfaceEventType() ==
		    QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
		detach();
	}
	return QWidget::eventFilter(watched, event);
}

void PlayerPicture::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	shown_ = true;
	refused_ = false;
	sync();
}

void PlayerPicture::hideEvent(QHideEvent *event)
{
	QWidget::hideEvent(event);
	shown_ = false;
	sync();
}

void PlayerPicture::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	sync();
}

void PlayerPicture::paintEvent(QPaintEvent *) {}

void PlayerPicture::sync()
{
	if (!surface_) {
		return;
	}
	const qreal ratio = devicePixelRatio();
	const auto pixelWidth = static_cast<uint32_t>(std::lround(width() * ratio));
	const auto pixelHeight = static_cast<uint32_t>(std::lround(height() * ratio));
	const bool drawn = shown_ && pixelWidth != 0 && pixelHeight != 0;
	if (!attached_) {
		// A window being taken down has no id, or is no longer marked created.
		const WId window = internalWinId();
		if (drawn && !refused_ && window != 0 && testAttribute(Qt::WA_WState_Created)) {
			watchWindow();
			attached_ = surface_->attach(static_cast<uintptr_t>(window), pixelWidth, pixelHeight);
			refused_ = !attached_;
		}
		return;
	}
	if (pixelWidth != 0 && pixelHeight != 0) {
		surface_->resize(pixelWidth, pixelHeight);
	}
	surface_->setEnabled(drawn);
}

void PlayerPicture::detach() noexcept
{
	if (attached_) {
		surface_->detach();
		attached_ = false;
	}
}

void PlayerPicture::watchWindow()
{
	QWindow *window = windowHandle();
	if (window == window_) {
		return;
	}
	if (window_) {
		window_->removeEventFilter(this);
		disconnect(window_, nullptr, this, nullptr);
	}
	window_ = window;
	if (window_) {
		window_->installEventFilter(this);
		connect(window_, &QWindow::screenChanged, this, [this] {
			watchScreen();
			sync();
		});
	}
	watchScreen();
}

void PlayerPicture::watchScreen()
{
	QScreen *screen = window_ ? window_->screen() : nullptr;
	if (screen == screen_) {
		return;
	}
	if (screen_) {
		disconnect(screen_, nullptr, this, nullptr);
	}
	screen_ = screen;
	if (screen_) {
		// Queued, so that the windows on the screen have taken its new pixel ratio first.
		connect(screen_, &QScreen::logicalDotsPerInchChanged, this, [this] { sync(); }, Qt::QueuedConnection);
	}
}

} // namespace tapeloop::ui
