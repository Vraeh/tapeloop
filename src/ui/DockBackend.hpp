// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/BufferSettings.hpp"
#include "core/EncoderPolicy.hpp"
#include "core/ReplayState.hpp"

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
	// How its running encoder takes the frames; anything but Texture gets a note, which
	// for Readback says why.
	EncoderPath encoderPath = EncoderPath::Texture;
	ReadbackReason readbackReason = ReadbackReason::None;
	// Its running encoder is the one chosen in the advanced settings, which explains a
	// path that is not the optimal one.
	bool chosenEncoder = false;
	// The encoder chosen in the advanced settings could not start for it, or is not
	// available, and the automatic order went on.
	bool choiceSkipped = false;
	// Its HEVC encoder failed while it ran, and it uses H.264 from its next start.
	bool hevcFailed = false;
	// It holds less than its length, as the buffers together need more memory than they
	// may use.
	bool budgetLimited = false;
};

// A replay as the dock lists it.
struct DockReplay {
	uint64_t id = 0;
	std::chrono::system_clock::time_point capturedAt;
	size_t sources = 0;
	std::vector<std::string> tags;
	// The broadcast it belongs to; the dock groups replays under it.
	std::string broadcast;
	ReplayState state = ReplayState::Stored;
	// The name of its file without the extension, which is all a damaged replay has to
	// show.
	std::string fileName;
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
	// The computer's memory in bytes, zero when it cannot be read.
	virtual uint64_t physicalMemory() const = 0;

	virtual bool running() const = 0;
	virtual bool manualControlEnabled() const = 0;
	// Starts the buffers when they are stopped and stops them when they run. False when
	// refused.
	virtual bool toggleRunning() = 0;

	// Keeps what every buffer holds as a replay; zero when there was nothing to keep.
	virtual uint64_t captureReplay() = 0;
	// Newest first, the replays of a broadcast together; with a tag, only the replays
	// that carry it.
	virtual std::vector<DockReplay> replays(const std::string &tag = {}) const = 0;
	// Every tag, sorted.
	virtual std::vector<std::string> replayTags() const = 0;
	// Creates the tag if it is new. False when the replay is gone or the name not valid.
	virtual bool tagReplay(uint64_t id, const std::string &tag) = 0;
	// False when the replay is gone or does not carry the tag.
	virtual bool untagReplay(uint64_t id, const std::string &tag) = 0;
	// Takes the tag off every replay that carries it and forgets it. False when there is
	// no such tag.
	virtual bool deleteTag(const std::string &tag) = 0;
	// The replay that goes on air next; zero when there is none.
	virtual uint64_t currentReplay() const = 0;
	// The replay captured last, from the dock or by the hotkey; zero before the first.
	virtual uint64_t lastCapture() const = 0;
	virtual void pickReplay(uint64_t id) = 0;
};

} // namespace tapeloop::ui
