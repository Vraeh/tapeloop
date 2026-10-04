// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "obs/CaptureManager.hpp"
#include "ui/DockBackend.hpp"

#include <functional>
#include <utility>

namespace tapeloop::obs {

// The dock's view of the capture manager: the video inputs of the scene collection,
// sorted by name regardless of case, with their selection and capture state.
class ManagerDockBackend : public ui::DockBackend {
public:
	// requestSave runs after every change of the settings, so that the plugin can have
	// the scene collection saved at once and a crash right after an edit loses nothing.
	explicit ManagerDockBackend(CaptureManager &manager, std::function<void()> requestSave = {})
		: manager_(manager),
		  requestSave_(std::move(requestSave))
	{
	}

	std::vector<ui::DockSource> sources() const override;
	BufferSettings settings() const override { return manager_.settings(); }
	void setSettings(const BufferSettings &settings) override;

	bool running() const override { return manager_.running(); }
	bool manualControlEnabled() const override { return manager_.manualControlEnabled(); }
	bool toggleRunning() override { return manager_.running() ? manager_.manualStop() : manager_.manualStart(); }

private:
	CaptureManager &manager_;
	std::function<void()> requestSave_;
};

} // namespace tapeloop::obs
