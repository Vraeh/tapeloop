// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "../core/TempDirectory.hpp"
#include "ObsFixture.hpp"
#include "TestPattern.hpp"
#include "UiTasks.hpp"

#include "core/FileIo.hpp"
#include "core/ReplayPlayerState.hpp"
#include "obs/CaptureManager.hpp"
#include "obs/ReplayHotkeys.hpp"
#include "obs/ReplayPlayer.hpp"
#include "obs/ReplaySource.hpp"
#include "obs/SequencePlayer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <obs.hpp>
#include <util/config-file.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using tapeloop::AirPhase;
using tapeloop::BufferSettings;
using tapeloop::obs::CaptureManager;
using tapeloop::obs::HotkeyAction;
using tapeloop::obs::kHotkeyActions;
using tapeloop::obs::PlayerState;
using tapeloop::obs::ReplayHotkeys;
using tapeloop::obs::SequencePlayer;
using tapeloop::test::ObsFixture;
using tapeloop::test::UiTasks;
using tapeloop::test::waitFor;

namespace {

// The names D-069 gives them, in the order of HotkeyAction.
constexpr std::array<const char *, kHotkeyActions> kDescriptions{
	"Tapeloop: Start or stop buffers",
	"Tapeloop: Capture replay",
	"Tapeloop: Put replay on air",
	"Tapeloop: Take replay off air",
	"Tapeloop: Play or pause",
	"Tapeloop: Reverse direction",
	"Tapeloop: Next frame",
	"Tapeloop: Previous frame",
	"Tapeloop: Faster",
	"Tapeloop: Slower",
	"Tapeloop: Normal speed",
	"Tapeloop: Scrub forward (hold)",
	"Tapeloop: Scrub backward (hold)",
	"Tapeloop: Skip to next source",
	"Tapeloop: Newer replay",
	"Tapeloop: Older replay",
};

// The plugin's locale, as obs_module_text() reads it. Not through libobs's text lookup,
// whose memory the fixture would count as leaked.
const std::map<std::string, std::string> &locale()
{
	static const std::map<std::string, std::string> strings = [] {
		std::map<std::string, std::string> read;
		std::ifstream file(TAPELOOP_LOCALE_FILE);
		std::string line;
		while (std::getline(file, line)) {
			const size_t equals = line.find('=');
			if (equals != std::string::npos && line.size() >= equals + 3 && line[equals + 1] == '"' &&
			    line.back() == '"') {
				read[line.substr(0, equals)] = line.substr(equals + 2, line.size() - equals - 3);
			}
		}
		return read;
	}();
	return strings;
}

const char *localeText(const char *key)
{
	const auto found = locale().find(key);
	return found != locale().end() ? found->second.c_str() : key;
}

struct Registered {
	std::string name;
	std::string description;
	obs_hotkey_id id = OBS_INVALID_HOTKEY_ID;
};

std::vector<Registered> tapeloopHotkeys()
{
	std::vector<Registered> found;
	obs_enum_hotkeys(
		[](void *data, obs_hotkey_id id, obs_hotkey_t *hotkey) {
			const std::string name = obs_hotkey_get_name(hotkey);
			if (name.starts_with("tapeloop.") &&
			    obs_hotkey_get_registerer_type(hotkey) == OBS_HOTKEY_REGISTERER_FRONTEND) {
				static_cast<std::vector<Registered> *>(data)->push_back(
					{name, obs_hotkey_get_description(hotkey), id});
			}
			return true;
		},
		&found);
	return found;
}

struct ConfigCloser {
	void operator()(config_t *config) const noexcept { config_close(config); }
};

// Presses and releases a hotkey as OBS's frontend does, which routes them to its own thread.
class Keys {
public:
	Keys() { obs_hotkey_enable_callback_rerouting(true); }
	~Keys() { obs_hotkey_enable_callback_rerouting(false); }

	Keys(const Keys &) = delete;
	Keys &operator=(const Keys &) = delete;
	Keys(Keys &&) = delete;
	Keys &operator=(Keys &&) = delete;

	static void press(const ReplayHotkeys &hotkeys, HotkeyAction action)
	{
		obs_hotkey_trigger_routed_callback(hotkeys.id(action), true);
		obs_hotkey_trigger_routed_callback(hotkeys.id(action), false);
	}
};

// Hotkeys go to the UI thread through the graphics thread. Once it has run a task queued
// after them, they are all on the UI thread's queue.
void handOver()
{
	obs_queue_task(OBS_TASK_GRAPHICS, [](void *) noexcept {}, nullptr, true);
}

class OfflineHost : public tapeloop::obs::CaptureHost {
public:
	bool streamingActive() const override { return false; }
	bool recordingActive() const override { return false; }
	void requestSave() override {}
	std::string recordingFolder() const override { return tapeloop::utf8FromPath(dir.path()); }
	std::string sceneCollectionName() const override { return "Liga"; }

	tapeloop::test::TempDirectory dir;
};

bool playerUntil(const SequencePlayer &sequence, bool (*condition)(const PlayerState &))
{
	return waitFor([&] { return condition(sequence.player()->state()); }, std::chrono::seconds(10));
}

} // namespace

