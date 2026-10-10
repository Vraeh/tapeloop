// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/EncoderPolicy.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using namespace std::chrono_literals;
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
	for (const auto &group : groups) {
		all.insert(all.end(), group.begin(), group.end());
	}
	return all;
}

EncoderInfo apple()
{
	return encoder("com.apple.videotoolbox.videoencoder.ave.avc", "h264", Vendor::Apple, false);
}

using Ids = std::vector<std::string>;

Ids candidates(const std::vector<EncoderInfo> &encoders, Vendor renderVendor, bool preferHevc = false)
{
	EncoderPreferences preferences;
	preferences.preferHevc = preferHevc;
	Ids ids;
	for (const EncoderInfo &info : tapeloop::replayEncoderCandidates(encoders, renderVendor, preferences)) {
		ids.push_back(info.id);
	}
	return ids;
}

} // namespace

TEST_CASE("replay candidates on a Radeon with an Intel iGPU start with AMF")
{
	const std::vector<EncoderInfo> encoders = combined({x264(), intel(), amd()});
	CHECK(candidates(encoders, Vendor::Amd) == Ids{"h264_texture_amf", "obs_qsv11_v2", "obs_x264"});
	CHECK(candidates(encoders, Vendor::Amd, true) ==
	      Ids{"h265_texture_amf", "h264_texture_amf", "obs_qsv11_hevc", "obs_qsv11_v2", "obs_x264"});
}

TEST_CASE("replay candidates on an NVIDIA GPU with an Intel iGPU follow the render adapter")
{
	const std::vector<EncoderInfo> encoders = combined({x264(), intel(), nvidia()});
	CHECK(candidates(encoders, Vendor::Nvidia) == Ids{"obs_nvenc_h264_tex", "obs_qsv11_v2", "obs_x264"});
	CHECK(candidates(encoders, Vendor::Intel) == Ids{"obs_qsv11_v2", "obs_nvenc_h264_tex", "obs_x264"});
}

TEST_CASE("replay candidates with a single vendor")
{
	CHECK(candidates(combined({intel(), x264()}), Vendor::Intel) == Ids{"obs_qsv11_v2", "obs_x264"});
	CHECK(candidates(combined({intel(), x264()}), Vendor::Intel, true) ==
	      Ids{"obs_qsv11_hevc", "obs_qsv11_v2", "obs_x264"});
	CHECK(candidates(combined({{apple()}, x264()}), Vendor::Apple) == Ids{apple().id, "obs_x264"});
	CHECK(candidates(x264(), Vendor::Unknown, true) == Ids{"obs_x264"});
	CHECK(candidates({}, Vendor::Nvidia).empty());
}

TEST_CASE("replay candidates keep the fixed vendor order when the render vendor is unknown")
{
	const std::vector<EncoderInfo> encoders = combined({x264(), amd(), {apple()}, intel(), nvidia()});
	const Ids expected = {"obs_nvenc_h264_tex", "obs_qsv11_v2", "h264_texture_amf", apple().id, "obs_x264"};
	CHECK(candidates(encoders, Vendor::Unknown) == expected);
	CHECK(candidates(encoders, Vendor::Software) == expected);
}

