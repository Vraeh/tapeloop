// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "FakeBackend.hpp"
#include "FakeSurface.hpp"
#include "ui/PlayerPicture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QScreen>
#include <QWidget>
#include <QWindow>

#include <cmath>
#include <memory>

using tapeloop::test::FakeBackend;
using tapeloop::test::FakeSurface;
using tapeloop::test::SurfaceLog;
using tapeloop::ui::PlayerPicture;

namespace {

// Lets a test take its native window down while the widgets stay.
class Host : public QWidget {
public:
	using QWidget::destroy;
};

uint32_t pixels(const QWidget &widget, int length)
{
	return static_cast<uint32_t>(std::lround(length * widget.devicePixelRatio()));
}

} // namespace

TEST_CASE("the picture draws while shown with a size and follows the widget", "[ui][picture]")
{
	FakeBackend backend;
	QWidget window;
	window.resize(400, 300);
	auto *picture = new PlayerPicture(backend.pictureSurface(), &window);
	picture->setGeometry(0, 0, 320, 180);
	const SurfaceLog &log = backend.surface;
	CHECK(log.attaches == 0);

	window.show();
	REQUIRE(log.attached);
	CHECK(log.enabled);
	CHECK(log.window == picture->winId());
	CHECK(log.width == pixels(*picture, 320));
	CHECK(log.height == pixels(*picture, 180));

	picture->resize(200, 100);
	CHECK(log.width == pixels(*picture, 200));
	CHECK(log.height == pixels(*picture, 100));

	picture->hide();
	CHECK_FALSE(log.enabled);
	CHECK(log.attached);
	picture->show();
	CHECK(log.enabled);

	// Minimized, Qt hides the window's children without taking them off screen.
	window.showMinimized();
	CHECK(picture->isVisible());
	CHECK_FALSE(log.enabled);
	window.showNormal();
	CHECK(log.enabled);

	// Drawing nothing at no size.
	picture->resize(0, 0);
	CHECK_FALSE(log.enabled);
	picture->resize(160, 90);
	CHECK(log.enabled);
	CHECK(log.width == pixels(*picture, 160));

	CHECK(log.attaches == 1);
	CHECK(log.detaches == 0);
	delete picture;
	CHECK(log.detaches == 1);
	CHECK(log.destroyed);
	CHECK(log.misuses == 0);
}

TEST_CASE("the picture follows its window to another screen and a new resolution", "[ui][picture]")
{
	SurfaceLog log;
	QWidget window;
	window.resize(400, 300);
	auto *picture = new PlayerPicture(std::make_unique<FakeSurface>(log), &window);
	picture->setGeometry(0, 0, 320, 180);
	window.show();
	REQUIRE(log.attached);
	QWindow *native = picture->windowHandle();
	REQUIRE(native);
	QScreen *screen = native->screen();
	REQUIRE(screen);

	// Either changes the pixel ratio, and so the size in device pixels, with no resize of
	// the widget. The offscreen platform has one screen of one ratio, so only the size the
	// surface is given again can be checked.
	int resizes = log.resizes;
	Q_EMIT native->screenChanged(screen);
	CHECK(log.resizes == resizes + 1);
	resizes = log.resizes;
	Q_EMIT screen->logicalDotsPerInchChanged(screen->logicalDotsPerInch());
	QApplication::processEvents();
	CHECK(log.resizes == resizes + 1);
	CHECK(log.width == pixels(*picture, 320));
	CHECK(log.height == pixels(*picture, 180));
	CHECK(log.attaches == 1);
	CHECK(log.misuses == 0);
}

TEST_CASE("the picture waits for a size before it draws", "[ui][picture]")
{
	SurfaceLog log;
	QWidget window;
	window.resize(400, 300);
	auto *picture = new PlayerPicture(std::make_unique<FakeSurface>(log), &window);
	picture->setGeometry(0, 0, 0, 0);
	window.show();
	CHECK(log.attaches == 0);

	picture->resize(100, 50);
	REQUIRE(log.attached);
	CHECK(log.enabled);
	CHECK(log.width == pixels(*picture, 100));
	CHECK(log.misuses == 0);
}

TEST_CASE("the picture lets go of a window before it goes", "[ui][picture]")
{
	SurfaceLog log;
	Host window;
	window.resize(400, 300);
	auto *picture = new PlayerPicture(std::make_unique<FakeSurface>(log), &window);
	picture->setGeometry(0, 0, 320, 180);
	window.show();
	REQUIRE(log.attached);
	const WId first = picture->winId();
	bool windowThere = false;
	log.onDetach = [&] {
		windowThere = picture->windowHandle() && picture->windowHandle()->handle();
	};

	// Taking the window down takes down the picture's native window with it.
	window.hide();
	window.destroy();
	CHECK_FALSE(log.attached);
	CHECK(log.detaches == 1);
	CHECK(windowThere);
	window.show();
	REQUIRE(log.attached);
	CHECK(log.attaches == 2);
	CHECK(log.window == picture->winId());
	CHECK(log.window != first);

	windowThere = false;
	delete picture;
	CHECK(log.detaches == 2);
	CHECK(windowThere);
	CHECK(log.destroyed);
	CHECK(log.misuses == 0);
}

TEST_CASE("a surface that refuses a window is asked again when the picture is shown", "[ui][picture]")
{
	SurfaceLog log;
	log.refuse = true;
	QWidget window;
	window.resize(400, 300);
	auto *picture = new PlayerPicture(std::make_unique<FakeSurface>(log), &window);
	picture->setGeometry(0, 0, 320, 180);
	window.show();
	CHECK(log.attaches == 1);
	CHECK_FALSE(log.attached);
	picture->resize(200, 100);
	CHECK(log.attaches == 1);

	log.refuse = false;
	picture->hide();
	picture->show();
	CHECK(log.attaches == 2);
	CHECK(log.attached);
	CHECK(log.misuses == 0);
}

TEST_CASE("without a player the picture stays blank", "[ui][picture]")
{
	FakeBackend backend;
	backend.picture = false;
	CHECK_FALSE(backend.pictureSurface());
	QWidget window;
	auto *picture = new PlayerPicture(backend.pictureSurface(), &window);
	picture->setGeometry(0, 0, 320, 180);
	window.show();
	picture->resize(100, 50);
	picture->hide();
	CHECK(backend.surface.attaches == 0);
}
