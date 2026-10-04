// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/ReplaySize.hpp"

#include <QString>

#include <functional>

class QComboBox;

namespace tapeloop::ui {

// Looks a string up by its key in data/locale; the plugin passes obs_module_text.
using TextLookup = std::function<QString(const char *key)>;

// Fills combo with the replay resolutions the dock offers.
void addResolutions(QComboBox &combo, const TextLookup &text);
ReplayResolution resolutionAt(int index);
int indexOfResolution(ReplayResolution resolution);

} // namespace tapeloop::ui
