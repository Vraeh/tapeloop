// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include <cstdint>

namespace tapeloop::test {

struct CanvasFormat {
	uint32_t width = 640;
	uint32_t height = 360;
	uint32_t fps = 30;
};

// Starts libobs for one test case, on the OpenGL renderer of whatever X display
// DISPLAY names, with the obs-x264 plugin, the test pattern source and the buffer
// output loaded, and shuts it down when the test case ends. The test case fails if
// libobs still counts allocations of its own after the shutdown, or reports objects or
// views it had to free itself.
class ObsFixture {
public:
	ObsFixture();
	~ObsFixture();

	ObsFixture(const ObsFixture &) = delete;
	ObsFixture &operator=(const ObsFixture &) = delete;

	// Resets the main canvas. Nothing may be encoding.
	static void resetCanvas(CanvasFormat format);

private:
	long allocationsBefore_ = 0;
};

} // namespace tapeloop::test
