// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "ObsFixture.hpp"
#include "TestPattern.hpp"

#include "obs/CaptureManager.hpp"
#include "obs/SettingsData.hpp"

#include <catch2/catch_test_macros.hpp>
#include <obs.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <initializer_list>
#include <string>
#include <vector>

using namespace std::chrono_literals;
using tapeloop::BufferSettings;
using tapeloop::FrameSize;
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
	void requestSave() override { ++saves; }

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
}

TEST_CASE_METHOD(ObsFixture, "a VLC source or slideshow is kept active only when it does not restart", "[obs][manager]")
{
	struct Case {
		const char *behavior;
		bool held;
	};
	for (const char *id : {tapeloop::test::kVlcStandInId, tapeloop::test::kSlideshowStandInId}) {
		for (const Case &each : {Case{"stop_restart", false}, Case{"pause_unpause", true},
					 Case{"always_play", true}, Case{"", false}}) {
			CAPTURE(id, each.behavior);
			OBSSourceAutoRelease media = patternWith(id, "Clip", 640, 360, false);
			OBSDataAutoRelease setting = obs_data_create();
			obs_data_set_string(setting, "playback_behavior", each.behavior);
			obs_source_update(media, setting);
			const std::string uuid = uuidOf(media);
			FakeHost host;
			CaptureManager manager(host);
			manager.setSettings(activating(uuid));
			REQUIRE(manager.manualStart());
			CHECK(manager.status(uuid).stats.state == CaptureState::Running);
			CHECK(obs_source_active(media) == each.held);
			CHECK(manager.status(uuid).activationLeftOut == !each.held);
			REQUIRE(manager.manualStop());
		}
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

TEST_CASE_METHOD(ObsFixture, "a replay keeps what every captured buffer holds", "[obs][manager][replay]")
{
	OBSSourceAutoRelease first = createTestPattern(320, 180, "First");
	OBSSourceAutoRelease second = createTestPattern(320, 180, "Second");
	const std::string firstUuid = uuidOf(first);
	const std::string secondUuid = uuidOf(second);
	FakeHost host;
	CaptureManager manager(host);
	BufferSettings settings = selecting(firstUuid);
	settings.sources[secondUuid].selected = true;
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
	REQUIRE(waitFor([&] { return gops(firstUuid) >= 2 && gops(secondUuid) >= 2; }, 60s));

	const uint64_t id = manager.captureReplay();
	REQUIRE(id != 0);
	const tapeloop::Moment *moment = manager.library().find(id);
	REQUIRE(moment);
	REQUIRE(moment->clips.size() == 2);
	// Both sources render on the same video clock, so their clips end together.
	const auto out = [&](const std::string &uuid) {
		for (const tapeloop::MomentClip &clip : moment->clips) {
			if (clip.sourceKey == uuid) {
				return clip.clip.out();
			}
		}
		FAIL("no clip of " << uuid);
		return tapeloop::Nanoseconds{0};
	};
	const tapeloop::Nanoseconds gap = out(firstUuid) - out(secondUuid);
	CHECK(std::chrono::abs(gap) <= 100ms);
	CHECK(moment->end - moment->start == settings.length);

	// The buffers keep recording, and a later capture is a replay of its own.
	const size_t before = gops(firstUuid);
	REQUIRE(waitFor([&] { return gops(firstUuid) > before; }, 60s));
	CHECK(manager.status(firstUuid).stats.state == CaptureState::Running);
	const uint64_t later = manager.captureReplay();
	REQUIRE(later != 0);
	CHECK(manager.library().list() == std::vector<uint64_t>{later, id});
	CHECK(manager.library().current() == later);
	CHECK(manager.library().capturedAt(later) >= manager.library().capturedAt(id));

	// Buffers that stopped still hold what they recorded.
	REQUIRE(manager.manualStop());
	const uint64_t stopped = manager.captureReplay();
	REQUIRE(stopped != 0);
	CHECK(manager.library().find(stopped)->clips.size() == 2);

	// A source no longer selected takes its buffer with it.
	settings.sources[secondUuid].selected = false;
	manager.setSettings(settings);
	const uint64_t alone = manager.captureReplay();
	REQUIRE(alone != 0);
	REQUIRE(manager.library().find(alone)->clips.size() == 1);
	CHECK(manager.library().find(alone)->clips[0].sourceKey == firstUuid);
}

TEST_CASE("settings saved before the other-adapter setting allow other adapters", "[obs][manager]")
{
	OBSDataAutoRelease data = createSettingsData(tapeloop::saveSettings(BufferSettings{}));
	obs_data_erase(data, "allow_other_adapters");
	CHECK(readSettingsData(data).allowOtherAdapters);
	obs_data_set_bool(data, "allow_other_adapters", false);
	CHECK_FALSE(readSettingsData(data).allowOtherAdapters);
}
