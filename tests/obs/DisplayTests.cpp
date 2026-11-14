// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "../core/StoredReplays.hpp"
#include "ObsFixture.hpp"
#include "PlayerClips.hpp"
#include "UiTasks.hpp"

#include "core/Clip.hpp"
#include "core/ReplayLibrary.hpp"
#include "decode/FFmpegDecoder.hpp"
#include "decode/PictureDecoder.hpp"
#include "obs/ReplaySource.hpp"
#include "obs/SequencePlayer.hpp"
#include "obs/SourceDisplay.hpp"

#include <catch2/catch_test_macros.hpp>
#include <obs.h>
#include <obs.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

// Last, as Xlib's macros clash with names in the headers above.
#include <X11/Xlib.h>
#include <X11/Xutil.h>

using namespace std::chrono_literals;
using tapeloop::Clip;
using tapeloop::ReplayLibrary;
using tapeloop::decode::PictureDecoder;
using tapeloop::obs::registerReplaySource;
using tapeloop::obs::SequencePlayer;
using tapeloop::obs::SourceDisplay;
using tapeloop::test::captureClip;
using tapeloop::test::captureOf;
using tapeloop::test::ObsFixture;
using tapeloop::test::sourceOf;
using tapeloop::test::UiTasks;
using tapeloop::test::waitFor;

namespace {

constexpr uint32_t kWhite = 0xFFFFFF;
constexpr uint32_t kBlack = 0x000000;
// What the test windows show where nothing draws, to tell it from the display's black.
constexpr unsigned long kBlue = 0x0000FF;
constexpr uint32_t kUnread = 0xFFFFFFFF;

// A source that draws a white picture of kProbeWidth by kProbeHeight and reports the size
// asked, counting its draws. No canvas shows it, so every draw is the display's.
struct Probe {
	std::atomic<int> draws{0};
	std::atomic<uint32_t> width{0};
	std::atomic<uint32_t> height{0};
};

Probe probe;

constexpr const char *kProbeId = "tapeloop_test_probe";
constexpr uint32_t kProbeWidth = 320;
constexpr uint32_t kProbeHeight = 180;

const char *probeName(void *) noexcept
{
	return "Tapeloop test probe";
}

void *createProbe(obs_data_t *, obs_source_t *) noexcept
{
	return &probe;
}

void destroyProbe(void *) noexcept {}

uint32_t probeWidth(void *data) noexcept
{
	return static_cast<Probe *>(data)->width;
}

uint32_t probeHeight(void *data) noexcept
{
	return static_cast<Probe *>(data)->height;
}

void renderProbe(void *data, gs_effect_t *) noexcept
{
	++static_cast<Probe *>(data)->draws;
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	vec4 white;
	vec4_set(&white, 1.0f, 1.0f, 1.0f, 1.0f);
	gs_effect_set_vec4(gs_effect_get_param_by_name(solid, "color"), &white);
	while (gs_effect_loop(solid, "Solid")) {
		gs_draw_sprite(nullptr, 0, kProbeWidth, kProbeHeight);
	}
}

OBSSourceAutoRelease createProbeSource(uint32_t width, uint32_t height)
{
	probe.draws = 0;
	probe.width = width;
	probe.height = height;
	obs_source_info info = {};
	info.id = kProbeId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	info.get_name = probeName;
	info.create = createProbe;
	info.destroy = destroyProbe;
	info.get_width = probeWidth;
	info.get_height = probeHeight;
	info.video_render = renderProbe;
	obs_register_source(&info);
	return obs_source_create_private(kProbeId, "probe", nullptr);
}

bool readFailed = false;

int failRead(Display *, XErrorEvent *) noexcept
{
	readFailed = true;
	return 0;
}

// A window on the X display the tests run on, for a display to draw into. Made before
// the display and gone after it, as a frontend's are.
class XWindow {
public:
	XWindow(uint32_t width, uint32_t height) : display_(XOpenDisplay(nullptr))
	{
		REQUIRE(display_);
		const int screen = DefaultScreen(display_);
		window_ = XCreateSimpleWindow(display_, RootWindow(display_, screen), 0, 0, width, height, 0, kBlue,
					      kBlue);
		XMapWindow(display_, window_);
		XSync(display_, False);
	}

