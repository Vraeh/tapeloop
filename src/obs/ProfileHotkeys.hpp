// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include <obs.h>
#include <util/config-file.h>

namespace tapeloop::obs {

// Gives a hotkey registered with obs_hotkey_register_frontend the bindings a profile
// holds for it, none when it holds none. OBS's settings write the bindings of every such
// hotkey into the profile, section Hotkeys, under the hotkey's name, as JSON with the
// bindings under "bindings"; the frontend reads back only its own, when it starts and
// when the profile changes. UI thread.
void loadProfileHotkey(obs_hotkey_id id, const char *name, config_t *profile) noexcept;

} // namespace tapeloop::obs
