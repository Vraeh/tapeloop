// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/EncoderPolicy.hpp"
#include "core/SourceBuffer.hpp"

#include <obs.hpp>

namespace tapeloop::test {

// An encoded video output that copies the packets of its encoder into a SourceBuffer,
// after giving it the encoder's codec configuration on start. libobs offers an output
// no way to reach objects of whoever created it, so the buffer's address travels in
// its settings; see bufferOutputSettings.
inline constexpr const char *kBufferOutputId = "tapeloop_test_buffer_output";

void registerBufferOutput();

// Settings for an output of kBufferOutputId writing into buffer, which must outlive it.
OBSDataAutoRelease bufferOutputSettings(SourceBuffer &buffer);

// The encoder settings in the obs_data form encoders read.
OBSDataAutoRelease toObsData(const EncoderSettings &settings);

} // namespace tapeloop::test