TEST_CASE("a chosen encoder goes first, and other adapters can be left out")
{
	const std::vector<EncoderInfo> encoders = combined({x264(), intel(), nvidia()});
	const auto order = [&](const EncoderPreferences &preferences) {
		Ids ids;
		for (const EncoderInfo &info :
		     tapeloop::replayEncoderCandidates(encoders, Vendor::Nvidia, preferences)) {
			ids.push_back(info.id);
		}
		return ids;
	};
	EncoderPreferences preferences;
	preferences.preferHevc = false;
	// The choice comes first, and the automatic order follows it without it.
	preferences.chosen = "obs_qsv11_v2";
	CHECK(order(preferences) == Ids{"obs_qsv11_v2", "obs_nvenc_h264_tex", "obs_x264"});
	preferences.chosen = "obs_x264";
	CHECK(order(preferences) == Ids{"obs_x264", "obs_nvenc_h264_tex", "obs_qsv11_v2"});
	// Even an HEVC encoder the H.264 setting would leave out, since the user chose it.
	preferences.chosen = "obs_nvenc_hevc_tex";
	CHECK(order(preferences) == Ids{"obs_nvenc_hevc_tex", "obs_nvenc_h264_tex", "obs_qsv11_v2", "obs_x264"});
	// A choice that is not there, or that a replay cannot hold, changes nothing.
	preferences.chosen = "obs_qsv11_av1";
	CHECK(order(preferences) == Ids{"obs_nvenc_h264_tex", "obs_qsv11_v2", "obs_x264"});
	preferences.chosen = "obs_nvenc_av1_tex";
	CHECK(order(preferences) == Ids{"obs_nvenc_h264_tex", "obs_qsv11_v2", "obs_x264"});

	// Without other adapters, the render adapter's encoders go straight to x264.
	preferences.chosen.clear();
	preferences.otherAdapters = false;
	CHECK(order(preferences) == Ids{"obs_nvenc_h264_tex", "obs_x264"});
	// A choice on another adapter is still the user's.
	preferences.chosen = "obs_qsv11_v2";
	CHECK(order(preferences) == Ids{"obs_qsv11_v2", "obs_nvenc_h264_tex", "obs_x264"});

	// With the render adapter's vendor unknown, nothing counts as another adapter.
	preferences.chosen.clear();
	Ids unknown;
	for (const EncoderInfo &info : tapeloop::replayEncoderCandidates(encoders, Vendor::Unknown, preferences)) {
		unknown.push_back(info.id);
	}
	CHECK(unknown == Ids{"obs_nvenc_h264_tex", "obs_qsv11_v2", "obs_x264"});
}

TEST_CASE("a hidden or deprecated choice, and a software render vendor, leave the order alone")
{
	const std::vector<EncoderInfo> encoders = combined({x264(), intel(), nvidia()});
	const auto order = [&](Vendor render, const EncoderPreferences &preferences) {
		Ids ids;
		for (const EncoderInfo &info : tapeloop::replayEncoderCandidates(encoders, render, preferences)) {
			ids.push_back(info.id);
		}
		return ids;
	};
	EncoderPreferences preferences;
	preferences.preferHevc = false;
	const Ids automatic = {"obs_nvenc_h264_tex", "obs_qsv11_v2", "obs_x264"};
	preferences.chosen = "jim_nvenc";
	CHECK(order(Vendor::Nvidia, preferences) == automatic);
	preferences.chosen = "obs_nvenc_h264_soft";
	CHECK(order(Vendor::Nvidia, preferences) == automatic);

	// A Software render vendor is no adapter an encoder could share, so no hardware
	// encoder counts as another adapter's. Adapters that render in software come out as
	// Unknown, which the test above covers.
	preferences.chosen.clear();
	preferences.otherAdapters = false;
	CHECK(order(Vendor::Software, preferences) == automatic);
}

TEST_CASE("the encoders to choose from are those whose replays the buffer can hold")
{
	std::vector<EncoderInfo> encoders = combined({x264(), intel(), nvidia()});
	encoders.push_back(encoder("obs_qsv11_av1", "av1", Vendor::Intel, true));
	encoders.push_back(encoder("obs_nvenc_h264_soft", "h264", Vendor::Nvidia, false, true));
	encoders.push_back(encoder("obs_qsv11", "h264", Vendor::Intel, false, false, true));
	// An encoder of a vendor the settings know nothing of could add B-frames.
	encoders.push_back(encoder("plugin_h264", "h264", Vendor::Unknown, true));
	encoders.push_back(encoder("plugin_hevc", "hevc", Vendor::Unknown, false));
	Ids ids;
	for (const EncoderInfo &info : tapeloop::replayEncoderChoices(encoders)) {
		ids.push_back(info.id);
	}
	CHECK(ids == Ids{"obs_x264", "obs_qsv11_v2", "obs_qsv11_hevc", "obs_nvenc_h264_tex", "obs_nvenc_hevc_tex"});

	// Chosen anyway, as a choice saved by an earlier version, it gets the automatic order.
	EncoderPreferences preferences;
	preferences.chosen = "plugin_h264";
	CHECK(tapeloop::replayEncoderCandidates(encoders, Vendor::Nvidia, preferences).front().id ==
	      "obs_nvenc_hevc_tex");
}

