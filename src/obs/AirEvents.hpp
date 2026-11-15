// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include <obs.h>

#include <functional>

namespace tapeloop::obs {

class CaptureManager;
class SequencePlayer;

// What the frontend says, or the buffers do, that a replay on air has to answer.
enum class AirEvent {
	CollectionChanging,
	ScriptingShutdown,
	CollectionCleanup,
	CollectionChanged,
	Exit,
	BuffersStopped,
	ProgramTransitionStopped,
};

// Answers the event, on the UI thread. Apart from the frontend bridge because the libobs
// tests have no frontend to run the bridge with.
void handleAirEvent(AirEvent event, SequencePlayer &sequence, CaptureManager &manager);

// Loads the settings of the scene collection into the manager, and into the sequence the
// way a replay goes on air and the dock's list.
void loadCollection(obs_data_t *collection, SequencePlayer &sequence, CaptureManager &manager);

// The two steps of OBS's exit, in order. A failure letting go of the replay is logged and
// the manager still ends, as the frontend goes away whatever happens here.
void exitInOrder(const std::function<void()> &letGoOfAir, const std::function<void()> &endManager);

} // namespace tapeloop::obs
