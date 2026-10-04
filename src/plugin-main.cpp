// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "obs/CaptureOutput.hpp"
#include "obs/FrontendBridge.hpp"
#include "obs/ManagerDockBackend.hpp"
#include "ui/TapeloopDock.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <memory>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

namespace {

constexpr const char *kDockId = "tapeloop";

std::unique_ptr<tapeloop::obs::FrontendBridge> bridge;
std::unique_ptr<tapeloop::obs::ManagerDockBackend> dockBackend;

void addDock()
{
	dockBackend = std::make_unique<tapeloop::obs::ManagerDockBackend>(bridge->manager());
	auto *dock = new tapeloop::ui::TapeloopDock(*dockBackend, [](const char *key) {
		return QString::fromUtf8(obs_module_text(key));
	});
	// OBS owns the dock once it is added, and deletes it with the main window.
	if (!obs_frontend_add_dock_by_id(kDockId, obs_module_text("Dock.Title"), dock)) {
		delete dock;
	}
}

} // namespace

const char *obs_module_name()
{
	return "Tapeloop";
}

const char *obs_module_description()
{
	return obs_module_text("Description");
}

bool obs_module_load()
{
	try {
		tapeloop::obs::registerCaptureOutput();
		bridge = std::make_unique<tapeloop::obs::FrontendBridge>();
		addDock();
	} catch (...) {
		// OBS does not unload a module whose load failed.
		dockBackend.reset();
		bridge.reset();
		obs_log(LOG_ERROR, "could not start");
		return false;
	}
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload()
{
	// The dock went with the main window, and the frontend API with it.
	try {
		dockBackend.reset();
		bridge.reset();
	} catch (...) {
		obs_log(LOG_ERROR, "could not stop cleanly");
	}
	obs_log(LOG_INFO, "plugin unloaded");
}
