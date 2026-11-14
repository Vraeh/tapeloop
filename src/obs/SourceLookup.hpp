// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include <obs.hpp>

namespace tapeloop::obs {

// The source with this UUID, removed and private ones included. libobs reads a whole
// UUID's length from the key it is given, whatever the key's own length, and gives
// sources UUIDs of that length only. Saved settings can hold any string, so a key of
// another length, or null, is a source not found.
OBSSourceAutoRelease sourceByUuid(const char *uuid);

} // namespace tapeloop::obs
