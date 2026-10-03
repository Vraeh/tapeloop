// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/MediaTime.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <random>
#include <vector>

using tapeloop::Rational;
using tapeloop::rescale;

namespace {

constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
constexpr int32_t kInt32Max = std::numeric_limits<int32_t>::max();
constexpr Rational kNs = tapeloop::kNanosecondTimebase;
constexpr int64_t kSecondsPerDay = 24 * 60 * 60;

// Reference conversion for non-negative tick counts. Splitting off whole timebase
// periods first keeps every intermediate within 64 bits for the timebases tested.
int64_t referenceNanoseconds(int64_t ticks, Rational timebase)
{
	const int64_t nanosecondsPerPeriod = int64_t{timebase.num} * 1'000'000'000;
	const int64_t periods = ticks / timebase.den;
	const int64_t scaledRest = (ticks % timebase.den) * nanosecondsPerPeriod;
	int64_t result = periods * nanosecondsPerPeriod + scaledRest / timebase.den;
	if (2 * (scaledRest % timebase.den) >= timebase.den)
		++result;
	return result;
}

std::vector<int64_t> sampleTicks(int64_t ticksPerDay)
{
	std::vector<int64_t> ticks;
	for (int64_t t = 0; t < 2000; ++t)
		ticks.push_back(t);
	const int64_t stride = ticksPerDay / 20011;
	for (int64_t t = 2000; t < ticksPerDay - 2000; t += stride)
		ticks.push_back(t);
	for (int64_t t = ticksPerDay - 2000; t <= ticksPerDay; ++t)
		ticks.push_back(t);
	return ticks;
}

} // namespace

TEST_CASE("rescale converts common timebases to nanoseconds over 24 hours")
{
	const Rational timebases[] = {{1, 60}, {1001, 60000}, {1, 90000}, kNs};

	for (const Rational timebase : timebases) {
		CAPTURE(timebase.num, timebase.den);
		const int64_t ticksPerDay = kSecondsPerDay * timebase.den / timebase.num;

		for (const int64_t ticks : sampleTicks(ticksPerDay)) {
			CAPTURE(ticks);
			const int64_t expected = referenceNanoseconds(ticks, timebase);
			CHECK(rescale(ticks, timebase, kNs) == expected);
			CHECK(rescale(-ticks, timebase, kNs) == -expected);
			CHECK(rescale(expected, kNs, timebase) == ticks);
		}
	}
}

TEST_CASE("rescale rounds half away from zero")
{
	const Rational half{1, 2};
	const Rational quarter{1, 4};
	const Rational second{1, 1};

	CHECK(rescale(1, half, second) == 1);
	CHECK(rescale(-1, half, second) == -1);
	CHECK(rescale(3, half, second) == 2);
	CHECK(rescale(-3, half, second) == -2);
	CHECK(rescale(1, quarter, second) == 0);
	CHECK(rescale(-1, quarter, second) == 0);
	CHECK(rescale(3, quarter, second) == 1);
	CHECK(rescale(-3, quarter, second) == -1);
	CHECK(rescale(-1, {1, 3}, half) == -1);
	CHECK(rescale(0, half, second) == 0);

	const Rational ntsc{1001, 60000};
	CHECK(rescale(1, ntsc, kNs) == 16'683'333);
	CHECK(rescale(2, ntsc, kNs) == 33'366'667);
	CHECK(rescale(-2, ntsc, kNs) == -33'366'667);
}

TEST_CASE("rescale keeps every int64_t value when the timebase does not change")
{
	const Rational widest[] = {{1, 1}, {kInt32Max, 1}, {1, kInt32Max}, {kInt32Max, kInt32Max}};
	for (const Rational timebase : widest) {
		CAPTURE(timebase.num, timebase.den);
		CHECK(rescale(kMax, timebase, timebase) == kMax);
		CHECK(rescale(kMin, timebase, timebase) == kMin);
		CHECK(rescale(kMin + 1, timebase, timebase) == kMin + 1);
		CHECK(rescale(-1, timebase, timebase) == -1);
	}
}

