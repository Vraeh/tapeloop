// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include <obs.h>

#include <cstdint>
#include <string>

namespace tapeloop::obs {

// A video source that draws another one at less than full opacity: it renders its
// target into a texture and draws that with its colour and alpha scaled together, so
// a translucent overlay blends over the replay the way OBS blends any source. It has
// no audio. Tapeloop creates its sources itself, as private ones, and the type is kept
// out of OBS's list of sources to add. Registered once, at module load.
inline constexpr const char *kOverlayOpacityId = "tapeloop_overlay_opacity";

// The type draws with the effect in the file at effectPath.
void registerOverlayOpacity(std::string effectPath);

// A private source of kOverlayOpacityId showing target at opacity percent. It holds the
// target until it is destroyed. Null when libobs refuses or the effect does not load.
obs_source_t *createOverlayOpacity(obs_source_t *target, int32_t opacity);

// The source a source of kOverlayOpacityId shows, valid while that source is; any other
// source is its own.
obs_source_t *overlayTarget(obs_source_t *source);

} // namespace tapeloop::obs
