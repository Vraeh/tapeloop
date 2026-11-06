// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "../core/FakeDecoder.hpp"
#include "../core/StoredReplays.hpp"
#include "../core/TempDirectory.hpp"
#include "LogCounter.hpp"
#include "ObsFixture.hpp"
#include "TestPattern.hpp"
#include "UiTasks.hpp"

#include "core/BufferSettings.hpp"
#include "core/ReplayLibrary.hpp"
#include "core/ReplayPlayerState.hpp"
#include "core/ReplaySequence.hpp"
#include "core/ReplayStore.hpp"
#include "core/ReplayWriter.hpp"
#include "decode/PictureDecoder.hpp"
#include "obs/AirComposition.hpp"
#include "obs/AirEvents.hpp"
#include "obs/CaptureManager.hpp"
#include "obs/ReplayPlayer.hpp"
#include "obs/ReplaySource.hpp"
#include "obs/SequencePlayer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <obs.h>
#include <obs.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using tapeloop::AirMediaKind;
using tapeloop::AirPhase;
using tapeloop::AirSettings;
using tapeloop::DecodedFrame;
using tapeloop::DecodeStatus;
using tapeloop::Nanoseconds;
using tapeloop::ReplayLibrary;
using tapeloop::SequenceEntry;
using tapeloop::VideoCodec;
using tapeloop::decode::Picture;
using tapeloop::decode::PictureDecoder;
using tapeloop::obs::AirEvent;
using tapeloop::obs::CaptureHost;
using tapeloop::obs::CaptureManager;
using tapeloop::obs::exitInOrder;
using tapeloop::obs::handleAirEvent;
using tapeloop::obs::kAirChannel;
using tapeloop::obs::PlayerState;
using tapeloop::obs::registerReplaySource;
using tapeloop::obs::SequencePlayer;
using tapeloop::obs::TransitionCatalog;
using tapeloop::obs::TransitionTemplate;
using tapeloop::test::captureOf;
using tapeloop::test::FakeDecoder;
using tapeloop::test::Gops;
using tapeloop::test::kMediaStandInId;
using tapeloop::test::makeGops;
using tapeloop::test::ObsFixture;
using tapeloop::test::sourceOf;
using tapeloop::test::TempDirectory;
using tapeloop::test::UiTasks;
using tapeloop::test::waitFor;

namespace {

// A replay just captured, with a clip of every source the GOPs given: the buffer's GOPs,
// which the test holds, so nothing is read from disk.
class Captured {
public:
	Captured(ReplayLibrary &library, const std::vector<std::string> &keys, size_t gops) : gops_(makeGops(gops))
	{
		std::vector<tapeloop::CaptureSource> sources;
		for (const std::string &key : keys) {
			sources.push_back(sourceOf(key, gops_));
		}
		id_ = library.addCaptured(captureOf("unused", std::move(sources)), "Broadcast");
		REQUIRE(id_ != 0);
	}

	uint64_t id() const noexcept { return id_; }
	Nanoseconds lastFrame() const { return gops_.back()->lastTime(); }

private:
	Gops gops_;
	uint64_t id_ = 0;
};

// Decodes as a FakeDecoder does, and keeps the newest presentation time it was handed.
// Its frames have no picture, so the player shows nothing.
class NewestDecoder final : public PictureDecoder {
public:
	explicit NewestDecoder(std::atomic<int64_t> &newest) : newest_(newest) {}

	DecodeStatus open(VideoCodec codec, std::span<const uint8_t> config) noexcept override
	{
		return decoder_.open(codec, config);
	}

	DecodeStatus send(std::span<const uint8_t> data, int64_t pts, int64_t dts) noexcept override
	{
		if (pts > newest_.load()) {
			newest_.store(pts);
		}
		return decoder_.send(data, pts, dts);
	}

	DecodeStatus receive(DecodedFrame &frame) noexcept override { return decoder_.receive(frame); }
	DecodeStatus flush() noexcept override { return decoder_.flush(); }
	void reset() noexcept override { decoder_.reset(); }
	void release(const DecodedFrame &frame) noexcept override { decoder_.release(frame); }
	void close() noexcept override { decoder_.close(); }
	std::optional<Picture> picture(const DecodedFrame &) const noexcept override { return std::nullopt; }

private:
	std::atomic<int64_t> &newest_;
	FakeDecoder decoder_{[](int64_t pts) { return pts % tapeloop::test::kGopLength == 0; }, 0};
};

// Runs the UI thread's tasks until the condition holds, as OBS ticks on its own.
bool runUntil(const std::function<bool()> &condition)
{
	const auto deadline = std::chrono::steady_clock::now() + 10s;
	while (std::chrono::steady_clock::now() < deadline) {
		UiTasks::run();
		if (condition()) {
			return true;
		}
		std::this_thread::sleep_for(2ms);
	}
	return false;
}

// Waits, without running the UI thread's tasks, until the player's state holds.
bool playerUntil(const SequencePlayer &sequence, const std::function<bool(const PlayerState &)> &condition)
{
	const auto deadline = std::chrono::steady_clock::now() + 10s;
	while (std::chrono::steady_clock::now() < deadline) {
		if (condition(sequence.player()->state())) {
			return true;
		}
		std::this_thread::sleep_for(2ms);
	}
	return false;
}

// Keeps the graphics thread waiting, and with it every tick, while it lives.
class TicksHeld {
public:
	TicksHeld()
	{
		obs_queue_task(OBS_TASK_GRAPHICS, hold, this, false);
		std::unique_lock lock(mutex_);
		changed_.wait(lock, [this] { return holding_; });
	}

	~TicksHeld()
	{
		std::unique_lock lock(mutex_);
		released_ = true;
		changed_.notify_all();
		changed_.wait(lock, [this] { return !holding_; });
	}

	TicksHeld(const TicksHeld &) = delete;
	TicksHeld &operator=(const TicksHeld &) = delete;
	TicksHeld(TicksHeld &&) = delete;
	TicksHeld &operator=(TicksHeld &&) = delete;

private:
	static void hold(void *param) noexcept
	{
		auto &self = *static_cast<TicksHeld *>(param);
		std::unique_lock lock(self.mutex_);
		self.holding_ = true;
		self.changed_.notify_all();
		self.changed_.wait(lock, [&] { return self.released_; });
		self.holding_ = false;
		self.changed_.notify_all();
	}

	std::mutex mutex_;
	std::condition_variable changed_;
	bool holding_ = false;
	bool released_ = false;
};

// The source on air once the player has its clip and plays it.
std::string playing(SequencePlayer &sequence)
{
	const uint64_t token = sequence.state().token();
	REQUIRE(sequence.state().phase() == AirPhase::Source);
	REQUIRE(playerUntil(sequence, [](const PlayerState &state) { return state.loaded && state.playing; }));
	CHECK(sequence.state().token() == token);
	return std::string(sequence.state().source());
}

std::vector<std::string> keysOf(const SequencePlayer &sequence)
{
	std::vector<std::string> keys;
	for (const SequenceEntry &entry : sequence.state().sequence().entries()) {
		keys.push_back(entry.sourceKey + (entry.shown ? "" : "-"));
	}
	return keys;
}

const char *fadeName(void *) noexcept
{
	return "Tapeloop test fade";
}

void *createFade(obs_data_t *, obs_source_t *source) noexcept
{
	return source;
}

void destroyFade(void *) noexcept {}

void renderFade(void *data, gs_effect_t *) noexcept
{
	obs_transition_video_render(static_cast<obs_source_t *>(data), nullptr);
}

float fadeFrom(void *, float t) noexcept
{
	return 1.0f - t;
}

float fadeTo(void *, float t) noexcept
{
	return t;
}

bool renderFadeAudio(void *data, uint64_t *time, obs_source_audio_mix *audio, uint32_t mixers, size_t channels,
		     size_t sampleRate) noexcept
{
	return obs_transition_audio_render(static_cast<obs_source_t *>(data), time, audio, mixers, channels, sampleRate,
					   fadeFrom, fadeTo);
}

// Stands in for OBS's fade, which the harness does not load: it draws nothing while it
// runs and stops once its time is up, as OBS's own transitions do.
void registerFade()
{
	obs_source_info info = {};
	info.id = "fade_transition";
	info.type = OBS_SOURCE_TYPE_TRANSITION;
	info.get_name = fadeName;
	info.create = createFade;
	info.destroy = destroyFade;
	info.video_render = renderFade;
	info.audio_render = renderFadeAudio;
	obs_register_source(&info);
}

constexpr const char *kStingerId = "tapeloop_test_stinger";

// Stands in for how OBS's stinger gets its length, which the harness does not load. Its
// setup is in its update, which libobs defers to the next tick, and makes the media its
// "media_ms" and "open_after_ms" settings say. On starting, it asks that media its length
// and adds a quarter of a second, and without the media it has no length at all.
struct Stinger {
	obs_source_t *source = nullptr;
	OBSSourceAutoRelease media;
	std::atomic<bool> running{false};
};

const char *stingerName(void *) noexcept
{
	return "Tapeloop test stinger";
}

void *createStinger(obs_data_t *, obs_source_t *source) noexcept
{
	auto *stinger = new (std::nothrow) Stinger;
	if (stinger) {
		stinger->source = source;
		obs_transition_enable_fixed(source, true, 0);
		obs_source_update(source, nullptr);
	}
	return stinger;
}

void destroyStinger(void *data) noexcept
{
	delete static_cast<Stinger *>(data);
}

void updateStinger(void *data, obs_data_t *settings) noexcept
{
	auto &stinger = *static_cast<Stinger *>(data);
	if (!obs_data_has_user_value(settings, "media_ms")) {
		return;
	}
	const OBSDataAutoRelease media = obs_data_create();
	obs_data_set_int(media, "duration_ms", obs_data_get_int(settings, "media_ms"));
	obs_data_set_int(media, "open_after_ms", obs_data_get_int(settings, "open_after_ms"));
	stinger.media = obs_source_create_private(kMediaStandInId, "stinger media", media);
}

void startStinger(void *data) noexcept
{
	auto &stinger = *static_cast<Stinger *>(data);
	if (!stinger.media) {
		return;
	}
	if (stinger.running) {
		obs_source_media_restart(stinger.media);
		return;
	}
	const int64_t length = obs_source_media_get_duration(stinger.media);
	obs_transition_enable_fixed(stinger.source, true, static_cast<uint32_t>(length) + 250);
	obs_source_add_active_child(stinger.source, stinger.media);
	stinger.running = true;
}

void stopStinger(void *data) noexcept
{
	auto &stinger = *static_cast<Stinger *>(data);
	if (stinger.media) {
		obs_source_remove_active_child(stinger.source, stinger.media);
	}
	stinger.running = false;
}

void enumStingerActive(void *data, obs_source_enum_proc_t callback, void *param) noexcept
{
	const auto &stinger = *static_cast<Stinger *>(data);
	if (stinger.media && stinger.running) {
		callback(stinger.source, stinger.media, param);
	}
}

void enumStingerAll(void *data, obs_source_enum_proc_t callback, void *param) noexcept
{
	const auto &stinger = *static_cast<Stinger *>(data);
	if (stinger.media) {
		callback(stinger.source, stinger.media, param);
	}
}

void renderStinger(void *data, gs_effect_t *) noexcept
{
	obs_transition_video_render(static_cast<Stinger *>(data)->source, nullptr);
}

bool renderStingerAudio(void *data, uint64_t *time, obs_source_audio_mix *audio, uint32_t mixers, size_t channels,
			size_t sampleRate) noexcept
{
	return obs_transition_audio_render(static_cast<Stinger *>(data)->source, time, audio, mixers, channels,
					   sampleRate, fadeFrom, fadeTo);
}

void registerStinger()
{
	obs_source_info info = {};
	info.id = kStingerId;
	info.type = OBS_SOURCE_TYPE_TRANSITION;
	info.get_name = stingerName;
	info.create = createStinger;
	info.destroy = destroyStinger;
	info.update = updateStinger;
	info.video_render = renderStinger;
	info.audio_render = renderStingerAudio;
	info.enum_active_sources = enumStingerActive;
	info.enum_all_sources = enumStingerAll;
	info.transition_start = startStinger;
	info.transition_stop = stopStinger;
	obs_register_source(&info);
}

// Counts a signal of a source, which it keeps until it goes.
class SignalCount {
public:
	SignalCount(obs_source_t *source, const char *signal) : source_(obs_source_get_ref(source)), signal_(signal)
	{
		signal_handler_connect(obs_source_get_signal_handler(source_), signal_, count, this);
	}

