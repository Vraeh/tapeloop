// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "obs/SourceLookup.hpp"

#include <util/platform.h>

#include <cstring>

namespace tapeloop::obs {

OBSSourceAutoRelease sourceByUuid(const char *uuid)
{
	if (!uuid || strnlen(uuid, UUID_STR_LENGTH + 1) != UUID_STR_LENGTH) {
		return nullptr;
	}
	return obs_get_source_by_uuid(uuid);
}

} // namespace tapeloop::obs
