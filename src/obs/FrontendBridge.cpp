// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "obs/FrontendBridge.hpp"

#include <util/base.h>

#include <new>
#include <utility>

namespace tapeloop::obs {
namespace {

void runTask(void *param) noexcept
{
	std::unique_ptr<std::function<void()>> task(static_cast<std::function<void()> *>(param));
	try {
		(*task)();
	} catch (...) {
		blog(LOG_ERROR, "[tapeloop] A task on the UI thread failed");
	}
}

} // namespace

FrontendBridge::FrontendBridge() : manager_(std::make_unique<CaptureManager>(static_cast<CaptureHost &>(*this)))
{
	obs_frontend_add_event_callback(handleEvent, this);
	obs_frontend_add_save_callback(handleSave, this);
	obs_add_tick_callback(handleTick, this);
}

FrontendBridge::~FrontendBridge()
{
	obs_remove_tick_callback(handleTick, this);
	removeFrontendCallbacks();
	manager_.reset();
}

void FrontendBridge::removeFrontendCallbacks() noexcept
{
	if (!frontendCallbacks_) {
		return;
	}
	frontendCallbacks_ = false;
	obs_frontend_remove_save_callback(handleSave, this);
	obs_frontend_remove_event_callback(handleEvent, this);
}

bool FrontendBridge::streamingActive() const
{
	return obs_frontend_streaming_active();
}

bool FrontendBridge::recordingActive() const
{
	return obs_frontend_recording_active();
}

void FrontendBridge::requestSave()
{
	// The frontend saves on a later pass of the UI thread's event loop, so the edits of
	// one pass make one save; it does nothing while it has saving turned off, as during
	// a collection load.
	obs_frontend_save();
}

void FrontendBridge::handleEvent(obs_frontend_event event, void *data) noexcept
{
	auto &bridge = *static_cast<FrontendBridge *>(data);
	CaptureManager &manager = *bridge.manager_;
	try {
		switch (event) {
		case OBS_FRONTEND_EVENT_STREAMING_STARTED:
			manager.onStreaming(true);
			break;
		case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
			manager.onStreaming(false);
			break;
		case OBS_FRONTEND_EVENT_RECORDING_STARTED:
			manager.onRecording(true);
			break;
		case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
			manager.onRecording(false);
			break;
		case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
			manager.onSceneCollectionCleanup();
			break;
		case OBS_FRONTEND_EVENT_EXIT:
			manager.onExit();
			bridge.removeFrontendCallbacks();
			break;
		default:
			break;
		}
	} catch (...) {
		blog(LOG_ERROR, "[tapeloop] Handling a frontend event failed");
	}
}

void FrontendBridge::handleSave(obs_data_t *collection, bool saving, void *data) noexcept
{
	CaptureManager &manager = *static_cast<FrontendBridge *>(data)->manager_;
	try {
		if (saving) {
			manager.save(collection);
		} else {
			manager.load(collection);
		}
	} catch (...) {
		blog(LOG_ERROR, "[tapeloop] Saving or loading the settings failed");
	}
}

void FrontendBridge::handleTick(void *data, float seconds) noexcept
{
	// Ticks run on the graphics thread, where captures cannot start or stop; the poll
	// goes to the UI thread.
	auto &bridge = *static_cast<FrontendBridge *>(data);
	bridge.sincePoll_ += seconds;
	if (bridge.sincePoll_ < 1.0f) {
		return;
	}
	bridge.sincePoll_ = 0.0f;
	auto *task = new (std::nothrow) std::function<void()>;
	if (!task) {
		return;
	}
	try {
		*task = [manager = bridge.manager_.get(), alive = std::weak_ptr<int>(bridge.alive_)] {
			if (alive.lock()) {
				manager->poll();
			}
		};
	} catch (...) {
		delete task;
		return;
	}
	obs_queue_task(OBS_TASK_UI, runTask, task, false);
}

} // namespace tapeloop::obs
