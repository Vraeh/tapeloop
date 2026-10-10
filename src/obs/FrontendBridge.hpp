// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "obs/CaptureManager.hpp"

#include <obs-frontend-api.h>

#include <functional>
#include <memory>
#include <string>

namespace tapeloop::obs {

// The capture manager wired to the OBS frontend: streaming and recording events, scene
// collection cleanup and exit, the scene collection save callback and save requests,
// the replays on disk read once OBS has loaded, a poll every second on the UI thread,
// and the hotkey that captures a replay, saved with the scene collection. Created and
// destroyed on the UI thread, at module load and unload.
class FrontendBridge : private CaptureHost {
public:
	FrontendBridge();
	~FrontendBridge() override;

	FrontendBridge(const FrontendBridge &) = delete;
	FrontendBridge &operator=(const FrontendBridge &) = delete;

	CaptureManager &manager() noexcept { return *manager_; }

private:
	bool streamingActive() const override;
	bool recordingActive() const override;
	void requestSave() override;
	std::string recordingFolder() const override;
	std::string sceneCollectionName() const override;
	// The frontend API is gone by the time the module unloads, so the callbacks go at
	// the exit event.
	void removeFrontendCallbacks() noexcept;

	static void handleEvent(obs_frontend_event event, void *data) noexcept;
	static void handleSave(obs_data_t *collection, bool saving, void *data) noexcept;
	static void handleTick(void *data, float seconds) noexcept;
	static void handleCaptureHotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed) noexcept;
	// Runs work on the UI thread, where captures start and stop, unless the bridge is
	// gone by then.
	void onUiThread(void (*work)(CaptureManager &)) noexcept;

	std::unique_ptr<CaptureManager> manager_;
	obs_hotkey_id captureHotkey_ = OBS_INVALID_HOTKEY_ID;
	bool frontendCallbacks_ = true;
	// Touched only on the graphics thread.
	float sincePoll_ = 0.0f;
	std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

} // namespace tapeloop::obs
