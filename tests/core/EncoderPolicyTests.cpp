// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/EncoderPolicy.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace std::chrono_literals;
using tapeloop::chooseReplayEncoder;
using tapeloop::EncoderInfo;
using tapeloop::EncoderPreferences;
using tapeloop::EncoderSettings;
using tapeloop::Nanoseconds;
using tapeloop::ReplayEncoderParams;
using tapeloop::Vendor;

namespace {

EncoderInfo encoder(const char *id, const char *codec, Vendor vendor, bool passTexture, bool deprecated = false,
		    bool internal = false)
{
	return {id, codec, vendor, passTexture, deprecated, internal};
}

// The H.264 and HEVC ids OBS 32.0.4 registers for each vendor, including some to avoid.
std::vector<EncoderInfo> nvidia()
{
	return {
		encoder("jim_nvenc", "h264", Vendor::Nvidia, true, true),
		encoder("jim_hevc_nvenc", "hevc", Vendor::Nvidia, true, true),
		encoder("obs_nvenc_h264_cuda", "h264", Vendor::Nvidia, false, true),
		encoder("obs_nvenc_h264_soft", "h264", Vendor::Nvidia, false, false, true),
		encoder("obs_nvenc_hevc_soft", "hevc", Vendor::Nvidia, false, false, true),
		encoder("obs_nvenc_h264_tex", "h264", Vendor::Nvidia, true),
		encoder("obs_nvenc_hevc_tex", "hevc", Vendor::Nvidia, true),
		encoder("obs_nvenc_av1_tex", "av1", Vendor::Nvidia, true),
	};
}

std::vector<EncoderInfo> intel()
{
	return {
		encoder("obs_qsv11", "h264", Vendor::Intel, true, true),
		encoder("obs_qsv11_soft", "h264", Vendor::Intel, false, true, true),
		encoder("obs_qsv11_soft_v2", "h264", Vendor::Intel, false, false, true),
		encoder("obs_qsv11_v2", "h264", Vendor::Intel, true),
		encoder("obs_qsv11_hevc_soft", "hevc", Vendor::Intel, false, false, true),
		encoder("obs_qsv11_hevc", "hevc", Vendor::Intel, true),
	};
}

std::vector<EncoderInfo> amd()
{
	return {
		encoder("h264_fallback_amf", "h264", Vendor::Amd, false, false, true),
		encoder("h264_texture_amf", "h264", Vendor::Amd, true),
		encoder("h265_fallback_amf", "hevc", Vendor::Amd, false, false, true),
		encoder("h265_texture_amf", "hevc", Vendor::Amd, true),
	};
}

std::vector<EncoderInfo> x264()
{
	return {encoder("obs_x264", "h264", Vendor::Software, false)};
}

std::vector<EncoderInfo> combined(std::vector<std::vector<EncoderInfo>> groups)
{
	std::vector<EncoderInfo> all;
	for (const auto &group : groups)
		all.insert(all.end(), group.begin(), group.end());
	return all;
}

std::string chosen(const std::vector<EncoderInfo> &encoders, bool preferHevc = false)
{
	EncoderPreferences preferences;
	preferences.preferHevc = preferHevc;
	const std::optional<EncoderInfo> choice = chooseReplayEncoder(encoders, preferences);
	return choice ? choice->id : "none";
}

} // namespace

TEST_CASE("chooseReplayEncoder prefers NVIDIA, then Intel, AMD, Apple and x264")
{
	CHECK(chosen(combined({x264(), nvidia()})) == "obs_nvenc_h264_tex");
	CHECK(chosen(combined({x264(), intel(), nvidia()})) == "obs_nvenc_h264_tex");
	CHECK(chosen(combined({x264(), intel()})) == "obs_qsv11_v2");
	CHECK(chosen(combined({x264(), amd(), intel()})) == "obs_qsv11_v2");
	CHECK(chosen(combined({x264(), amd()})) == "h264_texture_amf");
	CHECK(chosen(x264()) == "obs_x264");
	CHECK(chosen({}) == "none");

	const EncoderInfo apple = encoder("com.apple.videotoolbox.videoencoder.ave.avc", "h264", Vendor::Apple, false);
	CHECK(chosen(combined({x264(), {apple}})) == apple.id);
}

TEST_CASE("chooseReplayEncoder does not depend on the order encoders are listed in")
{
	std::vector<EncoderInfo> all = combined({nvidia(), intel(), amd(), x264()});
	std::reverse(all.begin(), all.end());
	CHECK(chosen(all) == "obs_nvenc_h264_tex");
	std::rotate(all.begin(), all.begin() + 5, all.end());
	CHECK(chosen(all) == "obs_nvenc_h264_tex");
}

