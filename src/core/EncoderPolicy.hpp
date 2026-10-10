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

// How an encoder takes the frames of a capture. Only Texture is the optimal path; the
// others work, and the plugin says so where it uses them.
enum class EncoderPath {
	// A hardware encoder that takes OBS's textures on the adapter OBS renders on.
	Texture,
	// A hardware encoder that takes every frame read back through memory: one on another
	// adapter than the one OBS renders on, one without texture input, or any when OBS has
	// no NV12 textures to give.
	Readback,
	// x264, on the CPU.
	Software,
};

// Why a hardware encoder takes its frames through memory, for the note that says so.
enum class ReadbackReason {
	None,
	// It runs on another adapter than the one OBS renders on.
	OtherAdapter,
	// It takes no textures at all.
	NoTextureInput,
	// OBS gives it no NV12 textures, which its renderer or adapter may not have.
	NoTextures,
};

// renderVendor is that of the adapter OBS renders on, and nv12Textures whether OBS hands
// the capture NV12 textures. A texture encoder of another vendor's adapter takes
// frames through memory too: OBS 32's QuickSync, for one, hands itself over to its
// non-texture variant when OBS renders elsewhere. VideoToolbox takes no textures, but
// on a Mac it is the encoder of the adapter OBS renders on and the best path there is.
EncoderPath encoderPathOf(const EncoderInfo &encoder, Vendor renderVendor, bool nv12Textures) noexcept;
// None unless encoderPathOf gives Readback for the same encoder; the first reason that
// holds, in the order of the enum, when several do.
ReadbackReason readbackReasonOf(const EncoderInfo &encoder, Vendor renderVendor, bool nv12Textures) noexcept;

// The vendor of an OBS encoder id, from the ids OBS 32 registers; Unknown for any other.
// VideoToolbox ids cannot be told apart from its software encoders and all count as
// Apple.
Vendor encoderVendor(std::string_view id);

// The vendor of a graphics adapter, from the name its driver reports; Unknown when the
// name gives none, as for software renderers.
Vendor adapterVendor(std::string_view name);

struct EncoderPreferences {
	// Try each vendor's HEVC encoder before its H.264 one. HEVC gives the same picture at
	// a lower bitrate, so less memory per buffer and less disk per replay.
	bool preferHevc = true;
	// An encoder the user chose, tried before all others when it is one of the
	// replayEncoderChoices; empty for the automatic order alone.
	std::string chosen;
	// Hardware encoders of another vendor than the adapter OBS renders on, whose frames
	// travel through memory. Without them the automatic order goes from the render
	// adapter's encoders straight to x264.
	bool otherAdapters = true;
};

// The encoders to try for a replay buffer, best first. Texture encoders of the vendor
// whose adapter OBS renders on come first, then the other hardware encoders (that
// vendor's own, then NVIDIA, Intel, AMD and VideoToolbox), then x264. Within a vendor,
// texture encoders go before the others and, when HEVC is preferred, each HEVC encoder
// goes right before the H.264 one. When the render vendor is Unknown or Software, the
// order is the fixed one: NVIDIA, Intel and AMD texture encoders, VideoToolbox, x264.
// Only H.264 encoders qualify, and HEVC ones when preferred; never deprecated or
// internal ones, nor those of an Unknown vendor. Between equals the first listed
// comes first. Without other adapters, hardware encoders of another vendor than a hardware
// render vendor are left out. An encoder the user chose goes before all of them, even
// one this order leaves out, as long as it is one of the replayEncoderChoices.
std::vector<EncoderInfo> replayEncoderCandidates(std::span<const EncoderInfo> encoders, Vendor renderVendor,
						 const EncoderPreferences &preferences);

// The encoders a user may choose for replays: those that encode H.264 or HEVC, which a
// replay can hold, that OBS neither hides nor deprecates, and of a known vendor, since
// only those get settings without B-frames, in the order given.
std::vector<EncoderInfo> replayEncoderChoices(std::span<const EncoderInfo> encoders);

// The candidates for a source whose HEVC encoder failed while it ran: the H.264 encoders
// of that encoder's vendor take its place, or the place of the first of them when it
// comes earlier, those among the candidates in their order, else those a user may
// choose. Candidates without the encoder that failed, as after the user chose another
// one, stay as they are.
std::vector<EncoderInfo> candidatesAfterHevcFailure(std::span<const EncoderInfo> candidates,
						    std::span<const EncoderInfo> encoders, std::string_view failedId);

struct ReplayEncoderParams {
	int64_t width = 1920;
	int64_t height = 1080;
	// How long one frame lasts, in seconds: {1001, 60000} is 59.94 fps.
	Rational frameDuration{1, 60};

	// The H.264 bitrate at 1080p60, scaled by pixel rate and capped. HEVC gets this share
	// of it, for now: the picture HEVC gives at it is still to be measured.
	int32_t referenceBitrateKbps = 30'000;
	int32_t hevcBitratePercent = 60;
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

// For an encoder of that codec, as obs_get_encoder_codec names it.
int64_t replayBitrateKbps(const ReplayEncoderParams &params, std::string_view codec);
// The packet bytes a buffer of the given length may hold at the given bitrate, with
// room for the bitrate to run over its target: half as much again, for now.
size_t replayByteBudget(int64_t bitrateKbps, Nanoseconds length);
// What a buffer with this byte budget holds at its target bitrate: the budget without the
// margin replayByteBudget adds. A buffer given that much holds its length unless the
// bitrate runs over.
size_t nominalReplayBytes(size_t byteBudget) noexcept;
// The GOP length in frames, rounded to the nearest frame and at least one.
int64_t gopFrames(Nanoseconds gop, Rational frameDuration);

} // namespace tapeloop
