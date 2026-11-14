// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "obs/PreviewDisplay.hpp"

#include "ui/Guarded.hpp"

#include <plugin-support.h>

#include <QEvent>
#include <QPlatformSurfaceEvent>
#include <QScreen>
#include <QWindow>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif !defined(__APPLE__)
#include <obs-nix-platform.h>
#endif

#include <algorithm>
#include <cmath>

namespace tapeloop::obs {
namespace {

int toPixels(int length, qreal ratio)
{
	return std::max(1, static_cast<int>(std::lround(length * ratio)));
}

bool nativeWindowOf(QWindow &window, gs_window &target)
{
#if defined(_WIN32)
	target.hwnd = reinterpret_cast<void *>(window.winId());
	return true;
#elif defined(__APPLE__)
	target.view = reinterpret_cast<id>(window.winId());
	return true;
#else
	switch (obs_get_nix_platform()) {
	case OBS_NIX_PLATFORM_X11_EGL:
		target.id = static_cast<uint32_t>(window.winId());
		target.display = obs_get_nix_platform_display();
		return true;
	case OBS_NIX_PLATFORM_WAYLAND: {
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
		target.display = reinterpret_cast<void *>(window.winId());
		return target.display != nullptr;
#else
		// Older Qt hands out the Wayland surface only through its private API.
		static bool logged = false;
		if (!logged) {
			obs_log(LOG_WARNING, "the overlay editor has no preview on Wayland before Qt 6.9");
			logged = true;
		}
		return false;
#endif
	}
	default:
		return false;
	}
#endif
}

} // namespace

PreviewDisplay::PreviewDisplay(QWidget *parent) : QWidget(parent)
{
	setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_StaticContents);
	setAttribute(Qt::WA_NoSystemBackground);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setAttribute(Qt::WA_DontCreateNativeAncestors);
	setAttribute(Qt::WA_NativeWindow);
	setFocusPolicy(Qt::NoFocus);
	watchWindow();
}

PreviewDisplay::~PreviewDisplay()
{
	// The window goes after this part of the widget, and must not reach back into it.
	if (QWindow *window = windowHandle()) {
		window->removeEventFilter(this);
		window->disconnect(this);
	}
	renderer_.destroyDisplay();
}

void PreviewDisplay::setOverlays(const OverlaySettings &overlays, std::optional<size_t> selected)
{
	renderer_.setOverlays(overlays, selected);
}

void PreviewDisplay::setFrame(const QRectF &canvas, double handle)
{
	canvas_ = canvas;
	handle_ = handle;
	updateFrame();
}

void PreviewDisplay::setActive(bool active)
{
	active_ = active;
	updateEnabled();
}

bool PreviewDisplay::eventFilter(QObject *watched, QEvent *event)
{
	if (watched == windowHandle()) {
		if (event->type() == QEvent::Expose) {
			resizeDisplay();
		} else if (event->type() == QEvent::PlatformSurface &&
			   static_cast<QPlatformSurfaceEvent *>(event)->surfaceEventType() ==
				   QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
			renderer_.destroyDisplay();
		}
	}
	return QWidget::eventFilter(watched, event);
}

void PreviewDisplay::paintEvent(QPaintEvent *)
{
	createDisplay();
}

void PreviewDisplay::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	resizeDisplay();
}

void PreviewDisplay::moveEvent(QMoveEvent *event)
{
	QWidget::moveEvent(event);
	renderer_.updateColorSpace();
}

void PreviewDisplay::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	watchWindow();
	resizeDisplay();
	updateEnabled();
}

void PreviewDisplay::hideEvent(QHideEvent *event)
{
	QWidget::hideEvent(event);
	updateEnabled();
}

#ifdef _WIN32
bool PreviewDisplay::nativeEvent(const QByteArray &, void *message, qintptr *)
{
	if (static_cast<const MSG *>(message)->message == WM_DISPLAYCHANGE) {
		renderer_.updateColorSpace();
	}
	return false;
}
#endif

void PreviewDisplay::watchWindow()
{
	QWindow *window = windowHandle();
	if (watching_ || !window) {
		return;
	}
	window->installEventFilter(this);
	connect(window, &QWindow::screenChanged, this, [this](QScreen *) {
		ui::guarded([this] {
			resizeDisplay();
			renderer_.updateColorSpace();
		});
	});
	watching_ = true;
}

void PreviewDisplay::createDisplay()
{
	QWindow *window = windowHandle();
	if (renderer_.hasDisplay() || !window || !window->isExposed()) {
		return;
	}
	const qreal ratio = devicePixelRatio();
	gs_init_data info = {};
	info.cx = static_cast<uint32_t>(toPixels(width(), ratio));
	info.cy = static_cast<uint32_t>(toPixels(height(), ratio));
	info.format = GS_BGRA;
	info.zsformat = GS_ZS_NONE;
	if (!nativeWindowOf(*window, info.window)) {
		return;
	}
	if (!renderer_.createDisplay(info) && !failed_) {
		obs_log(LOG_WARNING, "could not make the overlay editor's preview");
		failed_ = true;
	}
}

void PreviewDisplay::resizeDisplay()
{
	createDisplay();
	const qreal ratio = devicePixelRatio();
	renderer_.resize(static_cast<uint32_t>(toPixels(width(), ratio)),
			 static_cast<uint32_t>(toPixels(height(), ratio)));
	updateFrame();
}

void PreviewDisplay::updateFrame()
{
	const qreal ratio = devicePixelRatio();
	PreviewFrame frame;
	frame.left = static_cast<float>(canvas_.left() * ratio);
	frame.top = static_cast<float>(canvas_.top() * ratio);
	frame.width = static_cast<float>(canvas_.width() * ratio);
	frame.height = static_cast<float>(canvas_.height() * ratio);
	frame.handle = static_cast<float>(handle_ * ratio);
	frame.line = static_cast<float>(std::max<qreal>(1.0, std::round(ratio)));
	renderer_.setFrame(frame);
}

void PreviewDisplay::updateEnabled()
{
	renderer_.setEnabled(active_ && isVisible());
}

ui::OverlayView *PreviewDisplayFactory::createOverlayView(QWidget *parent)
{
	return new PreviewDisplay(parent);
}

} // namespace tapeloop::obs
