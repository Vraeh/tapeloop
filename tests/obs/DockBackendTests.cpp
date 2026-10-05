// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "ObsFixture.hpp"
#include "TestEncoders.hpp"
#include "TestPattern.hpp"

#include "obs/CaptureManager.hpp"
#include "obs/ManagerDockBackend.hpp"

#include <catch2/catch_test_macros.hpp>
#include <obs.hpp>

#include <algorithm>
#include <chrono>
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
	void requestSave() override { ++saves; }

	int saves = 0;
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

	// The encoders a replay can use, as OBS names them, and none that encodes what a
	// replay cannot hold.
	const auto choices = backend.encoderChoices();
	const auto x264 = std::find_if(choices.begin(), choices.end(), [](const tapeloop::ui::EncoderChoice &choice) {
		return choice.id == "obs_x264";
	});
	REQUIRE(x264 != choices.end());
	CHECK_FALSE(x264->name.empty());
	CHECK(std::none_of(choices.begin(), choices.end(), [](const tapeloop::ui::EncoderChoice &choice) {
		return choice.id == tapeloop::test::kAv1EncoderId;
	}));

	// The main canvas keeps its scenes until they are removed, as the frontend does
	// before shutting down; libobs frees source types before canvases.
	obs_source_remove(obs_scene_get_source(scene));
}

TEST_CASE_METHOD(ObsFixture, "the dock sees a source waiting for its size", "[obs][dock]")
{
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_int(settings, "width", 320);
	obs_data_set_int(settings, "height", 180);
	obs_data_set_bool(settings, "size_only_when_shown", true);
	OBSSourceAutoRelease display = obs_source_create(tapeloop::test::kTestPatternId, "Display", settings, nullptr);

	OfflineHost host;
	CaptureManager manager(host);
	ManagerDockBackend backend(manager);
	BufferSettings selected = backend.settings();
	selected.startWithOutputs = false;
	selected.sources[obs_source_get_uuid(display)].selected = true;
	backend.setSettings(selected);
	REQUIRE(backend.toggleRunning());

	const auto sources = backend.sources();
	REQUIRE(sources.size() == 1);
	CHECK(sources[0].state == SourceState::Waiting);
	REQUIRE(backend.toggleRunning());
}

TEST_CASE_METHOD(ObsFixture, "the dock sees a source left out of activation", "[obs][dock]")
{
	OBSSourceAutoRelease camera = createTestPattern(320, 180, "Camera");
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_int(settings, "width", 320);
	obs_data_set_int(settings, "height", 180);
	OBSSourceAutoRelease clip = obs_source_create(tapeloop::test::kMediaStandInId, "Clip", settings, nullptr);

	OfflineHost host;
	CaptureManager manager(host);
	ManagerDockBackend backend(manager);
	BufferSettings selected = backend.settings();
	selected.startWithOutputs = false;
	selected.activateOffAir = true;
	selected.sources[obs_source_get_uuid(camera)].selected = true;
	selected.sources[obs_source_get_uuid(clip)].selected = true;
	backend.setSettings(selected);
	REQUIRE(backend.toggleRunning());

	const auto sources = backend.sources();
	REQUIRE(sources.size() == 2);
	CHECK_FALSE(sources[0].activationLeftOut);
	CHECK(sources[1].activationLeftOut);
	REQUIRE(backend.toggleRunning());
}

TEST_CASE_METHOD(ObsFixture, "every settings change from the dock asks the host for one save", "[obs][dock]")
{
	OBSSourceAutoRelease camera = createTestPattern(320, 180, "Camera");
	OfflineHost host;
	CaptureManager manager(host);
	ManagerDockBackend backend(manager);

	BufferSettings settings = backend.settings();
	settings.startWithOutputs = false;
	settings.sources[obs_source_get_uuid(camera)].selected = true;
	backend.setSettings(settings);
	CHECK(host.saves == 1);
	settings.length = std::chrono::seconds(90);
	backend.setSettings(settings);
	CHECK(host.saves == 2);

	// Starting and stopping the buffers is not an edit of the settings.
	REQUIRE(backend.toggleRunning());
	CHECK(host.saves == 2);

	// Nor is what an edit starts or stops while the buffers run.
	settings.sources[obs_source_get_uuid(camera)].selected = false;
	backend.setSettings(settings);
	CHECK(host.saves == 3);
	settings.sources[obs_source_get_uuid(camera)].selected = true;
	backend.setSettings(settings);
	CHECK(host.saves == 4);
	REQUIRE(backend.toggleRunning());
	CHECK(host.saves == 4);
}
