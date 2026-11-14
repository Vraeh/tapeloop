// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/BufferSettings.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace std::chrono_literals;
using tapeloop::AirMediaKind;
using tapeloop::BufferSettings;
using tapeloop::ReplayResolution;
using tapeloop::ResolutionMode;
using tapeloop::SavedSettings;
using tapeloop::SavedSource;
using tapeloop::SourceIdentity;
using tapeloop::SourceSettings;

namespace {

constexpr tapeloop::CanvasSize kCanvas{1920, 1080};

} // namespace

TEST_CASE("buffer settings start from the defaults")
{
	const BufferSettings settings;
	CHECK(settings.length == 60s);
	CHECK(settings.resolution.mode == ResolutionMode::Canvas);
	CHECK(settings.startWithOutputs);
	CHECK_FALSE(settings.forceH264);
	CHECK(settings.selectedSources().empty());
}

TEST_CASE("per source overrides take the place of the global settings")
{
	BufferSettings settings;
	settings.length = 90s;
	settings.resolution = {ResolutionMode::Fixed, 720};
	settings.sources["a"] = {true, 30s, ReplayResolution{ResolutionMode::Output, 1080}, std::nullopt};
	settings.sources["b"] = {true, std::nullopt, std::nullopt, std::nullopt};
	settings.sources["c"] = {false, 120s, std::nullopt, std::nullopt};

	CHECK(settings.lengthFor("a") == 30s);
	CHECK(settings.resolutionFor("a").mode == ResolutionMode::Output);
	CHECK(settings.lengthFor("b") == 90s);
	CHECK(settings.resolutionFor("b").height == 720);
	CHECK(settings.lengthFor("unknown") == 90s);
	CHECK(settings.selectedSources() == std::vector<std::string>{"a", "b"});
}

TEST_CASE("encoders prefer HEVC unless the settings keep replays in H.264")
{
	BufferSettings settings;
	CHECK(settings.encoderPreferences().preferHevc);
	settings.forceH264 = true;
	CHECK_FALSE(settings.encoderPreferences().preferHevc);
}

TEST_CASE("the settings pass the chosen encoder and the other-adapter choice on")
{
	BufferSettings settings;
	CHECK(settings.encoderPreferences().chosen.empty());
	CHECK(settings.encoderPreferences().otherAdapters);
	settings.replayEncoder = "obs_qsv11_v2";
	settings.allowOtherAdapters = false;
	CHECK(settings.encoderPreferences().chosen == "obs_qsv11_v2");
	CHECK_FALSE(settings.encoderPreferences().otherAdapters);
}

TEST_CASE("buffer settings survive a save and a load")
{
	BufferSettings settings;
	settings.length = 45s;
	settings.resolution = {ResolutionMode::Output, 1080};
	settings.startWithOutputs = false;
	settings.forceH264 = true;
	settings.replayEncoder = "obs_nvenc_hevc_tex";
	settings.allowOtherAdapters = false;
	settings.showAdvanced = true;
	settings.bufferMemoryMiB = 2048;
	settings.sources["a"] = {true, 120s, ReplayResolution{ResolutionMode::Fixed, 2160}, std::nullopt};
	settings.sources["b"] = {false, std::nullopt, ReplayResolution{ResolutionMode::Canvas, 1080}, std::nullopt};
	settings.sources["c"] = {true, std::nullopt, std::nullopt, std::nullopt};

	const SavedSettings saved = tapeloop::saveSettings(settings);
	CHECK(saved.version == tapeloop::kSettingsVersion);
	CHECK(saved.lengthSeconds == 45);
	CHECK(saved.resolution == "output");
	CHECK(saved.showAdvanced);
	REQUIRE(saved.sources.size() == 3);
	CHECK(saved.sources[0].uuid == "a");
	CHECK(saved.sources[0].resolution == "fixed");
	CHECK(saved.sources[0].height == 2160);
	CHECK_FALSE(saved.sources[2].lengthSeconds);

	const std::optional<BufferSettings> loaded = tapeloop::loadSettings(saved, kCanvas);
	REQUIRE(loaded);
	CHECK(*loaded == settings);

	// A memory budget below nothing is the automatic one.
	SavedSettings negative = saved;
	negative.bufferMemoryMiB = -1;
	CHECK(tapeloop::loadSettings(negative, kCanvas)->bufferMemoryMiB == 0);
}

