// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "ObsFixture.hpp"
#include "TestPattern.hpp"

#include "obs/CaptureManager.hpp"
#include "obs/SettingsData.hpp"

#include <catch2/catch_test_macros.hpp>
#include <obs.hpp>

#include <chrono>
#include <string>

using namespace std::chrono_literals;
using tapeloop::BufferSettings;
using tapeloop::FrameSize;
using tapeloop::ReplayResolution;
using tapeloop::ResolutionMode;
using tapeloop::SourceSettings;
using tapeloop::obs::CaptureManager;
using tapeloop::obs::CaptureState;
using tapeloop::test::createTestPattern;
using tapeloop::test::ObsFixture;
using tapeloop::test::waitFor;

namespace {

// Stands in for the frontend: the test sets the outputs.
class FakeHost : public tapeloop::obs::CaptureHost {
public:
	bool streaming = false;
	bool recording = false;

	bool streamingActive() const override { return streaming; }
	bool recordingActive() const override { return recording; }
};

std::string uuidOf(obs_source_t *source)
{
	return obs_source_get_uuid(source);
}

BufferSettings selecting(const std::string &uuid)
{
	BufferSettings settings;
	settings.sources[uuid].selected = true;
	return settings;
}

bool hasGops(const CaptureManager &manager, const std::string &uuid, size_t count)
{
	const tapeloop::SourceBuffer *buffer = manager.buffer(uuid);
	return buffer && buffer->stats().gopCount >= count;
}

} // namespace

TEST_CASE_METHOD(ObsFixture, "the manager follows streaming and recording", "[obs][manager]")
{
	OBSSourceAutoRelease pattern = createTestPattern(640, 360);
	const std::string uuid = uuidOf(pattern);
	FakeHost host;
	CaptureManager manager(host);
	manager.setSettings(selecting(uuid));
	CHECK_FALSE(manager.running());
	CHECK(manager.buffer(uuid) == nullptr);

	manager.onStreaming(true);
	CHECK(manager.running());
	CHECK(manager.status(uuid).selected);
	CHECK(manager.status(uuid).stats.state == CaptureState::Running);
	CHECK_FALSE(manager.manualStop());
	REQUIRE(waitFor([&] { return hasGops(manager, uuid, 2); }, 60s));

	manager.onRecording(true);
	manager.onStreaming(false);
	CHECK(manager.status(uuid).stats.state == CaptureState::Running);
	manager.onRecording(false);
	CHECK(manager.status(uuid).stats.state == CaptureState::Stopped);
	// The buffer stays until the next start.
	CHECK(hasGops(manager, uuid, 2));
}

TEST_CASE_METHOD(ObsFixture, "the manual control drives the buffers without auto start", "[obs][manager]")
{
	OBSSourceAutoRelease pattern = createTestPattern(640, 360);
	const std::string uuid = uuidOf(pattern);
	FakeHost host;
	CaptureManager manager(host);
	BufferSettings settings = selecting(uuid);
	settings.startWithOutputs = false;
	manager.setSettings(settings);
	manager.onStreaming(true);
	CHECK_FALSE(manager.running());
	CHECK(manager.manualControlEnabled());

	REQUIRE(manager.manualStart());
	CHECK(manager.status(uuid).stats.state == CaptureState::Running);
	REQUIRE(manager.manualStop());
	CHECK(manager.status(uuid).stats.state == CaptureState::Stopped);

	settings.startWithOutputs = true;
	manager.setSettings(settings);
	CHECK(manager.running());
	CHECK_FALSE(manager.manualStop());
}

TEST_CASE_METHOD(ObsFixture, "selecting and unselecting a source while the buffers run", "[obs][manager]")
{
	OBSSourceAutoRelease first = createTestPattern(640, 360);
	OBSSourceAutoRelease second = createTestPattern(320, 180);
	FakeHost host;
	CaptureManager manager(host);
	REQUIRE(manager.manualStart());

	BufferSettings settings = selecting(uuidOf(first));
	manager.setSettings(settings);
	CHECK(manager.status(uuidOf(first)).stats.state == CaptureState::Running);

	settings.sources[uuidOf(second)].selected = true;
	manager.setSettings(settings);
	CHECK(manager.status(uuidOf(second)).stats.state == CaptureState::Running);
	CHECK(manager.status(uuidOf(second)).stats.outputSize == FrameSize{320, 180});

	settings.sources[uuidOf(first)].selected = false;
	manager.setSettings(settings);
	CHECK(manager.buffer(uuidOf(first)) == nullptr);
	CHECK(manager.status(uuidOf(second)).stats.state == CaptureState::Running);
}