TEST_CASE("after its HEVC encoder fails a source tries the same vendor's H.264 first")
{
	const std::vector<EncoderInfo> encoders = combined({x264(), intel(), nvidia(), amd()});
	const auto after = [&](const std::vector<EncoderInfo> &before, const char *failed) {
		Ids ids;
		for (const EncoderInfo &info : tapeloop::candidatesAfterHevcFailure(before, encoders, failed)) {
			ids.push_back(info.id);
		}
		return ids;
	};
	const auto order = [&](Vendor render, const EncoderPreferences &preferences) {
		return tapeloop::replayEncoderCandidates(encoders, render, preferences);
	};
	EncoderPreferences preferences;

	// In the automatic order the vendor's H.264 comes right after its HEVC.
	CHECK(after(order(Vendor::Nvidia, preferences), "obs_nvenc_hevc_tex") ==
	      Ids{"obs_nvenc_h264_tex", "obs_qsv11_hevc", "obs_qsv11_v2", "h265_texture_amf", "h264_texture_amf",
		  "obs_x264"});
	CHECK(after(order(Vendor::Amd, preferences), "h265_texture_amf") ==
	      Ids{"h264_texture_amf", "obs_nvenc_hevc_tex", "obs_nvenc_h264_tex", "obs_qsv11_hevc", "obs_qsv11_v2",
		  "obs_x264"});

	// A chosen encoder of another adapter: its vendor's H.264 is one a user may choose,
	// although the automatic order leaves it out.
	preferences.chosen = "obs_nvenc_hevc_tex";
	preferences.otherAdapters = false;
	const std::vector<EncoderInfo> chosen = order(Vendor::Intel, preferences);
	REQUIRE(chosen.front().id == "obs_nvenc_hevc_tex");
	CHECK(after(chosen, "obs_nvenc_hevc_tex") ==
	      Ids{"obs_nvenc_h264_tex", "obs_qsv11_hevc", "obs_qsv11_v2", "obs_x264"});

	// A vendor with no H.264 to choose only loses the encoder that failed.
	const std::vector<EncoderInfo> hevcOnly = {encoder("obs_nvenc_hevc_tex", "hevc", Vendor::Nvidia, true),
						   x264()[0]};
	const std::vector<EncoderInfo> left =
		tapeloop::candidatesAfterHevcFailure(hevcOnly, hevcOnly, "obs_nvenc_hevc_tex");
	REQUIRE(left.size() == 1);
	CHECK(left[0].id == "obs_x264");
	CHECK(tapeloop::candidatesAfterHevcFailure({}, {}, "obs_qsv11_hevc").empty());
}

TEST_CASE("replay candidates fall back to H.264 on a vendor without HEVC")
{
	std::vector<EncoderInfo> h264Only = nvidia();
	std::erase_if(h264Only, [](const EncoderInfo &info) { return info.codec != "h264"; });
	const std::vector<EncoderInfo> encoders = combined({h264Only, intel(), x264()});

	const Ids expected = {"obs_nvenc_h264_tex", "obs_qsv11_hevc", "obs_qsv11_v2", "obs_x264"};
	CHECK(candidates(encoders, Vendor::Nvidia, true) == expected);
	CHECK(candidates(encoders, Vendor::Unknown, true) == expected);
}

TEST_CASE("replay candidates do not depend on the order encoders are listed in")
{
	std::vector<EncoderInfo> all = combined({nvidia(), intel(), amd(), x264()});
	const Ids expected = candidates(all, Vendor::Intel, true);
	CHECK(expected == Ids{"obs_qsv11_hevc", "obs_qsv11_v2", "obs_nvenc_hevc_tex", "obs_nvenc_h264_tex",
			      "h265_texture_amf", "h264_texture_amf", "obs_x264"});
	std::reverse(all.begin(), all.end());
	CHECK(candidates(all, Vendor::Intel, true) == expected);
	std::rotate(all.begin(), all.begin() + 5, all.end());
	CHECK(candidates(all, Vendor::Intel, true) == expected);
}