TEST_CASE("settings of another version are not loaded")
{
	SavedSettings saved = tapeloop::saveSettings(BufferSettings{});
	saved.version = 2;
	CHECK_FALSE(tapeloop::loadSettings(saved, kCanvas));
	saved.version = 0;
	CHECK_FALSE(tapeloop::loadSettings(saved, kCanvas));
}

TEST_CASE("loaded settings are brought back into range")
{
	SavedSettings saved;
	saved.lengthSeconds = 5;
	saved.resolution = "sideways";
	saved.height = 1080;
	saved.sources.push_back({"a", "Camera", true, 100'000, std::string("fixed"), 999, std::nullopt});
	saved.sources.push_back({"b", "", true, -40, std::string("fixed"), std::nullopt, std::nullopt});
	saved.sources.push_back({"", "Nameless", true, std::nullopt, std::nullopt, std::nullopt, std::nullopt});

	const std::optional<BufferSettings> loaded = tapeloop::loadSettings(saved, kCanvas);
	REQUIRE(loaded);
	CHECK(loaded->length == 10s);
	CHECK(loaded->resolution.mode == ResolutionMode::Canvas);
	REQUIRE(loaded->sources.size() == 2);
	CHECK(loaded->sources.at("a").length == 300s);
	CHECK(loaded->sources.at("a").resolution->mode == ResolutionMode::Canvas);
	CHECK(loaded->sources.at("b").length == 10s);
	CHECK(loaded->sources.at("b").resolution->mode == ResolutionMode::Canvas);

	saved.lengthSeconds = INT64_MAX;
	CHECK(tapeloop::loadSettings(saved, kCanvas)->length == 300s);
	saved.lengthSeconds = INT64_MIN;
	CHECK(tapeloop::loadSettings(saved, kCanvas)->length == 10s);
}

TEST_CASE("a replay goes on air with a fade and no intro or outro by default")
{
	const BufferSettings settings;
	CHECK_FALSE(settings.air.intro.present());
	CHECK_FALSE(settings.air.outro.present());
	CHECK(settings.air.into.name.empty());
	CHECK(settings.air.into.duration == 300ms);
	CHECK(settings.air.back.name.empty());
	CHECK(settings.air.back.duration == 300ms);

	// A kind without its file shows nothing.
	tapeloop::AirMedia media{AirMediaKind::Video, "", 3s};
	CHECK_FALSE(media.present());
	media.path = "intro.mp4";
	CHECK(media.present());
}

TEST_CASE("on-air settings survive a save and a load")
{
	BufferSettings settings;
	settings.air.intro = {AirMediaKind::Video, "C:/Clips/intro.mov", 3s};
	settings.air.into = {"Stinger", 1200ms};
	settings.air.outro = {AirMediaKind::Image, "C:/Clips/sponsor.png", 4500ms};
	settings.air.back = {"Luma Wipe", 800ms};

	const SavedSettings saved = tapeloop::saveSettings(settings);
	CHECK(saved.intro.kind == "video");
	CHECK(saved.intro.path == "C:/Clips/intro.mov");
	CHECK(saved.into.name == "Stinger");
	CHECK(saved.into.durationMs == 1200);
	CHECK(saved.outro.kind == "image");
	CHECK(saved.outro.imageMs == 4500);
	CHECK(saved.back.durationMs == 800);

	const std::optional<BufferSettings> loaded = tapeloop::loadSettings(saved, kCanvas);
	REQUIRE(loaded);
	CHECK(loaded->air == settings.air);

	CHECK(tapeloop::saveSettings(BufferSettings{}).intro.kind == "none");
}

TEST_CASE("loaded on-air settings are brought back into range")
{
	SavedSettings saved;
	saved.intro = {"slideshow", "show.json", 3000};
	saved.into = {"Fade", 0};
	saved.outro = {"image", "logo.png", INT64_MAX};
	saved.back = {"Cut", INT64_MAX};

	const std::optional<BufferSettings> loaded = tapeloop::loadSettings(saved, kCanvas);
	REQUIRE(loaded);
	CHECK(loaded->air.intro.kind == AirMediaKind::None);
	CHECK(loaded->air.into.duration == 50ms);
	CHECK(loaded->air.outro.imageTime == 60s);
	CHECK(loaded->air.back.duration == 20s);

	saved.outro.imageMs = -5;
	CHECK(tapeloop::loadSettings(saved, kCanvas)->air.outro.imageTime == 500ms);
}

TEST_CASE("buffer lengths and fixed heights stay within what the dock offers")
{
	CHECK(tapeloop::clampBufferLength(1s) == 10s);
	CHECK(tapeloop::clampBufferLength(61s) == 61s);
	CHECK(tapeloop::clampBufferLength(1h) == 300s);
	for (const uint32_t height : {360u, 480u, 720u, 1080u, 2160u}) {
		CHECK(tapeloop::isFixedHeight(height));
	}
	CHECK_FALSE(tapeloop::isFixedHeight(0));
	CHECK_FALSE(tapeloop::isFixedHeight(1440));
}

TEST_CASE("settings of a UUID that is gone move to the one source with its saved name")
{
	BufferSettings settings;
	settings.sources["old-camera"] = {true, 30s, std::nullopt, std::nullopt};
	settings.sources["old-wide"] = {false, std::nullopt, ReplayResolution{ResolutionMode::Fixed, 720},
					std::nullopt};
	settings.sources["kept"] = {true, std::nullopt, std::nullopt, std::nullopt};
	const std::map<std::string, std::string> names = {{"old-camera", "Camera"},
							  {"old-wide", "Wide"},
							  {"kept", "Kept"}};
	const std::vector<SourceIdentity> sources = {{"new-camera", "Camera"},
						     {"new-wide", "Wide"},
						     {"kept", "Kept"},
						     {"other", "Other"}};

	const auto moves = tapeloop::matchSourcesByName(settings, names, sources);
	CHECK(moves == std::map<std::string, std::string>{{"old-camera", "new-camera"}, {"old-wide", "new-wide"}});
	CHECK(settings.sources.size() == 3);
	CHECK(settings.sources.at("new-camera") == SourceSettings{true, 30s, std::nullopt, std::nullopt});
	CHECK(settings.sources.at("new-wide").resolution == ReplayResolution{ResolutionMode::Fixed, 720});
	CHECK(settings.sources.at("kept").selected);
}

TEST_CASE("a saved name matches nothing when the choice is not clear")
{
	const std::map<std::string, std::string> names = {{"gone", "Camera"},
							  {"also-gone", "Camera"},
							  {"unnamed", ""},
							  {"lost", "Lost"}};

	SECTION("two sources share the name")
	{
		BufferSettings settings;
		settings.sources["gone"] = {true, std::nullopt, std::nullopt, std::nullopt};
		const auto moves =
			tapeloop::matchSourcesByName(settings, names, {{"first", "Camera"}, {"second", "Camera"}});
		CHECK(moves.empty());
		CHECK(settings.sources.contains("gone"));
	}

	SECTION("two missing UUIDs claim the same source")
	{
		BufferSettings settings;
		settings.sources["gone"] = {true, std::nullopt, std::nullopt, std::nullopt};
		settings.sources["also-gone"] = {false, 30s, std::nullopt, std::nullopt};
		const auto moves = tapeloop::matchSourcesByName(settings, names, {{"camera", "Camera"}});
		CHECK(moves.empty());
		CHECK(settings.sources.size() == 2);
	}

	SECTION("the source with the name has settings of its own")
	{
		BufferSettings settings;
		settings.sources["gone"] = {true, 30s, std::nullopt, std::nullopt};
		settings.sources["camera"] = {false, std::nullopt, std::nullopt, std::nullopt};
		const auto moves = tapeloop::matchSourcesByName(settings, names, {{"camera", "Camera"}});
		CHECK(moves.empty());
		CHECK_FALSE(settings.sources.at("camera").selected);
		CHECK(settings.sources.contains("gone"));
	}

	SECTION("no name was saved, or no source has it")
	{
		BufferSettings settings;
		settings.sources["unnamed"] = {true, std::nullopt, std::nullopt, std::nullopt};
		settings.sources["lost"] = {true, std::nullopt, std::nullopt, std::nullopt};
		settings.sources["unknown"] = {true, std::nullopt, std::nullopt, std::nullopt};
		const auto moves = tapeloop::matchSourcesByName(settings, names, {{"camera", ""}, {"x", "Camera"}});
		CHECK(moves.empty());
		CHECK(settings.sources.size() == 3);
	}
}

TEST_CASE("activation of sources off air is global with a per source override")
{
	BufferSettings settings;
	CHECK_FALSE(settings.activateFor("a"));
	settings.sources["a"].activateOffAir = true;
	settings.sources["b"].selected = true;
	CHECK(settings.activateFor("a"));
	CHECK_FALSE(settings.activateFor("b"));

	settings.activateOffAir = true;
	settings.sources["c"].activateOffAir = false;
	CHECK(settings.activateFor("b"));
	CHECK_FALSE(settings.activateFor("c"));
	CHECK(settings.activateFor("unknown"));

	const auto loaded = tapeloop::loadSettings(tapeloop::saveSettings(settings), kCanvas);
	REQUIRE(loaded);
	CHECK(*loaded == settings);
	CHECK(loaded->sources.at("a").activateOffAir == true);
	CHECK_FALSE(loaded->sources.at("b").activateOffAir.has_value());
}

TEST_CASE("only a capturable source takes the settings of a name, but any source counts as present")
{
	BufferSettings settings;
	settings.sources["gone"] = {true, std::nullopt, std::nullopt, std::nullopt};
	settings.sources["audio"] = {false, 30s, std::nullopt, std::nullopt};
	const std::map<std::string, std::string> names = {{"gone", "Mic"}, {"audio", "Mic"}};

	SECTION("no capturable source has the name")
	{
		const auto moves = tapeloop::matchSourcesByName(settings, names, {{"audio", "Mic", false}});
		CHECK(moves.empty());
		CHECK(settings.sources.contains("gone"));
	}
	SECTION("a capturable source has it")
	{
		// Were the audio input taken as missing, it would claim the camera too and
		// neither would move.
		const auto moves = tapeloop::matchSourcesByName(settings, names,
								{{"audio", "Mic", false}, {"camera", "Mic", true}});
		CHECK(moves == std::map<std::string, std::string>{{"gone", "camera"}});
	}
	CHECK(settings.sources.at("audio").length == 30s);
}

TEST_CASE("overlays are saved with the settings and rescaled to the canvas they load at")
{
	BufferSettings settings;
	settings.overlays.canvas = {1280, 720};
	tapeloop::OverlayItem logo;
	logo.uuid = "logo";
	logo.name = "Logo";
	logo.anchor = tapeloop::OverlayAnchor::BottomRight;
	logo.x = 40;
	logo.y = 20;
	logo.width = 320;
	logo.height = 180;
	logo.opacity = 60;
	settings.overlays.items.push_back(logo);

	const SavedSettings saved = tapeloop::saveSettings(settings);
	CHECK(saved.overlays.canvasWidth == 1280);
	REQUIRE(saved.overlays.items.size() == 1);
	CHECK(saved.overlays.items[0].anchor == "bottom_right");

	const auto same = tapeloop::loadSettings(saved, {1280, 720});
	REQUIRE(same);
	CHECK(*same == settings);

	const auto larger = tapeloop::loadSettings(saved, kCanvas);
	REQUIRE(larger);
	CHECK(larger->overlays.canvas == kCanvas);
	REQUIRE(larger->overlays.items.size() == 1);
	const tapeloop::OverlayItem &scaled = larger->overlays.items[0];
	CHECK(scaled.x == 60);
	CHECK(scaled.y == 30);
	CHECK(scaled.width == 480);
	CHECK(scaled.height == 270);
	CHECK(scaled.opacity == 60);
}

TEST_CASE("an overlay of a UUID that is gone shows the one source with its saved name")
{
	tapeloop::OverlaySettings overlays;
	const auto add = [&overlays](const char *uuid, const char *name) {
		tapeloop::OverlayItem item;
		item.uuid = uuid;
		item.name = name;
		overlays.items.push_back(item);
	};
	add("old-logo", "Logo");
	add("old-logo", "Logo");
	add("kept", "Kept");
	add("old-score", "Score");
	add("", "Clock");

	const std::vector<SourceIdentity> sources = {{"new-logo", "Logo"},
						     {"kept", "Kept"},
						     {"new-score", "Score"},
						     {"new-clock", "Clock"}};
	const auto moves = tapeloop::matchOverlaysByName(overlays, sources);
	CHECK(moves ==
	      std::map<std::string, std::string>{{"Logo", "new-logo"}, {"Score", "new-score"}, {"Clock", "new-clock"}});
	CHECK(overlays.items[0].uuid == "new-logo");
	CHECK(overlays.items[1].uuid == "new-logo");
	CHECK(overlays.items[2].uuid == "kept");
	CHECK(overlays.items[3].uuid == "new-score");
	CHECK(overlays.items[4].uuid == "new-clock");
}

TEST_CASE("an overlay's saved name matches nothing when the choice is not clear")
{
	tapeloop::OverlaySettings overlays;
	const auto add = [&overlays](const char *uuid, const char *name) {
		tapeloop::OverlayItem item;
		item.uuid = uuid;
		item.name = name;
		overlays.items.push_back(item);
	};

	SECTION("two sources share the name")
	{
		add("gone", "Logo");
		CHECK(tapeloop::matchOverlaysByName(overlays, {{"a", "Logo"}, {"b", "Logo"}}).empty());
	}
	SECTION("two missing UUIDs claim the same source")
	{
		add("gone", "Logo");
		add("also-gone", "Logo");
		CHECK(tapeloop::matchOverlaysByName(overlays, {{"logo", "Logo"}}).empty());
		CHECK(overlays.items[1].uuid == "also-gone");
	}
	SECTION("the source with the name cannot be shown")
	{
		add("gone", "Mic");
		CHECK(tapeloop::matchOverlaysByName(overlays, {{"mic", "Mic", false}}).empty());
	}
	SECTION("no name was saved")
	{
		add("gone", "");
		CHECK(tapeloop::matchOverlaysByName(overlays, {{"logo", ""}}).empty());
	}
	CHECK(overlays.items[0].uuid == "gone");
}

TEST_CASE("an overlay whose source is there stays on it, even shared with a moved one")
{
	tapeloop::OverlaySettings overlays;
	tapeloop::OverlayItem item;
	item.uuid = "logo";
	item.name = "Old name";
	overlays.items.push_back(item);
	item.uuid = "gone";
	item.name = "Logo";
	overlays.items.push_back(item);

	const auto moves = tapeloop::matchOverlaysByName(overlays, {{"logo", "Logo"}});
	CHECK(moves == std::map<std::string, std::string>{{"Logo", "logo"}});
	CHECK(overlays.items[0].uuid == "logo");
	CHECK(overlays.items[1].uuid == "logo");
}
