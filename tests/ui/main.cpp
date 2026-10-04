// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include <catch2/catch_session.hpp>

#include <QApplication>

int main(int argc, char *argv[])
{
	// No display is needed, and the widgets render the same everywhere.
	qputenv("QT_QPA_PLATFORM", "offscreen");
	QApplication app(argc, argv);
	return Catch::Session().run(argc, argv);
}
