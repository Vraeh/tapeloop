// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/BufferSettings.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

using namespace std::chrono_literals;
using tapeloop::BufferSettings;
using tapeloop::ReplayResolution;
using tapeloop::ResolutionMode;
using tapeloop::SavedSettings;
using tapeloop::SavedSource;
using tapeloop::SourceSettings;

TEST_CASE("buffer settings start from the defaults")
{
	const BufferSettings settings;
	CHECK(settings.length == 60s);
	CHECK(settings.resolution.mode == ResolutionMode::Canvas);
	CHECK(settings.startWithOutputs);
	CHECK(settings.selectedSources().empty());
}

TEST_CASE("per source overrides take the place of the global settings")
{
	BufferSettings settings;
	settings.length = 90s;
	settings.resolution = {ResolutionMode::Fixed, 720};
	settings.sources["a"] = {true, 30s, ReplayResolution{ResolutionMode::Output, 1080}};
	settings.sources["b"] = {true, std::nullopt, std::nullopt};
	settings.sources["c"] = {false, 120s, std::nullopt};

	CHECK(settings.lengthFor("a") == 30s);
	CHECK(settings.resolutionFor("a").mode == ResolutionMode::Output);
	CHECK(settings.lengthFor("b") == 90s);
	CHECK(settings.resolutionFor("b").height == 720);
	CHECK(settings.lengthFor("unknown") == 90s);
	CHECK(settings.selectedSources() == std::vector<std::string>{"a", "b"});
}

TEST_CASE("buffer settings survive a save and a load")
{
	BufferSettings settings;
	settings.length = 45s;
	settings.resolution = {ResolutionMode::Output, 1080};
	settings.startWithOutputs = false;
	settings.sources["a"] = {true, 120s, ReplayResolution{ResolutionMode::Fixed, 2160}};
	settings.sources["b"] = {false, std::nullopt, ReplayResolution{ResolutionMode::Canvas, 1080}};
	settings.sources["c"] = {true, std::nullopt, std::nullopt};

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
	saved.sources.push_back({"a", "Camera", true, 100'000, std::string("fixed"), 999});
	saved.sources.push_back({"b", "", true, -40, std::string("fixed"), std::nullopt});
	saved.sources.push_back({"", "Nameless", true, std::nullopt, std::nullopt, std::nullopt});

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
	for (const uint32_t height : {360u, 480u, 720u, 1080u, 2160u})
		CHECK(tapeloop::isFixedHeight(height));
	CHECK_FALSE(tapeloop::isFixedHeight(0));
	CHECK_FALSE(tapeloop::isFixedHeight(1440));
}
