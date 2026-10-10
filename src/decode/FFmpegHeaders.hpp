// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

// Included before any FFmpeg header, so that a build that finds another FFmpeg's headers
// first stops here. libobs puts obs-deps' include directory, which holds the headers of
// OBS's own FFmpeg, on every target that links it; compiled against those and linked with
// ours, the code would build without a word and fail at run time.
extern "C" {
#include <libavcodec/version.h>
#include <libavformat/version.h>
#include <libavutil/version.h>
}

// FFmpeg 8.1, which cmake/ffmpeg/BuildFFmpeg.cmake builds.
#if LIBAVCODEC_VERSION_MAJOR != 62 || LIBAVFORMAT_VERSION_MAJOR != 62 || LIBAVUTIL_VERSION_MAJOR != 60
#error "These are not the headers of the FFmpeg that cmake/ffmpeg/BuildFFmpeg.cmake builds"
#endif
