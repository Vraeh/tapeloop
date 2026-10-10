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
	int picks = 0;
	std::vector<tapeloop::ui::DockReplay> captured;
	// The broadcast the next capture belongs to.
	std::string capturing;
	std::vector<std::string> tags;
	uint64_t memory = uint64_t{16} << 30;
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

	uint64_t captureReplay() override
	{
		const uint64_t id = ++captures;
		tapeloop::ui::DockReplay replay;
		replay.id = id;
		replay.capturedAt = std::chrono::system_clock::now();
		replay.sources = 2;
		replay.broadcast = capturing;
		captured.insert(captured.begin(), replay);
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
	uint64_t physicalMemory() const override { return memory; }
	bool tagReplay(uint64_t id, const std::string &tag) override
	{
		for (auto &replay : captured) {
			if (replay.id == id && tag.find_first_not_of(' ') != std::string::npos &&
			    std::find(replay.tags.begin(), replay.tags.end(), tag) == replay.tags.end()) {
				replay.tags.push_back(tag);
				if (std::find(tags.begin(), tags.end(), tag) == tags.end()) {
					tags.push_back(tag);
				}
				return true;
			}
		}
		return false;
	}
	bool untagReplay(uint64_t id, const std::string &tag) override
	{
		for (auto &replay : captured) {
			const auto carried = std::find(replay.tags.begin(), replay.tags.end(), tag);
			if (replay.id == id && carried != replay.tags.end()) {
				replay.tags.erase(carried);
				return true;
			}
		}
		return false;
	}
	bool deleteTag(const std::string &tag) override
	{
		const auto known = std::find(tags.begin(), tags.end(), tag);
		if (known == tags.end()) {
			return false;
		}
		tags.erase(known);
		for (auto &replay : captured) {
			replay.tags.erase(std::remove(replay.tags.begin(), replay.tags.end(), tag), replay.tags.end());
		}
		return true;
	}
	uint64_t currentReplay() const override { return picked; }
	uint64_t lastCapture() const override { return captures; }
	// As the library does, a damaged replay is never picked.
	void pickReplay(uint64_t id) override
	{
		++picks;
		for (const auto &replay : captured) {
			if (replay.id == id && replay.state == tapeloop::ReplayState::Damaged) {
				return;
			}
		}
		picked = id;
	}

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