	~XWindow()
	{
		XDestroyWindow(display_, window_);
		XCloseDisplay(display_);
	}

	XWindow(const XWindow &) = delete;
	XWindow &operator=(const XWindow &) = delete;
	XWindow(XWindow &&) = delete;
	XWindow &operator=(XWindow &&) = delete;

	uintptr_t id() const noexcept { return window_; }

	void resize(uint32_t width, uint32_t height)
	{
		XResizeWindow(display_, window_, width, height);
		XSync(display_, False);
	}

	// The colour at x, y as 0xRRGGBB, with the window the renderer made inside this one,
	// or kUnread when it cannot be read.
	uint32_t pixel(int x, int y) const
	{
		XSync(display_, False);
		readFailed = false;
		XErrorHandler previous = XSetErrorHandler(failRead);
		XImage *image = XGetImage(display_, window_, x, y, 1, 1, AllPlanes, ZPixmap);
		XSync(display_, False);
		XSetErrorHandler(previous);
		if (!image) {
			return kUnread;
		}
		const auto value = static_cast<uint32_t>(XGetPixel(image, 0, 0) & 0xFFFFFF);
		XDestroyImage(image);
		return readFailed ? kUnread : value;
	}

private:
	Display *display_;
	Window window_ = 0;
};

// Lets a few frames go by.
void waitFrames()
{
	std::this_thread::sleep_for(200ms);
}

// The grey or white of the test pattern's upper part, once encoded and decoded.
bool light(uint32_t pixel)
{
	return pixel != kUnread && ((pixel >> 8) & 0xFF) >= 0x60;
}

std::unique_ptr<PictureDecoder> makeDecoder(bool)
{
	return std::make_unique<tapeloop::decode::FFmpegDecoder>();
}

} // namespace

TEST_CASE_METHOD(ObsFixture, "a source display fits the source in its window with black bars", "[obs][display]")
{
	XWindow window(400, 100);
	OBSSourceAutoRelease source = createProbeSource(320, 180);
	SourceDisplay display(source);
	REQUIRE(display.attach(window.id(), 400, 100));
	// 320 by 180 fits 400 by 100 as 177 by 100, from 111 across.
	CHECK(waitFor(
		[&] {
			return window.pixel(200, 50) == kWhite && window.pixel(120, 50) == kWhite &&
			       window.pixel(100, 50) == kBlack && window.pixel(300, 50) == kBlack;
		},
		5s));

	// And 200 by 300 as 200 by 112, from 94 down.
	window.resize(200, 300);
	display.resize(200, 300);
	CHECK(waitFor(
		[&] {
			return window.pixel(100, 150) == kWhite && window.pixel(100, 100) == kWhite &&
			       window.pixel(100, 80) == kBlack && window.pixel(100, 220) == kBlack;
		},
		5s));
}

TEST_CASE_METHOD(ObsFixture, "a source display draws only while enabled and attached", "[obs][display]")
{
	XWindow window(320, 180);
	OBSSourceAutoRelease source = createProbeSource(320, 180);
	SourceDisplay display(source);
	REQUIRE(display.attach(window.id(), 320, 180));
	REQUIRE(waitFor([] { return probe.draws > 2; }, 5s));

	display.setEnabled(false);
	// A draw may be under way as it is disabled.
	waitFrames();
	int draws = probe.draws;
	waitFrames();
	CHECK(probe.draws == draws);

	display.setEnabled(true);
	CHECK(waitFor([&] { return probe.draws > draws + 2; }, 5s));

	// Detaching waits for a draw under way.
	display.detach();
	draws = probe.draws;
	waitFrames();
	CHECK(probe.draws == draws);
	display.detach();

	// And it can draw again.
	REQUIRE(display.attach(window.id(), 320, 180));
	CHECK(waitFor([&] { return probe.draws > draws + 2; }, 5s));
}

