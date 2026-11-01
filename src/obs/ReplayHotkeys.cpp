// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "obs/ReplayHotkeys.hpp"

#include "core/ReplayLibrary.hpp"
#include "core/ReplayPlayerState.hpp"
#include "obs/CaptureManager.hpp"
#include "obs/ProfileHotkeys.hpp"
#include "obs/ReplayPlayer.hpp"
#include "obs/SequencePlayer.hpp"

#include <util/base.h>

#include <algorithm>
#include <iterator>
#include <new>
#include <utility>
#include <vector>

namespace tapeloop::obs {
namespace {

struct HotkeyInfo {
	const char *name;
	const char *textKey;
};

// In the order of HotkeyAction.
constexpr std::array<HotkeyInfo, kHotkeyActions> kHotkeys{{
	{"tapeloop.start_stop_buffers", "Hotkey.StartStopBuffers"},
	{"tapeloop.capture_replay", "Hotkey.CaptureReplay"},
	{"tapeloop.put_on_air", "Hotkey.PutOnAir"},
	{"tapeloop.take_off_air", "Hotkey.TakeOffAir"},
	{"tapeloop.play_pause", "Hotkey.PlayPause"},
	{"tapeloop.reverse", "Hotkey.Reverse"},
	{"tapeloop.next_frame", "Hotkey.NextFrame"},
	{"tapeloop.previous_frame", "Hotkey.PreviousFrame"},
	{"tapeloop.faster", "Hotkey.Faster"},
	{"tapeloop.slower", "Hotkey.Slower"},
	{"tapeloop.normal_speed", "Hotkey.NormalSpeed"},
	{"tapeloop.scrub_forward", "Hotkey.ScrubForward"},
	{"tapeloop.scrub_backward", "Hotkey.ScrubBackward"},
	{"tapeloop.skip_source", "Hotkey.SkipSource"},
	{"tapeloop.newer_replay", "Hotkey.NewerReplay"},
	{"tapeloop.older_replay", "Hotkey.OlderReplay"},
}};

size_t indexOf(HotkeyAction action) noexcept
{
	return static_cast<size_t>(action);
}

// What a press or release hands to the UI thread.
struct HotkeyTask {
	std::weak_ptr<int> alive;
	const ReplayHotkeys::Handler *handler = nullptr;
	HotkeyAction action = HotkeyAction::CaptureReplay;
	bool pressed = false;
};

// The next replay that is not damaged from the current one, newer or older, as the full
// list has them; zero when there is none that way.
uint64_t nextReplay(const ReplayLibrary &library, bool newer)
{
	const std::vector<uint64_t> ids = library.list();
	const auto current = std::find(ids.begin(), ids.end(), library.current());
	if (current == ids.end()) {
		return 0;
	}
	const auto playable = [&](uint64_t id) {
		const Replay *replay = library.find(id);
		return replay && replay->state != ReplayState::Damaged;
	};
	if (newer) {
		const auto found = std::find_if(std::make_reverse_iterator(current), ids.rend(), playable);
		return found == ids.rend() ? 0 : *found;
	}
	const auto found = std::find_if(current + 1, ids.end(), playable);
	return found == ids.end() ? 0 : *found;
}

void runPlayerControl(HotkeyAction action, ReplayPlayer &player)
{
	switch (action) {
	case HotkeyAction::PlayPause:
		player.togglePause();
		break;
	case HotkeyAction::Reverse:
		player.reverse();
		break;
	case HotkeyAction::NextFrame:
		player.step(1);
		break;
	case HotkeyAction::PreviousFrame:
		player.step(-1);
		break;
	case HotkeyAction::Faster:
		player.faster();
		break;
	case HotkeyAction::Slower:
		player.slower();
		break;
	case HotkeyAction::NormalSpeed:
		player.normalSpeed();
		break;
	case HotkeyAction::ScrubForward:
		player.beginScrub(ScrubDirection::Forward);
		break;
	case HotkeyAction::ScrubBackward:
		player.beginScrub(ScrubDirection::Backward);
		break;
	default:
		break;
	}
}

} // namespace

ReplayHotkeys::ReplayHotkeys(Handler handler, const char *(*text)(const char *key)) : handler_(std::move(handler))
{
	ids_.fill(OBS_INVALID_HOTKEY_ID);
	for (size_t i = 0; i < kHotkeyActions; ++i) {
		bindings_[i] = {this, static_cast<HotkeyAction>(i)};
	}
	// Registered last: a hotkey can fire at once, and reads the bindings and alive_.
	for (size_t i = 0; i < kHotkeyActions; ++i) {
		ids_[i] = obs_hotkey_register_frontend(kHotkeys[i].name, text(kHotkeys[i].textKey), handle,
						       &bindings_[i]);
	}
}

ReplayHotkeys::~ReplayHotkeys()
{
	// Unregistering waits for a hotkey running now, after which none is handed over.
	for (const obs_hotkey_id id : ids_) {
		if (id != OBS_INVALID_HOTKEY_ID) {
			obs_hotkey_unregister(id);
		}
	}
	alive_.reset();
}

void ReplayHotkeys::load(config_t *profile) noexcept
{
	for (size_t i = 0; i < kHotkeyActions; ++i) {
		if (ids_[i] != OBS_INVALID_HOTKEY_ID) {
			loadProfileHotkey(ids_[i], kHotkeys[i].name, profile);
		}
	}
}

obs_hotkey_id ReplayHotkeys::id(HotkeyAction action) const noexcept
{
	return ids_[indexOf(action)];
}

const char *ReplayHotkeys::name(HotkeyAction action) noexcept
{
	return kHotkeys[indexOf(action)].name;
}

const char *ReplayHotkeys::textKey(HotkeyAction action) noexcept
{
	return kHotkeys[indexOf(action)].textKey;
}

void ReplayHotkeys::handle(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed) noexcept
{
	const auto &binding = *static_cast<const Binding *>(data);
	const bool hold = binding.action == HotkeyAction::ScrubForward || binding.action == HotkeyAction::ScrubBackward;
	if (!pressed && !hold) {
		return;
	}
	std::unique_ptr<HotkeyTask> task(new (std::nothrow) HotkeyTask);
	if (!task) {
		return;
	}
	task->alive = binding.owner->alive_;
	task->handler = &binding.owner->handler_;
	task->action = binding.action;
	task->pressed = pressed;
	// libobs calls a hotkey with its hotkey lock held, which OBS does on its UI thread, and
	// OBS runs a task queued from that thread for it at once. Queued from the graphics
	// thread it runs on a later pass instead, after the lock is let go. Both queues keep
	// the presses in order.
	obs_queue_task(OBS_TASK_GRAPHICS, toUiThread, task.release(), false);
}

void ReplayHotkeys::toUiThread(void *param) noexcept
{
	obs_queue_task(OBS_TASK_UI, run, param, false);
}

void ReplayHotkeys::run(void *param) noexcept
{
	const std::unique_ptr<HotkeyTask> task(static_cast<HotkeyTask *>(param));
	if (!task->alive.lock()) {
		return;
	}
	try {
		(*task->handler)(task->action, task->pressed);
	} catch (...) {
		blog(LOG_ERROR, "[tapeloop] A hotkey failed");
	}
}

void runHotkey(HotkeyAction action, bool pressed, CaptureManager &manager, SequencePlayer &sequence)
{
	// OBS still delivers presses queued before it cleared a collection or began to exit,
	// while it does.
	if (sequence.closed()) {
		return;
	}
	if (!pressed) {
		// Only a scrub hears of its release, and ends whatever it began.
		if (ReplayPlayer *player = sequence.player()) {
			player->endScrub();
		}
		return;
	}
	switch (action) {
	case HotkeyAction::StartStopBuffers:
		if (manager.running()) {
			manager.manualStop();
		} else {
			manager.manualStart();
		}
		return;
	case HotkeyAction::CaptureReplay:
		manager.captureReplay();
		return;
	case HotkeyAction::PutOnAir:
		if (const Replay *replay = manager.library().find(manager.library().current())) {
			sequence.putOnAir(*replay);
		}
		return;
	case HotkeyAction::TakeOffAir:
		sequence.takeOffAir();
		return;
	case HotkeyAction::SkipSource:
		sequence.skip();
		return;
	case HotkeyAction::NewerReplay:
	case HotkeyAction::OlderReplay:
		if (const uint64_t id = nextReplay(manager.library(), action == HotkeyAction::NewerReplay)) {
			manager.pickReplay(id);
		}
		return;
	default:
		break;
	}
	if (sequence.state().phase() == AirPhase::Source && sequence.player()) {
		runPlayerControl(action, *sequence.player());
	}
}

} // namespace tapeloop::obs
