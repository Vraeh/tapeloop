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

void registerTestEncoders();

} // namespace tapeloop::test
