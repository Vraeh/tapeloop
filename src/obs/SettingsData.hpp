// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/BufferSettings.hpp"

#include <obs.h>

namespace tapeloop::obs {

// The obs_data form of the saved settings, stored under kSettingsKey in the scene
// collection. Optional fields are left out when not set.
inline constexpr const char *kSettingsKey = "tapeloop";

// The caller owns the result.
obs_data_t *createSettingsData(const SavedSettings &saved);
SavedSettings readSettingsData(obs_data_t *data);

} // namespace tapeloop::obs
