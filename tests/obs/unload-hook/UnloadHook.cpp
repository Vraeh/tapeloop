// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

// A module whose obs_module_unload calls back into the test, so a test can run code at
// the point of obs_shutdown where a plugin's unload runs.

#include <obs-module.h>

OBS_DECLARE_MODULE()

namespace {

void (*hook)(void *) = nullptr;
void *hookParam = nullptr;

} // namespace

extern "C" EXPORT void tapeloop_test_set_unload_hook(void (*callback)(void *), void *param)
{
	hook = callback;
	hookParam = param;
}

bool obs_module_load()
{
	return true;
}

void obs_module_unload()
{
	if (hook)
		hook(hookParam);
}
