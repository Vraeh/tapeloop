// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

namespace tapeloop::test {

// An H.264 video encoder that never initializes, as a hardware encoder without its
// device does.
inline constexpr const char *kFailingEncoderId = "tapeloop_test_failing";
// An H.264 video encoder that initializes and then fails on its first frame, which
// libobs reports to the output as a null packet.
inline constexpr const char *kBrokenEncoderId = "tapeloop_test_broken";
// An AV1 video encoder, a codec replays do not hold, that fails on its first frame.
inline constexpr const char *kAv1EncoderId = "tapeloop_test_av1";
// An HEVC video encoder that makes every frame a keyframe of a few made-up bytes.
inline constexpr const char *kHevcEncoderId = "tapeloop_test_hevc";

void registerTestEncoders();

// How many times libobs initialized an encoder of kAv1EncoderId since
// registerTestEncoders.
int av1EncoderInitializations();

} // namespace tapeloop::test
