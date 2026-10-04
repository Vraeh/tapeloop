// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/SourceBuffer.hpp"

#include <obs.h>

#include <atomic>

namespace tapeloop::obs {

// Where an output of kCaptureOutputId delivers. It must outlive the output.
struct CaptureTarget {
	SourceBuffer *buffer = nullptr;
	// Empties the buffer once the encoder has initialized, so a start that fails before
	// keeps what the buffer held.
	bool clearOnStart = false;
	// Set on the encoder's thread when the encoder reports an error or a packet cannot
	// be stored; the output stops itself then.
	std::atomic<bool> failed{false};
};

// An encoded video output that gives its buffer the encoder's codec configuration on
// start and copies every packet into it. Registered once, at module load.
inline constexpr const char *kCaptureOutputId = "tapeloop_capture";

void registerCaptureOutput();

// Creates an output of kCaptureOutputId writing to target. Null when libobs refuses.
obs_output_t *createCaptureOutput(const char *name, CaptureTarget &target);

} // namespace tapeloop::obs