	~SignalCount() { signal_handler_disconnect(obs_source_get_signal_handler(source_), signal_, count, this); }

	SignalCount(const SignalCount &) = delete;
	SignalCount &operator=(const SignalCount &) = delete;
	SignalCount(SignalCount &&) = delete;
	SignalCount &operator=(SignalCount &&) = delete;

	int value() const noexcept { return count_.load(); }

private:
	static void count(void *data, calldata_t *) noexcept { ++static_cast<SignalCount *>(data)->count_; }

	OBSSourceAutoRelease source_;
	const char *signal_;
	std::atomic<int> count_{0};
};

// What the replay's channel holds, empty for nothing.
std::string channelName()
{
	const OBSSourceAutoRelease source = obs_get_output_source(kAirChannel);
	return source ? std::string(obs_source_get_name(source)) : std::string();
}

// What the channel shows: the part a transition on it goes to or holds.
std::string shownName()
{
	const OBSSourceAutoRelease source = obs_get_output_source(kAirChannel);
	if (!source) {
		return {};
	}
	if (obs_source_get_type(source) != OBS_SOURCE_TYPE_TRANSITION) {
		return obs_source_get_name(source);
	}
	const OBSSourceAutoRelease shown = obs_transition_get_active_source(source);
	return shown ? std::string(obs_source_get_name(shown)) : std::string();
}

std::string idOnChannel()
{
	const OBSSourceAutoRelease source = obs_get_output_source(kAirChannel);
	return source ? std::string(obs_source_get_unversioned_id(source)) : std::string();
}

int64_t markerOnChannel()
{
	const OBSSourceAutoRelease source = obs_get_output_source(kAirChannel);
	if (!source) {
		return -1;
	}
	const OBSDataAutoRelease settings = obs_source_get_settings(source);
	return obs_data_get_int(settings, "marker");
}

// How far the transition on the channel has gone, from 0 to 1.
float timeOnChannel()
{
	const OBSSourceAutoRelease source = obs_get_output_source(kAirChannel);
	return source ? obs_transition_get_time(source) : 1.0f;
}

// A file for an intro or outro, which the stand-ins of OBS's media and image sources
// never read.
std::string fileIn(const TempDirectory &folder, const char *name)
{
	const std::filesystem::path path = folder.path() / name;
	std::ofstream(path) << "unread";
	return path.string();
}

// The length the stand-in of OBS's media source gives.
void setLength(obs_source_t *media, int64_t milliseconds)
{
	const OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_int(settings, "duration_ms", milliseconds);
	obs_source_update(media, settings);
}

class Catalog final : public TransitionCatalog {
public:
	std::optional<TransitionTemplate> find(const std::string &name) const override
	{
		if (name == "Swipe") {
			return TransitionTemplate{"fade_transition", R"({"marker": 7})"};
		}
		if (name == "Ghost") {
			return TransitionTemplate{"ghost_transition", "{}"};
		}
		if (name == "Stinger") {
			return TransitionTemplate{kStingerId, stinger};
		}
		return std::nullopt;
	}

	std::vector<std::string> names() const override { return {"Swipe", "Ghost"}; }

	// The settings of the user's stinger, which the user may edit in OBS.
	std::string stinger = "{}";
};

// Runs the UI thread's tasks for a while, failing should the condition stop holding.
bool holdsFor(std::chrono::milliseconds time, const std::function<bool()> &condition)
{
	const auto deadline = std::chrono::steady_clock::now() + time;
	while (std::chrono::steady_clock::now() < deadline) {
		UiTasks::run();
		if (!condition()) {
			return false;
		}
		std::this_thread::sleep_for(2ms);
	}
	return true;
}

// A sequence and a replay long enough to stay on air, for a test to set up as it needs. The
// replay cuts on air and off unless the test registers the fade first.
struct AirSetup {
	AirSetup()
	{
		registerReplaySource({});
		sequence.emplace(library);
		REQUIRE(sequence->player());
	}

	~AirSetup()
	{
		sequence.reset();
		obs_wait_for_destroy_queue();
	}

	AirSetup(const AirSetup &) = delete;
	AirSetup &operator=(const AirSetup &) = delete;
	AirSetup(AirSetup &&) = delete;
	AirSetup &operator=(AirSetup &&) = delete;

	void putOnAir() { REQUIRE(sequence->putOnAir(captured.id())); }
	AirPhase phase() const { return sequence->state().phase(); }
	bool leaving() const { return sequence->air().leaving(); }

	const UiTasks ui;
	const TempDirectory folder;
	ReplayLibrary library;
	const Captured captured{library, {"a"}, 120};
	std::optional<SequencePlayer> sequence;
};

// Stands in for the frontend, which the harness does not load.
class EventHost final : public CaptureHost {
public:
	bool streamingActive() const override { return false; }
	bool recordingActive() const override { return false; }
	void requestSave() override {}
	std::string recordingFolder() const override { return {}; }
	std::string sceneCollectionName() const override { return "Tests"; }
	void buffersStopped() override { ++stops; }

	int stops = 0;
};

// A replay with an outro on air, and a manager with no buffers, to answer an event.
struct EventSetup : AirSetup {
	EventSetup()
	{
		settings.air.outro = {AirMediaKind::Image, fileIn(folder, "outro.png"), 20000ms};
		// No such source, of the length libobs reads whatever the key's size.
		settings.sources["00000000-0000-0000-0000-000000000000"].selected = true;
		manager.setSettings(settings);
		sequence->setAir(settings.air);
		putOnAir();
		REQUIRE(phase() == AirPhase::Source);
		REQUIRE(sequence->air().hasOutro());
	}

	void answer(AirEvent event) { handleAirEvent(event, *sequence, manager); }

	// Off air at once, with no outro and no way back.
	void checkCut() const
	{
		CHECK(phase() == AirPhase::Live);
		CHECK_FALSE(sequence->air().onAir());
		CHECK(channelName().empty());
	}

	tapeloop::BufferSettings settings;
	EventHost host;
	CaptureManager manager{host};
};

// OBS's program: its transition on channel 0, showing a scene.
struct ProgramSetup : EventSetup {
	ProgramSetup()
	{
		REQUIRE(scene);
		REQUIRE(transition);
		obs_transition_set(transition, scene);
		obs_set_output_source(0, transition);
	}

	~ProgramSetup() { obs_set_output_source(0, nullptr); }

	ProgramSetup(const ProgramSetup &) = delete;
	ProgramSetup &operator=(const ProgramSetup &) = delete;
	ProgramSetup(ProgramSetup &&) = delete;
	ProgramSetup &operator=(ProgramSetup &&) = delete;

	// Runs the transition to its end, where OBS says it stopped.
	void runTo(obs_source_t *target)
	{
		const SignalCount stops(transition, "transition_video_stop");
		REQUIRE(obs_transition_start(transition, OBS_TRANSITION_MODE_AUTO, 1, target));
		REQUIRE(waitFor([&] { return stops.value() > 0; }, 10s));
		answer(AirEvent::ProgramTransitionStopped);
	}

	void checkStillOn() const
	{
		CHECK(phase() == AirPhase::Source);
		CHECK(sequence->air().onAir());
		CHECK_FALSE(channelName().empty());
	}

	const OBSSourceAutoRelease scene = obs_source_create_private(tapeloop::test::kTestPatternId, "Scene", nullptr);
	const OBSSourceAutoRelease transition = obs_source_create_private("fade_transition", "Program", nullptr);
};

// Whether a source was active at every change of the replay's channel, which libobs signals
// before it activates what comes on or lets go of what goes off.
class ActiveAtChange {
public:
	explicit ActiveAtChange(obs_source_t *source) : source_(source)
	{
		signal_handler_connect(obs_get_signal_handler(), "channel_change", changed, this);
	}

	~ActiveAtChange() { signal_handler_disconnect(obs_get_signal_handler(), "channel_change", changed, this); }

	ActiveAtChange(const ActiveAtChange &) = delete;
	ActiveAtChange &operator=(const ActiveAtChange &) = delete;
	ActiveAtChange(ActiveAtChange &&) = delete;
	ActiveAtChange &operator=(ActiveAtChange &&) = delete;

	int changes() const noexcept { return changes_.load(); }
	bool active() const noexcept { return active_.load(); }

private:
	static void changed(void *data, calldata_t *call) noexcept
	{
		if (calldata_int(call, "channel") != kAirChannel) {
			return;
		}
		auto &self = *static_cast<ActiveAtChange *>(data);
		++self.changes_;
		if (!obs_source_active(self.source_)) {
			self.active_ = false;
		}
	}

	obs_source_t *source_;
	std::atomic<int> changes_{0};
	std::atomic<bool> active_{true};
};

// The parts fill the canvas whatever their size, keeping their shape.
void checkTransitionOnChannelFits()
{
	const OBSSourceAutoRelease transition = obs_get_output_source(kAirChannel);
	REQUIRE(transition);
	REQUIRE(obs_source_get_type(transition) == OBS_SOURCE_TYPE_TRANSITION);
	obs_video_info video = {};
	REQUIRE(obs_get_video_info(&video));
	uint32_t width = 0;
	uint32_t height = 0;
	obs_transition_get_size(transition, &width, &height);
	CHECK(width == video.base_width);
	CHECK(height == video.base_height);
	CHECK(obs_transition_get_scale_type(transition) == OBS_TRANSITION_SCALE_ASPECT);
	CHECK(obs_transition_get_alignment(transition) == OBS_ALIGN_CENTER);
}

bool listPublic(void *param, obs_source_t *source) noexcept
{
	if (!obs_obj_is_private(source)) {
		static_cast<std::vector<std::string> *>(param)->push_back(std::string("public ") +
									  obs_source_get_name(source));
	}
	return true;
}

bool listInput(void *param, obs_source_t *source) noexcept
{
	static_cast<std::vector<std::string> *>(param)->push_back(std::string("input ") + obs_source_get_name(source));
	return true;
}

bool listSceneItem(obs_scene_t *scene, obs_sceneitem_t *item, void *param) noexcept
{
	static_cast<std::vector<std::string> *>(param)->push_back(
		std::string("item ") + obs_source_get_name(obs_scene_get_source(scene)) + " " +
		obs_source_get_name(obs_sceneitem_get_source(item)));
	return true;
}

bool listScene(void *param, obs_source_t *source) noexcept
{
	static_cast<std::vector<std::string> *>(param)->push_back(std::string("scene ") + obs_source_get_name(source));
	obs_scene_enum_items(obs_scene_from_source(source), listSceneItem, param);
	return true;
}

// What OBS's lists show: every public object, the scenes and what they hold, the inputs, and
// the program, each with a word that says which list it is from.
std::vector<std::string> listsOfObs()
{
	std::vector<std::string> lists;
	obs_enum_all_sources(listPublic, &lists);
	obs_enum_scenes(listScene, &lists);
	obs_enum_sources(listInput, &lists);
	const OBSSourceAutoRelease program = obs_get_output_source(0);
	lists.push_back(std::string("program ") + (program ? obs_source_get_name(program) : ""));
	std::sort(lists.begin(), lists.end());
	return lists;
}

} // namespace

