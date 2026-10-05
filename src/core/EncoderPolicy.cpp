// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/EncoderPolicy.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace tapeloop {
namespace {

constexpr int32_t kReferencePixels = 1920 * 1080;
constexpr int32_t kReferenceFramesPerSecond = 60;
// Wider than any video OBS can render, small enough that width times height fits.
constexpr int64_t kMaxDimension = int64_t{1} << 20;
// The longest keyframe interval the OBS encoder properties offer anywhere.
constexpr Nanoseconds kLongestGop = std::chrono::seconds(10);
// Keeps the bitrate and 1.3 times it within the 16 bits the QSV plugin stores them in.
constexpr int64_t kMaxQsvBitrateKbps = 50'000;

bool isHardware(Vendor vendor)
{
	return vendor == Vendor::Nvidia || vendor == Vendor::Intel || vendor == Vendor::Amd || vendor == Vendor::Apple;
}

int vendorOrder(Vendor vendor, Vendor renderVendor)
{
	if (vendor == renderVendor) {
		return 0;
	}
	switch (vendor) {
	case Vendor::Nvidia:
		return 1;
	case Vendor::Intel:
		return 2;
	case Vendor::Amd:
		return 3;
	case Vendor::Apple:
	case Vendor::Software:
	case Vendor::Unknown:
		break;
	}
	return 4;
}

// Sort key of a candidate, compared lexicographically: tier, vendor, texture, codec.
// Encoders that do not qualify get nothing.
std::optional<std::array<int, 4>> placementOf(const EncoderInfo &encoder, Vendor renderVendor,
					      const EncoderPreferences &preferences)
{
	if (encoder.deprecated || encoder.internal) {
		return std::nullopt;
	}

	int codec = 0;
	if (encoder.codec == "h264") {
		codec = 1;
	} else if (encoder.codec != "hevc" || !preferences.preferHevc) {
		return std::nullopt;
	}

	if (encoder.vendor == Vendor::Software) {
		return std::array<int, 4>{2, 0, 0, codec};
	}
	if (!isHardware(encoder.vendor)) {
		return std::nullopt;
	}
	if (!isHardware(renderVendor) && !encoder.passTexture && encoder.vendor != Vendor::Apple) {
		return std::nullopt;
	}
	if (!preferences.otherAdapters && isHardware(renderVendor) && encoder.vendor != renderVendor) {
		return std::nullopt;
	}

	// OBS 32 registers the non-texture NVENC, QuickSync and AMF encoders as internal or
	// deprecated. Their texture ids fall back to them when the texture path cannot
	// start, as on another vendor's adapter, so there those ids stand for the
	// non-texture encoders.
	const int tier = encoder.vendor == renderVendor && encoder.passTexture ? 0 : 1;
	return std::array<int, 4>{tier, vendorOrder(encoder.vendor, renderVendor), encoder.passTexture ? 0 : 1, codec};
}

// A frame duration that is not positive cannot come from OBS; 60 fps stands in.
Rational validFrameDuration(Rational frameDuration)
{
	if (frameDuration.num <= 0 || frameDuration.den <= 0) {
		return {1, kReferenceFramesPerSecond};
	}
	return frameDuration;
}

SettingValue text(std::string value)
{
	return SettingValue(std::move(value));
}

SettingValue number(int64_t value)
{
	return SettingValue(value);
}

SettingValue flag(bool value)
{
	return SettingValue(value);
}

bool startsWith(std::string_view text, std::string_view prefix)
{
	return text.substr(0, prefix.size()) == prefix;
}

bool endsWith(std::string_view text, std::string_view suffix)
{
	return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

std::string lowercase(std::string_view text)
{
	std::string lower(text);
	std::transform(lower.begin(), lower.end(), lower.begin(),
		       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return lower;
}

} // namespace

EncoderPath encoderPathOf(const EncoderInfo &encoder, Vendor renderVendor, bool nv12Textures) noexcept
{
	if (encoder.vendor == Vendor::Software) {
		return EncoderPath::Software;
	}
	if (encoder.vendor == Vendor::Apple && renderVendor == Vendor::Apple) {
		return EncoderPath::Texture;
	}
	// An Unknown render vendor tells nothing about where the encoder runs.
	const bool otherAdapter = renderVendor != Vendor::Unknown && renderVendor != Vendor::Software &&
				  encoder.vendor != renderVendor;
	if (!encoder.passTexture || otherAdapter || !nv12Textures) {
		return EncoderPath::Readback;
	}
	return EncoderPath::Texture;
}

Vendor encoderVendor(std::string_view id)
{
	if (startsWith(id, "obs_nvenc_") || startsWith(id, "jim_") || id == "ffmpeg_nvenc" ||
	    id == "ffmpeg_hevc_nvenc") {
		return Vendor::Nvidia;
	}
	if (startsWith(id, "obs_qsv11")) {
		return Vendor::Intel;
	}
	if (endsWith(id, "_texture_amf") || endsWith(id, "_fallback_amf")) {
		return Vendor::Amd;
	}
	if (startsWith(id, "com.apple.videotoolbox.videoencoder.")) {
		return Vendor::Apple;
	}
	if (id == "obs_x264") {
		return Vendor::Software;
	}
	return Vendor::Unknown;
}

Vendor adapterVendor(std::string_view name)
{
	const std::string lower = lowercase(name);
	if (lower.find("nvidia") != std::string::npos) {
		return Vendor::Nvidia;
	}
	if (lower.find("radeon") != std::string::npos || lower.find("amd") != std::string::npos) {
		return Vendor::Amd;
	}
	if (lower.find("intel") != std::string::npos) {
		return Vendor::Intel;
	}
	if (lower.find("apple") != std::string::npos) {
		return Vendor::Apple;
	}
	return Vendor::Unknown;
}

std::vector<EncoderInfo> replayEncoderCandidates(std::span<const EncoderInfo> encoders, Vendor renderVendor,
						 const EncoderPreferences &preferences)
{
	struct Candidate {
		std::array<int, 4> placement;
		const EncoderInfo *encoder;
	};
	std::vector<Candidate> candidates;
	for (const EncoderInfo &encoder : encoders) {
		if (const std::optional<std::array<int, 4>> placement =
			    placementOf(encoder, renderVendor, preferences)) {
			candidates.push_back({*placement, &encoder});
		}
	}
	std::stable_sort(candidates.begin(), candidates.end(),
			 [](const Candidate &a, const Candidate &b) { return a.placement < b.placement; });

	std::vector<EncoderInfo> ordered;
	ordered.reserve(candidates.size() + 1);
	// A choice the user made goes first, even one the automatic order leaves out, as long
	// as a replay can hold what it encodes.
	if (!preferences.chosen.empty()) {
		for (const EncoderInfo &choice : replayEncoderChoices(encoders)) {
			if (choice.id == preferences.chosen) {
				ordered.push_back(choice);
				break;
			}
		}
	}
	for (const Candidate &candidate : candidates) {
		if (candidate.encoder->id != preferences.chosen) {
			ordered.push_back(*candidate.encoder);
		}
	}
	return ordered;
}

std::vector<EncoderInfo> replayEncoderChoices(std::span<const EncoderInfo> encoders)
{
	std::vector<EncoderInfo> choices;
	for (const EncoderInfo &encoder : encoders) {
		if (!encoder.deprecated && !encoder.internal && (encoder.codec == "h264" || encoder.codec == "hevc")) {
			choices.push_back(encoder);
		}
	}
	return choices;
}

EncoderSettings buildReplaySettings(const EncoderInfo &encoder, const ReplayEncoderParams &params)
{
	const Rational frameDuration = validFrameDuration(params.frameDuration);
	const Nanoseconds gop = std::clamp(params.gop, Nanoseconds{0}, kLongestGop);
	const Nanoseconds fallbackGop =
		std::clamp(params.fallbackGop, Nanoseconds{std::chrono::seconds(1)}, kLongestGop);
	const int64_t keyintSeconds = std::chrono::ceil<std::chrono::seconds>(fallbackGop).count();
	const std::string gopLength = std::to_string(gopFrames(gop, frameDuration));

	int64_t bitrate = replayBitrateKbps(params, encoder.codec);

	// Key names and values are those read by the OBS 32.0.4 encoder plugins. The
	// free-form option strings are the only way to get a GOP shorter than a second.
	EncoderSettings settings;
	switch (encoder.vendor) {
	case Vendor::Nvidia:
		settings = {
			{"rate_control", text("CBR")},
			{"bf", number(0)},
			{"preset", text("p1")},
			{"tune", text("ull")},
			{"multipass", text("disabled")},
			{"lookahead", flag(false)},
			{"adaptive_quantization", flag(false)},
			{"repeat_headers", flag(true)},
			{"opts", text("keyint=" + gopLength)},
		};
		break;
	case Vendor::Intel:
		// The QSV plugin keeps the bitrate in 16 bits and, on newer Intel GPUs,
		// derives a window maximum of 1.3 times it in 16 bits as well.
		bitrate = std::min<int64_t>(bitrate, kMaxQsvBitrateKbps);
		settings = {
			{"rate_control", text("CBR")},  {"bframes", number(0)},         {"target_usage", text("TU7")},
			{"latency", text("ultra-low")}, {"repeat_headers", flag(true)},
		};
		break;
	case Vendor::Amd:
		settings = {
			{"rate_control", text("CBR")},
			{"preset", text("speed")},
		};
		// The plugin's repeat_headers would space the headers by keyint_sec, not by
		// the GOP set in the options, and HEVC has no such setting, so both codecs
		// get the header placement through the options.
		if (encoder.codec == "hevc") {
			// For HEVC the plugin's keyint option sets GOPs per IDR, not the GOP
			// length, so the AMF property is named directly, with one GOP per IDR.
			// HEVC on AMF has no B-frames to turn off.
			settings.insert({"ffmpeg_opts", text("HevcGOPSize=" + gopLength +
							     " gops_per_idr=1 header_insertion_mode=idr")});
		} else {
			settings.insert({
				{"bf", number(0)},
				{"ffmpeg_opts", text("keyint=" + gopLength + " header_spacing=" + gopLength)},
			});
		}
		break;
	case Vendor::Apple:
		// VideoToolbox writes the parameter sets into every keyframe on its own.
		settings = {
			{"rate_control", text("CBR")},
			{"bframes", flag(false)},
		};
		break;
	case Vendor::Software:
		settings = {
			{"rate_control", text("CBR")},  {"bf", number(0)},
			{"preset", text("ultrafast")},  {"tune", text("zerolatency")},
			{"repeat_headers", flag(true)}, {"x264opts", text("keyint=" + gopLength)},
		};
		break;
	case Vendor::Unknown:
		break;
	}

	settings.insert({
		{"bitrate", number(bitrate)},
		{"keyint_sec", number(keyintSeconds)},
	});
	return settings;
}

int64_t replayBitrateKbps(const ReplayEncoderParams &params, std::string_view codec)
{
	const Rational frameDuration = validFrameDuration(params.frameDuration);
	const int64_t pixels = std::clamp<int64_t>(params.width, 0, kMaxDimension) *
			       std::clamp<int64_t>(params.height, 0, kMaxDimension);
	int32_t reference = std::max(params.referenceBitrateKbps, 1);
	if (codec == "hevc") {
		const int32_t percent = std::clamp(params.hevcBitratePercent, 1, 100);
		reference = static_cast<int32_t>(std::max<int64_t>(int64_t{reference} * percent / 100, 1));
	}
	const int64_t atReferenceRate = rescale(pixels, {reference, kReferencePixels}, {1, 1});
	const int64_t scaled =
		rescale(atReferenceRate, {frameDuration.den, kReferenceFramesPerSecond}, {frameDuration.num, 1});
	return std::clamp<int64_t>(scaled, 1, std::max(params.maxBitrateKbps, 1));
}

size_t replayByteBudget(int64_t bitrateKbps, Nanoseconds length)
{
	// kbps times nanoseconds is 10^-6 bits; over 8 bits per byte and times 3/2 for the
	// margin that is 3 / (16 * 10^6) bytes.
	constexpr int64_t kMaxBitrate = std::numeric_limits<int32_t>::max() / 3;
	const int32_t rate = static_cast<int32_t>(std::clamp<int64_t>(bitrateKbps, 1, kMaxBitrate));
	const int64_t bytes = rescale(std::max(length, Nanoseconds{0}).count(), {3 * rate, 16'000'000}, {1, 1});
	return static_cast<size_t>(bytes);
}

int64_t gopFrames(Nanoseconds gop, Rational frameDuration)
{
	const int64_t frames =
		rescale(std::max(gop, Nanoseconds{0}).count(), kNanosecondTimebase, validFrameDuration(frameDuration));
	return std::max<int64_t>(frames, 1);
}

} // namespace tapeloop