TEST_CASE_METHOD(ObsFixture, "scene collection cleanup lets go of every source", "[obs][manager]")
{
	OBSSourceAutoRelease first = createTestPattern(640, 360);
	OBSSourceAutoRelease second = createTestPattern(320, 180);
	const std::string firstUuid = uuidOf(first);
	const std::string secondUuid = uuidOf(second);
	OBSWeakSourceAutoRelease firstWeak = obs_source_get_weak_source(first);
	OBSWeakSourceAutoRelease secondWeak = obs_source_get_weak_source(second);
	FakeHost host;
	CaptureManager manager(host);
	BufferSettings settings = selecting(firstUuid);
	settings.sources[secondUuid].selected = true;
	manager.setSettings(settings);
	manager.onStreaming(true);
	REQUIRE(manager.status(firstUuid).stats.state == CaptureState::Running);
	REQUIRE(manager.status(secondUuid).stats.state == CaptureState::Running);

	// With the test's own references gone, the captures still keep the sources alive.
	first = nullptr;
	second = nullptr;
	CHECK_FALSE(obs_weak_source_expired(firstWeak));
	CHECK_FALSE(obs_weak_source_expired(secondWeak));

	manager.onSceneCollectionCleanup();
	CHECK(obs_weak_source_expired(firstWeak));
	CHECK(obs_weak_source_expired(secondWeak));
	CHECK(manager.buffer(firstUuid) == nullptr);
	CHECK(manager.settings().sources.empty());
}

TEST_CASE_METHOD(ObsFixture, "a source that changes size is captured again at its new size", "[obs][manager]")
{
	OBSSourceAutoRelease pattern = createTestPattern(640, 360);
	const std::string uuid = uuidOf(pattern);
	FakeHost host;
	CaptureManager manager(host);
	manager.setSettings(selecting(uuid));
	REQUIRE(manager.manualStart());
	REQUIRE(waitFor([&] { return hasGops(manager, uuid, 2); }, 60s));
	const tapeloop::SourceBuffer *buffer = manager.buffer(uuid);

	OBSDataAutoRelease smaller = obs_data_create();
	obs_data_set_int(smaller, "width", 320);
	obs_data_set_int(smaller, "height", 180);
	obs_source_update(pattern, smaller);
	REQUIRE(waitFor([&] { return obs_source_get_width(pattern) == 320; }, 5s));

	manager.poll();
	CHECK(manager.status(uuid).stats.outputSize == FrameSize{320, 180});
	CHECK(manager.buffer(uuid) == buffer);
	const auto newest = buffer->stats().newestTime;
	REQUIRE(waitFor([&] { return buffer->stats().newestTime > newest + 1s; }, 60s));
	CHECK(buffer->stats().discontinuities == 1);
}

TEST_CASE_METHOD(ObsFixture, "a removed source stops its capture and stays stopped", "[obs][manager]")
{
	OBSSourceAutoRelease pattern = createTestPattern(640, 360);
	const std::string uuid = uuidOf(pattern);
	FakeHost host;
	CaptureManager manager(host);
	manager.setSettings(selecting(uuid));
	REQUIRE(manager.manualStart());
	REQUIRE(manager.status(uuid).stats.state == CaptureState::Running);

	// The test still holds the source, so it can be found by its UUID.
	obs_source_remove(pattern);
	CHECK(manager.buffer(uuid) != nullptr);
	manager.poll();
	CHECK(manager.buffer(uuid) == nullptr);
	manager.poll();
	CHECK(manager.buffer(uuid) == nullptr);
	manager.setSettings(manager.settings());
	CHECK(manager.buffer(uuid) == nullptr);
}