TEST_CASE_METHOD(ObsFixture, "a sequence plays the shown sources of a replay in list order, then goes back to live",
		 "[obs][sequence]")
{
	registerReplaySource({});
	const UiTasks ui;
	ReplayLibrary library;
	std::optional<SequencePlayer> sequence(std::in_place, library);
	REQUIRE(sequence->source());
	REQUIRE(sequence->player());

	// Clips of a sixth of a second, which play out by themselves.
	const Captured captured(library, {"b", "a", "c", "d"}, 2);
	sequence->setEntries({{"c", true}, {"d", false}, {"a", true}});
	REQUIRE(sequence->putOnAir(captured.id()));
	// The source the list did not have joins it at the end, shown.
	CHECK(keysOf(*sequence) == std::vector<std::string>{"c", "d-", "a", "b"});

	std::vector<std::string> order;
	uint64_t token = 0;
	REQUIRE(runUntil([&] {
		if (sequence->state().token() != token) {
			token = sequence->state().token();
			if (sequence->state().phase() == AirPhase::Source) {
				order.push_back(std::string(sequence->state().source()));
			}
		}
		return sequence->state().phase() == AirPhase::Live;
	}));
	CHECK(order == std::vector<std::string>{"c", "a", "b"});
	REQUIRE(playerUntil(*sequence, [](const PlayerState &state) { return !state.loaded; }));

	// Another goes on air from the start of the list.
	REQUIRE(sequence->putOnAir(captured.id()));
	CHECK(sequence->state().source() == "c");
	sequence.reset();
	obs_wait_for_destroy_queue();
}

TEST_CASE_METHOD(ObsFixture, "a sequence follows the list as it changes while a replay plays", "[obs][sequence]")
{
	registerReplaySource({});
	const UiTasks ui;
	ReplayLibrary library;
	std::optional<SequencePlayer> sequence(std::in_place, library);
	REQUIRE(sequence->player());

	// Clips of ten seconds, which move on only when told.
	const Captured captured(library, {"a", "b", "c", "d"}, 120);
	sequence->setEntries({{"a", true}, {"b", true}, {"c", true}, {"d", true}});
	REQUIRE(sequence->putOnAir(captured.id()));
	CHECK(playing(*sequence) == "a");

	REQUIRE(sequence->move(3, 1));
	REQUIRE(sequence->setShown("b", false));
	CHECK(keysOf(*sequence) == std::vector<std::string>{"a", "d", "b-", "c"});
	CHECK(playing(*sequence) == "a");
	sequence->skip();
	CHECK(playing(*sequence) == "d");

	REQUIRE(sequence->setShown("b", true));
	// Shown above the source on air, it waits for the next replay.
	REQUIRE(sequence->move(2, 0));
	CHECK(keysOf(*sequence) == std::vector<std::string>{"b", "a", "d", "c"});
	CHECK(playing(*sequence) == "d");

	// Hiding the source on air ends it at once.
	REQUIRE(sequence->setShown("d", false));
	CHECK(playing(*sequence) == "c");

	// So does a list without it; nothing followed it, so the replay is over.
	sequence->setEntries({{"b", true}, {"a", true}, {"d", false}});
	CHECK(sequence->state().phase() == AirPhase::Live);
	REQUIRE(playerUntil(*sequence, [](const PlayerState &state) { return !state.loaded; }));
	CHECK_FALSE(sequence->setShown("c", true));

	sequence.reset();
	obs_wait_for_destroy_queue();
}

TEST_CASE_METHOD(ObsFixture, "a sequence keeps the speed for the next source and plays it forward", "[obs][sequence]")
{
	registerReplaySource({});
	const UiTasks ui;
	ReplayLibrary library;
	std::optional<SequencePlayer> sequence(std::in_place, library);
	REQUIRE(sequence->player());

	const Captured captured(library, {"a", "b", "c"}, 120);
	REQUIRE(sequence->putOnAir(captured.id()));
	CHECK(playing(*sequence) == "a");
	CHECK(sequence->player()->state().rate == 1000);

	sequence->player()->faster();
	sequence->player()->reverse();
	REQUIRE(playerUntil(*sequence, [](const PlayerState &state) { return state.rate == -2000; }));
	sequence->skip();
	CHECK(playing(*sequence) == "b");
	REQUIRE(playerUntil(*sequence, [](const PlayerState &state) { return state.rate == 2000; }));

	sequence->player()->slower();
	sequence->player()->slower();
	REQUIRE(playerUntil(*sequence, [](const PlayerState &state) { return state.rate == 750; }));
	sequence->skip();
	CHECK(playing(*sequence) == "c");
	REQUIRE(playerUntil(*sequence, [](const PlayerState &state) { return state.rate == 750; }));

	// Each replay starts at real time.
	sequence->takeOffAir();
	REQUIRE(sequence->putOnAir(captured.id()));
	CHECK(playing(*sequence) == "a");
	REQUIRE(playerUntil(*sequence, [](const PlayerState &state) { return state.rate == 1000; }));

	sequence.reset();
	obs_wait_for_destroy_queue();
}

TEST_CASE_METHOD(ObsFixture, "a speed given in the frame of a skip or an end carries to the next source",
		 "[obs][sequence]")
{
	registerReplaySource({});
	const UiTasks ui;
	ReplayLibrary library;
	std::optional<SequencePlayer> sequence(std::in_place, library);
	REQUIRE(sequence->player());

	const Captured longer(library, {"a", "b"}, 120);
	REQUIRE(sequence->putOnAir(longer.id()));
	CHECK(playing(*sequence) == "a");
	uint64_t clip = sequence->player()->state().clip;
	{
		const TicksHeld held;
		sequence->player()->faster();
		sequence->skip();
	}
	CHECK(sequence->state().source() == "b");
	REQUIRE(playerUntil(*sequence, [&](const PlayerState &state) { return state.clip != clip; }));
	CHECK(sequence->player()->state().rate == 2000);

	// Clips of a sixth of a second, which play out by themselves; the end waits for the
	// UI thread, and the speed is given before it gets there.
	sequence->takeOffAir();
	const Captured shorter(library, {"a", "b"}, 2);
	REQUIRE(sequence->putOnAir(shorter.id()));
	REQUIRE(playerUntil(*sequence, [&](const PlayerState &state) {
		clip = state.clip;
		return state.ended;
	}));
	REQUIRE(waitFor([] { return UiTasks::pending() > 0; }, 10s));
	{
		const TicksHeld held;
		sequence->player()->faster();
		CHECK(UiTasks::run() == 1);
	}
	CHECK(sequence->state().source() == "b");
	REQUIRE(playerUntil(*sequence, [&](const PlayerState &state) { return state.clip != clip; }));
	CHECK(sequence->player()->state().rate == 2000);

	sequence.reset();
	obs_wait_for_destroy_queue();
}

TEST_CASE_METHOD(ObsFixture, "a sequence goes back to live when cut short, and not before", "[obs][sequence]")
{
	registerReplaySource({});
	const UiTasks ui;
	ReplayLibrary library;
	std::optional<SequencePlayer> sequence(std::in_place, library);
	REQUIRE(sequence->player());

	const Captured captured(library, {"a", "b"}, 120);
	REQUIRE(sequence->putOnAir(captured.id()));
	CHECK(playing(*sequence) == "a");
	// One replay at a time.
	CHECK_FALSE(sequence->putOnAir(captured.id()));

	// Someone who takes the clip off its end as it gets there keeps it on air.
	sequence->player()->seek(captured.lastFrame() - 50ms);
	REQUIRE(playerUntil(*sequence, [](const PlayerState &state) { return state.ended; }));
	sequence->player()->step(-1);
	REQUIRE(playerUntil(*sequence, [](const PlayerState &state) { return !state.ended; }));
	REQUIRE(UiTasks::pending() > 0);
	UiTasks::run();
	CHECK(sequence->state().phase() == AirPhase::Source);
	CHECK(sequence->state().source() == "a");

	// Played to the end again, it moves on.
	sequence->player()->play();
	REQUIRE(runUntil([&] { return sequence->state().source() == "b"; }));
	CHECK(playing(*sequence) == "b");

	sequence->takeOffAir();
	CHECK(sequence->state().phase() == AirPhase::Live);
	REQUIRE(playerUntil(*sequence, [](const PlayerState &state) { return !state.loaded; }));
	sequence->skip();
	CHECK(sequence->state().phase() == AirPhase::Live);

	sequence.reset();
	obs_wait_for_destroy_queue();
}

TEST_CASE_METHOD(ObsFixture, "a sequence back to live lets go of the replay", "[obs][sequence]")
{
	registerReplaySource({});
	const UiTasks ui;
	ReplayLibrary library;
	std::optional<SequencePlayer> sequence(std::in_place, library);
	REQUIRE(sequence->player());

	const Captured captured(library, {"a"}, 120);
	REQUIRE(sequence->putOnAir(captured.id()));
	const std::weak_ptr<const tapeloop::ReplayIndex> index = library.find(captured.id())->index;
	// Gone from the library, the replay stays with the sequence while it is on air.
	REQUIRE(library.remove(captured.id()));
	CHECK_FALSE(index.expired());
	sequence->takeOffAir();
	CHECK(index.expired());

	sequence.reset();
	obs_wait_for_destroy_queue();
}

TEST_CASE_METHOD(ObsFixture, "a sequence leaves out what has nothing to play", "[obs][sequence]")
{
	registerReplaySource({});
	const UiTasks ui;
	ReplayLibrary library;
	std::optional<SequencePlayer> sequence(std::in_place, library);
	REQUIRE(sequence->player());

	const Captured captured(library, {"a", "b"}, 2);
	sequence->setEntries({{"a", false}, {"b", false}});
	CHECK_FALSE(sequence->putOnAir(captured.id()));
	CHECK(sequence->state().phase() == AirPhase::Live);

	REQUIRE(sequence->setShown("a", true));
	CHECK_FALSE(sequence->putOnAir(0));

	// Found on disk, with a manifest that cannot be read.
	tapeloop::FoundReplay found;
	found.manifest = "missing.tlr";
	found.broadcast = "Broadcast";
	found.intact = true;
	found.id = tapeloop::newReplayId();
	found.sources = {{"a", "Camera a"}};
	const uint64_t unreadable = library.addFound(found);
	REQUIRE(unreadable != 0);
	CHECK_FALSE(sequence->putOnAir(unreadable));

	// Damaged, even if what it names can be read.
	const TempDirectory folder;
	found.manifest =
		tapeloop::ReplayWriter().write(captureOf(folder.path(), {sourceOf("a", makeGops(2))})).manifest;
	found.intact = false;
	const uint64_t damaged = library.addFound(found);
	REQUIRE(damaged != 0);
	CHECK_FALSE(sequence->putOnAir(damaged));
	CHECK(sequence->state().phase() == AirPhase::Live);

	sequence.reset();
	obs_wait_for_destroy_queue();
}