TEST_CASE_METHOD(ObsFixture, "every control has a frontend hotkey under the name the settings show", "[obs][hotkey]")
{
	REQUIRE_FALSE(locale().empty());
	std::optional<ReplayHotkeys> hotkeys(std::in_place, [](HotkeyAction, bool) {}, localeText);

	const std::vector<Registered> registered = tapeloopHotkeys();
	REQUIRE(registered.size() == kHotkeyActions);
	for (size_t i = 0; i < kHotkeyActions; ++i) {
		const auto action = static_cast<HotkeyAction>(i);
		const Registered &hotkey = registered[i];
		CHECK(hotkey.id == hotkeys->id(action));
		CHECK(hotkey.name == ReplayHotkeys::name(action));
		CHECK(hotkey.description == kDescriptions[i]);
	}
	// The one a profile may already bind keeps its name.
	CHECK(std::string(ReplayHotkeys::name(HotkeyAction::CaptureReplay)) == "tapeloop.capture_replay");

	// Their bindings come from the profile, each under its own name.
	tapeloop::test::TempDirectory dir;
	const std::unique_ptr<config_t, ConfigCloser> profile(
		config_create((dir.path() / "basic.ini").string().c_str()));
	REQUIRE(profile);
	config_set_string(profile.get(), "Hotkeys", ReplayHotkeys::name(HotkeyAction::Faster),
			  R"({"bindings":[{"key":"OBS_KEY_F7"}]})");
	hotkeys->load(profile.get());
	OBSDataArrayAutoRelease faster = obs_hotkey_save(hotkeys->id(HotkeyAction::Faster));
	REQUIRE(obs_data_array_count(faster) == 1);
	OBSDataAutoRelease binding = obs_data_array_item(faster, 0);
	CHECK(std::string(obs_data_get_string(binding, "key")) == "OBS_KEY_F7");
	OBSDataArrayAutoRelease slower = obs_hotkey_save(hotkeys->id(HotkeyAction::Slower));
	CHECK(obs_data_array_count(slower) == 0);

	hotkeys.reset();
	CHECK(tapeloopHotkeys().empty());
}

TEST_CASE_METHOD(ObsFixture, "hotkeys reach the UI thread in order while they last", "[obs][hotkey]")
{
	const UiTasks ui;
	const Keys keys;
	std::vector<std::pair<HotkeyAction, bool>> heard;
	std::optional<ReplayHotkeys> hotkeys(
		std::in_place, [&](HotkeyAction action, bool pressed) { heard.emplace_back(action, pressed); },
		localeText);

	Keys::press(*hotkeys, HotkeyAction::PlayPause);
	Keys::press(*hotkeys, HotkeyAction::ScrubBackward);
	Keys::press(*hotkeys, HotkeyAction::NextFrame);
	// Not while libobs calls them, though OBS calls them on its UI thread.
	CHECK(heard.empty());
	handOver();
	UiTasks::run();
	// Only a scrub, which lasts while the key is held, hears of its release.
	CHECK(heard == std::vector<std::pair<HotkeyAction, bool>>{{HotkeyAction::PlayPause, true},
								  {HotkeyAction::ScrubBackward, true},
								  {HotkeyAction::ScrubBackward, false},
								  {HotkeyAction::NextFrame, true}});

	// What is still on its way when they go is dropped.
	Keys::press(*hotkeys, HotkeyAction::Faster);
	hotkeys.reset();
	handOver();
	REQUIRE(UiTasks::pending() == 1);
	UiTasks::run();
	CHECK(heard.size() == 4);
}

TEST_CASE_METHOD(ObsFixture, "hotkeys run once libobs has let go of its hotkeys", "[obs][hotkey]")
{
	const UiTasks ui;
	const Keys keys;
	bool triggering = false;
	std::vector<bool> heardWhileTriggering;
	// Whether another thread could use libobs's hotkeys while the handler ran: OBS's own
	// hotkey thread and every source destroyed need them.
	bool othersCouldUseThem = false;
	std::atomic<bool> enumerated{false};
	std::thread other;
	std::optional<ReplayHotkeys> hotkeys(
		std::in_place,
		[&](HotkeyAction, bool) {
			heardWhileTriggering.push_back(triggering);
			other = std::thread([&] {
				obs_enum_hotkeys([](void *, obs_hotkey_id, obs_hotkey_t *) { return false; }, nullptr);
				enumerated = true;
			});
			othersCouldUseThem = waitFor([&] { return enumerated.load(); }, std::chrono::seconds(5));
		},
		localeText);

	triggering = true;
	obs_hotkey_trigger_routed_callback(hotkeys->id(HotkeyAction::CaptureReplay), true);
	triggering = false;
	handOver();
	UiTasks::run();
	if (other.joinable()) {
		other.join();
	}
	CHECK(heardWhileTriggering == std::vector<bool>{false});
	CHECK(othersCouldUseThem);
}

