// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "obs/CaptureOutput.hpp"
#include "obs/FrontendBridge.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <memory>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

namespace {

std::unique_ptr<tapeloop::obs::FrontendBridge> bridge;

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
	} catch (...) {
		obs_log(LOG_ERROR, "could not start");
		return false;
	}
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload()
{
	try {
		bridge.reset();
	} catch (...) {
		obs_log(LOG_ERROR, "could not stop cleanly");
	}
	obs_log(LOG_INFO, "plugin unloaded");
}
