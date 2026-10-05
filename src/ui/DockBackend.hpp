// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/BufferSettings.hpp"

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
};

// What the dock reads and changes. The plugin implements it on top of the capture
// manager; tests replace it, so the widgets run without OBS.
class DockBackend {
public:
	virtual ~DockBackend() = default;

	virtual std::vector<DockSource> sources() const = 0;
	virtual BufferSettings settings() const = 0;
	virtual void setSettings(const BufferSettings &settings) = 0;

	virtual bool running() const = 0;
	virtual bool manualControlEnabled() const = 0;
	// Starts the buffers when they are stopped and stops them when they run. False when
	// refused.
	virtual bool toggleRunning() = 0;
};

} // namespace tapeloop::ui