TEST_CASE_METHOD(ObsFixture, "a closed sequence goes back to live and forgets its list until opened", "[obs][sequence]")
{
	registerReplaySource({});
	const UiTasks ui;
	ReplayLibrary library;
	std::optional<SequencePlayer> sequence(std::in_place, library);
	REQUIRE(sequence->player());

	const Captured captured(library, {"a", "b"}, 120);
	sequence->setEntries({{"b", true}, {"a", false}});
	REQUIRE(sequence->putOnAir(captured.id()));
	CHECK(playing(*sequence) == "b");

	sequence->close();
	CHECK(sequence->closed());
	CHECK(sequence->state().phase() == AirPhase::Live);
	CHECK(sequence->state().sequence().entries().empty());
	REQUIRE(playerUntil(*sequence, [](const PlayerState &state) { return !state.loaded; }));
	CHECK_FALSE(sequence->putOnAir(captured.id()));
	CHECK(sequence->state().phase() == AirPhase::Live);

	sequence->open();
	CHECK_FALSE(sequence->closed());
	REQUIRE(sequence->putOnAir(captured.id()));
	CHECK(keysOf(*sequence) == std::vector<std::string>{"a", "b"});
	CHECK(playing(*sequence) == "a");

	sequence.reset();
	obs_wait_for_destroy_queue();
}

TEST_CASE_METHOD(ObsFixture, "an end that reaches the UI thread after the sequence is gone does nothing",
		 "[obs][sequence]")
{
	registerReplaySource({});
	const UiTasks ui;
	ReplayLibrary library;
	// On the heap, so that a sanitizer sees a use once it is gone.
	auto sequence = std::make_unique<SequencePlayer>(library);
	REQUIRE(sequence->player());

	// A clip of a sixth of a second plays out by itself, and its end waits for the UI
	// thread.
	const Captured captured(library, {"a"}, 2);
	REQUIRE(sequence->putOnAir(captured.id()));
	REQUIRE(waitFor([] { return UiTasks::pending() > 0; }, 10s));
	sequence.reset();
	obs_wait_for_destroy_queue();
	CHECK(UiTasks::run() == 1);
}

TEST_CASE_METHOD(ObsFixture, "a replay put on air while written plays its next sources from its files",
		 "[obs][sequence]")
{
	// Declared before the source, whose decoding thread writes it until it is destroyed.
	std::atomic<int64_t> newest{-1};
	registerReplaySource(
		[&newest](bool) -> std::unique_ptr<PictureDecoder> { return std::make_unique<NewestDecoder>(newest); });
	const UiTasks ui;
	const TempDirectory folder;
	ReplayLibrary library;
	std::optional<SequencePlayer> sequence(std::in_place, library);
	REQUIRE(sequence->player());

	Gops first = makeGops(120);
	Gops second = makeGops(120, 1000);
	std::optional<tapeloop::ReplayCapture> capture(
		captureOf(folder.path(), {sourceOf("a", first), sourceOf("b", second)}));
	const uint64_t id = library.addCaptured(*capture, "Broadcast");
	REQUIRE(sequence->putOnAir(id));
	CHECK(playing(*sequence) == "a");

	tapeloop::WrittenReplay written = tapeloop::ReplayWriter().write(*capture);
	REQUIRE(library.stored(id, written.manifest, std::move(written.index)));
	// The buffers let go of the GOPs.
	capture.reset();
	first.clear();
	second.clear();
	sequence->skip();
	CHECK(playing(*sequence) == "b");
	CHECK(waitFor([&] { return newest.load() >= 1000; }, 10s));

	sequence.reset();
	obs_wait_for_destroy_queue();
}

TEST_CASE_METHOD(ObsFixture, "a source on air while its replay is written plays on from the files", "[obs][sequence]")
{
	// Declared before the source, whose decoding thread writes it until it is destroyed.
	std::atomic<int64_t> newest{-1};
	registerReplaySource(
		[&newest](bool) -> std::unique_ptr<PictureDecoder> { return std::make_unique<NewestDecoder>(newest); });
	const UiTasks ui;
	const TempDirectory folder;
	ReplayLibrary library;
	std::optional<SequencePlayer> sequence(std::in_place, library);
	REQUIRE(sequence->player());

	// A replay of one source, the whole replay of a one camera match.
	Gops gops = makeGops(120);
	const Nanoseconds late = gops[110]->startTime();
	std::optional<tapeloop::ReplayCapture> capture(captureOf(folder.path(), {sourceOf("a", gops)}));
	const uint64_t id = library.addCaptured(*capture, "Broadcast");
	REQUIRE(sequence->putOnAir(id));
	CHECK(playing(*sequence) == "a");

	tapeloop::WrittenReplay written = tapeloop::ReplayWriter().write(*capture);
	REQUIRE(library.stored(id, written.manifest, std::move(written.index)));
	// The buffer lets go of the GOPs while the source plays.
	capture.reset();
	gops.clear();
	sequence->player()->seek(late);
	CHECK(waitFor([&] { return newest.load() >= 110 * tapeloop::test::kGopLength; }, 10s));

	sequence.reset();
	obs_wait_for_destroy_queue();
}

TEST_CASE_METHOD(ObsFixture, "a source on air reads its files as soon as the store has written them", "[obs][sequence]")
{
	// Declared before the source, whose decoding thread writes it until it is destroyed.
	std::atomic<int64_t> newest{-1};
	registerReplaySource(
		[&newest](bool) -> std::unique_ptr<PictureDecoder> { return std::make_unique<NewestDecoder>(newest); });
	const UiTasks ui;
	const TempDirectory folder;
	ReplayLibrary library;
	std::optional<SequencePlayer> sequence(std::in_place, library);
	REQUIRE(sequence->player());

	Gops gops = makeGops(120);
	const Nanoseconds late = gops[110]->startTime();
	tapeloop::ReplayCapture capture = captureOf(folder.path(), {sourceOf("a", gops)});
	const uint64_t id = library.addCaptured(capture, "Broadcast");
	REQUIRE(sequence->putOnAir(id));
	CHECK(playing(*sequence) == "a");

	// The buffer lets go of the GOPs while the store writes them, and the store once they
	// are written, before anyone has taken in that they are.
	tapeloop::ReplayStore store;
	store.write(std::move(capture), library.find(id)->files);
	gops.clear();
	store.waitUntilIdle();
	sequence->player()->seek(late);
	CHECK(waitFor([&] { return newest.load() >= 110 * tapeloop::test::kGopLength; }, 10s));

	sequence.reset();
	obs_wait_for_destroy_queue();
}

TEST_CASE_METHOD(ObsFixture, "a replay goes on air through its intro and outro and back to live, in order",
		 "[obs][sequence][air]")
{
	registerReplaySource({});
	registerFade();
	const UiTasks ui;
	const TempDirectory folder;
	ReplayLibrary library;
	std::optional<SequencePlayer> sequence(std::in_place, library);
	REQUIRE(sequence->player());

	AirSettings air;
	air.intro = {AirMediaKind::Video, fileIn(folder, "intro.mp4")};
	air.into.duration = 100ms;
	air.outro = {AirMediaKind::Image, fileIn(folder, "outro.png"), 500ms};
	air.back.duration = 100ms;
	sequence->setAir(air);
	obs_source_t *intro = sequence->air().partSource(AirPhase::Intro);
	REQUIRE(intro);
	REQUIRE(sequence->air().partSource(AirPhase::Outro));
	setLength(intro, 60000);
	// Connected after the composition's own, so it counts once the composition saw it.
	std::optional<SignalCount> started(std::in_place, intro, "media_started");

	// Clips of a sixth of a second, which play out by themselves.
	const Captured captured(library, {"a"}, 2);
	CHECK(channelName().empty());
	REQUIRE(sequence->putOnAir(captured.id()));
	CHECK(sequence->state().phase() == AirPhase::Intro);
	CHECK(channelName() == "Tapeloop in");
	CHECK(shownName() == "Tapeloop intro");
	CHECK_FALSE(sequence->player()->state().loaded);
	REQUIRE(runUntil([&] { return started->value() > 0; }));
	CHECK(sequence->state().phase() == AirPhase::Intro);

	obs_source_media_ended(intro);
	REQUIRE(runUntil([&] { return sequence->state().phase() != AirPhase::Intro; }));
	CHECK(sequence->state().phase() == AirPhase::Source);
	CHECK(channelName() == "Tapeloop in");
	CHECK(shownName() == "Tapeloop replay");

	REQUIRE(runUntil([&] { return sequence->state().phase() != AirPhase::Source; }));
	const auto outroShown = std::chrono::steady_clock::now();
	CHECK(sequence->state().phase() == AirPhase::Outro);
	CHECK(channelName() == "Tapeloop in");
	CHECK(shownName() == "Tapeloop outro");
	REQUIRE(playerUntil(*sequence, [](const PlayerState &state) { return !state.loaded; }));

	REQUIRE(runUntil([&] { return sequence->state().phase() != AirPhase::Outro; }));
	CHECK(std::chrono::steady_clock::now() - outroShown >= 450ms);
	CHECK(sequence->state().phase() == AirPhase::Live);
	CHECK(sequence->air().leaving());
	CHECK(channelName() == "Tapeloop out");

	REQUIRE(runUntil([&] { return !sequence->air().onAir(); }));
	CHECK(channelName().empty());
	CHECK_FALSE(sequence->air().leaving());

	started.reset();
	sequence.reset();
	obs_wait_for_destroy_queue();
}

TEST_CASE_METHOD(ObsFixture, "an intro ends only after it started on air", "[obs][sequence][air]")
{
	registerReplaySource({});
	registerFade();
	const UiTasks ui;
	const TempDirectory folder;
	ReplayLibrary library;
	std::optional<SequencePlayer> sequence(std::in_place, library);
	REQUIRE(sequence->player());

	AirSettings air;
	air.intro = {AirMediaKind::Video, fileIn(folder, "intro.mp4")};
	sequence->setAir(air);
	obs_source_t *intro = sequence->air().partSource(AirPhase::Intro);
	REQUIRE(intro);
	setLength(intro, 60000);
	std::optional<SignalCount> started(std::in_place, intro, "media_started");

	const Captured captured(library, {"a"}, 120);
	{
		const TicksHeld held;
		REQUIRE(sequence->putOnAir(captured.id()));
		// The end a video taken off air earlier sends late, before it started again.
		obs_source_media_ended(intro);
	}
	REQUIRE(runUntil([&] { return started->value() > 0; }));
	CHECK(holdsFor(300ms, [&] { return sequence->state().phase() == AirPhase::Intro; }));

	obs_source_media_ended(intro);
	REQUIRE(runUntil([&] { return sequence->state().phase() != AirPhase::Intro; }));
	CHECK(playing(*sequence) == "a");

	started.reset();
	sequence.reset();
	obs_wait_for_destroy_queue();
}

