// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "ui/DockBackend.hpp"

#include <algorithm>
#include <string>
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
	uint64_t captures = 0;
	uint64_t picked = 0;
	std::vector<tapeloop::ui::DockReplay> captured;
	std::vector<std::string> tags;

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

	bool running() const override { return isRunning; }
	bool manualControlEnabled() const override { return manualEnabled; }

	uint64_t captureReplay() override
	{
		const uint64_t id = ++captures;
		captured.insert(captured.begin(), {id, std::chrono::system_clock::now(), 2, {}});
		picked = id;
		return id;
	}
	std::vector<tapeloop::ui::DockReplay> replays(const std::string &tag = {}) const override
	{
		std::vector<tapeloop::ui::DockReplay> listed;
		for (const auto &replay : captured) {
			if (tag.empty() ||
			    std::find(replay.tags.begin(), replay.tags.end(), tag) != replay.tags.end()) {
				listed.push_back(replay);
			}
		}
		return listed;
	}
	std::vector<std::string> replayTags() const override { return tags; }
	bool tagReplay(uint64_t id, const std::string &tag) override
	{
		for (auto &replay : captured) {
			if (replay.id == id && tag.find_first_not_of(' ') != std::string::npos) {
				replay.tags.push_back(tag);
				if (std::find(tags.begin(), tags.end(), tag) == tags.end()) {
					tags.push_back(tag);
				}
				return true;
			}
		}
		return false;
	}
	uint64_t currentReplay() const override { return picked; }
	void pickReplay(uint64_t id) override { picked = id; }

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
