// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "../core/StoredReplays.hpp"
#include "../core/TempDirectory.hpp"
#include "AllocationCounter.hpp"
#include "LogCounter.hpp"
#include "ObsFixture.hpp"
#include "TestEncoders.hpp"
#include "TestPattern.hpp"

#include "core/FileIo.hpp"
#include "core/GopReader.hpp"
#include "core/ReplayWriter.hpp"
#include "obs/CaptureManager.hpp"
#include "obs/ManagerDockBackend.hpp"
#include "obs/SettingsData.hpp"

#include <catch2/catch_test_macros.hpp>
#include <obs.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using tapeloop::BufferSettings;
using tapeloop::FrameSize;
using tapeloop::Replay;
using tapeloop::ReplayState;
using tapeloop::ReplayResolution;
using tapeloop::ResolutionMode;
using tapeloop::SavedSettings;
using tapeloop::SavedSource;
using tapeloop::SourceSettings;
using tapeloop::obs::CaptureManager;
using tapeloop::obs::CaptureState;
using tapeloop::obs::createSettingsData;
using tapeloop::obs::readSettingsData;
using tapeloop::test::createTestPattern;
using tapeloop::test::LogCounter;
using tapeloop::test::ObsFixture;
using tapeloop::test::waitFor;

namespace {

namespace fs = std::filesystem;

// Stands in for the frontend: the test sets the outputs, and OBS records into a folder of
// its own.
class FakeHost : public tapeloop::obs::CaptureHost {
public:
	bool streaming = false;
	bool recording = false;
	tapeloop::test::TempDirectory dir;
	std::string folder = tapeloop::utf8FromPath(dir.path());
	std::string collection = "F\xC3\xBAtbol: Liga/2026";

	bool streamingActive() const override { return streaming; }
	bool recordingActive() const override { return recording; }
	void requestSave() override { ++saves; }
	std::string recordingFolder() const override { return folder; }
	std::string sceneCollectionName() const override { return collection; }

	int saves = 0;
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

const tapeloop::StoredSource &sourceOf(const Replay &replay, const std::string &uuid)
{
	for (const tapeloop::StoredSource &source : replay.index->sources) {
		if (source.key == uuid) {
			return source;
		}
	}
	FAIL("no source " << uuid);
	return replay.index->sources.front();
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
	CHECK(manager.status(uuid).stats.state == CaptureState::Waiting);

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
	CHECK(manager.status(uuid).stats.state == CaptureState::Waiting);

	OBSDataAutoRelease sized = obs_data_create();
	obs_data_set_int(sized, "width", 640);
	obs_data_set_int(sized, "height", 360);
	obs_source_update(pattern, sized);
	REQUIRE(waitFor([&] { return obs_source_get_width(pattern) == 640; }, 5s));
	manager.poll();
	CHECK(manager.status(uuid).stats.state == CaptureState::Running);
}

// A manager capturing one test pattern with the given encoder, running with two GOPs.
TEST_CASE_METHOD(ObsFixture, "a buffer whose chosen encoder cannot start says so", "[obs][manager]")
{
	// The NVENC stand-ins refuse to start until a test enables them, as NVENC does
	// without its driver.
	OBSSourceAutoRelease pattern = createTestPattern(320, 180, "Pattern");
	const std::string uuid = uuidOf(pattern);
	FakeHost host;
	CaptureManager manager(host);
	tapeloop::obs::ManagerDockBackend backend(manager);
	BufferSettings settings = selecting(uuid);
	settings.startWithOutputs = false;
	settings.replayEncoder = tapeloop::test::kNvencH264Id;
	manager.setSettings(settings);
	LogCounter said("the encoder chosen for replays, could not start");
	REQUIRE(manager.manualStart());
	const tapeloop::obs::CaptureStats stats = manager.status(uuid).stats;
	CHECK(stats.state == CaptureState::Running);
	CHECK(stats.encoderId != tapeloop::test::kNvencH264Id);
	CHECK(stats.choiceSkipped);
	CHECK(said.lines == 1);
	const std::vector<tapeloop::ui::DockSource> sources = backend.sources();
	const auto source = std::find_if(sources.begin(), sources.end(),
					 [&](const tapeloop::ui::DockSource &listed) { return listed.uuid == uuid; });
	REQUIRE(source != sources.end());
	CHECK(source->choiceSkipped);
	CHECK_FALSE(source->chosenEncoder);
	REQUIRE(manager.manualStop());

	// The automatic order has no choice to skip.
	settings.replayEncoder.clear();
	manager.setSettings(settings);
	REQUIRE(manager.manualStart());
	CHECK_FALSE(manager.status(uuid).stats.choiceSkipped);
	CHECK(said.lines == 1);
	manager.manualStop();
}

struct NvencCapture {
	OBSSourceAutoRelease pattern = createTestPattern(320, 180, "Pattern");
	std::string uuid = uuidOf(pattern);
	FakeHost host;
	CaptureManager manager{host};
	BufferSettings settings = selecting(uuid);

	explicit NvencCapture(const char *encoder)
	{
		tapeloop::test::enableTestNvenc();
		settings.startWithOutputs = false;
		settings.replayEncoder = encoder;
		manager.setSettings(settings);
		REQUIRE(manager.manualStart());
		REQUIRE(manager.status(uuid).stats.encoderId == encoder);
		REQUIRE(waitFor([&] { return hasGops(manager, uuid, 2); }, 60s));
	}

	void failEncoding()
	{
		tapeloop::test::failTestNvenc(true);
		REQUIRE(waitFor([&] { return manager.status(uuid).stats.state == CaptureState::Failed; }, 60s));
		tapeloop::test::failTestNvenc(false);
	}