TEST_CASE_METHOD(ObsFixture, "a video intro or outro ends by its length when its end never comes",
		 "[obs][sequence][air]")
{
	registerReplaySource({});
	registerFade();
	const UiTasks ui;
	const TempDirectory folder;
	ReplayLibrary library;
	std::optional<SequencePlayer> sequence(std::in_place, library);
	REQUIRE(sequence->player());

	AirSettings air;
	air.intro = {AirMediaKind::Video, fileIn(folder, "intro.mp4")};
	air.outro = {AirMediaKind::Video, fileIn(folder, "outro.mp4")};
	sequence->setAir(air);
	REQUIRE(sequence->air().partSource(AirPhase::Intro));
	REQUIRE(sequence->air().partSource(AirPhase::Outro));
	setLength(sequence->air().partSource(AirPhase::Intro), 300);

	const Captured captured(library, {"a"}, 2);
	const auto start = std::chrono::steady_clock::now();
	REQUIRE(sequence->putOnAir(captured.id()));
	// Its length and two seconds more, counted from when it started.
	REQUIRE(runUntil([&] { return sequence->state().phase() != AirPhase::Intro; }));
	const auto introTime = std::chrono::steady_clock::now() - start;
	CHECK(introTime >= 2300ms);
	CHECK(introTime < 4500ms);

	// One without a length, which says nothing of its time either, gets five seconds.
	REQUIRE(runUntil([&] { return sequence->state().phase() == AirPhase::Outro; }));
	const auto outroShown = std::chrono::steady_clock::now();
	REQUIRE(runUntil([&] { return sequence->state().phase() != AirPhase::Outro; }));
	CHECK(std::chrono::steady_clock::now() - outroShown >= 4900ms);
	CHECK(sequence->state().phase() == AirPhase::Live);

	sequence.reset();
	obs_wait_for_destroy_queue();
}

TEST_CASE_METHOD(ObsFixture, "a replay cut short goes through its outro, and closed goes off air at once",
		 "[obs][sequence][air]")
{
	registerReplaySource({});
	registerFade();
	const UiTasks ui;
	const TempDirectory folder;
	ReplayLibrary library;
	std::optional<SequencePlayer> sequence(std::in_place, library);
	REQUIRE(sequence->player());

	AirSettings air;
	air.intro = {AirMediaKind::Image, fileIn(folder, "intro.png"), 500ms};
	air.outro = {AirMediaKind::Image, fileIn(folder, "outro.png"), 20000ms};
	sequence->setAir(air);

	const Captured captured(library, {"a"}, 120);
	REQUIRE(sequence->putOnAir(captured.id()));
	CHECK(sequence->state().phase() == AirPhase::Intro);
	REQUIRE(runUntil([&] { return sequence->state().phase() == AirPhase::Source; }));
	CHECK(playing(*sequence) == "a");
	sequence->takeOffAir();
	CHECK(sequence->state().phase() == AirPhase::Outro);
	CHECK(shownName() == "Tapeloop outro");
	REQUIRE(playerUntil(*sequence, [](const PlayerState &state) { return !state.loaded; }));
	sequence->takeOffAir();
	CHECK(sequence->state().phase() == AirPhase::Live);
	CHECK(channelName() == "Tapeloop out");
	REQUIRE(runUntil([&] { return !sequence->air().onAir(); }));

	REQUIRE(sequence->putOnAir(captured.id()));
	REQUIRE(runUntil([&] { return sequence->state().phase() == AirPhase::Source; }));
	CHECK(playing(*sequence) == "a");
	// Neither the outro nor the way back: the collection they were shown over goes away.
	sequence->close();
	CHECK(sequence->state().phase() == AirPhase::Live);
	CHECK_FALSE(sequence->air().onAir());
	CHECK(channelName().empty());
	REQUIRE(playerUntil(*sequence, [](const PlayerState &state) { return !state.loaded; }));
	CHECK_FALSE(sequence->putOnAir(captured.id()));
	CHECK(channelName().empty());

	sequence->open();
	REQUIRE(sequence->putOnAir(captured.id()));
	CHECK(sequence->state().phase() == AirPhase::Intro);

	sequence.reset();
	obs_wait_for_destroy_queue();
}

TEST_CASE_METHOD(ObsFixture, "a replay whose channel another source takes goes back to live", "[obs][sequence][air]")
{
	registerReplaySource({});
	registerFade();
	const UiTasks ui;
	const TempDirectory folder;
	ReplayLibrary library;
	std::optional<SequencePlayer> sequence(std::in_place, library);
	REQUIRE(sequence->player());

	// The outro stays off the channel too: it would take it back from the other source.
	AirSettings air;
	air.outro = {AirMediaKind::Image, fileIn(folder, "outro.png"), 20000ms};
	sequence->setAir(air);

	const Captured captured(library, {"a"}, 120);
	REQUIRE(sequence->putOnAir(captured.id()));
	CHECK(playing(*sequence) == "a");

	const OBSSourceAutoRelease other =
		obs_source_create_private(tapeloop::test::kTestPatternId, "Another plugin", nullptr);
	REQUIRE(other);
	obs_set_output_source(kAirChannel, other);
	REQUIRE(runUntil([&] { return sequence->state().phase() == AirPhase::Live; }));
	CHECK_FALSE(sequence->air().onAir());
	CHECK(channelName() == "Another plugin");
	REQUIRE(playerUntil(*sequence, [](const PlayerState &state) { return !state.loaded; }));

	// The channel is the other source's until it lets go.
	CHECK_FALSE(sequence->putOnAir(captured.id()));
	CHECK(channelName() == "Another plugin");
	obs_set_output_source(kAirChannel, nullptr);
	REQUIRE(sequence->putOnAir(captured.id()));
	CHECK(channelName() == "Tapeloop in");

	sequence.reset();
	CHECK(channelName().empty());
	obs_wait_for_destroy_queue();
}

TEST_CASE_METHOD(ObsFixture, "a replay put on air during the way back stays on air", "[obs][sequence][air]")
{
	registerReplaySource({});
	registerFade();
	const UiTasks ui;
	ReplayLibrary library;
	std::optional<SequencePlayer> sequence(std::in_place, library);
	REQUIRE(sequence->player());

	AirSettings air;
	air.back.duration = 1000ms;
	sequence->setAir(air);

	const Captured captured(library, {"a", "b"}, 120);
	REQUIRE(sequence->putOnAir(captured.id()));
	CHECK(playing(*sequence) == "a");
	sequence->takeOffAir();
	CHECK(sequence->air().leaving());
	CHECK(channelName() == "Tapeloop out");
	// The way back fades out the last frame.
	CHECK(sequence->player()->state().loaded);

	REQUIRE(sequence->putOnAir(captured.id()));
	CHECK_FALSE(sequence->air().leaving());
	CHECK(channelName() == "Tapeloop in");
	CHECK(holdsFor(1500ms, [&] {
		return sequence->state().phase() == AirPhase::Source && sequence->air().onAir() &&
		       channelName() == "Tapeloop in";
	}));
	CHECK(playing(*sequence) == "a");

	sequence.reset();
	obs_wait_for_destroy_queue();
}

TEST_CASE_METHOD(ObsFixture, "a video intro put back on air during the way back starts over and plays out",
		 "[obs][sequence][air]")
{
	registerReplaySource({});
	registerFade();
	const UiTasks ui;
	const TempDirectory folder;
	ReplayLibrary library;
	std::optional<SequencePlayer> sequence(std::in_place, library);
	REQUIRE(sequence->player());

	AirSettings air;
	air.intro = {AirMediaKind::Video, fileIn(folder, "intro.mp4")};
	air.into.duration = 100ms;
	air.back.duration = 1000ms;
	sequence->setAir(air);
	obs_source_t *intro = sequence->air().partSource(AirPhase::Intro);
	REQUIRE(intro);
	setLength(intro, 10000);
	std::optional<SignalCount> started(std::in_place, intro, "media_started");

	const Captured captured(library, {"a"}, 120);
	REQUIRE(sequence->putOnAir(captured.id()));
	REQUIRE(runUntil([&] { return started->value() == 1; }));
	sequence->takeOffAir();
	REQUIRE(sequence->air().leaving());
	CHECK(channelName() == "Tapeloop out");

	// It never goes inactive on the way from one transition to the other.
	REQUIRE(sequence->putOnAir(captured.id()));
	CHECK(shownName() == "Tapeloop intro");
	REQUIRE(runUntil([&] { return started->value() == 2; }));
	// Past the wait for a video that never starts, and short of its length.
	CHECK(holdsFor(5500ms, [&] { return sequence->state().phase() == AirPhase::Intro; }));

	obs_source_media_ended(intro);
	REQUIRE(runUntil([&] { return sequence->state().phase() == AirPhase::Source; }));

	started.reset();
	sequence.reset();
	obs_wait_for_destroy_queue();
}

TEST_CASE_METHOD(ObsFixture, "a video intro put on air just as the way back ends starts over", "[obs][sequence][air]")
{
	registerReplaySource({});
	registerFade();
	const UiTasks ui;
	const TempDirectory folder;
	ReplayLibrary library;
	std::optional<SequencePlayer> sequence(std::in_place, library);
	REQUIRE(sequence->player());

	AirSettings air;
	air.intro = {AirMediaKind::Video, fileIn(folder, "intro.mp4")};
	air.into.duration = 100ms;
	air.back.duration = 100ms;
	sequence->setAir(air);
	obs_source_t *intro = sequence->air().partSource(AirPhase::Intro);
	REQUIRE(intro);
	setLength(intro, 10000);
	std::optional<SignalCount> started(std::in_place, intro, "media_started");

	const Captured captured(library, {"a"}, 120);
	REQUIRE(sequence->putOnAir(captured.id()));
	REQUIRE(runUntil([&] { return started->value() == 1; }));
	sequence->takeOffAir();

	// The way back ended, which the UI thread has yet to hear, and libobs may not have seen
	// the intro go inactive.
	const OBSSourceAutoRelease out = obs_get_output_source(kAirChannel);
	REQUIRE(waitFor([&] { return !OBSSourceAutoRelease(obs_transition_get_source(out, OBS_TRANSITION_SOURCE_A)); },
			10s));
	REQUIRE(sequence->air().leaving());

	REQUIRE(sequence->putOnAir(captured.id()));
	REQUIRE(runUntil([&] { return started->value() >= 2; }));
	CHECK(holdsFor(5500ms, [&] { return sequence->state().phase() == AirPhase::Intro; }));

	started.reset();
	sequence.reset();
	obs_wait_for_destroy_queue();
}

TEST_CASE_METHOD(ObsFixture, "a replay goes on air with copies of the user's transitions, or else fades",
		 "[obs][sequence][air]")
{
	registerReplaySource({});
	registerFade();
	const UiTasks ui;
	const Catalog catalog;
	ReplayLibrary library;
	std::optional<SequencePlayer> sequence(std::in_place, library);
	REQUIRE(sequence->player());
	sequence->setTransitions(&catalog);

	AirSettings air;
	air.into = {"Swipe", 100ms};
	// One OBS has, of a kind no module registered.
	air.back = {"Ghost", 100ms};
	sequence->setAir(air);

	const Captured captured(library, {"a"}, 120);
	REQUIRE(sequence->putOnAir(captured.id()));
	CHECK(channelName() == "Tapeloop in");
	CHECK(idOnChannel() == "fade_transition");
	CHECK(markerOnChannel() == 7);
	sequence->takeOffAir();
	CHECK(channelName() == "Tapeloop out");
	CHECK(idOnChannel() == "fade_transition");
	CHECK(markerOnChannel() == 0);
	REQUIRE(runUntil([&] { return !sequence->air().onAir(); }));

	// One OBS does not have.
	air.into.name = "Unknown";
	sequence->setAir(air);
	REQUIRE(sequence->putOnAir(captured.id()));
	CHECK(channelName() == "Tapeloop in");
	CHECK(idOnChannel() == "fade_transition");
	CHECK(markerOnChannel() == 0);

	sequence.reset();
	obs_wait_for_destroy_queue();
}