TEST_CASE_METHOD(ObsFixture, "a source display shows nothing of a source with no size, which it still draws",
		 "[obs][display]")
{
	XWindow window(320, 180);
	OBSSourceAutoRelease source = createProbeSource(0, 0);
	SourceDisplay display(source);
	REQUIRE(display.attach(window.id(), 320, 180));
	// A replay source has to draw its first picture to report its size.
	REQUIRE(waitFor([] { return probe.draws > 2; }, 5s));
	waitFrames();
	CHECK(window.pixel(160, 90) == kBlack);
	CHECK(window.pixel(10, 10) == kBlack);

	probe.width = kProbeWidth;
	probe.height = kProbeHeight;
	CHECK(waitFor([&] { return window.pixel(160, 90) == kWhite && window.pixel(10, 10) == kWhite; }, 5s));
}

TEST_CASE_METHOD(ObsFixture, "a source display outlives the source it draws", "[obs][display]")
{
	XWindow window(320, 180);
	OBSSourceAutoRelease source = createProbeSource(320, 180);
	SourceDisplay display(source);
	REQUIRE(display.attach(window.id(), 320, 180));
	REQUIRE(waitFor([&] { return window.pixel(160, 90) == kWhite; }, 5s));

	source = nullptr;
	obs_wait_for_destroy_queue();
	const int draws = probe.draws;
	// The background still shows, with nothing on it.
	CHECK(waitFor([&] { return window.pixel(160, 90) == kBlack; }, 5s));
	CHECK(probe.draws == draws);
	display.resize(200, 100);
	waitFrames();
	display.detach();
}

TEST_CASE_METHOD(ObsFixture, "the player's picture shows the replay cued, and either may go first", "[obs][display]")
{
	const Clip clip = captureClip(2);
	registerReplaySource(makeDecoder);
	const UiTasks ui;
	ReplayLibrary library;
	std::optional<SequencePlayer> sequence(std::in_place, library);
	REQUIRE(sequence->source());
	XWindow window(400, 100);
	std::optional<SourceDisplay> picture(std::in_place, sequence->source());
	REQUIRE(picture->attach(window.id(), 400, 100));
	// With no clip the player has no size, and only the background shows.
	CHECK(waitFor([&] { return window.pixel(200, 30) == kBlack; }, 5s));

	const tapeloop::test::Gops gops(clip.gops().begin(), clip.gops().end());
	std::vector<tapeloop::CaptureSource> sources;
	sources.push_back(sourceOf("a", gops));
	REQUIRE(library.addCaptured(captureOf("unused", std::move(sources)), "Broadcast") != 0);
	sequence->cueCurrent();
	REQUIRE(waitFor(
		[&] {
			UiTasks::run();
			return sequence->cuedSource() == "a";
		},
		10s));
	// 320 by 180 in 400 by 100, between black bars.
	CHECK(waitFor(
		[&] {
			return light(window.pixel(200, 30)) && window.pixel(50, 30) == kBlack &&
			       window.pixel(350, 30) == kBlack;
		},
		10s));

	window.resize(200, 300);
	picture->resize(200, 300);
	CHECK(waitFor(
		[&] {
			return light(window.pixel(100, 120)) && window.pixel(100, 40) == kBlack &&
			       window.pixel(100, 260) == kBlack;
		},
		10s));
	picture->setEnabled(false);
	waitFrames();
	picture->setEnabled(true);
	CHECK(waitFor([&] { return light(window.pixel(100, 120)); }, 5s));

	SECTION("the player goes first")
	{
		sequence.reset();
		obs_wait_for_destroy_queue();
		CHECK(waitFor([&] { return window.pixel(100, 120) == kBlack; }, 5s));
		picture.reset();
	}
	SECTION("the picture goes first")
	{
		picture.reset();
		waitFrames();
		sequence.reset();
		obs_wait_for_destroy_queue();
	}
}