TEST_CASE("chooseReplayEncoder ignores deprecated, internal and unknown encoders")
{
	const std::vector<EncoderInfo> leftovers = {
		encoder("jim_nvenc", "h264", Vendor::Nvidia, true, true),
		encoder("obs_nvenc_h264_soft", "h264", Vendor::Nvidia, false, false, true),
		encoder("obs_qsv11", "h264", Vendor::Intel, true, true),
		encoder("ffmpeg_vaapi_tex", "h264", Vendor::Unknown, true),
	};
	CHECK(chosen(leftovers) == "none");
	CHECK(chosen(combined({leftovers, x264()})) == "obs_x264");
}

TEST_CASE("chooseReplayEncoder uses HEVC only when asked and available")
{
	CHECK(chosen(combined({nvidia(), x264()}), true) == "obs_nvenc_hevc_tex");
	CHECK(chosen(combined({intel(), x264()}), true) == "obs_qsv11_hevc");
	CHECK(chosen(combined({nvidia(), x264()}), false) == "obs_nvenc_h264_tex");
	CHECK(chosen(x264(), true) == "obs_x264");

	// The preference outranks the vendor order: Intel HEVC beats NVIDIA H.264.
	std::vector<EncoderInfo> h264Only = nvidia();
	std::erase_if(h264Only, [](const EncoderInfo &info) { return info.codec != "h264"; });
	CHECK(chosen(combined({h264Only, intel()}), true) == "obs_qsv11_hevc");
	CHECK(chosen(h264Only, true) == "obs_nvenc_h264_tex");
}

