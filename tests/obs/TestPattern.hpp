// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include <obs.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace tapeloop::test {

// A video source drawn on the GPU: a white bar that moves one step per frame over a
// grey background, and under it the number of the frame in binary, one black or white
// cell per bit, most significant first, so a decoded frame can be traced back to the
// frame rendered. Settings: "width" and "height", which may change while it runs,
// "size_only_when_shown", which makes it report 0x0 while nothing shows it, as display,
// window and game captures do, and "size_only_when_active", as a media source that
// plays only while active.
inline constexpr const char *kTestPatternId = "tapeloop_test_pattern";
// The same pattern under the id of OBS's media source, which the harness does not load,
// with its "restart_on_activate" setting, on by default as there.
inline constexpr const char *kMediaStandInId = "ffmpeg_source";
// The same under the ids of OBS's VLC source, image slideshow and image source, with
// the defaults of the VLC source's and the slideshow's playback behavior.
inline constexpr const char *kVlcStandInId = "vlc_source";
inline constexpr const char *kSlideshowStandInId = "slideshow";
inline constexpr const char *kImageStandInId = "image_source";
// The same under the id of the browser source of obs-browser, with its
// "restart_when_active" setting, off by default as there.
inline constexpr const char *kBrowserStandInId = "browser_source";
inline constexpr int kFrameNumberBits = 16;

void registerTestPattern();

// An input with audio only and no samples, to show what video-only code leaves out.
inline constexpr const char *kSilenceId = "tapeloop_test_silence";

void registerSilence();

// A white frame and a steady tone, to tell whether a source reaches the program picture
// or the program audio.
inline constexpr const char *kToneId = "tapeloop_test_tone";

void registerTone();

OBSSourceAutoRelease createTestPattern(uint32_t width, uint32_t height, const char *name = "pattern");

// The frame number drawn into a decoded frame, read from its luma plane. Empty when a
// cell is neither clearly black nor clearly white.
std::optional<uint32_t> readFrameNumber(const uint8_t *luma, ptrdiff_t stride, int width, int height);

} // namespace tapeloop::test
