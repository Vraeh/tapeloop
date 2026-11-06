// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "obs/AirEvents.hpp"

#include "obs/CaptureManager.hpp"
#include "obs/SequencePlayer.hpp"

#include <obs.hpp>

namespace tapeloop::obs {

namespace {

// OBS says nothing of Fade to Black by name, but only it runs the program's transition to
// no source at all: a scene switch always has a scene to go to.
bool programIsBlack()
{
	const OBSSourceAutoRelease program = obs_get_output_source(0);
	if (!program || obs_source_get_type(program) != OBS_SOURCE_TYPE_TRANSITION) {
		return false;
	}
	const OBSSourceAutoRelease shown = obs_transition_get_active_source(program);
	return !shown;
}

} // namespace

void handleAirEvent(AirEvent event, SequencePlayer &sequence, CaptureManager &manager)
{
	switch (event) {
	case AirEvent::CollectionChanging:
	case AirEvent::ScriptingShutdown:
		// The last the frontend says before it empties every channel, which would look
		// like another source taking the replay's.
		sequence.close();
		break;
	case AirEvent::CollectionCleanup:
		// Duplicating or renaming a collection says nothing before this.
		sequence.close();
		manager.onSceneCollectionCleanup();
		// A collection with no file yet loads nothing, so it starts from what the
		// cleanup left.
		sequence.setAir(manager.settings().air);
		break;
	case AirEvent::CollectionChanged:
		// OBS sends it after every collection it loads, the first one included;
		// FINISHED_LOADING comes only once.
		sequence.open();
		break;
	case AirEvent::Exit:
		// OBS unloads its modules before it destroys the sources still held, so those
		// the composition made from other modules go now.
		exitInOrder([&sequence] { sequence.destroyAir(); }, [&manager] { manager.onExit(); });
		break;
	case AirEvent::BuffersStopped:
		// The program goes on, so the replay goes back to live the way the user takes it
		// off.
		sequence.takeOffAir();
		break;
	case AirEvent::ProgramTransitionStopped:
		if (programIsBlack()) {
			sequence.programWentBlack();
		}
		break;
	}
}

void exitInOrder(const std::function<void()> &letGoOfAir, const std::function<void()> &endManager)
{
	try {
		letGoOfAir();
	} catch (...) {
		blog(LOG_ERROR, "[tapeloop] Letting go of the replay failed as OBS exits");
	}
	endManager();
}

} // namespace tapeloop::obs
