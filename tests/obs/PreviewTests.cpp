// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "ObsFixture.hpp"
#include "TestPattern.hpp"

#include "core/OverlayLayout.hpp"
#include "obs/PreviewRenderer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <obs.hpp>
#include <xcb/xcb.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>

using tapeloop::OverlayItem;
using tapeloop::OverlaySettings;
using tapeloop::obs::PreviewFrame;
using tapeloop::obs::PreviewRenderer;
using tapeloop::test::createTestPattern;
using tapeloop::test::ObsFixture;
using tapeloop::test::waitFor;
using namespace std::chrono_literals;

namespace {

// A top-level window of the X server the harness runs on, for OBS to draw into.
class TestWindow {
public:
	TestWindow(uint16_t width, uint16_t height) : connection_(xcb_connect(nullptr, nullptr))
	{
		if (xcb_connection_has_error(connection_) != 0) {
			return;
		}
		const xcb_screen_t *screen = xcb_setup_roots_iterator(xcb_get_setup(connection_)).data;
		window_ = xcb_generate_id(connection_);
		xcb_create_window(connection_, XCB_COPY_FROM_PARENT, window_, screen->root, 0, 0, width, height, 0,
				  XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, 0, nullptr);
		xcb_map_window(connection_, window_);
		// A round trip, so the window is mapped before OBS puts its own inside.
		std::free(xcb_get_input_focus_reply(connection_, xcb_get_input_focus(connection_), nullptr));
	}

	~TestWindow()
	{
		if (window_ != 0) {
			xcb_destroy_window(connection_, window_);
		}
		xcb_disconnect(connection_);
	}

	TestWindow(const TestWindow &) = delete;
	TestWindow &operator=(const TestWindow &) = delete;
	TestWindow(TestWindow &&) = delete;
	TestWindow &operator=(TestWindow &&) = delete;

	uint32_t id() const noexcept { return window_; }

	// As 0xRRGGBB, what is on screen at that point, OBS's window included.
	std::optional<uint32_t> pixel(int16_t x, int16_t y) const
	{
		const std::unique_ptr<xcb_get_image_reply_t, decltype(&std::free)> reply(
			xcb_get_image_reply(connection_,
					    xcb_get_image(connection_, XCB_IMAGE_FORMAT_Z_PIXMAP, window_, x, y, 1, 1,
							  UINT32_MAX),
					    nullptr),
			&std::free);
		if (!reply || xcb_get_image_data_length(reply.get()) < 4) {
			return std::nullopt;
		}
		uint32_t value = 0;
		std::memcpy(&value, xcb_get_image_data(reply.get()), sizeof(value));
		return value & 0xFFFFFFU;
	}

private:
	xcb_connection_t *connection_;
	xcb_window_t window_ = 0;
};

gs_init_data displayOf(const TestWindow &window, uint32_t width, uint32_t height)
{
	gs_init_data info = {};
	info.cx = width;
	info.cy = height;
	info.format = GS_BGRA;
	info.zsformat = GS_ZS_NONE;
	info.window.id = window.id();
	return info;
}

OverlaySettings overlaysOf(obs_source_t *source)
{
	OverlaySettings overlays;
	overlays.canvas = {640, 360};
	OverlayItem item;
	item.uuid = obs_source_get_uuid(source);
	item.name = obs_source_get_name(source);
	item.x = 320;
	item.y = 180;
	item.width = 160;
	item.height = 90;
	overlays.items.push_back(item);
	return overlays;
}

bool near(std::optional<uint32_t> pixel, uint32_t expected)
{
	if (!pixel) {
		return false;
	}
	for (int shift = 0; shift < 24; shift += 8) {
		const int got = static_cast<int>((*pixel >> shift) & 0xFFU);
		const int want = static_cast<int>((expected >> shift) & 0xFFU);
		if (std::abs(got - want) > 2) {
			return false;
		}
	}
	return true;
}

} // namespace

