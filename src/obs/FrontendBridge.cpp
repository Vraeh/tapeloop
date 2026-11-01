// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "obs/FrontendBridge.hpp"

#ifdef TAPELOOP_FFMPEG
#include "decode/Mp4Writer.hpp"
#endif

#include <obs-module.h>
#include <util/base.h>

#include <new>
#include <string>
#include <utility>

namespace tapeloop::obs {
namespace {

// MP4 needs the plugin's FFmpeg, which macOS does not have yet.
ReplayExporter::WriterFactory mp4Writers()
{
#ifdef TAPELOOP_FFMPEG
	return [] {
		return std::make_unique<decode::Mp4Writer>();
	};
#else
	return {};
#endif
}

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

FrontendBridge::FrontendBridge()
	: manager_(std::make_unique<CaptureManager>(static_cast<CaptureHost &>(*this), mp4Writers())),
	  sequence_(std::make_unique<SequencePlayer>()),
	  // A hotkey pressed now runs on a later pass of the UI thread, once all of this is
	  // built.
	  hotkeys_(std::make_unique<ReplayHotkeys>(
		  [this](HotkeyAction action, bool pressed) { runHotkey(action, pressed, *manager_, *sequence_); },
		  obs_module_text))
{
	obs_frontend_add_event_callback(handleEvent, this);
	obs_frontend_add_save_callback(handleSave, this);
	obs_add_tick_callback(handleTick, this);
}

FrontendBridge::~FrontendBridge()
{
	obs_remove_tick_callback(handleTick, this);
	hotkeys_.reset();
	sequence_.reset();
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

std::string FrontendBridge::recordingFolder() const
{
	char *path = obs_frontend_get_current_record_output_path();
	std::string folder = path ? path : "";
	bfree(path);
	return folder;
}

std::string FrontendBridge::sceneCollectionName() const
{
	char *name = obs_frontend_get_current_scene_collection();
	std::string collection = name ? name : "";
	bfree(name);
	return collection;
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
		case OBS_FRONTEND_EVENT_FINISHED_LOADING:
			bridge.hotkeys_->load(obs_frontend_get_profile_config());
			manager.loadLibrary();
			break;
		case OBS_FRONTEND_EVENT_PROFILE_CHANGED:
			bridge.hotkeys_->load(obs_frontend_get_profile_config());
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
	auto &bridge = *static_cast<FrontendBridge *>(data);
	CaptureManager &manager = *bridge.manager_;
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
	bridge.onUiThread([](CaptureManager &manager) { manager.poll(); });
}

void FrontendBridge::onUiThread(void (*work)(CaptureManager &)) noexcept
{
	std::unique_ptr<std::function<void()>> task(new (std::nothrow) std::function<void()>);
	if (!task) {
		return;
	}
	try {
		*task = [work, manager = manager_.get(), alive = std::weak_ptr<int>(alive_)] {
			if (alive.lock()) {
				work(*manager);
			}
		};
	} catch (...) {
		return;
	}
	obs_queue_task(OBS_TASK_UI, runTask, task.release(), false);
}

} // namespace tapeloop::obs
