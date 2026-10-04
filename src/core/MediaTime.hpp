// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include <chrono>
#include <cstdint>
#include <limits>

namespace tapeloop {

// Every time in the core is on the shared OBS system clock.
using Nanoseconds = std::chrono::nanoseconds;

// Times come from encoders and callers the core does not control, so sums and
// differences clamp to the representable range instead of overflowing.
constexpr Nanoseconds saturatingAdd(Nanoseconds a, Nanoseconds b) noexcept
{
	constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
	constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
	const int64_t x = a.count();
	const int64_t y = b.count();
	if (y > 0 && x > kMax - y) {
		return Nanoseconds{kMax};
	}
	if (y < 0 && x < kMin - y) {
		return Nanoseconds{kMin};
	}
	return Nanoseconds{x + y};
}

constexpr Nanoseconds saturatingSub(Nanoseconds a, Nanoseconds b) noexcept
{
	constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
	constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
	const int64_t x = a.count();
	const int64_t y = b.count();
	if (y < 0 && x > kMax + y) {
		return Nanoseconds{kMax};
	}
	if (y > 0 && x < kMin + y) {
		return Nanoseconds{kMin};
	}
	return Nanoseconds{x - y};
}

// One tick lasts num / den seconds, the same convention as the timebase of an OBS
// encoder packet. Both parts must be positive.
struct Rational {
	int32_t num = 1;
	int32_t den = 1;
};

inline constexpr Rational kNanosecondTimebase{1, 1'000'000'000};

// Converts a tick count from one timebase to another, rounding half away from zero.
// Exact for every input whose result fits in int64_t; results that do not fit
// saturate to the nearest representable value.
int64_t rescale(int64_t value, Rational from, Rational to) noexcept;

} // namespace tapeloop
