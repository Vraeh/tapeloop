// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include <obs.h>

#include <string>

namespace tapeloop::obs {

// A source users add to their own scenes to show the replay where they want it: every
// one draws the picture of the replay being played, fitted inside a box the size of the
// main canvas, and nothing while no replay is loaded. It decodes nothing itself.
// Registered once, at module load.
inline constexpr const char *kReplayViewId = "tapeloop_replay_view";

void registerReplayView(std::string displayName);

// The replay source, of kReplaySourceId, the views draw; null for none. The source must
// stay valid until it is replaced or unset. The UI thread.
void setViewedReplay(obs_source_t *replay);
// Unsets it if it is that source.
void unsetViewedReplay(obs_source_t *replay);

// Whether the scene holds a view anywhere in it, hidden or inside a nested scene or
// group.
bool holdsReplayView(obs_source_t *scene);

} // namespace tapeloop::obs