TEST_CASE("replay candidates leave out deprecated, internal, unknown and AV1 encoders")
{
	const std::vector<EncoderInfo> leftovers = {
		encoder("jim_nvenc", "h264", Vendor::Nvidia, true, true),
		encoder("obs_nvenc_h264_soft", "h264", Vendor::Nvidia, false, false, true),
		encoder("obs_qsv11", "h264", Vendor::Intel, true, true),
		encoder("ffmpeg_vaapi_tex", "h264", Vendor::Unknown, true),
		encoder("obs_nvenc_av1_tex", "av1", Vendor::Nvidia, true),
	};
	CHECK(candidates(leftovers, Vendor::Nvidia, true).empty());
	CHECK(candidates(combined({leftovers, x264()}), Vendor::Nvidia, true) == Ids{"obs_x264"});
}

TEST_CASE("replay candidates put non-texture hardware encoders after texture ones")
{
	const std::vector<EncoderInfo> encoders = combined({
		{encoder("future_nvenc_cpu", "h264", Vendor::Nvidia, false)},
		{encoder("future_qsv_cpu", "h264", Vendor::Intel, false)},
		x264(),
		intel(),
		nvidia(),
	});
	CHECK(candidates(encoders, Vendor::Nvidia) ==
	      Ids{"obs_nvenc_h264_tex", "future_nvenc_cpu", "obs_qsv11_v2", "future_qsv_cpu", "obs_x264"});
	CHECK(candidates(encoders, Vendor::Intel) ==
	      Ids{"obs_qsv11_v2", "future_qsv_cpu", "obs_nvenc_h264_tex", "future_nvenc_cpu", "obs_x264"});
}

TEST_CASE("replay candidates need texture support from NVIDIA, Intel and AMD when the render vendor is unknown")
{
	const std::vector<EncoderInfo> encoders = {
		encoder("future_nvenc_cpu", "h264", Vendor::Nvidia, false),
		encoder("future_qsv_cpu", "h264", Vendor::Intel, false),
		encoder("future_amf_cpu", "h264", Vendor::Amd, false),
		encoder("future_qsv_internal", "h264", Vendor::Intel, true, false, true),
		encoder("obs_x264", "h264", Vendor::Software, false),
	};
	CHECK(candidates(encoders, Vendor::Unknown) == Ids{"obs_x264"});
	CHECK(candidates(encoders, Vendor::Software) == Ids{"obs_x264"});
	CHECK(candidates(encoders, Vendor::Amd) ==
	      Ids{"future_amf_cpu", "future_nvenc_cpu", "future_qsv_cpu", "obs_x264"});
}

TEST_CASE("replay candidates put HEVC before H.264 within each vendor")
{
	const std::vector<EncoderInfo> encoders = combined({x264(), amd(), intel(), nvidia()});
	CHECK(candidates(encoders, Vendor::Unknown, true) == Ids{"obs_nvenc_hevc_tex", "obs_nvenc_h264_tex",
								 "obs_qsv11_hevc", "obs_qsv11_v2", "h265_texture_amf",
								 "h264_texture_amf", "obs_x264"});

	const EncoderInfo appleHevc =
		encoder("com.apple.videotoolbox.videoencoder.ave.hevc", "hevc", Vendor::Apple, false);
	CHECK(candidates(combined({x264(), {apple(), appleHevc}}), Vendor::Apple, true) ==
	      Ids{appleHevc.id, apple().id, "obs_x264"});
	CHECK(candidates(combined({x264(), {apple(), appleHevc}}), Vendor::Apple) == Ids{apple().id, "obs_x264"});
}

TEST_CASE("replay candidates keep equal encoders in the order listed")
{
	const EncoderInfo first = encoder("first_tex", "h264", Vendor::Nvidia, true);
	const EncoderInfo second = encoder("second_tex", "h264", Vendor::Nvidia, true);
	CHECK(candidates({first, second}, Vendor::Nvidia) == Ids{"first_tex", "second_tex"});
	CHECK(candidates({second, first}, Vendor::Nvidia) == Ids{"second_tex", "first_tex"});
}