TEST_CASE("replay bitrate scales with pixel rate and is capped")
{
	ReplayEncoderParams params;
	CHECK(tapeloop::replayBitrateKbps(params) == 30'000);

	params.width = 1280;
	params.height = 720;
	CHECK(tapeloop::replayBitrateKbps(params) == 13'333);

	params.width = 1920;
	params.height = 1080;
	params.frameDuration = {1001, 60000};
	CHECK(tapeloop::replayBitrateKbps(params) == 29'970);
	params.frameDuration = {1, 30};
	CHECK(tapeloop::replayBitrateKbps(params) == 15'000);

	params.width = 3840;
	params.height = 2160;
	params.frameDuration = {1, 60};
	CHECK(tapeloop::replayBitrateKbps(params) == 100'000);
	params.maxBitrateKbps = 150'000;
	CHECK(tapeloop::replayBitrateKbps(params) == 120'000);

	params.width = 1;
	params.height = 1;
	CHECK(tapeloop::replayBitrateKbps(params) == 1);
	params.width = -5;
	CHECK(tapeloop::replayBitrateKbps(params) == 1);
}

TEST_CASE("GOP length in frames rounds to the nearest frame")
{
	CHECK(tapeloop::gopFrames(500ms, {1, 60}) == 30);
	CHECK(tapeloop::gopFrames(500ms, {1001, 60000}) == 30);
	CHECK(tapeloop::gopFrames(500ms, {1, 30}) == 15);
	CHECK(tapeloop::gopFrames(500ms, {1, 25}) == 13);
	CHECK(tapeloop::gopFrames(1s, {1001, 60000}) == 60);
	CHECK(tapeloop::gopFrames(1ms, {1, 60}) == 1);
	CHECK(tapeloop::gopFrames(0ms, {1, 60}) == 1);
}

TEST_CASE("replay settings for each encoder family")
{
	ReplayEncoderParams params;
	params.frameDuration = {1001, 60000};
	const auto settingsFor = [&](const char *id, const char *codec, Vendor vendor) {
		return tapeloop::buildReplaySettings(encoder(id, codec, vendor, true), params);
	};

	SECTION("NVENC")
	{
		const EncoderSettings expected = {
			{"rate_control", std::string("CBR")},
			{"bitrate", int64_t{29'970}},
			{"keyint_sec", int64_t{1}},
			{"bf", int64_t{0}},
			{"preset", std::string("p1")},
			{"tune", std::string("ull")},
			{"multipass", std::string("disabled")},
			{"lookahead", false},
			{"adaptive_quantization", false},
			{"opts", std::string("keyint=30")},
		};
		CHECK(settingsFor("obs_nvenc_h264_tex", "h264", Vendor::Nvidia) == expected);
		CHECK(settingsFor("obs_nvenc_hevc_tex", "hevc", Vendor::Nvidia) == expected);
	}

	SECTION("QuickSync")
	{
		const EncoderSettings expected = {
			{"rate_control", std::string("CBR")}, {"bitrate", int64_t{29'970}},
			{"keyint_sec", int64_t{1}},           {"bframes", int64_t{0}},
			{"target_usage", std::string("TU7")}, {"latency", std::string("ultra-low")},
		};
		CHECK(settingsFor("obs_qsv11_v2", "h264", Vendor::Intel) == expected);
		CHECK(settingsFor("obs_qsv11_hevc", "hevc", Vendor::Intel) == expected);
	}

	SECTION("AMF")
	{
		const EncoderSettings h264 = {
			{"rate_control", std::string("CBR")}, {"bitrate", int64_t{29'970}},
			{"keyint_sec", int64_t{1}},           {"bf", int64_t{0}},
			{"preset", std::string("speed")},     {"ffmpeg_opts", std::string("keyint=30")},
		};
		CHECK(settingsFor("h264_texture_amf", "h264", Vendor::Amd) == h264);

		const EncoderSettings hevc = {
			{"rate_control", std::string("CBR")},
			{"bitrate", int64_t{29'970}},
			{"keyint_sec", int64_t{1}},
			{"preset", std::string("speed")},
			{"ffmpeg_opts", std::string("HevcGOPSize=30")},
		};
		CHECK(settingsFor("h265_texture_amf", "hevc", Vendor::Amd) == hevc);
	}

	SECTION("VideoToolbox")
	{
		const EncoderSettings expected = {
			{"rate_control", std::string("CBR")},
			{"bitrate", int64_t{29'970}},
			{"keyint_sec", int64_t{1}},
			{"bframes", false},
		};
		CHECK(settingsFor("com.apple.videotoolbox.videoencoder.ave.avc", "h264", Vendor::Apple) == expected);
	}

	SECTION("x264")
	{
		const EncoderSettings expected = {
			{"rate_control", std::string("CBR")},
			{"bitrate", int64_t{29'970}},
			{"keyint_sec", int64_t{1}},
			{"bf", int64_t{0}},
			{"preset", std::string("ultrafast")},
			{"tune", std::string("zerolatency")},
			{"x264opts", std::string("keyint=30")},
		};
		CHECK(settingsFor("obs_x264", "h264", Vendor::Software) == expected);
	}
}

TEST_CASE("replay settings follow the GOP parameters")
{
	ReplayEncoderParams params;
	params.gop = 250ms;
	params.fallbackGop = 1500ms;
	const EncoderSettings settings =
		tapeloop::buildReplaySettings(encoder("obs_x264", "h264", Vendor::Software, false), params);
	CHECK(settings.at("x264opts") == tapeloop::SettingValue(std::string("keyint=15")));
	CHECK(settings.at("keyint_sec") == tapeloop::SettingValue(int64_t{2}));
}

TEST_CASE("chooseReplayEncoder needs texture support from NVIDIA, Intel and AMD")
{
	const std::vector<EncoderInfo> encoders = {
		encoder("future_nvenc_cpu", "h264", Vendor::Nvidia, false),
		encoder("future_qsv_internal", "h264", Vendor::Intel, true, false, true),
		encoder("obs_x264", "h264", Vendor::Software, false),
	};
	CHECK(chosen(encoders) == "obs_x264");
}

TEST_CASE("chooseReplayEncoder keeps the first of two equal encoders")
{
	const EncoderInfo first = encoder("first_tex", "h264", Vendor::Nvidia, true);
	const EncoderInfo second = encoder("second_tex", "h264", Vendor::Nvidia, true);
	CHECK(chosen({first, second}) == "first_tex");
	CHECK(chosen({second, first}) == "second_tex");
}

TEST_CASE("replay settings keep the QSV bitrate within 16 bits")
{
	ReplayEncoderParams params;
	params.width = 3840;
	params.height = 2160;
	const EncoderSettings intel =
		tapeloop::buildReplaySettings(encoder("obs_qsv11_v2", "h264", Vendor::Intel, true), params);
	CHECK(intel.at("bitrate") == tapeloop::SettingValue(int64_t{50'000}));

	const EncoderSettings nvidia =
		tapeloop::buildReplaySettings(encoder("obs_nvenc_h264_tex", "h264", Vendor::Nvidia, true), params);
	CHECK(nvidia.at("bitrate") == tapeloop::SettingValue(int64_t{100'000}));
}

TEST_CASE("replay settings stay within what OBS accepts on odd input")
{
	ReplayEncoderParams params;
	params.frameDuration = {0, 0};
	params.gop = Nanoseconds::max();
	params.fallbackGop = -1s;
	const EncoderSettings settings =
		tapeloop::buildReplaySettings(encoder("obs_x264", "h264", Vendor::Software, false), params);

	CHECK(settings.at("bitrate") == tapeloop::SettingValue(int64_t{30'000}));
	CHECK(settings.at("x264opts") == tapeloop::SettingValue(std::string("keyint=600")));
	CHECK(settings.at("keyint_sec") == tapeloop::SettingValue(int64_t{1}));

	params.fallbackGop = 1h;
	params.gop = -1s;
	const EncoderSettings clamped =
		tapeloop::buildReplaySettings(encoder("obs_x264", "h264", Vendor::Software, false), params);
	CHECK(clamped.at("keyint_sec") == tapeloop::SettingValue(int64_t{10}));
	CHECK(clamped.at("x264opts") == tapeloop::SettingValue(std::string("keyint=1")));
	CHECK(tapeloop::gopFrames(1s, {-1, 60}) == 60);
}
