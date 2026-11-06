// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "obs/AirEvents.hpp"

#include "obs/CaptureManager.hpp"
#include "obs/SequencePlayer.hpp"

namespace tapeloop::obs {

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
		sequence.destroyAir();
		manager.onExit();
		break;
	case AirEvent::BuffersStopped:
		// The program goes on, so the replay goes back to live the way the user takes it
		// off.
		sequence.takeOffAir();
		break;
	}
}

} // namespace tapeloop::obs
