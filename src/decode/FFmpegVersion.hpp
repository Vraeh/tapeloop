// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

namespace tapeloop::decode {

// The version of the FFmpeg linked into the plugin, as FFmpeg reports it.
const char *ffmpegVersion() noexcept;

} // namespace tapeloop::decode
