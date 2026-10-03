// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

namespace tapeloop {

// When the buffers run. With "start with streaming or recording" on, they run while
// either output is active and the manual start and stop is refused meanwhile; with it
// off, only the manual start and stop moves them, at any time.
class BufferLifecycle {
public:
	explicit BufferLifecycle(bool startWithOutputs = true) noexcept : startWithOutputs_(startWithOutputs) {}

	bool running() const noexcept { return running_; }
	bool startWithOutputs() const noexcept { return startWithOutputs_; }
	bool streaming() const noexcept { return streaming_; }
	bool recording() const noexcept { return recording_; }
	bool manualControlEnabled() const noexcept { return !startWithOutputs_ || !outputActive(); }

	// Turning it on while an output is active starts the buffers; turning it off leaves
	// them as they are.
	void setStartWithOutputs(bool on) noexcept;
	// Starts over, as for another scene collection: the buffers run only when its setting
	// follows the outputs and one of them is active.
	void reset(bool startWithOutputs, bool streaming, bool recording) noexcept;
	void setStreaming(bool active) noexcept;
	void setRecording(bool active) noexcept;

	// False when refused.
	bool manualStart() noexcept;
	bool manualStop() noexcept;

private:
	bool outputActive() const noexcept { return streaming_ || recording_; }
	void followOutputs() noexcept;

	bool startWithOutputs_;
	bool streaming_ = false;
	bool recording_ = false;
	bool running_ = false;
};

} // namespace tapeloop
