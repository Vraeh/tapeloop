// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/MediaTime.hpp"

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <variant>

namespace tapeloop {

// Software stands for obs_x264, the only software encoder the policy knows. Apple
// stands for VideoToolbox hardware encoders.
enum class Vendor { Unknown, Nvidia, Intel, Amd, Apple, Software };

// One registered OBS encoder, as the glue reads it from obs_enum_encoder_types and
// obs_get_encoder_caps.
struct EncoderInfo {
	std::string id;
	// As obs_get_encoder_codec reports it: "h264", "hevc" or "av1".
	std::string codec;
	Vendor vendor = Vendor::Unknown;
	bool passTexture = false;
	bool deprecated = false;
	bool internal = false;
};

struct EncoderPreferences {
	// Use HEVC when some encoder offers it, H.264 otherwise.
	bool preferHevc = false;
};

// Picks the replay encoder: NVIDIA, Intel and AMD texture encoders in that order, then
// VideoToolbox, then x264. Deprecated and internal ids never qualify; between equals
// the first listed wins.
std::optional<EncoderInfo> chooseReplayEncoder(std::span<const EncoderInfo> encoders,
					       const EncoderPreferences &preferences);

struct ReplayEncoderParams {
	int64_t width = 1920;
	int64_t height = 1080;
	// How long one frame lasts, in seconds: {1001, 60000} is 59.94 fps.
	Rational frameDuration{1, 60};

	// The bitrate at 1080p60, scaled by pixel rate and capped.
	int32_t referenceBitrateKbps = 30'000;
	int32_t maxBitrateKbps = 100'000;
	// GOP length where the encoder takes it in frames through free-form options, and
	// in whole seconds elsewhere. Both are kept within ten seconds.
	Nanoseconds gop = std::chrono::milliseconds(500);
	Nanoseconds fallbackGop = std::chrono::seconds(1);
};

using SettingValue = std::variant<int64_t, bool, std::string>;
// Keys and values for the encoder's obs_data, in the types obs_data_set_int,
// obs_data_set_bool and obs_data_set_string take. They must be written as user values:
// some plugins ignore keys that only have a default.
using EncoderSettings = std::map<std::string, SettingValue>;

EncoderSettings buildReplaySettings(const EncoderInfo &encoder, const ReplayEncoderParams &params);

int64_t replayBitrateKbps(const ReplayEncoderParams &params);
// The GOP length in frames, rounded to the nearest frame and at least one.
int64_t gopFrames(Nanoseconds gop, Rational frameDuration);

} // namespace tapeloop