TEST_CASE("encoder vendors come from the ids OBS registers")
{
	for (const auto &group : {nvidia(), intel(), amd(), x264()}) {
		for (const EncoderInfo &info : group) {
			CAPTURE(info.id);
			CHECK(tapeloop::encoderVendor(info.id) == info.vendor);
		}
	}
	CHECK(tapeloop::encoderVendor("jim_av1_nvenc") == Vendor::Nvidia);
	CHECK(tapeloop::encoderVendor("ffmpeg_nvenc") == Vendor::Nvidia);
	CHECK(tapeloop::encoderVendor("obs_qsv11_av1") == Vendor::Intel);
	CHECK(tapeloop::encoderVendor("av1_texture_amf") == Vendor::Amd);
	CHECK(tapeloop::encoderVendor(apple().id) == Vendor::Apple);
	CHECK(tapeloop::encoderVendor("ffmpeg_vaapi_tex") == Vendor::Unknown);
	CHECK(tapeloop::encoderVendor("obs_x264_extra") == Vendor::Unknown);
	CHECK(tapeloop::encoderVendor("") == Vendor::Unknown);
}

TEST_CASE("adapter vendors come from the driver's name")
{
	CHECK(tapeloop::adapterVendor("NVIDIA GeForce RTX 3060") == Vendor::Nvidia);
	CHECK(tapeloop::adapterVendor("AMD Radeon RX 7800 XT") == Vendor::Amd);
	CHECK(tapeloop::adapterVendor("Radeon RX 580 Series") == Vendor::Amd);
	CHECK(tapeloop::adapterVendor("Intel(R) UHD Graphics 770") == Vendor::Intel);
	CHECK(tapeloop::adapterVendor("Apple M1 Pro") == Vendor::Apple);
	CHECK(tapeloop::adapterVendor("Mesa llvmpipe (LLVM 21.1.8, 256 bits)") == Vendor::Unknown);
	CHECK(tapeloop::adapterVendor("Microsoft Basic Render Driver") == Vendor::Unknown);
	CHECK(tapeloop::adapterVendor("") == Vendor::Unknown);
}

