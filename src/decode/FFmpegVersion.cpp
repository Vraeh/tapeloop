// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "decode/FFmpegVersion.hpp"

extern "C" {
#include <libavutil/avutil.h>
}

namespace tapeloop::decode {

const char *ffmpegVersion() noexcept
{
	return av_version_info();
}

} // namespace tapeloop::decode
