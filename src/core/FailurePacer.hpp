// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/MediaTime.hpp"

#include <chrono>
#include <cstdint>
#include <optional>

namespace tapeloop {

// Paces the log lines of a failure that can repeat every frame: the first one is said at
// once and later ones at most once per interval, each with the number of failures left
// unsaid since the line before. Used from one thread.
class FailurePacer {
public:
	explicit FailurePacer(Nanoseconds interval = std::chrono::minutes{1}) noexcept : interval_(interval) {}

	// Counts a failure seen at `now`. Empty when it is not to be said; otherwise the
	// failures left unsaid since the last line.
	std::optional<uint64_t> fail(Nanoseconds now) noexcept;

private:
	Nanoseconds interval_;
	std::optional<Nanoseconds> lastSaid_;
	uint64_t unsaid_ = 0;
};

} // namespace tapeloop
