// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "obs/ReplayPlayer.hpp"

#include <obs.h>

namespace tapeloop::obs {

// A video source that plays clips through a ReplayPlayer of its own: OBS's ticks move
// the playhead by the time that passed, and the source draws the frame decoded for it
// at the size of that frame. Tapeloop creates its sources itself, as private ones, and
// the type is kept out of OBS's list of sources to add. Registered once, at module
// load.
inline constexpr const char *kReplaySourceId = "tapeloop_replay_player";

// Every player of the type makes its decoders with a copy of decoders.
void registerReplaySource(ReplayPlayer::DecoderFactory decoders);

// Creates a private source of kReplaySourceId, its player started. Null when libobs
// refuses, or the player cannot be made or started.
obs_source_t *createReplaySource(const char *name);

// The player of a source of kReplaySourceId, valid while the source is; null for any
// other source. Its controls and load() may be called from any thread.
ReplayPlayer *replayPlayer(obs_source_t *source);

} // namespace tapeloop::obs
