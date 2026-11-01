// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/EncoderPolicy.hpp"
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

inline constexpr std::chrono::milliseconds kMinAirTransition{50};
inline constexpr std::chrono::milliseconds kMaxAirTransition{20000};
inline constexpr std::chrono::milliseconds kDefaultAirTransition{300};
inline constexpr std::chrono::milliseconds kMinImageTime{500};
inline constexpr std::chrono::milliseconds kMaxImageTime{60000};
inline constexpr std::chrono::milliseconds kDefaultImageTime{3000};

enum class AirMediaKind { None, Video, Image };

// An intro or an outro: a video played to its end, or an image shown for a time.
struct AirMedia {
	AirMediaKind kind = AirMediaKind::None;
	// UTF-8.
	std::string path;
	std::chrono::milliseconds imageTime = kDefaultImageTime;

	// A kind without a file is none.
	bool present() const noexcept { return kind != AirMediaKind::None && !path.empty(); }
	bool operator==(const AirMedia &) const = default;
};

// One of the transitions the user has in OBS, by name, with its type and settings there;
// an empty name is OBS's fade.
struct AirTransition {
	std::string name;
	std::chrono::milliseconds duration = kDefaultAirTransition;

	bool operator==(const AirTransition &) const = default;
};

// How a replay goes on air and back: the transition from live into the intro, or into
// the replay without one, and the transition from the outro, or the replay without one,
// back to live. The steps in between are cuts.
struct AirSettings {
	AirMedia intro;
	AirTransition into;
	AirMedia outro;
	AirTransition back;

	bool operator==(const AirSettings &) const = default;
};

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
	// air, so that sources that only run on air run. The program does not change.
	bool activateOffAir = false;
	// Encode replays in H.264 even where the hardware encodes HEVC.
	bool forceH264 = false;
	// The encoder chosen for replays, empty for the automatic order, and whether that
	// order may use encoders on another adapter than the one OBS renders on.
	std::string replayEncoder;
	bool allowOtherAdapters = true;
	// Whether the dock shows the advanced settings.
	bool showAdvanced = false;
	// The memory every buffer may hold together, in MiB; zero for the automatic budget.
	int64_t bufferMemoryMiB = 0;
	std::map<std::string, SourceSettings> sources;
	AirSettings air;

	Nanoseconds lengthFor(const std::string &uuid) const;
	ReplayResolution resolutionFor(const std::string &uuid) const;
	bool activateFor(const std::string &uuid) const;
	std::vector<std::string> selectedSources() const;
	// What the encoder candidates are chosen by.
	EncoderPreferences encoderPreferences() const;

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

// The intro or outro kinds are "none", "video" or "image", and times are milliseconds.
struct SavedMedia {
	std::string kind = "none";
	std::string path;
	int64_t imageMs = kDefaultImageTime.count();
};

struct SavedTransition {
	std::string name;
	int64_t durationMs = kDefaultAirTransition.count();
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
	bool forceH264 = false;
	std::string replayEncoder;
	bool allowOtherAdapters = true;
	bool showAdvanced = false;
	int64_t bufferMemoryMiB = 0;
	std::vector<SavedSource> sources;
	SavedMedia intro;
	SavedTransition into;
	SavedMedia outro;
	SavedTransition back;
};

SavedSettings saveSettings(const BufferSettings &settings);

// Empty when the version is not kSettingsVersion. Values out of range are brought
// back into it, a resolution this build does not know becomes the canvas, and an intro
// or outro of a kind it does not know becomes none.
std::optional<BufferSettings> loadSettings(const SavedSettings &saved);

struct SourceIdentity {
	std::string uuid;
	std::string name;
	// Whether it can be captured, as the dock lists it: an input with video.
	bool capturable = true;
};

// A duplicated scene collection gives every source a new UUID. The settings saved for a
// UUID that `sources` does not have move to the capturable source with the name saved
// alongside, when exactly one has that name, it has no settings of its own, and no other
// missing UUID claims it too. Returns the moves, from the saved UUID to the new one.
std::map<std::string, std::string> matchSourcesByName(BufferSettings &settings,
						      const std::map<std::string, std::string> &savedNames,
						      const std::vector<SourceIdentity> &sources);

} // namespace tapeloop
