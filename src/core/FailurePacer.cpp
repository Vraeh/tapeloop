// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/FailurePacer.hpp"

namespace tapeloop {

std::optional<uint64_t> FailurePacer::fail(Nanoseconds now) noexcept
{
	// A clock that went back would otherwise keep failures unsaid until it was past the
	// last line again.
	if (lastSaid_ && now >= *lastSaid_ && saturatingSub(now, *lastSaid_) < interval_) {
		++unsaid_;
		return std::nullopt;
	}
	lastSaid_ = now;
	const uint64_t unsaid = unsaid_;
	unsaid_ = 0;
	return unsaid;
}

} // namespace tapeloop
