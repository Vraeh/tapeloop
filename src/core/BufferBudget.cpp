// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/BufferBudget.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace tapeloop {

uint64_t automaticBufferBudget(uint64_t physicalMemory) noexcept
{
	return physicalMemory == 0 ? std::numeric_limits<uint64_t>::max() : physicalMemory / 4;
}

uint64_t bufferBudget(int64_t settingMiB, uint64_t physicalMemory) noexcept
{
	if (settingMiB <= 0) {
		return automaticBufferBudget(physicalMemory);
	}
	constexpr uint64_t kMostMiB = std::numeric_limits<uint64_t>::max() >> 20;
	return std::min(static_cast<uint64_t>(settingMiB), kMostMiB) << 20;
}

bool budgetAboveSafeShare(uint64_t budget, uint64_t physicalMemory) noexcept
{
	return physicalMemory != 0 && budget > physicalMemory / 2;
}

std::vector<size_t> shareBudget(std::span<const size_t> needs, uint64_t budget)
{
	// The proportion is worked out in double, as the needs together, or a need times the
	// budget, can pass 64 bits; byte counts stay well inside its 53 bits of precision.
	uint64_t total = 0;
	bool overflows = false;
	double sum = 0;
	for (const size_t need : needs) {
		if (need > std::numeric_limits<uint64_t>::max() - total) {
			overflows = true;
		} else {
			total += need;
		}
		sum += static_cast<double>(need);
	}
	std::vector<size_t> shares(needs.begin(), needs.end());
	if (!overflows && total <= budget) {
		return shares;
	}
	const double part = static_cast<double>(budget) / sum;
	for (size_t &share : shares) {
		// Below the need it was worked out from, so it fits a size_t.
		const double scaled = std::floor(static_cast<double>(share) * part);
		if (scaled < static_cast<double>(share)) {
			share = static_cast<size_t>(scaled);
		}
	}
	return shares;
}

} // namespace tapeloop