TEST_CASE_METHOD(ObsFixture, "hotkeys run the buffers, pick replays and play them on air", "[obs][hotkey][replay]")
{
	tapeloop::obs::registerReplaySource({});
	const UiTasks ui;
	const Keys keys;
	OBSSourceAutoRelease camera = tapeloop::test::createTestPattern(320, 180, "Camera");
	const std::string uuid = obs_source_get_uuid(camera);
	OfflineHost host;
	CaptureManager manager(host);
	BufferSettings settings = manager.settings();
	settings.startWithOutputs = false;
	settings.sources[uuid].selected = true;
	manager.setSettings(settings);
	std::optional<SequencePlayer> sequence(std::in_place);
	REQUIRE(sequence->player());
	std::optional<ReplayHotkeys> hotkeys(
		std::in_place,
		[&](HotkeyAction action, bool pressed) { runHotkey(action, pressed, manager, *sequence); }, localeText);
	const auto press = [&](HotkeyAction action) {
		Keys::press(*hotkeys, action);
		handOver();
		UiTasks::run();
	};
	const auto hold = [&](HotkeyAction action, bool down) {
		obs_hotkey_trigger_routed_callback(hotkeys->id(action), down);
		handOver();
		UiTasks::run();
	};

	press(HotkeyAction::StartStopBuffers);
	REQUIRE(manager.running());
	REQUIRE(waitFor(
		[&] {
			const tapeloop::SourceBuffer *buffer = manager.buffer(uuid);
			return buffer && buffer->stats().gopCount >= 2;
		},
		std::chrono::seconds(60)));
	press(HotkeyAction::CaptureReplay);
	press(HotkeyAction::CaptureReplay);
	const std::vector<uint64_t> replays = manager.library().list();
	REQUIRE(replays.size() == 2);
	CHECK(manager.library().current() == replays[0]);

	// Older and newer stop at the ends of the list.
	press(HotkeyAction::NewerReplay);
	CHECK(manager.library().current() == replays[0]);
	press(HotkeyAction::OlderReplay);
	CHECK(manager.library().current() == replays[1]);
	press(HotkeyAction::OlderReplay);
	CHECK(manager.library().current() == replays[1]);
	press(HotkeyAction::NewerReplay);
	CHECK(manager.library().current() == replays[0]);

	// The player's controls wait for a replay on air.
	press(HotkeyAction::Faster);
	press(HotkeyAction::PutOnAir);
	REQUIRE(sequence->state().phase() == AirPhase::Source);
	CHECK(sequence->state().source() == uuid);
	REQUIRE(playerUntil(*sequence, [](const PlayerState &state) { return state.loaded && state.playing; }));
	CHECK(sequence->player()->state().rate == 1000);

	// Paused first, so that the clip, a few seconds long, stays on air.
	press(HotkeyAction::PlayPause);
	REQUIRE(playerUntil(*sequence, [](const PlayerState &state) { return !state.playing; }));
	press(HotkeyAction::Faster);
	REQUIRE(playerUntil(*sequence, [](const PlayerState &state) { return state.rate == 2000; }));
	press(HotkeyAction::Reverse);
	REQUIRE(playerUntil(*sequence, [](const PlayerState &state) { return state.rate == -2000; }));
	press(HotkeyAction::Slower);
	REQUIRE(playerUntil(*sequence, [](const PlayerState &state) { return state.rate == -1000; }));
	press(HotkeyAction::NormalSpeed);
	press(HotkeyAction::Reverse);
	REQUIRE(playerUntil(*sequence, [](const PlayerState &state) { return state.rate == 1000; }));
	const size_t frame = sequence->player()->state().frame;
	press(HotkeyAction::NextFrame);
	CHECK(waitFor([&] { return sequence->player()->state().frame == frame + 1; }, std::chrono::seconds(10)));
	press(HotkeyAction::PreviousFrame);
	CHECK(waitFor([&] { return sequence->player()->state().frame == frame; }, std::chrono::seconds(10)));

	// A scrub lasts while its key is held.
	hold(HotkeyAction::ScrubForward, true);
	REQUIRE(playerUntil(*sequence, [](const PlayerState &state) { return state.scrubbing; }));
	hold(HotkeyAction::ScrubForward, false);
	REQUIRE(playerUntil(*sequence, [](const PlayerState &state) { return !state.scrubbing; }));

	// The only source skipped, the replay is over.
	press(HotkeyAction::SkipSource);
	CHECK(sequence->state().phase() == AirPhase::Live);
	press(HotkeyAction::PutOnAir);
	CHECK(sequence->state().phase() == AirPhase::Source);
	press(HotkeyAction::TakeOffAir);
	CHECK(sequence->state().phase() == AirPhase::Live);
	REQUIRE(playerUntil(*sequence, [](const PlayerState &state) { return !state.loaded; }));

	press(HotkeyAction::StartStopBuffers);
	CHECK_FALSE(manager.running());
	manager.finishWrites();
	hotkeys.reset();
	sequence.reset();
	obs_wait_for_destroy_queue();
}
