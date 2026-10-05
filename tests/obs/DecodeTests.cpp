// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "decode/FFmpegVersion.hpp"

#include <catch2/catch_test_macros.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

#include <string>

TEST_CASE("decoding uses the FFmpeg built for the plugin", "[decode]")
{
	CHECK(std::string(tapeloop::decode::ffmpegVersion()) == "8.1.3");
	// Headers and libraries from the same build, not the system's libraries.
	CHECK(avcodec_version() == LIBAVCODEC_VERSION_INT);
	CHECK(avutil_version() == LIBAVUTIL_VERSION_INT);
	CHECK(avcodec_find_decoder(AV_CODEC_ID_H264) != nullptr);
	CHECK(avcodec_find_decoder(AV_CODEC_ID_HEVC) != nullptr);
	CHECK(avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO) == nullptr);
}
