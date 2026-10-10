// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "../core/TempDirectory.hpp"
#include "ObsFixture.hpp"

#include "obs/ProfileHotkeys.hpp"

#include <catch2/catch_test_macros.hpp>
#include <obs.hpp>
#include <util/config-file.h>

#include <memory>
#include <string>

using tapeloop::test::ObsFixture;

namespace {

constexpr const char *kName = "tapeloop.test_hotkey";

struct ConfigCloser {
	void operator()(config_t *config) const noexcept { config_close(config); }
};
using Config = std::unique_ptr<config_t, ConfigCloser>;

// A profile's configuration, as OBS's settings write a frontend hotkey's bindings into it.
Config profileBinding(const std::string &path, const char *key)
{
	Config profile(config_create(path.c_str()));
	REQUIRE(profile);
	OBSDataArrayAutoRelease bindings = obs_data_array_create();
	OBSDataAutoRelease binding = obs_data_create();
	obs_data_set_string(binding, "key", key);
	obs_data_array_push_back(bindings, binding);
	OBSDataAutoRelease saved = obs_data_create();
	obs_data_set_array(saved, "bindings", bindings);
	config_set_string(profile.get(), "Hotkeys", kName, obs_data_get_json(saved));
	return profile;
}

std::string boundKey(obs_hotkey_id id)
{
	OBSDataArrayAutoRelease bindings = obs_hotkey_save(id);
	if (obs_data_array_count(bindings) != 1) {
		return {};
	}
	OBSDataAutoRelease binding = obs_data_array_item(bindings, 0);
	return obs_data_get_string(binding, "key");
}

} // namespace

TEST_CASE_METHOD(ObsFixture, "a frontend hotkey takes its bindings from the profile", "[obs][hotkey]")
{
	tapeloop::test::TempDirectory dir;
	const obs_hotkey_id id = obs_hotkey_register_frontend(
		kName, "Test hotkey", [](void *, obs_hotkey_id, obs_hotkey_t *, bool) {}, nullptr);
	REQUIRE(id != OBS_INVALID_HOTKEY_ID);

	const Config first = profileBinding((dir.path() / "first.ini").string(), "OBS_KEY_F9");
	tapeloop::obs::loadProfileHotkey(id, kName, first.get());
	CHECK(boundKey(id) == "OBS_KEY_F9");

	// Another profile's bindings replace them.
	const Config second = profileBinding((dir.path() / "second.ini").string(), "OBS_KEY_F10");
	tapeloop::obs::loadProfileHotkey(id, kName, second.get());
	CHECK(boundKey(id) == "OBS_KEY_F10");

	// A profile without them, or a damaged entry, leaves the hotkey unbound.
	const Config empty(config_create((dir.path() / "empty.ini").string().c_str()));
	REQUIRE(empty);
	tapeloop::obs::loadProfileHotkey(id, kName, empty.get());
	OBSDataArrayAutoRelease none = obs_hotkey_save(id);
	CHECK(obs_data_array_count(none) == 0);
	tapeloop::obs::loadProfileHotkey(id, kName, first.get());
	config_set_string(first.get(), "Hotkeys", kName, "{not json");
	tapeloop::obs::loadProfileHotkey(id, kName, first.get());
	OBSDataArrayAutoRelease damaged = obs_hotkey_save(id);
	CHECK(obs_data_array_count(damaged) == 0);
	tapeloop::obs::loadProfileHotkey(id, kName, nullptr);
	CHECK(boundKey(id).empty());

	obs_hotkey_unregister(id);
}