// Past the quarter of a second a stand-in of OBS's stinger has without its media's length.
bool stingerPlays()
{
	return holdsFor(400ms, [] { return timeOnChannel() < 1.0f; });
}

TEST_CASE_METHOD(ObsFixture, "a transition with a length of its own runs it the first time, and after an edit in OBS",
		 "[obs][sequence][air]")
{
	registerReplaySource({});
	registerFade();
	registerStinger();
	const UiTasks ui;
	Catalog catalog;
	catalog.stinger = R"({"media_ms": 2000})";
	ReplayLibrary library;
	std::optional<SequencePlayer> sequence(std::in_place, library);
	REQUIRE(sequence->player());
	sequence->setTransitions(&catalog);

	AirSettings air;
	air.into = {"Stinger", 100ms};
	air.back = {"Stinger", 100ms};
	sequence->setAir(air);

	const Captured captured(library, {"a"}, 120);
	REQUIRE(sequence->putOnAir(captured.id()));
	CHECK(idOnChannel() == kStingerId);
	CHECK(stingerPlays());
	sequence->takeOffAir();
	CHECK(channelName() == "Tapeloop out");
	CHECK(stingerPlays());
	REQUIRE(runUntil([&] { return !sequence->air().onAir(); }));

	// The same again with the instances already there.
	REQUIRE(sequence->putOnAir(captured.id()));
	CHECK(stingerPlays());
	sequence->takeOffAir();
	CHECK(stingerPlays());
	REQUIRE(runUntil([&] { return !sequence->air().onAir(); }));

	// The user edits it, with nothing to tell the composition but the next replay.
	catalog.stinger = R"({"media_ms": 3000, "marker": 9})";
	REQUIRE(sequence->putOnAir(captured.id()));
	CHECK(markerOnChannel() == 9);
	CHECK(stingerPlays());
	sequence->takeOffAir();
	CHECK(markerOnChannel() == 9);
	CHECK(stingerPlays());

	sequence.reset();
	obs_wait_for_destroy_queue();
}

TEST_CASE_METHOD(ObsFixture, "a replay waits for a transition's video to open only when it was just made",
		 "[obs][sequence][air]")
{
	registerReplaySource({});
	registerFade();
	registerStinger();
	const UiTasks ui;
	Catalog catalog;
	catalog.stinger = R"({"media_ms": 2000, "open_after_ms": 150})";
	ReplayLibrary library;
	std::optional<SequencePlayer> sequence(std::in_place, library);
	REQUIRE(sequence->player());
	sequence->setTransitions(&catalog);

	AirSettings air;
	air.into = {"Stinger", 100ms};
	sequence->setAir(air);
	const Captured captured(library, {"a"}, 120);

	auto before = std::chrono::steady_clock::now();
	REQUIRE(sequence->putOnAir(captured.id()));
	const auto waited = std::chrono::steady_clock::now() - before;
	CHECK(stingerPlays());
	sequence->takeOffAir();
	REQUIRE(runUntil([&] { return !sequence->air().onAir(); }));

	// Made when the settings came, it has opened by the time a replay goes on air.
	catalog.stinger = R"({"media_ms": 2000, "open_after_ms": 150, "marker": 9})";
	sequence->setAir(air);
	CHECK(holdsFor(300ms, [] { return true; }));
	before = std::chrono::steady_clock::now();
	REQUIRE(sequence->putOnAir(captured.id()));
	const auto opened = std::chrono::steady_clock::now() - before;
	CHECK(markerOnChannel() == 9);
	CHECK(opened < waited / 2);
	CHECK(stingerPlays());

	sequence.reset();
	obs_wait_for_destroy_queue();
}

// What the composition does to a stand-in stinger, which it keeps until it goes.
class WatchedStinger {
public:
	explicit WatchedStinger(obs_source_t *transition)
		: stinger_(*static_cast<const Stinger *>(obs_obj_get_data(transition))),
		  stops_(transition, "transition_stop")
	{
	}

	int stops() const noexcept { return stops_.value(); }
	bool plays() const { return stinger_.running && obs_source_active(stinger_.media); }
	bool stopped() const { return !stinger_.running && !obs_source_active(stinger_.media); }

private:
	const Stinger &stinger_;
	SignalCount stops_;
};

// A replay put on air through the stand-in, whose video takes a second, with an image for
// intro that is on air for the time given.
struct StingerOnAir {
	explicit StingerOnAir(std::chrono::milliseconds image)
	{
		registerReplaySource({});
		registerFade();
		registerStinger();
		catalog.stinger = R"({"media_ms": 1000})";
		sequence.emplace(library);
		REQUIRE(sequence->player());
		sequence->setTransitions(&catalog);

		AirSettings air;
		air.intro = {AirMediaKind::Image, fileIn(folder, "intro.png"), image};
		air.into = {"Stinger", 100ms};
		air.back = {"Stinger", 100ms};
		sequence->setAir(air);

		REQUIRE(sequence->putOnAir(captured.id()));
		const OBSSourceAutoRelease channel = obs_get_output_source(kAirChannel);
		in.emplace(channel);
		REQUIRE(in->plays());
	}

	~StingerOnAir()
	{
		in.reset();
		out.reset();
		sequence.reset();
		obs_wait_for_destroy_queue();
	}

	StingerOnAir(const StingerOnAir &) = delete;
	StingerOnAir &operator=(const StingerOnAir &) = delete;
	StingerOnAir(StingerOnAir &&) = delete;
	StingerOnAir &operator=(StingerOnAir &&) = delete;

	void watchOut()
	{
		const OBSSourceAutoRelease channel = obs_get_output_source(kAirChannel);
		REQUIRE(std::string(obs_source_get_name(channel)) == "Tapeloop out");
		out.emplace(channel);
	}

	const UiTasks ui;
	const TempDirectory folder;
	Catalog catalog;
	ReplayLibrary library;
	const Captured captured{library, {"a"}, 120};
	std::optional<SequencePlayer> sequence;
	std::optional<WatchedStinger> in;
	std::optional<WatchedStinger> out;
};

TEST_CASE_METHOD(ObsFixture, "a part that ends under a transition stops it", "[obs][sequence][air]")
{
	StingerOnAir air(500ms);
	REQUIRE(runUntil([&] { return air.sequence->state().phase() == AirPhase::Source; }));
	// Short of its length, the video's and a quarter of a second, so the part did this.
	CHECK(air.in->stops() == 1);
	CHECK(air.in->stopped());
	CHECK(shownName() == "Tapeloop replay");
	CHECK(holdsFor(1000ms, [&] { return air.in->stops() == 1; }));
}

TEST_CASE_METHOD(ObsFixture, "a skip during a transition stops it at once", "[obs][sequence][air]")
{
	StingerOnAir air(20000ms);
	air.sequence->skip();
	CHECK(air.sequence->state().phase() == AirPhase::Source);
	CHECK(shownName() == "Tapeloop replay");
	CHECK(air.in->stops() == 1);
	CHECK(air.in->stopped());
}

TEST_CASE_METHOD(ObsFixture, "a replay taken off air during a transition stops it and goes back at once",
		 "[obs][sequence][air]")
{
	StingerOnAir air(20000ms);
	air.sequence->takeOffAir();
	air.watchOut();
	CHECK(air.in->stops() == 1);
	CHECK(air.in->stopped());
	CHECK(air.out->stops() == 0);
	CHECK(air.out->plays());

	// The way back runs out by itself, which leaving the air does not repeat.
	REQUIRE(runUntil([&] { return !air.sequence->air().onAir(); }));
	CHECK(air.out->stops() == 1);
	CHECK(air.out->stopped());
	CHECK(air.in->stops() == 1);
}

TEST_CASE_METHOD(ObsFixture, "a replay put back on air during the way back stops it at once", "[obs][sequence][air]")
{
	StingerOnAir air(20000ms);
	air.sequence->takeOffAir();
	air.watchOut();
	REQUIRE(air.out->plays());

	REQUIRE(air.sequence->putOnAir(air.captured.id()));
	CHECK(channelName() == "Tapeloop in");
	CHECK(air.out->stops() == 1);
	CHECK(air.out->stopped());
	CHECK(air.in->stops() == 1);
	CHECK(air.in->plays());
}

TEST_CASE_METHOD(ObsFixture, "a transition stops when another source takes the channel from it", "[obs][sequence][air]")
{
	StingerOnAir air(20000ms);
	const OBSSourceAutoRelease other =
		obs_source_create_private(tapeloop::test::kTestPatternId, "Another plugin", nullptr);
	REQUIRE(other);
	obs_set_output_source(kAirChannel, other);
	REQUIRE(runUntil([&] { return !air.sequence->air().onAir(); }));
	CHECK(air.in->stops() == 1);
	CHECK(air.in->stopped());
	obs_set_output_source(kAirChannel, nullptr);
}

TEST_CASE_METHOD(ObsFixture, "a transition stops with the sequence that ran it", "[obs][sequence][air]")
{
	StingerOnAir air(20000ms);
	air.sequence.reset();
	CHECK(air.in->stops() == 1);
	CHECK(air.in->stopped());
}

TEST_CASE_METHOD(ObsFixture, "a way back stops with the sequence that ran it", "[obs][sequence][air]")
{
	StingerOnAir air(20000ms);
	air.sequence->takeOffAir();
	air.watchOut();
	REQUIRE(air.out->plays());
	air.sequence.reset();
	CHECK(air.out->stops() == 1);
	CHECK(air.out->stopped());
}

TEST_CASE_METHOD(ObsFixture, "a transition that ran its length is not stopped again", "[obs][sequence][air]")
{
	StingerOnAir air(3000ms);
	REQUIRE(runUntil([&] { return air.in->stops() == 1; }));
	CHECK(air.sequence->state().phase() == AirPhase::Intro);
	CHECK(air.in->stopped());

	REQUIRE(runUntil([&] { return air.sequence->state().phase() == AirPhase::Source; }));
	CHECK(air.in->stops() == 1);
	air.sequence->takeOffAir();
	air.watchOut();
	CHECK(air.in->stops() == 1);
	REQUIRE(runUntil([&] { return !air.sequence->air().onAir(); }));
	CHECK(air.in->stops() == 1);
	CHECK(air.out->stops() == 1);
}

TEST_CASE_METHOD(ObsFixture, "a transition whose video never opens holds a replay back once, and briefly",
		 "[obs][sequence][air]")
{
	registerReplaySource({});
	registerFade();
	registerStinger();
	const UiTasks ui;
	Catalog catalog;
	catalog.stinger = R"({"media_ms": 2000, "open_after_ms": 600000})";
	ReplayLibrary library;
	std::optional<SequencePlayer> sequence(std::in_place, library);
	REQUIRE(sequence->player());
	sequence->setTransitions(&catalog);

	AirSettings air;
	air.into = {"Stinger", 100ms};
	sequence->setAir(air);
	const Captured captured(library, {"a"}, 120);

	const auto timePutOnAir = [&] {
		const auto before = std::chrono::steady_clock::now();
		REQUIRE(sequence->putOnAir(captured.id()));
		return std::chrono::steady_clock::now() - before;
	};
	const auto comeOff = [&] {
		sequence->takeOffAir();
		REQUIRE(runUntil([&] { return !sequence->air().onAir(); }));
	};

	const auto first = timePutOnAir();
	CHECK(first < 1s);
	CHECK(shownName() == "Tapeloop replay");
	// With no length to go by, it runs out at once as OBS's own does.
	CHECK(runUntil([] { return timeOnChannel() >= 1.0f; }));
	comeOff();

	const auto second = timePutOnAir();
	CHECK(second < first);
	comeOff();

	// One with no video at all has nothing to wait for.
	catalog.stinger = "{}";
	const auto noVideo = timePutOnAir();
	CHECK(noVideo < first);
	CHECK(shownName() == "Tapeloop replay");

	sequence.reset();
	obs_wait_for_destroy_queue();
}

