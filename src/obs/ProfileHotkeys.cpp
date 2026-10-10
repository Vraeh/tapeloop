// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "obs/ProfileHotkeys.hpp"

#include <obs.hpp>

namespace tapeloop::obs {

void loadProfileHotkey(obs_hotkey_id id, const char *name, config_t *profile) noexcept
{
	const char *json = profile ? config_get_string(profile, "Hotkeys", name) : nullptr;
	OBSDataAutoRelease saved = json ? obs_data_create_from_json(json) : nullptr;
	OBSDataArrayAutoRelease bindings = saved ? obs_data_get_array(saved, "bindings") : nullptr;
	obs_hotkey_load(id, bindings);
}

} // namespace tapeloop::obs