TEST_CASE_METHOD(ObsFixture, "a retried start empties the buffer when the first try would have", "[obs][manager]")
{
	OBSSourceAutoRelease pattern = createTestPattern(640, 360);
	const std::string uuid = uuidOf(pattern);
	FakeHost host;
	CaptureManager manager(host);
	manager.setSettings(selecting(uuid));
	REQUIRE(manager.manualStart());
	REQUIRE(waitFor([&] { return hasGops(manager, uuid, 1); }, 60s));
	REQUIRE(manager.manualStop());
	const auto previous = manager.buffer(uuid)->stats().newestTime;

	OBSDataAutoRelease sizeless = obs_data_create();
	obs_data_set_int(sizeless, "width", 0);
	obs_data_set_int(sizeless, "height", 0);
	obs_source_update(pattern, sizeless);
	REQUIRE(waitFor([&] { return obs_source_get_width(pattern) == 0; }, 5s));
	REQUIRE(manager.manualStart());
	CHECK(manager.status(uuid).stats.state == CaptureState::Stopped);

	OBSDataAutoRelease sized = obs_data_create();
	obs_data_set_int(sized, "width", 640);
	obs_data_set_int(sized, "height", 360);
	obs_source_update(pattern, sized);
	REQUIRE(waitFor([&] { return obs_source_get_width(pattern) == 640; }, 5s));
	manager.poll();
	REQUIRE(manager.status(uuid).stats.state == CaptureState::Running);
	REQUIRE(waitFor([&] { return hasGops(manager, uuid, 1); }, 60s));
	CHECK(manager.buffer(uuid)->stats().oldestTime > previous);
	CHECK(manager.buffer(uuid)->stats().discontinuities == 0);
}

TEST_CASE_METHOD(ObsFixture, "a source removed while stopped gives up its buffer at the next start", "[obs][manager]")
{
	OBSSourceAutoRelease pattern = createTestPattern(640, 360);
	const std::string uuid = uuidOf(pattern);
	FakeHost host;
	CaptureManager manager(host);
	manager.setSettings(selecting(uuid));
	REQUIRE(manager.manualStart());
	REQUIRE(manager.manualStop());
	REQUIRE(manager.buffer(uuid) != nullptr);

	obs_source_remove(pattern);
	REQUIRE(manager.manualStart());
	CHECK(manager.buffer(uuid) == nullptr);
	manager.poll();
	CHECK(manager.buffer(uuid) == nullptr);
}

TEST_CASE_METHOD(ObsFixture, "a scene collection loaded while live starts over with its own settings", "[obs][manager]")
{
	OBSSourceAutoRelease pattern = createTestPattern(640, 360);
	const std::string uuid = uuidOf(pattern);
	FakeHost host;
	CaptureManager manager(host);
	manager.setSettings(selecting(uuid));
	host.streaming = true;
	manager.onStreaming(true);
	REQUIRE(manager.status(uuid).stats.state == CaptureState::Running);

	BufferSettings manual = selecting(uuid);
	manual.startWithOutputs = false;
	OBSDataAutoRelease collection = obs_data_create();
	{
		CaptureManager writer(host);
		writer.setSettings(manual);
		writer.save(collection);
	}
	manager.onSceneCollectionCleanup();
	manager.load(collection);
	CHECK_FALSE(manager.running());
	CHECK(manager.buffer(uuid) == nullptr);
	CHECK(manager.manualStart());
	CHECK(manager.status(uuid).stats.state == CaptureState::Running);
	host.streaming = false;
	manager.onStreaming(false);
	CHECK(manager.running());
}

TEST_CASE_METHOD(ObsFixture, "after exit the manager starts nothing", "[obs][manager]")
{
	OBSSourceAutoRelease pattern = createTestPattern(640, 360);
	const std::string uuid = uuidOf(pattern);
	FakeHost host;
	CaptureManager manager(host);
	manager.setSettings(selecting(uuid));
	REQUIRE(manager.manualStart());
	manager.onExit();
	CHECK(manager.buffer(uuid) == nullptr);
	manager.poll();
	manager.setSettings(selecting(uuid));
	CHECK(manager.buffer(uuid) == nullptr);
}