TEST_CASE_METHOD(ObsFixture, "an intro whose file is missing is looked for again by the next replay",
		 "[obs][sequence][air]")
{
	registerReplaySource({});
	const UiTasks ui;
	const TempDirectory folder;
	ReplayLibrary library;
	std::optional<SequencePlayer> sequence(std::in_place, library);
	REQUIRE(sequence->player());

	const std::filesystem::path path = folder.path() / "intro.png";
	AirSettings air;
	air.intro = {AirMediaKind::Image, path.string(), 500ms};
	sequence->setAir(air);
	CHECK_FALSE(sequence->air().hasIntro());

	const Captured captured(library, {"a"}, 120);
	REQUIRE(sequence->putOnAir(captured.id()));
	CHECK(sequence->state().phase() == AirPhase::Source);
	sequence->takeOffAir();
	CHECK(sequence->state().phase() == AirPhase::Live);

	CHECK(fileIn(folder, "intro.png") == path.string());
	REQUIRE(sequence->putOnAir(captured.id()));
	CHECK(sequence->state().phase() == AirPhase::Intro);
	CHECK(sequence->air().hasIntro());

	sequence.reset();
	obs_wait_for_destroy_queue();
}

TEST_CASE_METHOD(ObsFixture, "without OBS's fade, a replay cuts on air and off", "[obs][sequence][air]")
{
	registerReplaySource({});
	const UiTasks ui;
	ReplayLibrary library;
	std::optional<SequencePlayer> sequence(std::in_place, library);
	REQUIRE(sequence->player());

	const Captured captured(library, {"a"}, 120);
	REQUIRE(sequence->putOnAir(captured.id()));
	{
		const OBSSourceAutoRelease onChannel = obs_get_output_source(kAirChannel);
		CHECK(onChannel.Get() == sequence->source());
	}
	CHECK(playing(*sequence) == "a");
	sequence->takeOffAir();
	CHECK(sequence->state().phase() == AirPhase::Live);
	CHECK(channelName().empty());
	REQUIRE(playerUntil(*sequence, [](const PlayerState &state) { return !state.loaded; }));

	sequence.reset();
	obs_wait_for_destroy_queue();
}

TEST_CASE_METHOD(ObsFixture, "a sequence lets go of what it made for air before OBS shuts down", "[obs][sequence][air]")
{
	registerReplaySource({});
	registerFade();
	const UiTasks ui;
	const TempDirectory folder;
	ReplayLibrary library;
	std::optional<SequencePlayer> sequence(std::in_place, library);
	REQUIRE(sequence->player());

	AirSettings air;
	air.intro = {AirMediaKind::Image, fileIn(folder, "intro.png"), 20000ms};
	air.outro = {AirMediaKind::Video, fileIn(folder, "outro.mp4")};
	sequence->setAir(air);

	std::vector<OBSWeakSourceAutoRelease> made;
	made.emplace_back(obs_source_get_weak_source(sequence->air().partSource(AirPhase::Intro)));
	made.emplace_back(obs_source_get_weak_source(sequence->air().partSource(AirPhase::Outro)));
	const Captured captured(library, {"a"}, 120);
	REQUIRE(sequence->putOnAir(captured.id()));
	{
		const OBSSourceAutoRelease in = obs_get_output_source(kAirChannel);
		made.emplace_back(obs_source_get_weak_source(in));
	}
	sequence->takeOffAir();
	CHECK(sequence->state().phase() == AirPhase::Outro);
	sequence->takeOffAir();
	CHECK(sequence->air().leaving());
	{
		const OBSSourceAutoRelease out = obs_get_output_source(kAirChannel);
		made.emplace_back(obs_source_get_weak_source(out));
	}
	for (const OBSWeakSourceAutoRelease &weak : made) {
		REQUIRE(weak);
		CHECK_FALSE(obs_weak_source_expired(weak));
	}

	sequence->destroyAir();
	CHECK(sequence->state().phase() == AirPhase::Live);
	CHECK(channelName().empty());
	// The audio thread holds what it mixes until the end of its turn.
	for (const OBSWeakSourceAutoRelease &weak : made) {
		CHECK(waitFor([&] { return obs_weak_source_expired(weak); }, 2000ms));
	}
	REQUIRE(playerUntil(*sequence, [](const PlayerState &state) { return !state.loaded; }));
	CHECK_FALSE(sequence->putOnAir(captured.id()));
	sequence->open();
	CHECK(sequence->closed());
	CHECK_FALSE(sequence->putOnAir(captured.id()));

	sequence.reset();
	obs_wait_for_destroy_queue();
}

TEST_CASE_METHOD(ObsFixture, "a video intro that never says it started gives way after the wait for it",
		 "[obs][sequence][air]")
{
	registerFade();
	AirSetup setup;
	AirSettings air;
	air.intro = {AirMediaKind::Video, fileIn(setup.folder, "intro.mp4")};
	setup.sequence->setAir(air);
	obs_source_t *intro = setup.sequence->air().partSource(AirPhase::Intro);
	REQUIRE(intro);
	const OBSDataAutoRelease silent = obs_data_create();
	obs_data_set_bool(silent, "restart_on_activate", false);
	obs_data_set_int(silent, "duration_ms", 60000);
	obs_source_update(intro, silent);
	// The update waits for a tick, and until then the video would say it started.
	REQUIRE(runUntil([&] { return obs_source_media_get_duration(intro) == 60000; }));

	const auto start = std::chrono::steady_clock::now();
	setup.putOnAir();
	CHECK(holdsFor(4000ms, [&] { return setup.phase() == AirPhase::Intro; }));
	REQUIRE(runUntil([&] { return setup.phase() != AirPhase::Intro; }));
	CHECK(std::chrono::steady_clock::now() - start < 8s);
	CHECK(setup.phase() == AirPhase::Source);
}

TEST_CASE_METHOD(ObsFixture, "a video intro that says it started again keeps to its first start",
		 "[obs][sequence][air]")
{
	registerFade();
	AirSetup setup;
	AirSettings air;
	air.intro = {AirMediaKind::Video, fileIn(setup.folder, "intro.mp4")};
	setup.sequence->setAir(air);
	obs_source_t *intro = setup.sequence->air().partSource(AirPhase::Intro);
	REQUIRE(intro);
	setLength(intro, 300);

	setup.putOnAir();
	// Its end never comes, so its length and the slack after it, counted from the first start,
	// end it within this.
	const auto giveUp = std::chrono::steady_clock::now() + 6s;
	while (setup.phase() == AirPhase::Intro && std::chrono::steady_clock::now() < giveUp) {
		obs_source_media_started(intro);
		UiTasks::run();
		std::this_thread::sleep_for(20ms);
	}
	CHECK(setup.phase() == AirPhase::Source);
}

TEST_CASE_METHOD(ObsFixture, "a video intro is made to play once from the start and stay open", "[obs][sequence][air]")
{
	AirSetup setup;
	AirSettings air;
	air.intro = {AirMediaKind::Video, fileIn(setup.folder, "intro.mp4")};
	setup.sequence->setAir(air);
	REQUIRE(setup.sequence->air().partSource(AirPhase::Intro));

	// OBS's media source has its own defaults, which these must not be left to. What it does
	// with them is not shown here, as the harness does not load it.
	const OBSDataAutoRelease settings = obs_source_get_settings(setup.sequence->air().partSource(AirPhase::Intro));
	const auto says = [&](const char *name, bool value) {
		return obs_data_has_user_value(settings, name) && obs_data_get_bool(settings, name) == value;
	};
	CHECK(says("is_local_file", true));
	CHECK(std::string(obs_data_get_string(settings, "local_file")) == air.intro.path);
	CHECK(says("looping", false));
	CHECK(says("restart_on_activate", true));
	CHECK(says("clear_on_media_end", false));
	CHECK(says("close_when_inactive", false));
}

TEST_CASE_METHOD(ObsFixture, "a replay closed just after another source took its channel leaves that source there",
		 "[obs][sequence][air]")
{
	registerFade();
	AirSetup setup;
	setup.putOnAir();
	const OBSSourceAutoRelease other =
		obs_source_create_private(tapeloop::test::kTestPatternId, "Another plugin", nullptr);
	REQUIRE(other);
	obs_set_output_source(kAirChannel, other);

	// Before the UI thread hears of it.
	setup.sequence->close();
	CHECK(setup.phase() == AirPhase::Live);
	CHECK(channelName() == "Another plugin");
	obs_set_output_source(kAirChannel, nullptr);
}

TEST_CASE_METHOD(ObsFixture, "a replay cut off air just after another source took its channel leaves that source there",
		 "[obs][sequence][air]")
{
	AirSetup setup;
	setup.putOnAir();
	const OBSSourceAutoRelease other =
		obs_source_create_private(tapeloop::test::kTestPatternId, "Another plugin", nullptr);
	REQUIRE(other);
	obs_set_output_source(kAirChannel, other);

	// Before the UI thread hears of it.
	setup.sequence->takeOffAir();
	CHECK(setup.phase() == AirPhase::Live);
	CHECK(channelName() == "Another plugin");
	obs_set_output_source(kAirChannel, nullptr);
}

TEST_CASE_METHOD(ObsFixture, "a channel given back before the UI thread looks keeps the replay on air",
		 "[obs][sequence][air]")
{
	registerFade();
	AirSetup setup;
	setup.putOnAir();
	{
		const OBSSourceAutoRelease ours = obs_get_output_source(kAirChannel);
		const OBSSourceAutoRelease other =
			obs_source_create_private(tapeloop::test::kTestPatternId, "Another plugin", nullptr);
		REQUIRE(other);
		obs_set_output_source(kAirChannel, other);
		obs_set_output_source(kAirChannel, ours);
	}

	REQUIRE(waitFor([] { return UiTasks::pending() > 0; }, 5000ms));
	CHECK(holdsFor(100ms, [&] { return setup.phase() == AirPhase::Source && setup.sequence->air().onAir(); }));
	CHECK(channelName() == "Tapeloop in");
}

TEST_CASE_METHOD(ObsFixture, "the end of a part skipped before the UI thread heard of it counts for nothing",
		 "[obs][sequence][air]")
{
	registerFade();
	AirSetup setup;
	AirSettings air;
	air.intro = {AirMediaKind::Image, fileIn(setup.folder, "intro.png"), 100ms};
	setup.sequence->setAir(air);
	setup.putOnAir();

	REQUIRE(waitFor([] { return UiTasks::pending() > 0; }, 5000ms));
	setup.sequence->skip();
	CHECK(setup.phase() == AirPhase::Source);
	CHECK(holdsFor(300ms, [&] { return setup.phase() == AirPhase::Source; }));
}