TEST_CASE("replay bitrate scales with pixel rate and is capped")
{
	ReplayEncoderParams params;
	CHECK(tapeloop::replayBitrateKbps(params, "h264") == 30'000);

	params.width = 1280;
	params.height = 720;
	CHECK(tapeloop::replayBitrateKbps(params, "h264") == 13'333);

	params.width = 1920;
	params.height = 1080;
	params.frameDuration = {1001, 60000};
	CHECK(tapeloop::replayBitrateKbps(params, "h264") == 29'970);
	params.frameDuration = {1, 30};
	CHECK(tapeloop::replayBitrateKbps(params, "h264") == 15'000);

	params.width = 3840;
	params.height = 2160;
	params.frameDuration = {1, 60};
	CHECK(tapeloop::replayBitrateKbps(params, "h264") == 100'000);
	params.maxBitrateKbps = 150'000;
	CHECK(tapeloop::replayBitrateKbps(params, "h264") == 120'000);

	params.width = 1;
	params.height = 1;
	CHECK(tapeloop::replayBitrateKbps(params, "h264") == 1);
	params.width = -5;
	CHECK(tapeloop::replayBitrateKbps(params, "h264") == 1);
}

TEST_CASE("HEVC gets a share of the H.264 bitrate")
{
	ReplayEncoderParams params;
	CHECK(tapeloop::replayBitrateKbps(params, "hevc") == 18'000);
	params.width = 1280;
	params.height = 720;
	CHECK(tapeloop::replayBitrateKbps(params, "hevc") == 8'000);

	params.hevcBitratePercent = 50;
	CHECK(tapeloop::replayBitrateKbps(params, "hevc") == 6'667);
	CHECK(tapeloop::replayBitrateKbps(params, "h264") == 13'333);
	params.hevcBitratePercent = 0;
	CHECK(tapeloop::replayBitrateKbps(params, "hevc") == 133);
	params.hevcBitratePercent = 250;
	CHECK(tapeloop::replayBitrateKbps(params, "hevc") == 13'333);

	// The cap holds whatever the codec.
	params.width = 7680;
	params.height = 4320;
	params.hevcBitratePercent = 60;
	CHECK(tapeloop::replayBitrateKbps(params, "hevc") == 100'000);

	// The byte budget follows the bitrate.
	CHECK(tapeloop::replayByteBudget(18'000, std::chrono::seconds(60)) * 30 ==
	      tapeloop::replayByteBudget(30'000, std::chrono::seconds(60)) * 18);
}

TEST_CASE("an encoder's path is the optimal one only when it takes OBS's textures on OBS's adapter")
{
	using tapeloop::EncoderPath;
	using tapeloop::encoderPathOf;
	const EncoderInfo nvenc = encoder("obs_nvenc_hevc_tex", "hevc", Vendor::Nvidia, true);
	const EncoderInfo qsv = encoder("obs_qsv11_v2", "h264", Vendor::Intel, true);
	const EncoderInfo amf = encoder("h264_texture_amf", "h264", Vendor::Amd, true);
	CHECK(encoderPathOf(nvenc, Vendor::Nvidia, true) == EncoderPath::Texture);
	CHECK(encoderPathOf(amf, Vendor::Amd, true) == EncoderPath::Texture);
	// QuickSync while OBS renders on NVIDIA: the iGPU of the match PC, reading back.
	CHECK(encoderPathOf(qsv, Vendor::Nvidia, true) == EncoderPath::Readback);
	CHECK(encoderPathOf(qsv, Vendor::Intel, true) == EncoderPath::Texture);
	// Without NV12 textures every encoder reads back.
	CHECK(encoderPathOf(nvenc, Vendor::Nvidia, false) == EncoderPath::Readback);
	CHECK(encoderPathOf(encoder("obs_qsv11_soft", "h264", Vendor::Intel, false), Vendor::Intel, true) ==
	      EncoderPath::Readback);
	// An adapter of unknown vendor says nothing about where the encoder runs.
	CHECK(encoderPathOf(nvenc, Vendor::Unknown, true) == EncoderPath::Texture);
	// VideoToolbox on a Mac is the best path there is, without textures.
	CHECK(encoderPathOf(encoder("com.apple.videotoolbox.videoencoder.ave.avc", "h264", Vendor::Apple, false),
			    Vendor::Apple, false) == EncoderPath::Texture);
	// On another adapter, as a Mac with a discrete card OBS does not render on.
	CHECK(encoderPathOf(encoder("com.apple.videotoolbox.videoencoder.ave.avc", "h264", Vendor::Apple, false),
			    Vendor::Amd, true) == EncoderPath::Readback);
	CHECK(encoderPathOf(encoder("obs_x264", "h264", Vendor::Software, false), Vendor::Nvidia, true) ==
	      EncoderPath::Software);
}

TEST_CASE("replay encoders prefer HEVC unless told otherwise, with H.264 of the same vendor after it")
{
	CHECK(EncoderPreferences{}.preferHevc);
	const std::vector<EncoderInfo> encoders = {
		encoder("obs_nvenc_h264_tex", "h264", Vendor::Nvidia, true),
		encoder("obs_nvenc_hevc_tex", "hevc", Vendor::Nvidia, true),
		encoder("obs_x264", "h264", Vendor::Software, false),
	};
	Ids preferred;
	for (const EncoderInfo &info : tapeloop::replayEncoderCandidates(encoders, Vendor::Nvidia, {})) {
		preferred.push_back(info.id);
	}
	CHECK(preferred == Ids{"obs_nvenc_hevc_tex", "obs_nvenc_h264_tex", "obs_x264"});
	CHECK(candidates(encoders, Vendor::Nvidia, false) == Ids{"obs_nvenc_h264_tex", "obs_x264"});
}

TEST_CASE("replay byte budget is the bitrate over the length plus half")
{
	CHECK(tapeloop::replayByteBudget(30'000, 60s) == 337'500'000);
	CHECK(tapeloop::replayByteBudget(8'000, 10s) == 15'000'000);
	CHECK(tapeloop::replayByteBudget(30'000, 0s) == 0);
	CHECK(tapeloop::replayByteBudget(30'000, -5s) == 0);
	CHECK(tapeloop::replayByteBudget(0, 1s) == 188);
	CHECK(tapeloop::replayByteBudget(-10, 1s) == 188);
	CHECK(tapeloop::replayByteBudget(int64_t{1} << 40, 1s) ==
	      static_cast<size_t>(std::numeric_limits<int32_t>::max() / 3 * int64_t{1000} * 3 / 16));
	CHECK(tapeloop::replayByteBudget(100'000, Nanoseconds::max()) == 172'938'225'691'027'046);
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
			{"repeat_headers", true},
			{"opts", std::string("keyint=30")},
		};
		CHECK(settingsFor("obs_nvenc_h264_tex", "h264", Vendor::Nvidia) == expected);
		EncoderSettings hevc = expected;
		hevc["bitrate"] = int64_t{17'982};
		CHECK(settingsFor("obs_nvenc_hevc_tex", "hevc", Vendor::Nvidia) == hevc);
	}

	SECTION("QuickSync")
	{
		const EncoderSettings expected = {
			{"rate_control", std::string("CBR")},
			{"bitrate", int64_t{29'970}},
			{"keyint_sec", int64_t{1}},
			{"bframes", int64_t{0}},
			{"target_usage", std::string("TU7")},
			{"latency", std::string("ultra-low")},
			{"repeat_headers", true},
		};
		CHECK(settingsFor("obs_qsv11_v2", "h264", Vendor::Intel) == expected);
		EncoderSettings hevc = expected;
		hevc["bitrate"] = int64_t{17'982};
		CHECK(settingsFor("obs_qsv11_hevc", "hevc", Vendor::Intel) == hevc);
	}

	SECTION("AMF")
	{
		const EncoderSettings h264 = {
			{"rate_control", std::string("CBR")},
			{"bitrate", int64_t{29'970}},
			{"keyint_sec", int64_t{1}},
			{"bf", int64_t{0}},
			{"preset", std::string("speed")},
			{"ffmpeg_opts", std::string("keyint=30 header_spacing=30")},
		};
		CHECK(settingsFor("h264_texture_amf", "h264", Vendor::Amd) == h264);

		const EncoderSettings hevc = {
			{"rate_control", std::string("CBR")},
			{"bitrate", int64_t{17'982}},
			{"keyint_sec", int64_t{1}},
			{"preset", std::string("speed")},
			{"ffmpeg_opts", std::string("HevcGOPSize=30 gops_per_idr=1 header_insertion_mode=idr")},
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
		EncoderSettings hevc = expected;
		hevc["bitrate"] = int64_t{17'982};
		CHECK(settingsFor("com.apple.videotoolbox.videoencoder.ave.hevc", "hevc", Vendor::Apple) == hevc);
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
			{"repeat_headers", true},
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

	const EncoderSettings amfH264 =
		tapeloop::buildReplaySettings(encoder("h264_texture_amf", "h264", Vendor::Amd, true), params);
	CHECK(amfH264.at("ffmpeg_opts") == tapeloop::SettingValue(std::string("keyint=15 header_spacing=15")));
	const EncoderSettings amfHevc =
		tapeloop::buildReplaySettings(encoder("h265_texture_amf", "hevc", Vendor::Amd, true), params);
	CHECK(amfHevc.at("ffmpeg_opts") ==
	      tapeloop::SettingValue(std::string("HevcGOPSize=15 gops_per_idr=1 header_insertion_mode=idr")));
}

TEST_CASE("replay settings keep the QSV bitrate within 16 bits")
{
	ReplayEncoderParams params;
	params.width = 3840;
	params.height = 2160;
	const EncoderSettings intel =
		tapeloop::buildReplaySettings(encoder("obs_qsv11_v2", "h264", Vendor::Intel, true), params);
	CHECK(intel.at("bitrate") == tapeloop::SettingValue(int64_t{50'000}));
	const EncoderSettings intelHevc =
		tapeloop::buildReplaySettings(encoder("obs_qsv11_hevc", "hevc", Vendor::Intel, true), params);
	CHECK(intelHevc.at("bitrate") == tapeloop::SettingValue(int64_t{50'000}));

	const EncoderSettings nvidia =
		tapeloop::buildReplaySettings(encoder("obs_nvenc_h264_tex", "h264", Vendor::Nvidia, true), params);
	CHECK(nvidia.at("bitrate") == tapeloop::SettingValue(int64_t{100'000}));
	const EncoderSettings nvidiaHevc =
		tapeloop::buildReplaySettings(encoder("obs_nvenc_hevc_tex", "hevc", Vendor::Nvidia, true), params);
	CHECK(nvidiaHevc.at("bitrate") == tapeloop::SettingValue(int64_t{72'000}));
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
