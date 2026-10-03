// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/MediaTime.hpp"

#include <cassert>
#include <limits>

namespace tapeloop {
namespace {

// MSVC has no 128-bit integer type, so the wide arithmetic is done on 64-bit halves
// on every compiler. The tests check it against __int128 where that exists.
struct UInt128 {
	uint64_t high = 0;
	uint64_t low = 0;
};

UInt128 multiply(uint64_t a, uint64_t b) noexcept
{
	constexpr uint64_t kLowMask = 0xffff'ffff;
	const uint64_t aLow = a & kLowMask;
	const uint64_t aHigh = a >> 32;
	const uint64_t bLow = b & kLowMask;
	const uint64_t bHigh = b >> 32;

	const uint64_t lowLow = aLow * bLow;
	const uint64_t lowHigh = aLow * bHigh;
	const uint64_t highLow = aHigh * bLow;
	const uint64_t highHigh = aHigh * bHigh;

	const uint64_t middle = (lowLow >> 32) + (lowHigh & kLowMask) + (highLow & kLowMask);

	return {highHigh + (lowHigh >> 32) + (highLow >> 32) + (middle >> 32), (middle << 32) | (lowLow & kLowMask)};
}

struct Division {
	uint64_t quotient = 0;
	uint64_t remainder = 0;
};

// Requires dividend.high < divisor, which keeps the quotient within 64 bits, and
// divisor < 2^63, which keeps the shifted remainder within 64 bits.
Division divide(UInt128 dividend, uint64_t divisor) noexcept
{
	assert(dividend.high < divisor && (divisor >> 63) == 0);

	Division result{0, dividend.high};
	for (int bit = 63; bit >= 0; --bit) {
		result.remainder = (result.remainder << 1) | ((dividend.low >> bit) & 1);
		result.quotient <<= 1;
		if (result.remainder >= divisor) {
			result.remainder -= divisor;
			result.quotient |= 1;
		}
	}
	return result;
}

} // namespace

int64_t rescale(int64_t value, Rational from, Rational to) noexcept
{
	assert(from.num > 0 && from.den > 0 && to.num > 0 && to.den > 0);

	// Both factors are below 2^62 and the magnitude is at most 2^63, so the
	// product needs at most 125 bits.
	const uint64_t multiplier = static_cast<uint64_t>(from.num) * static_cast<uint64_t>(to.den);
	const uint64_t divisor = static_cast<uint64_t>(from.den) * static_cast<uint64_t>(to.num);

	const bool negative = value < 0;
	const uint64_t magnitude = negative ? 0 - static_cast<uint64_t>(value) : static_cast<uint64_t>(value);
	const uint64_t limit = negative ? uint64_t{1} << 63
					: static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
	const int64_t saturated = negative ? std::numeric_limits<int64_t>::min() : std::numeric_limits<int64_t>::max();

	const UInt128 product = multiply(magnitude, multiplier);
	if (product.high >= divisor)
		return saturated;

	const Division division = divide(product, divisor);
	const bool roundUp = division.remainder >= divisor - division.remainder;
	if (division.quotient > limit || (roundUp && division.quotient == limit))
		return saturated;

	const uint64_t rounded = division.quotient + (roundUp ? 1u : 0u);
	return negative ? static_cast<int64_t>(0 - rounded) : static_cast<int64_t>(rounded);
}

} // namespace tapeloop
