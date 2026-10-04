// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/MediaTime.hpp"

#include <chrono>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

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

// The vendor of an OBS encoder id, from the ids OBS 32 registers; Unknown for any other.
// VideoToolbox ids cannot be told apart from its software encoders and all count as
// Apple.
Vendor encoderVendor(std::string_view id);

// The vendor of a graphics adapter, from the name its driver reports; Unknown when the
// name gives none, as for software renderers.
Vendor adapterVendor(std::string_view name);

struct EncoderPreferences {
	// Try each vendor's HEVC encoder before its H.264 one.
	bool preferHevc = false;
};

// The encoders to try for a replay buffer, best first. Texture encoders of the vendor
// whose adapter OBS renders on come first, then the other hardware encoders (that
// vendor's own, then NVIDIA, Intel, AMD and VideoToolbox), then x264. Within a vendor,
// texture encoders go before the others and, when HEVC is preferred, each HEVC encoder
// goes right before the H.264 one. When the render vendor is Unknown or Software, the
// order is the fixed one: NVIDIA, Intel and AMD texture encoders, VideoToolbox, x264.
// Only H.264 encoders qualify, and HEVC ones when preferred; never deprecated or
// internal ones, nor those of an Unknown vendor. Between equals the first listed
// comes first.
std::vector<EncoderInfo> replayEncoderCandidates(std::span<const EncoderInfo> encoders, Vendor renderVendor,
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
// The packet bytes a buffer of the given length may hold at the given bitrate, with
// room for the bitrate to run over its target: half as much again, for now.
size_t replayByteBudget(int64_t bitrateKbps, Nanoseconds length);
// The GOP length in frames, rounded to the nearest frame and at least one.
int64_t gopFrames(Nanoseconds gop, Rational frameDuration);

} // namespace tapeloop
