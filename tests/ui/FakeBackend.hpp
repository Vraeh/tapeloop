// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "ui/DockBackend.hpp"

#include <vector>

namespace tapeloop::test {

// Holds what the dock shows and records what it changes.
class FakeBackend : public tapeloop::ui::DockBackend {
public:
	std::vector<tapeloop::ui::DockSource> shown;
	BufferSettings current;
	bool isRunning = false;
	bool manualEnabled = true;
	int toggles = 0;
	int settingsChanges = 0;
	std::vector<tapeloop::ui::EncoderChoice> choices = {{"obs_nvenc_hevc_tex", "NVIDIA NVENC HEVC"},
							    {"obs_x264", "x264"}};

	std::vector<tapeloop::ui::DockSource> sources() const override
	{
		std::vector<tapeloop::ui::DockSource> sources = shown;
		for (auto &source : sources) {
			const auto found = current.sources.find(source.uuid);
			source.selected = found != current.sources.end() && found->second.selected;
		}
		return sources;
	}

	BufferSettings settings() const override { return current; }

	void setSettings(const BufferSettings &settings) override
	{
		current = settings;
		++settingsChanges;
	}

	std::vector<tapeloop::ui::EncoderChoice> encoderChoices() const override { return choices; }

	bool running() const override { return isRunning; }
	bool manualControlEnabled() const override { return manualEnabled; }

	bool toggleRunning() override
	{
		if (!manualEnabled) {
			return false;
		}
		isRunning = !isRunning;
		++toggles;
		return true;
	}
};

} // namespace tapeloop::test
