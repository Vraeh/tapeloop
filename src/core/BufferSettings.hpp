// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/MediaTime.hpp"
#include "core/ReplaySize.hpp"

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace tapeloop {

inline constexpr Nanoseconds kMinBufferLength = std::chrono::seconds(10);
inline constexpr Nanoseconds kMaxBufferLength = std::chrono::seconds(300);
inline constexpr Nanoseconds kDefaultBufferLength = std::chrono::seconds(60);

// Within the range the dock offers.
Nanoseconds clampBufferLength(Nanoseconds length);

// One of the fixed heights the dock offers: 360, 480, 720, 1080 or 2160.
bool isFixedHeight(uint32_t height);

struct SourceSettings {
	bool selected = false;
	std::optional<Nanoseconds> length;
	std::optional<ReplayResolution> resolution;
	std::optional<bool> activateOffAir;

	bool operator==(const SourceSettings &) const = default;
};

// What the dock edits, saved with the scene collection. Sources are known by UUID.
struct BufferSettings {
	Nanoseconds length = kDefaultBufferLength;
	ReplayResolution resolution;
	bool startWithOutputs = true;
	// Whether a selected source is kept active while it is captured, as if it were on
	// air: media sources play, captures hook their targets. The program does not change.
	bool activateOffAir = false;
	std::map<std::string, SourceSettings> sources;

	Nanoseconds lengthFor(const std::string &uuid) const;
	ReplayResolution resolutionFor(const std::string &uuid) const;
	bool activateFor(const std::string &uuid) const;
	std::vector<std::string> selectedSources() const;

	bool operator==(const BufferSettings &) const = default;
};

inline constexpr int64_t kSettingsVersion = 1;

// The settings in the shape they are saved in, one field per key. Lengths are whole
// seconds and resolutions are "canvas", "output" or "fixed" with a height.
struct SavedSource {
	std::string uuid;
	// For finding the source again when its UUID is gone, and for log lines about it;
	// empty when unknown.
	std::string name;
	bool selected = false;
	std::optional<int64_t> lengthSeconds;
	std::optional<std::string> resolution;
	std::optional<int64_t> height;
	std::optional<bool> activateOffAir;
};

// A reader skips the keys it does not know, so a field added later, as activateOffAir
// was, needs no new version.
struct SavedSettings {
	int64_t version = kSettingsVersion;
	int64_t lengthSeconds = 0;
	std::string resolution;
	int64_t height = 0;
	bool startWithOutputs = true;
	bool activateOffAir = false;
	std::vector<SavedSource> sources;
};

SavedSettings saveSettings(const BufferSettings &settings);

// Empty when the version is not kSettingsVersion. Values out of range are brought
// back into it, and a resolution this build does not know becomes the canvas.
std::optional<BufferSettings> loadSettings(const SavedSettings &saved);

struct SourceIdentity {
	std::string uuid;
	std::string name;
};

// A duplicated scene collection gives every source a new UUID. The settings saved for a
// UUID that `sources` does not have move to the source with the name saved alongside,
// when exactly one source has that name, it has no settings of its own, and no other
// missing UUID claims it too. Returns the moves, from the saved UUID to the new one.
std::map<std::string, std::string> matchSourcesByName(BufferSettings &settings,
						      const std::map<std::string, std::string> &savedNames,
						      const std::vector<SourceIdentity> &sources);

} // namespace tapeloop