TEST_CASE_METHOD(ObsFixture, "settings reach a running capture at its next start", "[obs][manager]")
{
	OBSSourceAutoRelease pattern = createTestPattern(1280, 720);
	const std::string uuid = uuidOf(pattern);
	FakeHost host;
	CaptureManager manager(host);
	BufferSettings settings = selecting(uuid);
	settings.resolution = {ResolutionMode::Fixed, 720};
	manager.setSettings(settings);
	REQUIRE(manager.manualStart());
	REQUIRE(manager.status(uuid).stats.outputSize == FrameSize{1280, 720});

	settings.resolution = {ResolutionMode::Fixed, 360};
	manager.setSettings(settings);
	CHECK(manager.status(uuid).stats.outputSize == FrameSize{1280, 720});
	REQUIRE(manager.manualStop());
	REQUIRE(manager.manualStart());
	CHECK(manager.status(uuid).stats.outputSize == FrameSize{640, 360});
}

TEST_CASE_METHOD(ObsFixture, "the manager catches up with outputs it was not told about", "[obs][manager]")
{
	OBSSourceAutoRelease pattern = createTestPattern(640, 360);
	const std::string uuid = uuidOf(pattern);
	FakeHost host;
	CaptureManager manager(host);
	manager.setSettings(selecting(uuid));
	host.recording = true;
	manager.poll();
	CHECK(manager.running());
	CHECK(manager.status(uuid).stats.state == CaptureState::Running);
	host.recording = false;
	manager.poll();
	CHECK_FALSE(manager.running());
}

TEST_CASE_METHOD(ObsFixture, "a source too small to start is retried", "[obs][manager]")
{
	OBSSourceAutoRelease pattern = createTestPattern(0, 0);
	const std::string uuid = uuidOf(pattern);
	FakeHost host;
	CaptureManager manager(host);
	manager.setSettings(selecting(uuid));
	REQUIRE(manager.manualStart());
	CHECK(manager.status(uuid).stats.state == CaptureState::Stopped);

	OBSDataAutoRelease sized = obs_data_create();
	obs_data_set_int(sized, "width", 640);
	obs_data_set_int(sized, "height", 360);
	obs_source_update(pattern, sized);
	REQUIRE(waitFor([&] { return obs_source_get_width(pattern) == 640; }, 5s));
	manager.poll();
	CHECK(manager.status(uuid).stats.state == CaptureState::Running);
}

TEST_CASE_METHOD(ObsFixture, "settings are saved with the scene collection and loaded back", "[obs][manager]")
{
	FakeHost host;
	BufferSettings settings;
	settings.length = 90s;
	settings.resolution = {ResolutionMode::Fixed, 720};
	settings.startWithOutputs = false;
	settings.sources["9f1c0a7e-0000-4000-8000-000000000001"] = {true, 30s, std::nullopt};
	settings.sources["9f1c0a7e-0000-4000-8000-000000000002"] =
		SourceSettings{false, std::nullopt, ReplayResolution{ResolutionMode::Output, 1080}};

	OBSDataAutoRelease collection = obs_data_create();
	{
		CaptureManager manager(host);
		manager.setSettings(settings);
		manager.save(collection);
	}
	CaptureManager loaded(host);
	loaded.load(collection);
	CHECK(loaded.settings() == settings);

	OBSDataAutoRelease empty = obs_data_create();
	loaded.load(empty);
	CHECK(loaded.settings() == BufferSettings{});
}

TEST_CASE_METHOD(ObsFixture, "settings of an unknown version are kept as they came", "[obs][manager]")
{
	OBSDataAutoRelease collection = obs_data_create();
	OBSDataAutoRelease future = obs_data_create();
	obs_data_set_int(future, "version", 2);
	obs_data_set_string(future, "something", "new");
	obs_data_set_obj(collection, tapeloop::obs::kSettingsKey, future);

	FakeHost host;
	CaptureManager manager(host);
	manager.load(collection);
	CHECK(manager.settings() == BufferSettings{});

	OBSDataAutoRelease saved = obs_data_create();
	manager.save(saved);
	OBSDataAutoRelease written = obs_data_get_obj(saved, tapeloop::obs::kSettingsKey);
	CHECK(obs_data_get_int(written, "version") == 2);
	CHECK(std::string(obs_data_get_string(written, "something")) == "new");
}