	std::string restartedWith()
	{
		REQUIRE(manager.manualStop());
		REQUIRE(manager.manualStart());
		return manager.status(uuid).stats.encoderId;
	}
};

TEST_CASE_METHOD(ObsFixture, "a source whose HEVC encoder fails uses that vendor's H.264 from its next start",
		 "[obs][manager]")
{
	NvencCapture capture(tapeloop::test::kNvencHevcId);
	capture.manager.poll();
	CHECK_FALSE(capture.manager.status(capture.uuid).hevcFailed);

	LogCounter noted("tries H.264 first from its next start");
	capture.failEncoding();
	capture.manager.poll();
	capture.manager.poll();
	CHECK(capture.manager.status(capture.uuid).hevcFailed);
	CHECK(noted.lines == 1);

	// The choice stays as it was; the next start goes without the encoder that failed.
	CHECK(capture.restartedWith() == tapeloop::test::kNvencH264Id);
	CHECK(capture.manager.settings().replayEncoder == tapeloop::test::kNvencHevcId);
	REQUIRE(waitFor([&] { return hasGops(capture.manager, capture.uuid, 2); }, 60s));
	CHECK(capture.manager.status(capture.uuid).stats.state == CaptureState::Running);
	CHECK(capture.manager.status(capture.uuid).hevcFailed);
	const std::vector<tapeloop::ui::DockSource> rows = tapeloop::obs::ManagerDockBackend(capture.manager).sources();
	const auto row = std::find_if(rows.begin(), rows.end(), [&](const tapeloop::ui::DockSource &source) {
		return source.uuid == capture.uuid;
	});
	REQUIRE(row != rows.end());
	CHECK(row->hevcFailed);

	// An encoder chosen after the failure is the one that starts.
	capture.settings.replayEncoder = "obs_x264";
	capture.manager.setSettings(capture.settings);
	CHECK(capture.restartedWith() == "obs_x264");
	capture.manager.manualStop();
}

TEST_CASE_METHOD(ObsFixture, "an HEVC failure counts when the capture stops before a poll sees it", "[obs][manager]")
{
	NvencCapture capture(tapeloop::test::kNvencHevcId);
	capture.failEncoding();
	SECTION("stopped")
	{
		CHECK(capture.restartedWith() == tapeloop::test::kNvencH264Id);
	}
	SECTION("unticked and ticked again, as during a stream")
	{
		capture.settings.sources[capture.uuid].selected = false;
		capture.manager.setSettings(capture.settings);
		capture.settings.sources[capture.uuid].selected = true;
		capture.manager.setSettings(capture.settings);
		CHECK(capture.manager.status(capture.uuid).stats.encoderId == tapeloop::test::kNvencH264Id);
	}
	CHECK(capture.manager.status(capture.uuid).hevcFailed);
	capture.manager.manualStop();
}

TEST_CASE_METHOD(ObsFixture, "a source whose H.264 encoder fails or that cannot store a packet gets no fallback",
		 "[obs][manager]")
{
	SECTION("H.264")
	{
		NvencCapture capture(tapeloop::test::kNvencH264Id);
		capture.failEncoding();
		capture.manager.poll();
		CHECK_FALSE(capture.manager.status(capture.uuid).hevcFailed);
		CHECK(capture.restartedWith() == tapeloop::test::kNvencH264Id);
		capture.manager.manualStop();
	}
	SECTION("a packet the buffer cannot store")
	{
		if (!tapeloop::test::kAllocationFailures) {
			SKIP("allocations cannot be made to fail in this configuration");
		}
		NvencCapture capture(tapeloop::test::kNvencHevcId);
		tapeloop::test::failTestNvencStore();
		REQUIRE(waitFor(
			[&] { return capture.manager.status(capture.uuid).stats.state == CaptureState::Failed; }, 60s));
		CHECK_FALSE(capture.manager.status(capture.uuid).stats.encoderFailed);
		capture.manager.poll();
		CHECK_FALSE(capture.manager.status(capture.uuid).hevcFailed);
		CHECK(capture.restartedWith() == tapeloop::test::kNvencHevcId);
		capture.manager.manualStop();
	}
}

TEST_CASE_METHOD(ObsFixture, "settings are saved with the scene collection and loaded back", "[obs][manager]")
{
	OBSSourceAutoRelease camera = createTestPattern(640, 360, "Camera");
	OBSSourceAutoRelease wide = createTestPattern(640, 360, "Wide");
	FakeHost host;
	BufferSettings settings;
	settings.length = 90s;
	settings.resolution = {ResolutionMode::Fixed, 720};
	settings.startWithOutputs = false;
	settings.activateOffAir = true;
	settings.forceH264 = true;
	settings.replayEncoder = "obs_x264";
	settings.allowOtherAdapters = false;
	settings.sources[uuidOf(camera)] = {true, 30s, std::nullopt, std::nullopt};
	settings.sources[uuidOf(wide)] =
		SourceSettings{false, std::nullopt, ReplayResolution{ResolutionMode::Output, 1080}, false};

	OBSDataAutoRelease collection = obs_data_create();
	{
		CaptureManager manager(host);
		BufferSettings withGone = settings;
		withGone.sources["9f1c0a7e-0000-4000-8000-000000000001"] = {true, 30s, std::nullopt, std::nullopt};
		manager.setSettings(withGone);
		manager.save(collection);
	}
	OBSDataAutoRelease written = obs_data_get_obj(collection, tapeloop::obs::kSettingsKey);
	const SavedSettings saved = readSettingsData(written);
	REQUIRE(saved.sources.size() == 2);
	CHECK(saved.sources[0].name + saved.sources[1].name ==
	      (uuidOf(camera) < uuidOf(wide) ? "CameraWide" : "WideCamera"));

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

	// An edit here would not be saved, so it asks for no save.
	BufferSettings edited = manager.settings();
	edited.startWithOutputs = false;
	manager.setSettings(edited);
	CHECK(host.saves == 0);
}

namespace {

// A collection as a duplicate of it would be saved: the same names under UUIDs the new
// sources do not have.
OBSDataAutoRelease collectionNaming(const std::vector<SavedSource> &sources)
{
	SavedSettings saved;
	saved.lengthSeconds = 60;
	saved.resolution = "canvas";
	saved.startWithOutputs = false;
	saved.sources = sources;
	OBSDataAutoRelease data = createSettingsData(saved);
	OBSDataAutoRelease collection = obs_data_create();
	obs_data_set_obj(collection, tapeloop::obs::kSettingsKey, data);
	return collection;
}

SavedSource namedSource(const char *uuid, const char *name, bool selected)
{
	SavedSource source;
	source.uuid = uuid;
	source.name = name;
	source.selected = selected;
	return source;
}

} // namespace

TEST_CASE_METHOD(ObsFixture, "a duplicated scene collection keeps its selection by name", "[obs][manager]")
{
	OBSSourceAutoRelease camera = createTestPattern(640, 360, "Camera");
	OBSSourceAutoRelease wide = createTestPattern(640, 360, "Wide");
	SavedSource wideSaved = namedSource("4f8beeda-0000-4000-8000-000000000002", "Wide", false);
	wideSaved.lengthSeconds = 30;
	OBSDataAutoRelease collection =
		collectionNaming({namedSource("4f8beeda-0000-4000-8000-000000000001", "Camera", true), wideSaved,
				  namedSource("4f8beeda-0000-4000-8000-000000000003", "Deleted", true)});

	FakeHost host;
	CaptureManager manager(host);
	manager.load(collection);
	REQUIRE(manager.settings().sources.size() == 3);
	CHECK(manager.settings().sources.at(uuidOf(camera)).selected);
	CHECK(manager.settings().lengthFor(uuidOf(wide)) == 30s);
	CHECK(manager.manualStart());
	CHECK(manager.status(uuidOf(camera)).stats.state == CaptureState::Running);

	OBSDataAutoRelease saved = obs_data_create();
	manager.save(saved);
	OBSDataAutoRelease written = obs_data_get_obj(saved, tapeloop::obs::kSettingsKey);
	const SavedSettings again = readSettingsData(written);
	REQUIRE(again.sources.size() == 2);
	for (const SavedSource &source : again.sources) {
		CHECK((source.uuid == uuidOf(camera) || source.uuid == uuidOf(wide)));
	}
	manager.manualStop();
}

TEST_CASE_METHOD(ObsFixture, "two saved sources with one name find neither of them", "[obs][manager]")
{
	// libobs renames an input that would share another input's name, so the ambiguity a
	// collection can hold among inputs is two saved entries naming the same one.
	OBSSourceAutoRelease camera = createTestPattern(640, 360, "Camera");
	OBSDataAutoRelease collection =
		collectionNaming({namedSource("4f8beeda-0000-4000-8000-000000000001", "Camera", true),
				  namedSource("4f8beeda-0000-4000-8000-000000000002", "Camera", false)});

	FakeHost host;
	CaptureManager manager(host);
	manager.load(collection);
	CHECK(manager.settings().selectedSources() == std::vector<std::string>{"4f8beeda-0000-4000-8000-000000000001"});
	CHECK_FALSE(manager.status(uuidOf(camera)).selected);

	OBSDataAutoRelease saved = obs_data_create();
	manager.save(saved);
	OBSDataAutoRelease written = obs_data_get_obj(saved, tapeloop::obs::kSettingsKey);
	CHECK(readSettingsData(written).sources.empty());
}

namespace {

// Like a display or window capture: no size while nothing shows it.
OBSSourceAutoRelease createHiddenPattern(const char *name)
{
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_int(settings, "width", 640);
	obs_data_set_int(settings, "height", 360);
	obs_data_set_bool(settings, "size_only_when_shown", true);
	return obs_source_create(tapeloop::test::kTestPatternId, name, settings, nullptr);
}

} // namespace

TEST_CASE_METHOD(ObsFixture, "a source with no size until it is shown starts once the capture shows it",
		 "[obs][manager]")
{
	OBSSourceAutoRelease display = createHiddenPattern("Display");
	const std::string uuid = uuidOf(display);
	REQUIRE(obs_source_get_width(display) == 0);

	FakeHost host;
	CaptureManager manager(host);
	BufferSettings settings = selecting(uuid);
	settings.startWithOutputs = false;
	manager.setSettings(settings);
	REQUIRE(manager.manualStart());
	CHECK(manager.status(uuid).stats.state == CaptureState::Waiting);
	CHECK(obs_source_showing(display));
	CHECK_FALSE(obs_source_active(display));
	CHECK(obs_source_get_width(display) == 640);

	manager.poll();
	CHECK(manager.status(uuid).stats.state == CaptureState::Running);
	CHECK(waitFor([&] { return hasGops(manager, uuid, 1); }, 60s));

	manager.manualStop();
	CHECK_FALSE(obs_source_showing(display));
}

TEST_CASE_METHOD(ObsFixture, "a source waiting for its size is let go of whenever its capture would stop",
		 "[obs][manager]")
{
	OBSSourceAutoRelease display = createHiddenPattern("Display");
	const std::string uuid = uuidOf(display);
	FakeHost host;
	CaptureManager manager(host);
	BufferSettings settings = selecting(uuid);
	settings.startWithOutputs = false;

	SECTION("buffers stopped")
	{
		manager.setSettings(settings);
		REQUIRE(manager.manualStart());
		REQUIRE(manager.status(uuid).stats.state == CaptureState::Waiting);
		manager.manualStop();
	}
	SECTION("source unselected")
	{
		manager.setSettings(settings);
		REQUIRE(manager.manualStart());
		BufferSettings none = settings;
		none.sources.clear();
		manager.setSettings(none);
	}
	SECTION("scene collection cleanup")
	{
		manager.setSettings(settings);
		REQUIRE(manager.manualStart());
		manager.onSceneCollectionCleanup();
	}
	SECTION("exit")
	{
		manager.setSettings(settings);
		REQUIRE(manager.manualStart());
		manager.onExit();
	}
	CHECK_FALSE(obs_source_showing(display));
	CHECK(manager.status(uuid).stats.state == CaptureState::Stopped);
}

namespace {

// Watches what the program renders and mixes: frames brighter than black and audio
// above silence.
struct ProgramProbe {
	std::atomic<int> frames{0};
	std::atomic<int> brightFrames{0};
	std::atomic<int> blocks{0};
	std::atomic<int> loudBlocks{0};
	uint32_t width = 0;
	uint32_t height = 0;

