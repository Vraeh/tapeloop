// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "obs/SettingsData.hpp"

#include <obs.hpp>

#include <string>

namespace tapeloop::obs {

obs_data_t *createSettingsData(const SavedSettings &saved)
{
	OBSDataAutoRelease data = obs_data_create();
	obs_data_set_int(data, "version", saved.version);
	obs_data_set_int(data, "length", saved.lengthSeconds);
	obs_data_set_string(data, "resolution", saved.resolution.c_str());
	obs_data_set_int(data, "height", saved.height);
	obs_data_set_bool(data, "start_with_outputs", saved.startWithOutputs);
	obs_data_set_bool(data, "activate_offair", saved.activateOffAir);
	obs_data_set_bool(data, "force_h264", saved.forceH264);
	obs_data_set_string(data, "replay_encoder", saved.replayEncoder.c_str());
	obs_data_set_bool(data, "allow_other_adapters", saved.allowOtherAdapters);
	obs_data_set_bool(data, "show_advanced", saved.showAdvanced);
	obs_data_set_int(data, "buffer_memory_mib", saved.bufferMemoryMiB);

	OBSDataArrayAutoRelease sources = obs_data_array_create();
	for (const SavedSource &source : saved.sources) {
		OBSDataAutoRelease entry = obs_data_create();
		obs_data_set_string(entry, "uuid", source.uuid.c_str());
		if (!source.name.empty()) {
			obs_data_set_string(entry, "name", source.name.c_str());
		}
		obs_data_set_bool(entry, "selected", source.selected);
		if (source.lengthSeconds) {
			obs_data_set_int(entry, "length", *source.lengthSeconds);
		}
		if (source.resolution) {
			obs_data_set_string(entry, "resolution", source.resolution->c_str());
		}
		if (source.height) {
			obs_data_set_int(entry, "height", *source.height);
		}
		if (source.activateOffAir) {
			obs_data_set_bool(entry, "activate_offair", *source.activateOffAir);
		}
		obs_data_array_push_back(sources, entry);
	}
	obs_data_set_array(data, "sources", sources);
	obs_data_addref(data);
	return data;
}

SavedSettings readSettingsData(obs_data_t *data)
{
	SavedSettings saved;
	saved.version = obs_data_get_int(data, "version");
	saved.lengthSeconds = obs_data_get_int(data, "length");
	saved.resolution = obs_data_get_string(data, "resolution");
	saved.height = obs_data_get_int(data, "height");
	saved.startWithOutputs = obs_data_get_bool(data, "start_with_outputs");
	saved.activateOffAir = obs_data_get_bool(data, "activate_offair");
	saved.forceH264 = obs_data_get_bool(data, "force_h264");
	saved.replayEncoder = obs_data_get_string(data, "replay_encoder");
	// Settings saved before this key existed allowed other adapters.
	saved.allowOtherAdapters = !obs_data_has_user_value(data, "allow_other_adapters") ||
				   obs_data_get_bool(data, "allow_other_adapters");
	saved.showAdvanced = obs_data_get_bool(data, "show_advanced");
	saved.bufferMemoryMiB = obs_data_get_int(data, "buffer_memory_mib");

	OBSDataArrayAutoRelease sources = obs_data_get_array(data, "sources");
	for (size_t i = 0; i < obs_data_array_count(sources); ++i) {
		OBSDataAutoRelease entry = obs_data_array_item(sources, i);
		SavedSource source;
		source.uuid = obs_data_get_string(entry, "uuid");
		source.name = obs_data_get_string(entry, "name");
		source.selected = obs_data_get_bool(entry, "selected");
		if (obs_data_has_user_value(entry, "length")) {
			source.lengthSeconds = obs_data_get_int(entry, "length");
		}
		if (obs_data_has_user_value(entry, "resolution")) {
			source.resolution = obs_data_get_string(entry, "resolution");
		}
		if (obs_data_has_user_value(entry, "height")) {
			source.height = obs_data_get_int(entry, "height");
		}
		if (obs_data_has_user_value(entry, "activate_offair")) {
			source.activateOffAir = obs_data_get_bool(entry, "activate_offair");
		}
		saved.sources.push_back(std::move(source));
	}
	return saved;
}

} // namespace tapeloop::obs
