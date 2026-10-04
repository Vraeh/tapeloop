// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/BufferLifecycle.hpp"

namespace tapeloop {

void BufferLifecycle::setStartWithOutputs(bool on) noexcept
{
	startWithOutputs_ = on;
	if (on && outputActive()) {
		running_ = true;
	}
}

void BufferLifecycle::reset(bool startWithOutputs, bool streaming, bool recording) noexcept
{
	startWithOutputs_ = startWithOutputs;
	streaming_ = streaming;
	recording_ = recording;
	running_ = startWithOutputs && outputActive();
}

void BufferLifecycle::setStreaming(bool active) noexcept
{
	streaming_ = active;
	followOutputs();
}

void BufferLifecycle::setRecording(bool active) noexcept
{
	recording_ = active;
	followOutputs();
}

bool BufferLifecycle::manualStart() noexcept
{
	if (!manualControlEnabled()) {
		return false;
	}
	running_ = true;
	return true;
}

bool BufferLifecycle::manualStop() noexcept
{
	if (!manualControlEnabled()) {
		return false;
	}
	running_ = false;
	return true;
}

void BufferLifecycle::followOutputs() noexcept
{
	if (startWithOutputs_) {
		running_ = outputActive();
	}
}

} // namespace tapeloop
