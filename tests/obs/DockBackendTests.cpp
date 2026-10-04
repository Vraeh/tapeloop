// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "ObsFixture.hpp"
#include "TestPattern.hpp"

#include "obs/CaptureManager.hpp"
#include "obs/ManagerDockBackend.hpp"

#include <catch2/catch_test_macros.hpp>
#include <obs.hpp>

#include <string>

using tapeloop::BufferSettings;
using tapeloop::obs::CaptureManager;
using tapeloop::obs::ManagerDockBackend;
using tapeloop::test::createTestPattern;
using tapeloop::test::ObsFixture;
using tapeloop::ui::SourceState;

namespace {

class OfflineHost : public tapeloop::obs::CaptureHost {
public:
	bool streamingActive() const override { return false; }
	bool recordingActive() const override { return false; }
};

} // namespace

TEST_CASE_METHOD(ObsFixture, "the dock sees the video inputs of the scene collection", "[obs][dock]")
{
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_int(settings, "width", 320);
	obs_data_set_int(settings, "height", 180);
	OBSSourceAutoRelease second = obs_source_create(tapeloop::test::kTestPatternId, "B camera", settings, nullptr);
	OBSSourceAutoRelease first = obs_source_create(tapeloop::test::kTestPatternId, "a camera", settings, nullptr);
	OBSSourceAutoRelease audio = obs_source_create(tapeloop::test::kSilenceId, "Microphone", nullptr, nullptr);
	OBSSceneAutoRelease scene = obs_scene_create("Scene");

	OfflineHost host;
	CaptureManager manager(host);
	ManagerDockBackend backend(manager);

	auto sources = backend.sources();
	// Audio inputs and scenes are not listed; names sort regardless of case.
	REQUIRE(sources.size() == 2);
	CHECK(sources[0].name == "a camera");
	CHECK(sources[1].name == "B camera");
	CHECK(sources[0].uuid == obs_source_get_uuid(first));
	CHECK_FALSE(sources[0].selected);

	BufferSettings selected = backend.settings();
	selected.sources[sources[1].uuid].selected = true;
	backend.setSettings(selected);
	CHECK(backend.manualControlEnabled());
	REQUIRE(backend.toggleRunning());
	CHECK(backend.running());

	sources = backend.sources();
	CHECK(sources[1].selected);
	CHECK(sources[1].state == SourceState::Running);
	CHECK(sources[0].state == SourceState::Stopped);

	REQUIRE(backend.toggleRunning());
	CHECK_FALSE(backend.running());
	CHECK(backend.sources()[1].state == SourceState::Stopped);

	// The main canvas keeps its scenes until they are removed, as the frontend does
	// before shutting down; libobs frees source types before canvases.
	obs_source_remove(obs_scene_get_source(scene));
}
