// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/BufferSettings.hpp"
#include "core/EncoderPolicy.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace tapeloop::ui {

// Waiting: the source has no picture yet, as display, window and game captures do until
// something shows them.
enum class SourceState { Stopped, Waiting, Running, Failed };

// A video source of the current scene collection as the dock lists it.
struct DockSource {
	std::string uuid;
	std::string name;
	bool selected = false;
	SourceState state = SourceState::Stopped;
	Nanoseconds buffered{0};
	uint64_t bytes = 0;
	// Not kept active off air although the settings ask: it restarts when it becomes
	// active.
	bool activationLeftOut = false;
	// How its running encoder takes the frames; anything but Texture gets a note.
	EncoderPath encoderPath = EncoderPath::Texture;
};

// A replay as the dock lists it.
struct DockReplay {
	uint64_t id = 0;
	std::chrono::system_clock::time_point capturedAt;
	size_t sources = 0;
	std::vector<std::string> tags;
};

// An encoder a user may choose for replays.
struct EncoderChoice {
	std::string id;
	// As OBS names it.
	std::string name;
};

// What the dock reads and changes. The plugin implements it on top of the capture
// manager; tests replace it, so the widgets run without OBS.
class DockBackend {
public:
	virtual ~DockBackend() = default;

	virtual std::vector<DockSource> sources() const = 0;
	virtual BufferSettings settings() const = 0;
	virtual void setSettings(const BufferSettings &settings) = 0;
	virtual std::vector<EncoderChoice> encoderChoices() const = 0;

	virtual bool running() const = 0;
	virtual bool manualControlEnabled() const = 0;
	// Starts the buffers when they are stopped and stops them when they run. False when
	// refused.
	virtual bool toggleRunning() = 0;

	// Keeps what every buffer holds as a replay; zero when there was nothing to keep.
	virtual uint64_t captureReplay() = 0;
	// Newest first; with a tag, only the replays that carry it.
	virtual std::vector<DockReplay> replays(const std::string &tag = {}) const = 0;
	// Every tag, sorted.
	virtual std::vector<std::string> replayTags() const = 0;
	// Creates the tag if it is new. False when the replay is gone or the name not valid.
	virtual bool tagReplay(uint64_t id, const std::string &tag) = 0;
	// The replay that goes on air next; zero when there is none.
	virtual uint64_t currentReplay() const = 0;
	virtual void pickReplay(uint64_t id) = 0;
};

} // namespace tapeloop::ui
