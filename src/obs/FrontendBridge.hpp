// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "obs/CaptureManager.hpp"

#include <obs-frontend-api.h>

#include <functional>
#include <memory>

namespace tapeloop::obs {

// The capture manager wired to the OBS frontend: streaming and recording events, scene
// collection cleanup and exit, the scene collection save callback, and a poll every
// second on the UI thread. Created and destroyed on the UI thread, at module load and
// unload.
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
	// The frontend API is gone by the time the module unloads, so the callbacks go at
	// the exit event.
	void removeFrontendCallbacks() noexcept;

	static void handleEvent(obs_frontend_event event, void *data) noexcept;
	static void handleSave(obs_data_t *collection, bool saving, void *data) noexcept;
	static void handleTick(void *data, float seconds) noexcept;

	std::unique_ptr<CaptureManager> manager_;
	bool frontendCallbacks_ = true;
	// Touched only on the graphics thread.
	float sincePoll_ = 0.0f;
	std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

} // namespace tapeloop::obs
