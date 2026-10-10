// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

namespace tapeloop {

enum class ReplayState {
	// Captured, and being written.
	Writing,
	Stored,
	// Writing it failed: it plays only while the buffers still hold its GOPs.
	NotSaved,
	// Found on disk with a damaged manifest or a segment gone: it cannot play.
	Damaged,
};

} // namespace tapeloop
