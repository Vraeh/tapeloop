// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "obs/SettingsData.hpp"

#include <obs.hpp>

#include <cmath>
#include <string>
#include <utility>

namespace tapeloop::obs {

namespace {

void setMedia(obs_data_t *data, const std::string &prefix, const SavedMedia &media)
{
	obs_data_set_string(data, prefix.c_str(), media.kind.c_str());
	obs_data_set_string(data, (prefix + "_file").c_str(), media.path.c_str());
	obs_data_set_int(data, (prefix + "_image_ms").c_str(), media.imageMs);
}

void setTransition(obs_data_t *data, const std::string &prefix, const SavedTransition &transition)
{
	obs_data_set_string(data, prefix.c_str(), transition.name.c_str());
	obs_data_set_int(data, (prefix + "_ms").c_str(), transition.durationMs);
}

// Settings saved before these keys existed keep the defaults of the saved structures.
void readString(obs_data_t *data, const std::string &key, std::string &value)
{
	if (obs_data_has_user_value(data, key.c_str())) {
		value = obs_data_get_string(data, key.c_str());
	}
}

void readInt(obs_data_t *data, const std::string &key, int64_t &value)
{
	if (obs_data_has_user_value(data, key.c_str())) {
		value = obs_data_get_int(data, key.c_str());
	}
}

void readMedia(obs_data_t *data, const std::string &prefix, SavedMedia &media)
{
	readString(data, prefix, media.kind);
	readString(data, prefix + "_file", media.path);
	readInt(data, prefix + "_image_ms", media.imageMs);
}

void readTransition(obs_data_t *data, const std::string &prefix, SavedTransition &transition)
{
	readString(data, prefix, transition.name);
	readInt(data, prefix + "_ms", transition.durationMs);
}

// Overlay values are whole numbers when saveOverlays() makes them, and read better as
// JSON integers.
void setNumber(obs_data_t *data, const char *key, double value)
{
	if (value >= -1e9 && value <= 1e9 && std::trunc(value) == value) {
		obs_data_set_int(data, key, static_cast<long long>(value));
	} else {
		obs_data_set_double(data, key, value);
	}
}

void setOverlays(obs_data_t *data, const SavedOverlays &overlays)
{
	OBSDataAutoRelease object = obs_data_create();
	obs_data_set_string(object, "mode", overlays.mode.c_str());
	OBSDataAutoRelease scene = obs_data_create();
	obs_data_set_string(scene, "uuid", overlays.sceneUuid.c_str());
	obs_data_set_string(scene, "name", overlays.sceneName.c_str());
	obs_data_set_obj(object, "scene", scene);
	OBSDataAutoRelease canvas = obs_data_create();
	setNumber(canvas, "width", overlays.canvasWidth);
	setNumber(canvas, "height", overlays.canvasHeight);
	obs_data_set_obj(object, "canvas", canvas);

	OBSDataArrayAutoRelease items = obs_data_array_create();
	for (const SavedOverlay &overlay : overlays.items) {
		OBSDataAutoRelease entry = obs_data_create();
		obs_data_set_string(entry, "uuid", overlay.uuid.c_str());
		obs_data_set_string(entry, "name", overlay.name.c_str());
		obs_data_set_bool(entry, "visible", overlay.visible);
		obs_data_set_string(entry, "anchor", overlay.anchor.c_str());
		setNumber(entry, "x", overlay.x);
		setNumber(entry, "y", overlay.y);
		setNumber(entry, "width", overlay.width);
		setNumber(entry, "height", overlay.height);
		obs_data_set_string(entry, "fit", overlay.fit.c_str());
		setNumber(entry, "opacity", overlay.opacity);
		obs_data_set_string(entry, "blend", overlay.blend.c_str());
		OBSDataAutoRelease crop = obs_data_create();
		setNumber(crop, "left", overlay.crop.left);
		setNumber(crop, "top", overlay.crop.top);
		setNumber(crop, "right", overlay.crop.right);
		setNumber(crop, "bottom", overlay.crop.bottom);
		obs_data_set_obj(entry, "crop", crop);
		obs_data_array_push_back(items, entry);
	}
	obs_data_set_array(object, "items", items);
	obs_data_set_obj(data, "overlays", object);
}

// The type of a key's value, or OBS_DATA_NULL when it has none. The overlays are read
// key by key and a value of another type, as a damaged or hand-edited file may hold,
// keeps the default of the saved structures.
obs_data_type typeOf(obs_data_t *data, const char *key)
{
	obs_data_item_t *item = obs_data_item_byname(data, key);
	if (!item) {
		return OBS_DATA_NULL;
	}
	const obs_data_type type = obs_data_item_has_user_value(item) ? obs_data_item_gettype(item) : OBS_DATA_NULL;
	obs_data_item_release(&item);
	return type;
}

void readText(obs_data_t *data, const char *key, std::string &value)
{
	if (typeOf(data, key) == OBS_DATA_STRING) {
		value = obs_data_get_string(data, key);
	}
}

void readNumber(obs_data_t *data, const char *key, double &value)
{
	if (typeOf(data, key) == OBS_DATA_NUMBER) {
		value = obs_data_get_double(data, key);
	}
}

void readFlag(obs_data_t *data, const char *key, bool &value)
{
	if (typeOf(data, key) == OBS_DATA_BOOLEAN) {
		value = obs_data_get_bool(data, key);
	}
}

// A whole number. A fraction, like a value of another type, keeps the default.
void readWhole(obs_data_t *data, const char *key, int64_t &value)
{
	obs_data_item_t *item = obs_data_item_byname(data, key);
	if (item && obs_data_item_has_user_value(item) && obs_data_item_gettype(item) == OBS_DATA_NUMBER &&
	    obs_data_item_numtype(item) == OBS_DATA_NUM_INT) {
		value = obs_data_item_get_int(item);
	}
	obs_data_item_release(&item);
}

obs_data_t *readObject(obs_data_t *data, const char *key)
{
	return typeOf(data, key) == OBS_DATA_OBJECT ? obs_data_get_obj(data, key) : nullptr;
}

SavedOverlays readOverlays(obs_data_t *object)
{
	SavedOverlays overlays;
	readText(object, "mode", overlays.mode);
	if (OBSDataAutoRelease scene = readObject(object, "scene")) {
		readText(scene, "uuid", overlays.sceneUuid);
		readText(scene, "name", overlays.sceneName);
	}
	if (OBSDataAutoRelease canvas = readObject(object, "canvas")) {
		readNumber(canvas, "width", overlays.canvasWidth);
		readNumber(canvas, "height", overlays.canvasHeight);
	}
	if (typeOf(object, "items") != OBS_DATA_ARRAY) {
		return overlays;
	}
	OBSDataArrayAutoRelease items = obs_data_get_array(object, "items");
	for (size_t i = 0; i < obs_data_array_count(items); ++i) {
		OBSDataAutoRelease entry = obs_data_array_item(items, i);
		if (!entry) {
			continue;
		}
		SavedOverlay overlay;
		readText(entry, "uuid", overlay.uuid);
		readText(entry, "name", overlay.name);
		readFlag(entry, "visible", overlay.visible);
		readText(entry, "anchor", overlay.anchor);
		readNumber(entry, "x", overlay.x);
		readNumber(entry, "y", overlay.y);
		readNumber(entry, "width", overlay.width);
		readNumber(entry, "height", overlay.height);
		readText(entry, "fit", overlay.fit);
		readNumber(entry, "opacity", overlay.opacity);
		readText(entry, "blend", overlay.blend);
		if (OBSDataAutoRelease crop = readObject(entry, "crop")) {
			readNumber(crop, "left", overlay.crop.left);
			readNumber(crop, "top", overlay.crop.top);
			readNumber(crop, "right", overlay.crop.right);
			readNumber(crop, "bottom", overlay.crop.bottom);
		}
		overlays.items.push_back(std::move(overlay));
	}
	return overlays;
}

} // namespace

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
	obs_data_set_string(data, "player_place", saved.playerPlace.c_str());
	obs_data_set_string(data, "player_shows", saved.playerShows.c_str());
	obs_data_set_string(data, "player_geometry", saved.playerGeometry.c_str());
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

	setMedia(data, "intro", saved.intro);
	setTransition(data, "intro_transition", saved.into);
	setMedia(data, "outro", saved.outro);
	setTransition(data, "back_transition", saved.back);
	obs_data_set_int(data, "air_channel", saved.airChannel);
	setOverlays(data, saved.overlays);
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
	saved.playerPlace = obs_data_get_string(data, "player_place");
	saved.playerShows = obs_data_get_string(data, "player_shows");
	saved.playerGeometry = obs_data_get_string(data, "player_geometry");
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

	readMedia(data, "intro", saved.intro);
	readTransition(data, "intro_transition", saved.into);
	readMedia(data, "outro", saved.outro);
	readTransition(data, "back_transition", saved.back);
	readWhole(data, "air_channel", saved.airChannel);
	if (OBSDataAutoRelease overlays = readObject(data, "overlays")) {
		saved.overlays = readOverlays(overlays);
	}
	return saved;
}

} // namespace tapeloop::obs
