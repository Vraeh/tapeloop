// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "obs/CaptureManager.hpp"
#include "ui/DockBackend.hpp"

namespace tapeloop::obs {

// The dock's view of the capture manager: the video inputs of the scene collection,
// sorted by name regardless of case, with their selection and capture state.
class ManagerDockBackend : public ui::DockBackend {
public:
	explicit ManagerDockBackend(CaptureManager &manager) : manager_(manager) {}

	std::vector<ui::DockSource> sources() const override;
	BufferSettings settings() const override { return manager_.settings(); }
	void setSettings(const BufferSettings &settings) override { manager_.setSettings(settings); }

	bool running() const override { return manager_.running(); }
	bool manualControlEnabled() const override { return manager_.manualControlEnabled(); }
	bool toggleRunning() override { return manager_.running() ? manager_.manualStop() : manager_.manualStart(); }

private:
	CaptureManager &manager_;
};

} // namespace tapeloop::obs