	ProgramProbe()
	{
		obs_video_info video = {};
		obs_get_video_info(&video);
		width = video.output_width;
		height = video.output_height;
		obs_add_raw_video_callback(nullptr, onFrame, this);
		obs_add_raw_audio_callback(0, nullptr, onAudio, this);
	}

	~ProgramProbe()
	{
		obs_remove_raw_video_callback(onFrame, this);
		obs_remove_raw_audio_callback(0, onAudio, this);
	}

	ProgramProbe(const ProgramProbe &) = delete;
	ProgramProbe &operator=(const ProgramProbe &) = delete;

	static void onFrame(void *param, video_data *frame) noexcept
	{
		auto &probe = *static_cast<ProgramProbe *>(param);
		uint8_t brightest = 0;
		for (uint32_t y = 0; y < probe.height; y += 8) {
			for (uint32_t x = 0; x < probe.width; x += 8) {
				brightest = std::max(brightest, frame->data[0][y * frame->linesize[0] + x]);
			}
		}
		// Limited range black is 16.
		if (brightest > 40) {
			++probe.brightFrames;
		}
		++probe.frames;
	}

	static void onAudio(void *param, size_t, audio_data *data) noexcept
	{
		auto &probe = *static_cast<ProgramProbe *>(param);
		const auto *samples = reinterpret_cast<const float *>(data->data[0]);
		for (uint32_t i = 0; i < data->frames; ++i) {
			if (std::fabs(samples[i]) > 0.01f) {
				++probe.loudBlocks;
				break;
			}
		}
		++probe.blocks;
	}
};

} // namespace

TEST_CASE_METHOD(ObsFixture, "a source activated off air reaches neither the program picture nor its audio",
		 "[obs][manager]")
{
	OBSSourceAutoRelease tone = obs_source_create(tapeloop::test::kToneId, "Media", nullptr, nullptr);
	const std::string uuid = uuidOf(tone);
	ProgramProbe probe;
	FakeHost host;
	CaptureManager manager(host);
	BufferSettings settings = selecting(uuid);
	settings.startWithOutputs = false;
	settings.activateOffAir = true;
	manager.setSettings(settings);
	REQUIRE(manager.manualStart());
	CHECK(obs_source_active(tone));
	CHECK(manager.status(uuid).stats.state == CaptureState::Running);

	const int framesBefore = probe.frames;
	const int blocksBefore = probe.blocks;
	REQUIRE(waitFor([&] { return probe.frames >= framesBefore + 30 && probe.blocks >= blocksBefore + 30; }, 30s));
	CHECK(probe.brightFrames == 0);
	CHECK(probe.loudBlocks == 0);

	// The probes see the tone once it is really on air.
	obs_set_output_source(0, tone);
	CHECK(waitFor([&] { return probe.brightFrames > 0 && probe.loudBlocks > 0; }, 30s));
	obs_set_output_source(0, nullptr);

	manager.manualStop();
	CHECK_FALSE(obs_source_active(tone));
}

TEST_CASE_METHOD(ObsFixture, "a selected source is activated only when the settings ask", "[obs][manager]")
{
	OBSSourceAutoRelease pattern = createTestPattern(640, 360, "Camera");
	const std::string uuid = uuidOf(pattern);
	FakeHost host;
	CaptureManager manager(host);
	BufferSettings settings = selecting(uuid);
	settings.startWithOutputs = false;
	manager.setSettings(settings);
	REQUIRE(manager.manualStart());
	CHECK(manager.status(uuid).stats.state == CaptureState::Running);
	CHECK_FALSE(obs_source_active(pattern));
	manager.manualStop();

	settings.sources[uuid].activateOffAir = true;
	manager.setSettings(settings);
	REQUIRE(manager.manualStart());
	CHECK(obs_source_active(pattern));
	manager.manualStop();

	settings.activateOffAir = true;
	settings.sources[uuid].activateOffAir = false;
	manager.setSettings(settings);
	REQUIRE(manager.manualStart());
	CHECK_FALSE(obs_source_active(pattern));
	manager.manualStop();
}

TEST_CASE_METHOD(ObsFixture, "every activation is let go of when its capture stops", "[obs][manager]")
{
	OBSSourceAutoRelease pattern = createTestPattern(640, 360, "Camera");
	const std::string uuid = uuidOf(pattern);
	FakeHost host;
	CaptureManager manager(host);
	BufferSettings settings = selecting(uuid);
	settings.startWithOutputs = false;
	settings.activateOffAir = true;
	manager.setSettings(settings);
	REQUIRE(manager.manualStart());
	REQUIRE(obs_source_active(pattern));

	SECTION("buffers stopped")
	{
		manager.manualStop();
	}
	SECTION("source unselected")
	{
		settings.sources.clear();
		manager.setSettings(settings);
	}
	SECTION("source removed")
	{
		obs_source_remove(pattern);
		manager.poll();
	}
	SECTION("scene collection cleanup")
	{
		manager.onSceneCollectionCleanup();
	}
	SECTION("exit")
	{
		manager.onExit();
	}
	CHECK_FALSE(obs_source_active(pattern));
}

TEST_CASE_METHOD(ObsFixture, "a source that cannot be captured is not left active", "[obs][manager]")
{
	OBSSourceAutoRelease huge = createTestPattern(20000, 360, "Huge");
	const std::string uuid = uuidOf(huge);
	FakeHost host;
	CaptureManager manager(host);
	BufferSettings settings = selecting(uuid);
	settings.startWithOutputs = false;
	settings.activateOffAir = true;
	manager.setSettings(settings);
	REQUIRE(manager.manualStart());
	CHECK(manager.status(uuid).stats.state == CaptureState::Stopped);
	CHECK_FALSE(obs_source_active(huge));
}

TEST_CASE_METHOD(ObsFixture, "a scene or an input without video never takes a saved name", "[obs][manager]")
{
	// A scene can share an input's name; scenes are named per canvas. Groups are listed
	// with the inputs.
	OBSSceneAutoRelease scene = obs_scene_create("Main");
	obs_source_t *group = obs_sceneitem_get_source(obs_scene_add_group(scene, "Group"));
	OBSSourceAutoRelease mic = obs_source_create(tapeloop::test::kSilenceId, "Mic", nullptr, nullptr);
	OBSSourceAutoRelease speaker = obs_source_create(tapeloop::test::kSilenceId, "Speaker", nullptr, nullptr);
	SavedSource speakerSaved = namedSource(obs_source_get_uuid(speaker), "Speaker", false);
	speakerSaved.lengthSeconds = 30;
	OBSDataAutoRelease collection =
		collectionNaming({namedSource("4f8beeda-0000-4000-8000-000000000001", "Main", true),
				  namedSource("4f8beeda-0000-4000-8000-000000000002", "Mic", true),
				  namedSource("4f8beeda-0000-4000-8000-000000000003", "Group", true), speakerSaved});

	FakeHost host;
	CaptureManager manager(host);
	manager.load(collection);
	CHECK(manager.settings().sources.size() == 4);
	CHECK_FALSE(manager.settings().sources.contains(obs_source_get_uuid(obs_scene_get_source(scene))));
	CHECK_FALSE(manager.settings().sources.contains(obs_source_get_uuid(group)));
	CHECK_FALSE(manager.settings().sources.contains(obs_source_get_uuid(mic)));

	// An input without video is still present, so its settings are written back.
	OBSDataAutoRelease saved = obs_data_create();
	manager.save(saved);
	OBSDataAutoRelease written = obs_data_get_obj(saved, tapeloop::obs::kSettingsKey);
	const SavedSettings again = readSettingsData(written);
	REQUIRE(again.sources.size() == 1);
	CHECK(again.sources[0].uuid == obs_source_get_uuid(speaker));
	CHECK(again.sources[0].lengthSeconds == 30);

	// The main canvas keeps its scenes until they are removed, as the frontend does
	// before shutting down.
	obs_source_remove(group);
	obs_source_remove(obs_scene_get_source(scene));
}

TEST_CASE_METHOD(ObsFixture, "a sizeless source that appears later is shown on the next poll", "[obs][manager]")
{
	const std::string uuid = "4f8beeda-0000-4000-8000-000000000009";
	FakeHost host;
	CaptureManager manager(host);
	BufferSettings settings = selecting(uuid);
	settings.startWithOutputs = false;
	manager.setSettings(settings);
	REQUIRE(manager.manualStart());
	CHECK(manager.status(uuid).stats.state == CaptureState::Stopped);

	// As a source of a collection loads, with the UUID the settings name.
	OBSDataAutoRelease patternSettings = obs_data_create();
	obs_data_set_int(patternSettings, "width", 640);
	obs_data_set_int(patternSettings, "height", 360);
	obs_data_set_bool(patternSettings, "size_only_when_shown", true);
	OBSDataAutoRelease saved = obs_data_create();
	obs_data_set_string(saved, "id", tapeloop::test::kTestPatternId);
	obs_data_set_string(saved, "name", "Display");
	obs_data_set_string(saved, "uuid", uuid.c_str());
	obs_data_set_obj(saved, "settings", patternSettings);
	OBSSourceAutoRelease display = obs_load_source(saved);
	REQUIRE(uuidOf(display) == uuid);
	REQUIRE(obs_source_get_width(display) == 0);

	manager.poll();
	CHECK(manager.status(uuid).stats.state == CaptureState::Waiting);
	CHECK(obs_source_showing(display));
	manager.poll();
	CHECK(manager.status(uuid).stats.state == CaptureState::Running);
	manager.manualStop();
}

TEST_CASE_METHOD(ObsFixture, "a removed source that was waiting for its size is let go of", "[obs][manager]")
{
	OBSSourceAutoRelease display = createHiddenPattern("Display");
	const std::string uuid = uuidOf(display);
	OBSWeakSourceAutoRelease weak = obs_source_get_weak_source(display);
	FakeHost host;
	CaptureManager manager(host);
	BufferSettings settings = selecting(uuid);
	settings.startWithOutputs = false;
	manager.setSettings(settings);
	REQUIRE(manager.manualStart());
	REQUIRE(manager.status(uuid).stats.state == CaptureState::Waiting);

	obs_source_remove(display);
	display = nullptr;
	manager.poll();
	CHECK(manager.status(uuid).stats.state == CaptureState::Stopped);
	OBSSourceAutoRelease left = obs_weak_source_get_source(weak);
	CHECK(left == nullptr);
	manager.manualStop();
}

namespace {

// A test pattern by settings, for the sizes and the behaviour of the cases below.
OBSSourceAutoRelease patternWith(const char *id, const char *name, uint32_t width, uint32_t height,
				 bool sizeOnlyWhenActive)
{
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_int(settings, "width", width);
	obs_data_set_int(settings, "height", height);
	obs_data_set_bool(settings, "size_only_when_active", sizeOnlyWhenActive);
	return obs_source_create(id, name, settings, nullptr);
}

BufferSettings activating(const std::string &uuid)
{
	BufferSettings settings = selecting(uuid);
	settings.startWithOutputs = false;
	settings.activateOffAir = true;
	return settings;
}

} // namespace

TEST_CASE_METHOD(ObsFixture, "a media source that restarts when activated is not kept active", "[obs][manager]")
{
	OBSSourceAutoRelease media = patternWith(tapeloop::test::kMediaStandInId, "Clip", 640, 360, false);
	const std::string uuid = uuidOf(media);
	FakeHost host;
	CaptureManager manager(host);
	manager.setSettings(activating(uuid));
	REQUIRE(manager.manualStart());
	CHECK(manager.status(uuid).stats.state == CaptureState::Running);
	CHECK(manager.status(uuid).activationLeftOut);
	CHECK_FALSE(obs_source_active(media));
	REQUIRE(manager.manualStop());

	// Without the restart it is kept active like any other source.
	OBSDataAutoRelease noRestart = obs_data_create();
	obs_data_set_bool(noRestart, "restart_on_activate", false);
	obs_source_update(media, noRestart);
	REQUIRE(manager.manualStart());
	CHECK_FALSE(manager.status(uuid).activationLeftOut);
	CHECK(obs_source_active(media));
	REQUIRE(manager.manualStop());
	CHECK_FALSE(obs_source_active(media));

	// A RIST input never restarts, whatever the setting says.
	OBSDataAutoRelease rist = obs_data_create();
	obs_data_set_bool(rist, "restart_on_activate", true);
	obs_data_set_bool(rist, "is_local_file", false);
	obs_data_set_string(rist, "input", "RIST://239.0.0.1:5000");
	obs_source_update(media, rist);
	REQUIRE(manager.manualStart());
	CHECK_FALSE(manager.status(uuid).activationLeftOut);
	REQUIRE(manager.manualStop());
}

TEST_CASE_METHOD(ObsFixture, "a VLC source or slideshow is kept active only when it always plays", "[obs][manager]")
{
	struct Case {
		// Null for a setting never saved, which reads as the plugin's default: stop and
		// restart for the VLC source, always play for the slideshow.
		const char *behavior;
		bool vlcHeld;
		bool slideshowHeld;
	};
	for (const char *id : {tapeloop::test::kVlcStandInId, tapeloop::test::kSlideshowStandInId}) {
		for (const Case &each : {Case{"stop_restart", false, false}, Case{"pause_unpause", false, false},
					 Case{"always_play", true, true}, Case{"ALWAYS_PLAY", true, true},
					 Case{"", false, false}, Case{nullptr, false, true}}) {
			const bool held = id == tapeloop::test::kVlcStandInId ? each.vlcHeld : each.slideshowHeld;
			CAPTURE(id, each.behavior ? each.behavior : "(not saved)");
			OBSSourceAutoRelease media = patternWith(id, "Clip", 640, 360, false);
			if (each.behavior) {
				OBSDataAutoRelease setting = obs_data_create();
				obs_data_set_string(setting, "playback_behavior", each.behavior);
				obs_source_update(media, setting);
			}
			const std::string uuid = uuidOf(media);
			FakeHost host;
			CaptureManager manager(host);
			manager.setSettings(activating(uuid));
			REQUIRE(manager.manualStart());
			CHECK(manager.status(uuid).stats.state == CaptureState::Running);
			CHECK(obs_source_active(media) == held);
			CHECK(manager.status(uuid).activationLeftOut == !held);
			REQUIRE(manager.manualStop());
		}
	}
}

TEST_CASE_METHOD(ObsFixture, "a browser source is kept active only when it does not refresh on activation",
		 "[obs][manager]")
{
	for (const bool refreshes : {false, true}) {
		CAPTURE(refreshes);
		OBSSourceAutoRelease browser =
			patternWith(tapeloop::test::kBrowserStandInId, "Scoreboard", 640, 360, false);
		if (refreshes) {
			OBSDataAutoRelease setting = obs_data_create();
			obs_data_set_bool(setting, "restart_when_active", true);
			obs_source_update(browser, setting);
		}
		const std::string uuid = uuidOf(browser);
		FakeHost host;
		CaptureManager manager(host);
		manager.setSettings(activating(uuid));
		REQUIRE(manager.manualStart());
		CHECK(manager.status(uuid).stats.state == CaptureState::Running);
		CHECK(obs_source_active(browser) == !refreshes);
		CHECK(manager.status(uuid).activationLeftOut == refreshes);
		REQUIRE(manager.manualStop());
	}
}

TEST_CASE_METHOD(ObsFixture, "an image source is not kept active", "[obs][manager]")
{
	OBSSourceAutoRelease image = patternWith(tapeloop::test::kImageStandInId, "Logo", 640, 360, false);
	const std::string uuid = uuidOf(image);
	FakeHost host;
	CaptureManager manager(host);
	manager.setSettings(activating(uuid));
	REQUIRE(manager.manualStart());
	CHECK(manager.status(uuid).stats.state == CaptureState::Running);
	CHECK_FALSE(obs_source_active(image));
	CHECK(manager.status(uuid).activationLeftOut);
	REQUIRE(manager.manualStop());
}

TEST_CASE_METHOD(ObsFixture, "a change of the restart setting applies while the buffers run", "[obs][manager]")
{
	OBSDataAutoRelease noRestart = obs_data_create();
	obs_data_set_bool(noRestart, "restart_on_activate", false);
	OBSDataAutoRelease restart = obs_data_create();
	obs_data_set_bool(restart, "restart_on_activate", true);
	OBSSourceAutoRelease media = patternWith(tapeloop::test::kMediaStandInId, "Clip", 640, 360, false);
	obs_source_update(media, noRestart);
	const std::string uuid = uuidOf(media);
	FakeHost host;
	CaptureManager manager(host);
	manager.setSettings(activating(uuid));
	REQUIRE(manager.manualStart());
	REQUIRE(obs_source_active(media));

	obs_source_update(media, restart);
	manager.poll();
	CHECK_FALSE(obs_source_active(media));
	CHECK(manager.status(uuid).activationLeftOut);
	CHECK(manager.status(uuid).stats.state == CaptureState::Running);

	obs_source_update(media, noRestart);
	manager.poll();
	CHECK(obs_source_active(media));
	CHECK_FALSE(manager.status(uuid).activationLeftOut);

	// Polling again takes no second activation.
	manager.poll();
	REQUIRE(manager.manualStop());
	CHECK_FALSE(obs_source_active(media));
}

TEST_CASE_METHOD(ObsFixture, "a running source follows the activation setting at once", "[obs][manager]")
{
	OBSSourceAutoRelease camera = patternWith(tapeloop::test::kTestPatternId, "Camera", 640, 360, false);
	OBSSourceAutoRelease media = patternWith(tapeloop::test::kMediaStandInId, "Clip", 640, 360, false);
	const std::string cameraUuid = uuidOf(camera);
	const std::string mediaUuid = uuidOf(media);
	FakeHost host;
	CaptureManager manager(host);
	BufferSettings settings = activating(cameraUuid);
	settings.sources[mediaUuid].selected = true;
	manager.setSettings(settings);
	REQUIRE(manager.manualStart());
	REQUIRE(obs_source_active(camera));
	REQUIRE(manager.status(mediaUuid).activationLeftOut);

	SECTION("turning it off lets go of the source and the note")
	{
		settings.activateOffAir = false;
		manager.setSettings(settings);
		CHECK_FALSE(obs_source_active(camera));
		CHECK_FALSE(manager.status(mediaUuid).activationLeftOut);
		settings.activateOffAir = true;
		manager.setSettings(settings);
		CHECK(obs_source_active(camera));
		CHECK(manager.status(mediaUuid).activationLeftOut);
		REQUIRE(manager.manualStop());
	}
	SECTION("stopping the buffers clears the note")
	{
		REQUIRE(manager.manualStop());
		CHECK_FALSE(obs_source_active(camera));
		CHECK(manager.status(mediaUuid).stats.state == CaptureState::Stopped);
		CHECK_FALSE(manager.status(mediaUuid).activationLeftOut);
	}
}

TEST_CASE_METHOD(ObsFixture, "a source with a size only while active starts once activated", "[obs][manager]")
{
	OBSSourceAutoRelease media = patternWith(tapeloop::test::kTestPatternId, "Media", 640, 360, true);
	const std::string uuid = uuidOf(media);
	REQUIRE(obs_source_get_width(media) == 0);
	FakeHost host;
	CaptureManager manager(host);
	manager.setSettings(activating(uuid));
	REQUIRE(manager.manualStart());
	manager.poll();
	CHECK(manager.status(uuid).stats.state == CaptureState::Running);
	REQUIRE(manager.manualStop());
	CHECK_FALSE(obs_source_active(media));
}

TEST_CASE_METHOD(ObsFixture, "a waiting source keeps one activation and gives it back when asked", "[obs][manager]")
{
	OBSSourceAutoRelease never = patternWith(tapeloop::test::kTestPatternId, "Never", 0, 0, false);
	const std::string uuid = uuidOf(never);
	FakeHost host;
	CaptureManager manager(host);
	BufferSettings settings = activating(uuid);
	manager.setSettings(settings);
	REQUIRE(manager.manualStart());
	REQUIRE(manager.status(uuid).stats.state == CaptureState::Waiting);
	REQUIRE(obs_source_active(never));

	SECTION("retries take no second activation")
	{
		manager.poll();
		manager.poll();
		manager.setSettings(settings);
		REQUIRE(manager.manualStop());
		CHECK_FALSE(obs_source_active(never));
	}
	SECTION("turning activation off lets go of it at the next retry")
	{
		settings.activateOffAir = false;
		manager.setSettings(settings);
		manager.poll();
		CHECK(manager.status(uuid).stats.state == CaptureState::Waiting);
		CHECK_FALSE(obs_source_active(never));
		REQUIRE(manager.manualStop());
	}
	SECTION("removing the source lets go of it")
	{
		OBSWeakSourceAutoRelease weak = obs_source_get_weak_source(never);
		obs_source_remove(never);
		never = nullptr;
		manager.poll();
		OBSSourceAutoRelease left = obs_weak_source_get_source(weak);
		CHECK(left == nullptr);
		manager.manualStop();
	}
}

TEST_CASE_METHOD(ObsFixture, "a replay takes the window of every captured buffer", "[obs][manager][replay]")
{
	OBSSourceAutoRelease first = createTestPattern(320, 180, "First");
	OBSSourceAutoRelease second = createTestPattern(320, 180, "Second");
	const std::string firstUuid = uuidOf(first);
	const std::string secondUuid = uuidOf(second);
	FakeHost host;
	CaptureManager manager(host);
	BufferSettings settings = selecting(firstUuid);
	settings.sources[secondUuid].selected = true;
	settings.sources[secondUuid].length = 2s;
	settings.startWithOutputs = false;
	manager.setSettings(settings);

	// Nothing captured yet, nothing to keep.
	CHECK(manager.captureReplay() == 0);
	CHECK(manager.library().size() == 0);

	REQUIRE(manager.manualStart());
	const auto gops = [&](const std::string &uuid) {
		const tapeloop::SourceBuffer *buffer = manager.buffer(uuid);
		return buffer ? buffer->stats().gopCount : 0;
	};
	const auto heldFor = [&](const std::string &uuid) {
		const tapeloop::SourceBuffer *buffer = manager.buffer(uuid);
		const tapeloop::SourceBufferStats stats = buffer ? buffer->stats() : tapeloop::SourceBufferStats{};
		return stats.gopCount != 0 ? stats.newestTime - stats.oldestTime : tapeloop::Nanoseconds{0};
	};
	REQUIRE(waitFor([&] { return heldFor(firstUuid) >= 3s && heldFor(secondUuid) >= 2s; }, 60s));

	const uint64_t id = manager.captureReplay();
	REQUIRE(id != 0);
	const Replay *replay = manager.library().find(id);
	REQUIRE(replay);
	REQUIRE(replay->index);
	const tapeloop::ReplayIndex &index = *replay->index;
	REQUIRE(index.sources.size() == 2);
	CHECK(sourceOf(*replay, firstUuid).name == "First");
	// Both sources render on the same video clock, so their clips end together.
	const tapeloop::Nanoseconds gap = sourceOf(*replay, firstUuid).out - sourceOf(*replay, secondUuid).out;
	CHECK(std::chrono::abs(gap) <= 100ms);
	// Each clip reaches back its own buffer's length from the capture, and the moment as
	// far as the longest of them.
	CHECK(index.end - index.start == settings.length);
	CHECK(sourceOf(*replay, firstUuid).in < index.end - 2s);
	CHECK(sourceOf(*replay, secondUuid).in >= index.end - 2s);
	CHECK(sourceOf(*replay, secondUuid).in - (index.end - 2s) <= 100ms);

	// The buffers keep recording, and a later capture is a replay of its own.
	const size_t before = gops(firstUuid);
	REQUIRE(waitFor([&] { return gops(firstUuid) > before; }, 60s));
	CHECK(manager.status(firstUuid).stats.state == CaptureState::Running);
	const uint64_t later = manager.captureReplay();
	REQUIRE(later != 0);
	CHECK(manager.library().list() == std::vector<uint64_t>{later, id});
	CHECK(manager.library().current() == later);
	CHECK(manager.library().find(later)->capturedAt >= manager.library().find(id)->capturedAt);

	// Buffers that stopped still hold what they recorded.
	REQUIRE(manager.manualStop());
	const uint64_t stopped = manager.captureReplay();
	REQUIRE(stopped != 0);
	CHECK(manager.library().find(stopped)->sources.size() == 2);

	// A source no longer selected takes its buffer with it.
	settings.sources[secondUuid].selected = false;
	manager.setSettings(settings);
	const uint64_t alone = manager.captureReplay();
	REQUIRE(alone != 0);
	REQUIRE(manager.library().find(alone)->sources.size() == 1);
	CHECK(manager.library().find(alone)->sources[0].key == firstUuid);
	manager.finishWrites();
}

TEST_CASE_METHOD(ObsFixture, "a replay reaches back the length its buffer started with", "[obs][manager][replay]")
{
	OBSSourceAutoRelease pattern = createTestPattern(320, 180, "Pattern");
	const std::string uuid = uuidOf(pattern);
	FakeHost host;
	CaptureManager manager(host);
	BufferSettings settings = selecting(uuid);
	settings.length = 2s;
	settings.startWithOutputs = false;
	manager.setSettings(settings);
	REQUIRE(manager.manualStart());
	const auto held = [&] {
		const tapeloop::SourceBuffer *buffer = manager.buffer(uuid);
		return buffer ? buffer->stats() : tapeloop::SourceBufferStats{};
	};
	REQUIRE(waitFor(
		[&] {
			const tapeloop::SourceBufferStats stats = held();
			return stats.gopCount >= 2 && stats.newestTime - stats.oldestTime >= 2s;
		},
		60s));
	// How far back the clip of a replay starts from the capture.
	const auto reachOf = [&](uint64_t id) {
		const Replay *replay = manager.library().find(id);
		REQUIRE(replay);
		REQUIRE(replay->index);
		REQUIRE(replay->index->sources.size() == 1);
		return replay->index->end - replay->index->sources[0].in;
	};

	// A running buffer keeps the length it started with until it starts again.
	settings.length = 1s;
	manager.setSettings(settings);
	REQUIRE(manager.status(uuid).stats.state == CaptureState::Running);
	const uint64_t running = manager.captureReplay();
	REQUIRE(running != 0);
	CHECK(reachOf(running) <= 2s);
	CHECK(reachOf(running) >= 2s - 100ms);

	// A stopped buffer gives what still lies in the window, and nothing once it all lies
	// before it, when the next poll lets go of it.
	REQUIRE(manager.manualStop());
	REQUIRE(held().gopCount != 0);
	const uint64_t stopped = manager.captureReplay();
	REQUIRE(stopped != 0);
	CHECK(reachOf(stopped) <= 2s);
	manager.poll();
	CHECK(held().gopCount != 0);
	std::this_thread::sleep_for(2500ms);
	CHECK(manager.captureReplay() == 0);
	manager.poll();
	CHECK(held().gopCount == 0);
	manager.finishWrites();
}

TEST_CASE_METHOD(ObsFixture, "a replay is saved in its broadcast folder and read back from it",
		 "[obs][manager][replay]")
{
	OBSSourceAutoRelease pattern = createTestPattern(320, 180, "Pattern");
	const std::string uuid = uuidOf(pattern);
	FakeHost host;
	CaptureManager manager(host);
	BufferSettings settings = selecting(uuid);
	settings.startWithOutputs = false;
	manager.setSettings(settings);
	REQUIRE(manager.manualStart());
	const fs::path folder = manager.broadcastFolder();
	CHECK(folder.parent_path() == host.dir.path() / "Tapeloop");
	CHECK(tapeloop::utf8FromPath(folder.filename()).starts_with("F\xC3\xBAtbol_ Liga_2026 20"));
	REQUIRE(waitFor([&] { return hasGops(manager, uuid, 3); }, 60s));

	const uint64_t id = manager.captureReplay();
	REQUIRE(id != 0);
	CHECK(manager.library().find(id)->state == ReplayState::Writing);
	manager.finishWrites();
	const Replay *replay = manager.library().find(id);
	REQUIRE(replay->state == ReplayState::Stored);
	CHECK(replay->manifest.parent_path() == folder);
	CHECK(replay->broadcast == tapeloop::utf8FromPath(folder.filename()));
	const std::optional<tapeloop::ReplayIndex> onDisk = tapeloop::readReplayIndex(replay->manifest);
	REQUIRE(onDisk);
	CHECK(onDisk->sources[0].frameTimes == replay->index->sources[0].frameTimes);

	// Every GOP reads back from disk as the buffer holds it. The last one was still being
	// encoded at the capture: only the capture held that copy, so only the disk has it now.
	tapeloop::GopReader reader;
	const tapeloop::StoredSource &source = replay->index->sources[0];
	REQUIRE(source.gops.size() == replay->live[0].size());
	size_t alive = 0;
	for (size_t gop = 0; gop < source.gops.size(); ++gop) {
		const std::shared_ptr<const tapeloop::Gop> read = reader.read(replay->manifest, source, gop);
		REQUIRE(read);
		if (const std::shared_ptr<const tapeloop::Gop> live = replay->live[0][gop].lock()) {
			++alive;
			CHECK(reader.read(replay->manifest, source, gop, live) == live);
			CHECK(tapeloop::test::sameGop(*read, *live));
		}
	}
	CHECK(alive + 1 >= source.gops.size());
	manager.manualStop();
}

TEST_CASE_METHOD(ObsFixture, "a replay that overlaps the one before writes only what is new", "[obs][manager][replay]")
{
	OBSSourceAutoRelease pattern = createTestPattern(320, 180, "Pattern");
	const std::string uuid = uuidOf(pattern);
	FakeHost host;
	CaptureManager manager(host);
	BufferSettings settings = selecting(uuid);
	settings.startWithOutputs = false;
	manager.setSettings(settings);
	REQUIRE(manager.manualStart());
	REQUIRE(waitFor([&] { return hasGops(manager, uuid, 3); }, 60s));
	const uint64_t first = manager.captureReplay();
	const size_t held = manager.buffer(uuid)->stats().gopCount;
	REQUIRE(waitFor([&] { return manager.buffer(uuid)->stats().gopCount > held; }, 60s));
	const uint64_t second = manager.captureReplay();
	manager.finishWrites();

	const tapeloop::StoredSource &before = manager.library().find(first)->index->sources[0];
	const tapeloop::StoredSource &after = manager.library().find(second)->index->sources[0];
	REQUIRE(manager.library().find(second)->state == ReplayState::Stored);
	// The GOPs both hold are the same chunks on disk.
	size_t shared = 0;
	for (const tapeloop::StoredGop &gop : after.gops) {
		for (const tapeloop::StoredGop &earlier : before.gops) {
			if (gop.segment == earlier.segment && gop.offset == earlier.offset) {
				++shared;
			}
		}
	}
	CHECK(shared + 2 >= before.gops.size());
	CHECK(shared < after.gops.size());
	manager.manualStop();
}

TEST_CASE_METHOD(ObsFixture, "replays and their tags are read back after OBS starts again", "[obs][manager][replay]")
{
	OBSSourceAutoRelease pattern = createTestPattern(320, 180, "Pattern");
	const std::string uuid = uuidOf(pattern);
	FakeHost host;
	std::vector<tapeloop::ReplayId> captured;
	{
		CaptureManager manager(host);
		BufferSettings settings = selecting(uuid);
		settings.startWithOutputs = false;
		manager.setSettings(settings);
		REQUIRE(manager.manualStart());
		REQUIRE(waitFor([&] { return hasGops(manager, uuid, 2); }, 60s));
		const uint64_t goal = manager.captureReplay();
		const uint64_t foul = manager.captureReplay();
		// One tagged while it is being written, one once it is on disk.
		CHECK(manager.tagReplay(goal, "Goal"));
		manager.finishWrites();
		CHECK(manager.tagReplay(foul, "Foul"));
		CHECK(manager.tagReplay(foul, "Penalty"));
		CHECK(manager.untagReplay(foul, "Penalty"));
		captured = {manager.library().find(goal)->uuid, manager.library().find(foul)->uuid};
		manager.onExit();
	}

	CaptureManager manager(host);
	manager.loadLibrary();
	manager.finishWrites();
	REQUIRE(manager.library().size() == 2);
	for (const uint64_t id : manager.library().list()) {
		const Replay *replay = manager.library().find(id);
		CHECK(replay->state == ReplayState::Stored);
		CHECK(replay->sources == std::vector<tapeloop::ReplaySource>{{uuid, "Pattern"}});
		CHECK(replay->broadcast.starts_with("F\xC3\xBAtbol_ Liga_2026 20"));
		if (replay->uuid == captured[0]) {
			CHECK(replay->tags == std::vector<std::string>{"Goal"});
		} else {
			CHECK(replay->uuid == captured[1]);
			CHECK(replay->tags == std::vector<std::string>{"Foul"});
		}
	}
	CHECK(manager.library().tags().size() == 2);

	// Deleting a tag reaches the replays on disk.
	CHECK(manager.deleteReplayTag("goal"));
	manager.finishWrites();
	CaptureManager again(host);
	again.loadLibrary();
	again.finishWrites();
	CHECK(again.library().list("Goal").empty());
	CHECK(again.library().list("Foul").size() == 1);
}

TEST_CASE_METHOD(ObsFixture, "a replay that cannot be saved still plays from the buffers", "[obs][manager][replay]")
{
	OBSSourceAutoRelease pattern = createTestPattern(320, 180, "Pattern");
	const std::string uuid = uuidOf(pattern);
	FakeHost host;
	// A file where the folder of replays goes.
	tapeloop::File(host.dir.path() / "Tapeloop", tapeloop::File::Mode::CreateNew).close();
	CaptureManager manager(host);
	BufferSettings settings = selecting(uuid);
	settings.startWithOutputs = false;
	manager.setSettings(settings);
	REQUIRE(manager.manualStart());
	REQUIRE(waitFor([&] { return hasGops(manager, uuid, 2); }, 60s));

	LogCounter failed("could not be saved");
	const uint64_t id = manager.captureReplay();
	manager.finishWrites();
	const Replay *replay = manager.library().find(id);
	CHECK(replay->state == ReplayState::NotSaved);
	CHECK_FALSE(replay->error.empty());
	CHECK(failed.lines == 1);
	CHECK(manager.library().current() == id);
	CHECK_FALSE(replay->live[0].front().expired());

	// Without a recording folder there is nowhere to save it.
	host.folder.clear();
	REQUIRE(manager.manualStop());
	REQUIRE(manager.manualStart());
	CHECK(manager.broadcastFolder().empty());
	REQUIRE(waitFor([&] { return hasGops(manager, uuid, 1); }, 60s));
	const uint64_t nowhere = manager.captureReplay();
	REQUIRE(nowhere != 0);
	CHECK(manager.library().find(nowhere)->state == ReplayState::NotSaved);
	manager.manualStop();
}

TEST_CASE_METHOD(ObsFixture, "a stored replay holds none of the GOPs its buffer let go of", "[obs][manager][replay]")
{
	OBSSourceAutoRelease pattern = createTestPattern(320, 180, "Pattern");
	const std::string uuid = uuidOf(pattern);
	FakeHost host;
	CaptureManager manager(host);
	BufferSettings settings = selecting(uuid);
	settings.startWithOutputs = false;
	manager.setSettings(settings);
	REQUIRE(manager.manualStart());
	REQUIRE(waitFor([&] { return hasGops(manager, uuid, 2); }, 60s));
	const uint64_t id = manager.captureReplay();
	manager.finishWrites();

	const size_t gops = manager.library().find(id)->live[0].size();
	REQUIRE(gops != 0);

	// Unselected, the source's buffer goes, and with it every GOP; the next poll lets go
	// of the replay's frames too, which its manifest has.
	settings.sources[uuid].selected = false;
	manager.setSettings(settings);
	CHECK(manager.buffer(uuid) == nullptr);
	manager.poll();
	const Replay *replay = manager.library().find(id);
	REQUIRE(replay->state == ReplayState::Stored);
	CHECK(replay->index == nullptr);
	CHECK(replay->live.empty());
	const std::optional<tapeloop::ReplayIndex> index = tapeloop::readReplayIndex(replay->manifest);
	REQUIRE(index);
	tapeloop::GopReader reader;
	for (size_t gop = 0; gop < gops; ++gop) {
		CHECK(reader.read(replay->manifest, index->sources[0], gop));
	}
	CHECK(reader.stats().read == gops);
}

TEST_CASE_METHOD(ObsFixture, "a broadcast is named when the buffers start", "[obs][manager][replay]")
{
	OBSSourceAutoRelease pattern = createTestPattern(320, 180, "Pattern");
	FakeHost host;
	CaptureManager manager(host);
	BufferSettings settings = selecting(uuidOf(pattern));
	settings.startWithOutputs = false;
	manager.setSettings(settings);
	CHECK(manager.broadcastFolder().empty());
	REQUIRE(manager.manualStart());
	const fs::path first = manager.broadcastFolder();
	REQUIRE_FALSE(first.empty());
	// Running on, the buffers keep it, whatever the collection is called now.
	host.collection = "Copa";
	manager.poll();
	CHECK(manager.broadcastFolder() == first);

	// A new collection starts a new broadcast.
	manager.onSceneCollectionCleanup();
	manager.setSettings(settings);
	REQUIRE(manager.manualStart());
	CHECK(tapeloop::utf8FromPath(manager.broadcastFolder().filename()).starts_with("Copa 20"));
	manager.manualStop();
}

TEST_CASE_METHOD(ObsFixture, "a collection switch keeps the replays and drops the pick", "[obs][manager][replay]")
{
	OBSSourceAutoRelease pattern = createTestPattern(320, 180, "Pattern");
	const std::string uuid = uuidOf(pattern);
	FakeHost host;
	CaptureManager manager(host);
	BufferSettings settings = selecting(uuid);
	settings.startWithOutputs = false;
	manager.setSettings(settings);
	REQUIRE(manager.manualStart());
	REQUIRE(waitFor([&] { return hasGops(manager, uuid, 2); }, 60s));
	const uint64_t first = manager.captureReplay();
	const uint64_t second = manager.captureReplay();
	REQUIRE(first != 0);
	REQUIRE(second != 0);
	REQUIRE(manager.tagReplay(first, "Goal"));
	REQUIRE(manager.pickReplay(first));
	REQUIRE(manager.library().current() == first);

	manager.onSceneCollectionCleanup();
	manager.finishWrites();
	CHECK(manager.library().list() == std::vector<uint64_t>{second, first});
	CHECK(manager.library().list("Goal") == std::vector<uint64_t>{first});
	CHECK_FALSE(manager.library().picked());
	CHECK(manager.library().current() == second);
	// The next collection picks among them as before.
	manager.setSettings(settings);
	CHECK(manager.pickReplay(first));
	CHECK(manager.library().current() == first);
}

TEST_CASE_METHOD(ObsFixture, "a collection switched while streaming starts a new broadcast", "[obs][manager][replay]")
{
	OBSSourceAutoRelease pattern = createTestPattern(320, 180, "Pattern");
	FakeHost host;
	host.streaming = true;
	CaptureManager manager(host);
	const BufferSettings settings = selecting(uuidOf(pattern));
	manager.setSettings(settings);
	REQUIRE(manager.running());
	REQUIRE(tapeloop::utf8FromPath(manager.broadcastFolder().filename()).starts_with("F\xC3\xBAtbol_ Liga_2026 20"));

	// The buffers stop with the old collection and start with the new one, while the
	// stream goes on throughout.
	host.collection = "Copa";
	manager.onSceneCollectionCleanup();
	manager.setSettings(settings);
	REQUIRE(manager.running());
	CHECK(tapeloop::utf8FromPath(manager.broadcastFolder().filename()).starts_with("Copa 20"));
	manager.onStreaming(false);
}

TEST_CASE_METHOD(ObsFixture, "a recording folder that appears later is used and read", "[obs][manager][replay]")
{
	OBSSourceAutoRelease pattern = createTestPattern(320, 180, "Pattern");
	const std::string uuid = uuidOf(pattern);
	FakeHost host;
	// A replay already there from an earlier broadcast.
	tapeloop::ReplayWriter().write(
		tapeloop::test::captureOf(host.dir.path() / "Tapeloop" / "Copa 2026-10-01 10-00",
					  {tapeloop::test::sourceOf("a", tapeloop::test::makeGops(2))}));
	const std::string folder = host.folder;
	host.folder.clear();
	CaptureManager manager(host);
	manager.loadLibrary();
	BufferSettings settings = selecting(uuid);
	settings.startWithOutputs = false;
	manager.setSettings(settings);
	REQUIRE(manager.manualStart());
	CHECK(manager.broadcastFolder().empty());
	REQUIRE(waitFor([&] { return hasGops(manager, uuid, 2); }, 60s));
	// Captures with no folder keep the broadcast the buffers started, whatever the
	// collection is called by then.
	host.collection = "Copa";
	const uint64_t unsaved = manager.captureReplay();
	REQUIRE(unsaved != 0);
	CHECK(manager.library().find(unsaved)->state == ReplayState::NotSaved);
	const std::string broadcast = manager.library().find(unsaved)->broadcast;
	CHECK(broadcast.starts_with("F\xC3\xBAtbol_ Liga_2026 "));

	host.folder = folder;
	const uint64_t id = manager.captureReplay();
	REQUIRE(id != 0);
	manager.finishWrites();
	CHECK(manager.library().find(id)->state == ReplayState::Stored);
	CHECK(manager.library().find(id)->broadcast == broadcast);
	CHECK(manager.broadcastFolder() == host.dir.path() / "Tapeloop" / tapeloop::pathFromUtf8(broadcast));
	CHECK(manager.library().size() == 3);
	manager.manualStop();
}

TEST_CASE_METHOD(ObsFixture, "tags edited while a replay is written reach its file before exit",
		 "[obs][manager][replay]")
{
	OBSSourceAutoRelease pattern = createTestPattern(320, 180, "Pattern");
	const std::string uuid = uuidOf(pattern);
	FakeHost host;
	CaptureManager manager(host);
	BufferSettings settings = selecting(uuid);
	settings.startWithOutputs = false;
	manager.setSettings(settings);
	REQUIRE(manager.manualStart());
	REQUIRE(waitFor([&] { return hasGops(manager, uuid, 2); }, 60s));
	const uint64_t kept = manager.captureReplay();
	const uint64_t untagged = manager.captureReplay();
	CHECK(manager.tagReplay(kept, "Goal"));
	CHECK(manager.tagReplay(kept, "Foul"));
	CHECK(manager.deleteReplayTag("Foul"));
	CHECK(manager.tagReplay(untagged, "Save"));
	CHECK(manager.untagReplay(untagged, "Save"));
	manager.onExit();

	const auto tagsOf = [&](uint64_t id) {
		return tapeloop::decodeTags(tapeloop::test::fileBytes(manager.library().find(id)->manifest));
	};
	CHECK(tagsOf(kept) == std::vector<std::string>{"Goal"});
	CHECK(tagsOf(untagged).empty());

	// Read again, the library has each replay once.
	manager.loadLibrary();
	manager.finishWrites();
	CHECK(manager.library().size() == 2);
}

TEST_CASE_METHOD(ObsFixture, "a tag that cannot be written says so in the log", "[obs][manager][replay]")
{
	OBSSourceAutoRelease pattern = createTestPattern(320, 180, "Pattern");
	const std::string uuid = uuidOf(pattern);
	FakeHost host;
	CaptureManager manager(host);
	BufferSettings settings = selecting(uuid);
	settings.startWithOutputs = false;
	manager.setSettings(settings);
	REQUIRE(manager.manualStart());
	REQUIRE(waitFor([&] { return hasGops(manager, uuid, 2); }, 60s));
	const uint64_t id = manager.captureReplay();
	manager.finishWrites();
	const fs::path manifest = manager.library().find(id)->manifest;
	REQUIRE(fs::remove(manifest));
	fs::create_directory(manifest);

	LogCounter failed("could not be saved");
	CHECK(manager.tagReplay(id, "Goal"));
	manager.finishWrites();
	CHECK(failed.lines == 1);
	manager.manualStop();
}

TEST_CASE_METHOD(ObsFixture, "captures the store keeps up with say nothing of the disk", "[obs][manager][replay]")
{
	OBSSourceAutoRelease pattern = createTestPattern(320, 180, "Pattern");
	const std::string uuid = uuidOf(pattern);
	FakeHost host;
	CaptureManager manager(host);
	BufferSettings settings = selecting(uuid);
	settings.startWithOutputs = false;
	manager.setSettings(settings);
	REQUIRE(manager.manualStart());
	REQUIRE(waitFor([&] { return hasGops(manager, uuid, 2); }, 60s));

	LogCounter behind("The disk falls behind");
	for (int capture = 0; capture < 4; ++capture) {
		CHECK(manager.captureReplay() != 0);
		manager.finishWrites();
	}
	CHECK(behind.lines == 0);
	manager.manualStop();
}

// A disk slow enough for the store to fall behind cannot be made from a test. Run with
// every fsync slowed down, as under strace -f -e trace=none -e inject=fsync:delay_exit=200000.
TEST_CASE_METHOD(ObsFixture, "captures that pile up on a slow disk say so once", "[.slow-disk]")
{
	OBSSourceAutoRelease pattern = createTestPattern(320, 180, "Pattern");
	const std::string uuid = uuidOf(pattern);
	FakeHost host;
	CaptureManager manager(host);
	BufferSettings settings = selecting(uuid);
	settings.startWithOutputs = false;
	manager.setSettings(settings);
	REQUIRE(manager.manualStart());
	REQUIRE(waitFor([&] { return hasGops(manager, uuid, 2); }, 60s));

	LogCounter behind("The disk falls behind");
	for (int capture = 0; capture < 6; ++capture) {
		manager.captureReplay();
	}
	CHECK(behind.lines == 1);
	manager.finishWrites();
	manager.captureReplay();
	manager.finishWrites();
	CHECK(behind.lines == 1);
	for (int capture = 0; capture < 6; ++capture) {
		manager.captureReplay();
	}
	CHECK(behind.lines == 2);
	manager.finishWrites();
	manager.manualStop();
}

TEST_CASE("the advanced switch is saved with the settings", "[obs][manager]")
{
	BufferSettings settings;
	settings.showAdvanced = true;
	OBSDataAutoRelease data = createSettingsData(tapeloop::saveSettings(settings));
	CHECK(obs_data_get_bool(data, "show_advanced"));
	CHECK(readSettingsData(data).showAdvanced);
	// Settings saved before it hide the advanced settings, as the switch did.
	obs_data_erase(data, "show_advanced");
	CHECK_FALSE(readSettingsData(data).showAdvanced);
}

TEST_CASE("settings saved before the other-adapter setting allow other adapters", "[obs][manager]")
{
	OBSDataAutoRelease data = createSettingsData(tapeloop::saveSettings(BufferSettings{}));
	obs_data_erase(data, "allow_other_adapters");
	CHECK(readSettingsData(data).allowOtherAdapters);
	obs_data_set_bool(data, "allow_other_adapters", false);
	CHECK_FALSE(readSettingsData(data).allowOtherAdapters);
}
