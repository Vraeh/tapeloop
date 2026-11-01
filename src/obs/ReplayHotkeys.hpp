// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include <obs.h>
#include <util/config-file.h>

#include <array>
#include <cstddef>
#include <functional>
#include <memory>

namespace tapeloop::obs {

class CaptureManager;
class SequencePlayer;

enum class HotkeyAction {
	StartStopBuffers,
	CaptureReplay,
	PutOnAir,
	TakeOffAir,
	PlayPause,
	Reverse,
	NextFrame,
	PreviousFrame,
	Faster,
	Slower,
	NormalSpeed,
	ScrubForward,
	ScrubBackward,
	SkipSource,
	NewerReplay,
	OlderReplay
};

inline constexpr size_t kHotkeyActions = 16;

// Tapeloop's hotkeys, registered as the frontend's own, so that OBS's settings list them
// and write their bindings into the profile. Every press, and the release of a scrub,
// goes to the handler on the UI thread, in the order they came, unless the hotkeys are
// gone by then. Created and destroyed on the UI thread.
class ReplayHotkeys {
public:
	using Handler = std::function<void(HotkeyAction action, bool pressed)>;
	// text gives the description of a locale key.
	ReplayHotkeys(Handler handler, const char *(*text)(const char *key));
	~ReplayHotkeys();

	ReplayHotkeys(const ReplayHotkeys &) = delete;
	ReplayHotkeys &operator=(const ReplayHotkeys &) = delete;
	ReplayHotkeys(ReplayHotkeys &&) = delete;
	ReplayHotkeys &operator=(ReplayHotkeys &&) = delete;

	// The bindings the profile holds for each.
	void load(config_t *profile) noexcept;
	obs_hotkey_id id(HotkeyAction action) const noexcept;
	// The name the profile keeps its bindings under, and the locale key of its description.
	static const char *name(HotkeyAction action) noexcept;
	static const char *textKey(HotkeyAction action) noexcept;

private:
	struct Binding {
		ReplayHotkeys *owner = nullptr;
		HotkeyAction action = HotkeyAction::CaptureReplay;
	};

	static void handle(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed) noexcept;
	static void run(void *param) noexcept;

	Handler handler_;
	std::array<Binding, kHotkeyActions> bindings_{};
	std::array<obs_hotkey_id, kHotkeyActions> ids_{};
	std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

// What each hotkey does: the buffers and the replay list through the manager, the replay
// on air through the sequence. The controls of the player act only while a source is on
// air. Newer and older move the replay that goes on air next through every replay that
// is not damaged, newest first. UI thread.
void runHotkey(HotkeyAction action, bool pressed, CaptureManager &manager, SequencePlayer &sequence);

} // namespace tapeloop::obs
