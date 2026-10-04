// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "obs/ObsEncoders.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <variant>

namespace tapeloop::obs {
namespace {

struct AdapterSearch {
	uint32_t index = 0;
	std::array<char, 256> name{};
};

bool matchAdapter(void *param, const char *name, uint32_t id) noexcept
{
	auto &search = *static_cast<AdapterSearch *>(param);
	if (id != search.index || !name) {
		return true;
	}
	std::strncpy(search.name.data(), name, search.name.size() - 1);
	return false;
}

} // namespace

std::vector<EncoderInfo> registeredVideoEncoders()
{
	std::vector<EncoderInfo> encoders;
	const char *id = nullptr;
	for (size_t i = 0; obs_enum_encoder_types(i, &id); ++i) {
		if (!id || obs_get_encoder_type(id) != OBS_ENCODER_VIDEO) {
			continue;
		}
		const uint32_t caps = obs_get_encoder_caps(id);
		const char *codec = obs_get_encoder_codec(id);
		EncoderInfo info;
		info.id = id;
		info.codec = codec ? codec : "";
		info.vendor = encoderVendor(id);
		info.passTexture = (caps & OBS_ENCODER_CAP_PASS_TEXTURE) != 0;
		info.deprecated = (caps & OBS_ENCODER_CAP_DEPRECATED) != 0;
		info.internal = (caps & OBS_ENCODER_CAP_INTERNAL) != 0;
		encoders.push_back(std::move(info));
	}
	return encoders;
}

Vendor renderAdapterVendor()
{
	obs_video_info video = {};
	if (!obs_get_video_info(&video)) {
		return Vendor::Unknown;
	}

	// Direct3D 11 names its adapters. Other backends list a placeholder there, while
	// OpenGL's renderer string names the GPU.
	AdapterSearch search;
	search.index = video.adapter;
	obs_enter_graphics();
	if (gs_get_device_type() == GS_DEVICE_DIRECT3D_11) {
		gs_enum_adapters(matchAdapter, &search);
	} else if (const char *renderer = gs_get_renderer()) {
		std::strncpy(search.name.data(), renderer, search.name.size() - 1);
	}
	obs_leave_graphics();
	return adapterVendor(search.name.data());
}

obs_data_t *createEncoderSettings(const EncoderSettings &settings)
{
	obs_data_t *data = obs_data_create();
	for (const auto &[key, value] : settings) {
		if (const int64_t *number = std::get_if<int64_t>(&value)) {
			obs_data_set_int(data, key.c_str(), *number);
		} else if (const bool *flag = std::get_if<bool>(&value)) {
			obs_data_set_bool(data, key.c_str(), *flag);
		} else if (const std::string *text = std::get_if<std::string>(&value)) {
			obs_data_set_string(data, key.c_str(), text->c_str());
		}
	}
	return data;
}

} // namespace tapeloop::obs