TEST_CASE("rescale saturates results outside the int64_t range")
{
	const Rational second{1, 1};
	const int64_t maxSeconds = kMax / 1'000'000'000;

	CHECK(rescale(maxSeconds, second, kNs) == maxSeconds * 1'000'000'000);
	CHECK(rescale(maxSeconds + 1, second, kNs) == kMax);
	CHECK(rescale(-maxSeconds - 1, second, kNs) == kMin);
	CHECK(rescale(kMax, second, kNs) == kMax);
	CHECK(rescale(kMin, second, kNs) == kMin);

	CHECK(rescale(kMax, kNs, second) == 9'223'372'037);
	CHECK(rescale(kMin, kNs, second) == -9'223'372'037);

	const Rational longest{kInt32Max, 1};
	const Rational shortest{1, kInt32Max};
	CHECK(rescale(1, longest, shortest) == int64_t{kInt32Max} * kInt32Max);
	CHECK(rescale(kMax, longest, shortest) == kMax);
	CHECK(rescale(kMin, longest, shortest) == kMin);
	CHECK(rescale(kMax, shortest, longest) == 2);
	CHECK(rescale(kMin, shortest, longest) == -2);
}

TEST_CASE("rescale rounds correctly at the edges of the int64_t range")
{
	// 6148914691236517205 * 3 / 2 is 2^63 - 0.5, which rounds to 2^63: exactly the
	// minimum when negative, one past the maximum when positive.
	CHECK(rescale(6'148'914'691'236'517'205, {3, 1}, {2, 1}) == kMax);
	CHECK(rescale(-6'148'914'691'236'517'205, {3, 1}, {2, 1}) == kMin);

	// 1190112520884487201 * 31 / 2 is 2^64 - 0.5, where rounding up would carry past
	// 64 bits.
	CHECK(rescale(1'190'112'520'884'487'201, {31, 1}, {2, 1}) == kMax);
	CHECK(rescale(-1'190'112'520'884'487'201, {31, 1}, {2, 1}) == kMin);
}

#if defined(__SIZEOF_INT128__)

namespace {

__extension__ typedef __int128 Int128;

int64_t reference128(int64_t value, Rational from, Rational to)
{
	const Int128 numerator = Int128{value} * from.num * to.den;
	const Int128 denominator = Int128{from.den} * to.num;
	Int128 quotient = numerator / denominator;
	const Int128 remainder = numerator % denominator;
	if (2 * (remainder < 0 ? -remainder : remainder) >= denominator)
		quotient += numerator < 0 ? -1 : 1;
	if (quotient > kMax)
		return kMax;
	if (quotient < kMin)
		return kMin;
	return static_cast<int64_t>(quotient);
}

} // namespace

TEST_CASE("rescale matches 128-bit arithmetic")
{
	std::mt19937_64 random(20261003);
	std::uniform_int_distribution<int64_t> anyValue(kMin, kMax);
	std::uniform_int_distribution<int64_t> smallValue(-100'000'000'000, 100'000'000'000);
	std::uniform_int_distribution<int32_t> anyPart(1, kInt32Max);
	std::uniform_int_distribution<int32_t> smallPart(1, 100'000);

	for (int i = 0; i < 200'000; ++i) {
		const int64_t value = (i % 2 == 0) ? anyValue(random) : smallValue(random);
		auto part = [&]() {
			return (i % 3 == 0) ? anyPart(random) : smallPart(random);
		};
		const Rational from{part(), part()};
		const Rational to{part(), part()};

		CAPTURE(value, from.num, from.den, to.num, to.den);
		REQUIRE(rescale(value, from, to) == reference128(value, from, to));
	}
}

#endif

TEST_CASE("saturating time arithmetic clamps instead of overflowing")
{
	using tapeloop::Nanoseconds;
	using tapeloop::saturatingAdd;
	using tapeloop::saturatingSub;

	const Nanoseconds max = Nanoseconds::max();
	const Nanoseconds min = Nanoseconds::min();
	const Nanoseconds one{1};

	CHECK(saturatingAdd(Nanoseconds{2}, Nanoseconds{3}) == Nanoseconds{5});
	CHECK(saturatingAdd(Nanoseconds{2}, Nanoseconds{-3}) == Nanoseconds{-1});
	CHECK(saturatingAdd(max, one) == max);
	CHECK(saturatingAdd(max - one, one) == max);
	CHECK(saturatingAdd(min, -one) == min);
	CHECK(saturatingAdd(min, max) == -one);
	CHECK(saturatingAdd(max, max) == max);
	CHECK(saturatingAdd(min, min) == min);

	CHECK(saturatingSub(Nanoseconds{2}, Nanoseconds{3}) == Nanoseconds{-1});
	CHECK(saturatingSub(max, -one) == max);
	CHECK(saturatingSub(min, one) == min);
	CHECK(saturatingSub(min + one, one) == min);
	CHECK(saturatingSub(Nanoseconds{0}, min) == max);
	CHECK(saturatingSub(-one, min) == max);
	CHECK(saturatingSub(max, max) == Nanoseconds{0});
	CHECK(saturatingSub(min, max) == min);
	CHECK(saturatingSub(max, min) == max);
}
