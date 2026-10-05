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
using tapeloop::BufferSettings;
using tapeloop::ReplayResolution;
using tapeloop::ResolutionMode;
using tapeloop::SavedSettings;
using tapeloop::SavedSource;
using tapeloop::SourceIdentity;
using tapeloop::SourceSettings;

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
	settings.sources["a"] = {true, 120s, ReplayResolution{ResolutionMode::Fixed, 2160}, std::nullopt};
	settings.sources["b"] = {false, std::nullopt, ReplayResolution{ResolutionMode::Canvas, 1080}, std::nullopt};
	settings.sources["c"] = {true, std::nullopt, std::nullopt, std::nullopt};

	const SavedSettings saved = tapeloop::saveSettings(settings);
	CHECK(saved.version == tapeloop::kSettingsVersion);
	CHECK(saved.lengthSeconds == 45);
	CHECK(saved.resolution == "output");
	REQUIRE(saved.sources.size() == 3);
	CHECK(saved.sources[0].uuid == "a");
	CHECK(saved.sources[0].resolution == "fixed");
	CHECK(saved.sources[0].height == 2160);
	CHECK_FALSE(saved.sources[2].lengthSeconds);

	const std::optional<BufferSettings> loaded = tapeloop::loadSettings(saved);
	REQUIRE(loaded);
	CHECK(*loaded == settings);
}

TEST_CASE("settings of another version are not loaded")
{
	SavedSettings saved = tapeloop::saveSettings(BufferSettings{});
	saved.version = 2;
	CHECK_FALSE(tapeloop::loadSettings(saved));
	saved.version = 0;
	CHECK_FALSE(tapeloop::loadSettings(saved));
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

	const std::optional<BufferSettings> loaded = tapeloop::loadSettings(saved);
	REQUIRE(loaded);
	CHECK(loaded->length == 10s);
	CHECK(loaded->resolution.mode == ResolutionMode::Canvas);
	REQUIRE(loaded->sources.size() == 2);
	CHECK(loaded->sources.at("a").length == 300s);
	CHECK(loaded->sources.at("a").resolution->mode == ResolutionMode::Canvas);
	CHECK(loaded->sources.at("b").length == 10s);
	CHECK(loaded->sources.at("b").resolution->mode == ResolutionMode::Canvas);

	saved.lengthSeconds = INT64_MAX;
	CHECK(tapeloop::loadSettings(saved)->length == 300s);
	saved.lengthSeconds = INT64_MIN;
	CHECK(tapeloop::loadSettings(saved)->length == 10s);
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

	const auto loaded = tapeloop::loadSettings(tapeloop::saveSettings(settings));
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
