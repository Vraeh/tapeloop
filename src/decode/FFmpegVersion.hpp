// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

namespace tapeloop::decode {

// The version of the FFmpeg linked into the plugin, as FFmpeg reports it.
const char *ffmpegVersion() noexcept;

// Silences FFmpeg's own messages, which go to stderr. For the fuzzers, which feed it
// broken bitstreams on purpose and include no FFmpeg header of their own.
void quietFFmpegLog() noexcept;

} // namespace tapeloop::decode