TEST_CASE_METHOD(ObsFixture, "the overlay preview draws the canvas, an overlay and its selection into a window",
		 "[obs][preview]")
{
	TestWindow window(400, 300);
	REQUIRE(window.id() != 0);
	OBSSourceAutoRelease overlay = createTestPattern(160, 90, "Overlay");
	PreviewRenderer preview;
	preview.setOverlays(overlaysOf(overlay), 0);
	// The canvas at half its size, centered: 320x180 from (40, 60). The overlay's box is
	// at (200, 150), 80x45.
	preview.setFrame({40.0f, 60.0f, 320.0f, 180.0f, 8.0f, 2.0f});
	preview.setEnabled(true);
	REQUIRE(preview.createDisplay(displayOf(window, 400, 300)));
	CHECK(preview.showing());

	const auto drawn = [&] {
		return near(window.pixel(100, 100), 0x5A5A5A);
	};
	REQUIRE(waitFor(drawn, 5s));
	CHECK(near(window.pixel(10, 10), 0x202020));
	CHECK(near(window.pixel(380, 280), 0x202020));
	// The top edge, a corner handle and the middle handle of the bottom edge.
	CHECK(near(window.pixel(240, 150), 0xFF0000));
	CHECK(near(window.pixel(197, 147), 0xFF0000));
	CHECK(near(window.pixel(240, 198), 0xFF0000));
	// The overlay, which is not the placeholder.
	CHECK_FALSE(near(window.pixel(230, 185), 0x5A5A5A));

	// Without a selection, the box goes.
	preview.setOverlays(overlaysOf(overlay), std::nullopt);
	CHECK(waitFor([&] { return !near(window.pixel(240, 150), 0xFF0000); }, 5s));

	preview.setEnabled(false);
	CHECK_FALSE(preview.showing());
	preview.destroyDisplay();
	CHECK_FALSE(preview.hasDisplay());
}

TEST_CASE_METHOD(ObsFixture, "the overlay preview shows its overlays only while it draws", "[obs][preview]")
{
	TestWindow window(200, 150);
	REQUIRE(window.id() != 0);
	OBSSourceAutoRelease overlay = createTestPattern(160, 90, "Overlay");
	PreviewRenderer preview;
	preview.setOverlays(overlaysOf(overlay), std::nullopt);
	preview.setFrame({0.0f, 0.0f, 200.0f, 112.5f, 8.0f, 1.0f});
	CHECK_FALSE(obs_source_showing(overlay));

	// Enabled before there is a display: nothing draws yet.
	preview.setEnabled(true);
	CHECK_FALSE(preview.showing());
	CHECK_FALSE(obs_source_showing(overlay));
	REQUIRE(preview.createDisplay(displayOf(window, 200, 150)));
	CHECK(preview.createDisplay(displayOf(window, 200, 150)));
	CHECK(obs_source_showing(overlay));

	preview.setEnabled(false);
	CHECK_FALSE(obs_source_showing(overlay));
	preview.setEnabled(true);
	CHECK(obs_source_showing(overlay));
	preview.resize(300, 200);
	preview.updateColorSpace();

	// The window going away takes the display with it, and the overlay is no longer shown.
	preview.destroyDisplay();
	CHECK_FALSE(preview.showing());
	CHECK_FALSE(obs_source_showing(overlay));
	CHECK(preview.enabled());
}

TEST_CASE_METHOD(ObsFixture, "an overlay preview without its display lets go of everything", "[obs][preview]")
{
	TestWindow window(200, 150);
	REQUIRE(window.id() != 0);
	OBSSourceAutoRelease overlay = createTestPattern(160, 90, "Overlay");
	OBSWeakSourceAutoRelease weakComposition;
	{
		PreviewRenderer preview;
		preview.setOverlays(overlaysOf(overlay), 0);
		preview.setEnabled(true);
		REQUIRE(preview.createDisplay(displayOf(window, 200, 150)));
		REQUIRE(preview.composition());
		weakComposition = obs_source_get_weak_source(preview.composition());
		CHECK(obs_source_showing(overlay));
	}
	CHECK_FALSE(obs_source_showing(overlay));
	CHECK(obs_weak_source_expired(weakComposition));
}
