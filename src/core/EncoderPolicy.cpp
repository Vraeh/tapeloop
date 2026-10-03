// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/EncoderPolicy.hpp"

#include <algorithm>
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

// Position in the order of preference, lower first. Encoders outside the order get
// nothing.
std::optional<int> rankOf(const EncoderInfo &encoder)
{
	switch (encoder.vendor) {
	case Vendor::Nvidia:
		return encoder.passTexture ? std::optional<int>(0) : std::nullopt;
	case Vendor::Intel:
		return encoder.passTexture ? std::optional<int>(1) : std::nullopt;
	case Vendor::Amd:
		return encoder.passTexture ? std::optional<int>(2) : std::nullopt;
	case Vendor::Apple:
		return 3;
	case Vendor::Software:
		return 4;
	case Vendor::Unknown:
		break;
	}
	return std::nullopt;
}

std::optional<EncoderInfo> bestFor(std::span<const EncoderInfo> encoders, const std::string &codec)
{
	const EncoderInfo *best = nullptr;
	int bestRank = 0;
	for (const EncoderInfo &encoder : encoders) {
		if (encoder.deprecated || encoder.internal || encoder.codec != codec)
			continue;
		const std::optional<int> rank = rankOf(encoder);
		if (rank && (!best || *rank < bestRank)) {
			best = &encoder;
			bestRank = *rank;
		}
	}
	return best ? std::optional<EncoderInfo>(*best) : std::nullopt;
}

// A frame duration that is not positive cannot come from OBS; 60 fps stands in.
Rational validFrameDuration(Rational frameDuration)
{
	if (frameDuration.num <= 0 || frameDuration.den <= 0)
		return {1, kReferenceFramesPerSecond};
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

} // namespace

std::optional<EncoderInfo> chooseReplayEncoder(std::span<const EncoderInfo> encoders,
					       const EncoderPreferences &preferences)
{
	if (preferences.preferHevc) {
		if (std::optional<EncoderInfo> hevc = bestFor(encoders, "hevc"))
			return hevc;
	}
	return bestFor(encoders, "h264");
}

EncoderSettings buildReplaySettings(const EncoderInfo &encoder, const ReplayEncoderParams &params)
{
	const Rational frameDuration = validFrameDuration(params.frameDuration);
	const Nanoseconds gop = std::clamp(params.gop, Nanoseconds{0}, kLongestGop);
	const Nanoseconds fallbackGop =
		std::clamp(params.fallbackGop, Nanoseconds{std::chrono::seconds(1)}, kLongestGop);
	const int64_t keyintSeconds = std::chrono::ceil<std::chrono::seconds>(fallbackGop).count();
	const std::string gopLength = std::to_string(gopFrames(gop, frameDuration));

	int64_t bitrate = replayBitrateKbps(params);

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
			{"opts", text("keyint=" + gopLength)},
		};
		break;
	case Vendor::Intel:
		// The QSV plugin keeps the bitrate in 16 bits and, on newer Intel GPUs,
		// derives a window maximum of 1.3 times it in 16 bits as well.
		bitrate = std::min<int64_t>(bitrate, kMaxQsvBitrateKbps);
		settings = {
			{"rate_control", text("CBR")},
			{"bframes", number(0)},
			{"target_usage", text("TU7")},
			{"latency", text("ultra-low")},
		};
		break;
	case Vendor::Amd:
		settings = {
			{"rate_control", text("CBR")},
			{"preset", text("speed")},
		};
		if (encoder.codec == "hevc") {
			// For HEVC the plugin's keyint option sets GOPs per IDR, not the GOP
			// length, so the AMF property is named directly. HEVC on AMF has no
			// B-frames to turn off.
			settings.insert({"ffmpeg_opts", text("HevcGOPSize=" + gopLength)});
		} else {
			settings.insert({
				{"bf", number(0)},
				{"ffmpeg_opts", text("keyint=" + gopLength)},
			});
		}
		break;
	case Vendor::Apple:
		settings = {
			{"rate_control", text("CBR")},
			{"bframes", flag(false)},
		};
		break;
	case Vendor::Software:
		settings = {
			{"rate_control", text("CBR")},
			{"bf", number(0)},
			{"preset", text("ultrafast")},
			{"tune", text("zerolatency")},
			{"x264opts", text("keyint=" + gopLength)},
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

int64_t replayBitrateKbps(const ReplayEncoderParams &params)
{
	const Rational frameDuration = validFrameDuration(params.frameDuration);
	const int64_t pixels = std::clamp<int64_t>(params.width, 0, kMaxDimension) *
			       std::clamp<int64_t>(params.height, 0, kMaxDimension);
	const int32_t reference = std::max(params.referenceBitrateKbps, 1);
	const int64_t atReferenceRate = rescale(pixels, {reference, kReferencePixels}, {1, 1});
	const int64_t scaled =
		rescale(atReferenceRate, {frameDuration.den, kReferenceFramesPerSecond}, {frameDuration.num, 1});
	return std::clamp<int64_t>(scaled, 1, std::max(params.maxBitrateKbps, 1));
}

int64_t gopFrames(Nanoseconds gop, Rational frameDuration)
{
	const int64_t frames =
		rescale(std::max(gop, Nanoseconds{0}).count(), kNanosecondTimebase, validFrameDuration(frameDuration));
	return std::max<int64_t>(frames, 1);
}

} // namespace tapeloop