TEST_CASE_METHOD(ObsFixture,
		 "the end of an outro cut short before the UI thread heard of it leaves the way back running",
		 "[obs][sequence][air]")
{
	registerFade();
	AirSetup setup;
	AirSettings air;
	air.outro = {AirMediaKind::Image, fileIn(setup.folder, "outro.png"), 100ms};
	air.back = {"", 20000ms};
	setup.sequence->setAir(air);
	setup.putOnAir();
	setup.sequence->takeOffAir();
	REQUIRE(setup.phase() == AirPhase::Outro);

	REQUIRE(waitFor([] { return UiTasks::pending() > 0; }, 5000ms));
	setup.sequence->takeOffAir();
	CHECK(setup.leaving());
	CHECK(holdsFor(300ms, [&] { return setup.leaving(); }));
}

TEST_CASE_METHOD(ObsFixture, "a part on air stays active as the way back takes over from it", "[obs][sequence][air]")
{
	registerFade();
	AirSetup setup;
	setup.putOnAir();
	REQUIRE(obs_source_active(setup.sequence->source()));

	const ActiveAtChange watched(setup.sequence->source());
	setup.sequence->takeOffAir();
	REQUIRE(setup.leaving());
	CHECK(watched.changes() == 1);
	CHECK(watched.active());
}

TEST_CASE_METHOD(ObsFixture, "a replay's transitions fit the parts to the canvas", "[obs][sequence][air]")
{
	registerFade();
	AirSetup setup;
	AirSettings air;
	air.back = {"", 20000ms};
	setup.sequence->setAir(air);

	setup.putOnAir();
	CHECK(channelName() == "Tapeloop in");
	checkTransitionOnChannelFits();

	setup.sequence->takeOffAir();
	REQUIRE(setup.leaving());
	CHECK(channelName() == "Tapeloop out");
	checkTransitionOnChannelFits();
}

TEST_CASE_METHOD(ObsFixture, "a way back that ran out lets go of the replay's last frame", "[obs][sequence][air]")
{
	registerFade();
	AirSetup setup;
	AirSettings air;
	air.back = {"", 100ms};
	setup.sequence->setAir(air);
	setup.putOnAir();
	CHECK(playing(*setup.sequence) == "a");

	setup.sequence->takeOffAir();
	REQUIRE(setup.leaving());
	REQUIRE(runUntil([&] { return !setup.sequence->air().onAir(); }));
	CHECK(playerUntil(*setup.sequence, [](const PlayerState &state) { return !state.loaded; }));
}

TEST_CASE_METHOD(ObsFixture, "a replay closed during a way back lets go of its last frame", "[obs][sequence][air]")
{
	registerFade();
	AirSetup setup;
	AirSettings air;
	air.back = {"", 20000ms};
	setup.sequence->setAir(air);
	setup.putOnAir();
	CHECK(playing(*setup.sequence) == "a");

	setup.sequence->takeOffAir();
	REQUIRE(setup.leaving());
	CHECK(setup.sequence->player()->state().loaded);

	setup.sequence->close();
	CHECK_FALSE(setup.sequence->air().onAir());
	CHECK(playerUntil(*setup.sequence, [](const PlayerState &state) { return !state.loaded; }));
}

TEST_CASE_METHOD(ObsFixture, "a replay on air changes nothing in OBS's lists", "[obs][sequence][air]")
{
	registerFade();
	const OBSSceneAutoRelease scene = obs_scene_create("Match");
	REQUIRE(scene);
	const OBSSourceAutoRelease camera =
		obs_source_create(tapeloop::test::kTestPatternId, "Camera", nullptr, nullptr);
	REQUIRE(camera);
	REQUIRE(obs_scene_add(scene, camera));
	obs_set_output_source(0, obs_scene_get_source(scene));

	const std::vector<std::string> before = listsOfObs();
	const std::vector<std::string> expected = {"input Camera",  "item Match Camera", "program Match",
						   "public Camera", "public Match",      "scene Match"};
	CHECK(before == expected);
	{
		AirSetup setup;
		AirSettings air;
		air.intro = {AirMediaKind::Image, fileIn(setup.folder, "intro.png"), 20000ms};
		air.outro = {AirMediaKind::Video, fileIn(setup.folder, "outro.mp4")};
		air.back = {"", 20000ms};
		setup.sequence->setAir(air);
		CHECK(listsOfObs() == before);

		setup.putOnAir();
		CHECK(setup.phase() == AirPhase::Intro);
		CHECK(listsOfObs() == before);

		setup.sequence->skip();
		CHECK(setup.phase() == AirPhase::Source);
		CHECK(listsOfObs() == before);

		setup.sequence->takeOffAir();
		CHECK(setup.phase() == AirPhase::Outro);
		CHECK(listsOfObs() == before);

		setup.sequence->takeOffAir();
		CHECK(setup.leaving());
		CHECK(listsOfObs() == before);

		setup.sequence->close();
		CHECK_FALSE(setup.sequence->air().onAir());
		CHECK(listsOfObs() == before);
	}
	CHECK(listsOfObs() == before);

	obs_set_output_source(0, nullptr);
	// The main canvas keeps its scenes until they are removed, as the frontend does before
	// shutting down.
	obs_source_remove(obs_scene_get_source(scene));
}

TEST_CASE_METHOD(ObsFixture, "a collection changing cuts a replay on air and keeps the next one off",
		 "[obs][sequence][air]")
{
	registerFade();
	EventSetup setup;
	setup.answer(AirEvent::CollectionChanging);
	setup.checkCut();
	CHECK(setup.sequence->closed());
	CHECK(setup.sequence->state().sequence().entries().empty());
	CHECK_FALSE(setup.sequence->putOnAir(setup.captured.id()));
}

TEST_CASE_METHOD(ObsFixture, "scripting shutting down cuts a replay on air and keeps the next one off",
		 "[obs][sequence][air]")
{
	registerFade();
	EventSetup setup;
	setup.answer(AirEvent::ScriptingShutdown);
	setup.checkCut();
	CHECK(setup.sequence->closed());
	CHECK_FALSE(setup.sequence->putOnAir(setup.captured.id()));
}

TEST_CASE_METHOD(ObsFixture, "a collection's cleanup cuts a replay on air and starts from what it leaves",
		 "[obs][sequence][air]")
{
	registerFade();
	EventSetup setup;
	setup.answer(AirEvent::CollectionCleanup);
	setup.checkCut();
	CHECK(setup.sequence->closed());
	CHECK(setup.manager.settings().sources.empty());
	CHECK_FALSE(setup.sequence->air().hasOutro());
}

TEST_CASE_METHOD(ObsFixture, "a collection change in the order OBS sends it leaves the next replay ready",
		 "[obs][sequence][air]")
{
	registerFade();
	EventSetup setup;
	setup.answer(AirEvent::CollectionChanging);
	setup.checkCut();
	setup.answer(AirEvent::CollectionCleanup);
	setup.checkCut();
	REQUIRE(setup.sequence->closed());

	setup.answer(AirEvent::CollectionChanged);
	CHECK_FALSE(setup.sequence->closed());
	CHECK(setup.sequence->putOnAir(setup.captured.id()));
	CHECK(setup.phase() == AirPhase::Source);
}

TEST_CASE_METHOD(ObsFixture, "OBS exiting lets go of what the composition made and ends the manager",
		 "[obs][sequence][air]")
{
	registerFade();
	EventSetup setup;
	setup.manager.onStreaming(true);
	REQUIRE(setup.manager.running());

	setup.answer(AirEvent::Exit);
	setup.checkCut();
	CHECK(setup.sequence->air().partSource(AirPhase::Outro) == nullptr);
	setup.answer(AirEvent::CollectionChanged);
	CHECK(setup.sequence->closed());
	// A manager that exited says nothing of the buffers stopping.
	setup.manager.onStreaming(false);
	CHECK(setup.host.stops == 0);
}

TEST_CASE("a failure letting go of a replay at exit still ends the manager", "[obs][sequence][air]")
{
	const tapeloop::test::LogCounter said("Letting go of the replay failed");
	int ended = 0;
	CHECK_NOTHROW(exitInOrder([] { throw std::runtime_error("no"); }, [&ended] { ++ended; }));
	CHECK(ended == 1);
	CHECK(said.lines == 1);
}

TEST_CASE("OBS's exit lets go of a replay before it ends the manager", "[obs][sequence][air]")
{
	std::vector<int> steps;
	exitInOrder([&steps] { steps.push_back(1); }, [&steps] { steps.push_back(2); });
	CHECK(steps == std::vector<int>{1, 2});
}

TEST_CASE_METHOD(ObsFixture, "buffers stopping takes a replay off air the way the user does", "[obs][sequence][air]")
{
	registerFade();
	EventSetup setup;
	setup.answer(AirEvent::BuffersStopped);
	CHECK(setup.phase() == AirPhase::Outro);
	CHECK(setup.sequence->air().onAir());
	CHECK_FALSE(setup.sequence->closed());
}

TEST_CASE_METHOD(ObsFixture, "a program faded to black cuts a replay on air with no outro", "[obs][sequence][air]")
{
	registerFade();
	ProgramSetup setup;
	setup.runTo(nullptr);
	setup.checkCut();
	CHECK_FALSE(setup.sequence->closed());
	CHECK_FALSE(setup.sequence->state().sequence().entries().empty());

	// Nothing refuses the next one, and coming back from black takes nothing off.
	setup.putOnAir();
	setup.runTo(setup.scene);
	setup.checkStillOn();
}

TEST_CASE_METHOD(ObsFixture, "a program faded to black ends a way back under way", "[obs][sequence][air]")
{
	registerFade();
	ProgramSetup setup;
	setup.sequence->takeOffAir();
	setup.sequence->skip();
	REQUIRE(setup.leaving());
	setup.runTo(nullptr);
	setup.checkCut();
	CHECK_FALSE(setup.leaving());
}

TEST_CASE_METHOD(ObsFixture, "a program gone black during a way back lets go of its last frame", "[obs][sequence][air]")
{
	registerFade();
	AirSetup setup;
	AirSettings air;
	air.back = {"", 20000ms};
	setup.sequence->setAir(air);
	setup.putOnAir();
	CHECK(playing(*setup.sequence) == "a");

	setup.sequence->takeOffAir();
	REQUIRE(setup.leaving());
	REQUIRE(setup.sequence->player()->state().loaded);

	setup.sequence->programWentBlack();
	CHECK_FALSE(setup.sequence->air().onAir());
	CHECK(playerUntil(*setup.sequence, [](const PlayerState &state) { return !state.loaded; }));
}

TEST_CASE_METHOD(ObsFixture, "a transition to another scene leaves a replay on air", "[obs][sequence][air]")
{
	registerFade();
	ProgramSetup setup;
	const OBSSourceAutoRelease other = obs_source_create_private(tapeloop::test::kTestPatternId, "Other", nullptr);
	setup.runTo(other);
	setup.checkStillOn();
}

TEST_CASE_METHOD(ObsFixture, "a program with no transition is not black", "[obs][sequence][air]")
{
	registerFade();
	ProgramSetup setup;
	obs_set_output_source(0, setup.scene);
	setup.answer(AirEvent::ProgramTransitionStopped);
	setup.checkStillOn();
	obs_set_output_source(0, nullptr);
	setup.answer(AirEvent::ProgramTransitionStopped);
	setup.checkStillOn();
}
