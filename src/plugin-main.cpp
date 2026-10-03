// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "obs/CaptureOutput.hpp"

#include <obs-module.h>
#include <plugin-support.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

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
	} catch (...) {
		obs_log(LOG_ERROR, "could not register the capture output");
		return false;
	}
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload()
{
	obs_log(LOG_INFO, "plugin unloaded");
}
