// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "decode/FFmpegVersion.hpp"

#include "decode/FFmpegHeaders.hpp"

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/log.h>
}

namespace tapeloop::decode {

const char *ffmpegVersion() noexcept
{
	return av_version_info();
}

void quietFFmpegLog() noexcept
{
	av_log_set_level(AV_LOG_QUIET);
}

} // namespace tapeloop::decode
