// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/FailurePacer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>

using tapeloop::FailurePacer;
using tapeloop::Nanoseconds;
using namespace std::chrono_literals;

TEST_CASE("A failure that repeats is said at once, then at most once a minute")
{
	FailurePacer pacer;
	const Nanoseconds start = 5s;
	CHECK(pacer.fail(start) == std::optional<uint64_t>{0});
	// Every frame of the next minute stays unsaid.
	for (int frame = 1; frame < 3600; ++frame) {
		REQUIRE_FALSE(pacer.fail(start + frame * 16666667ns).has_value());
	}
	CHECK(pacer.fail(start + 59s).has_value() == false);
	// The next line counts the failures left unsaid.
	CHECK(pacer.fail(start + 60s) == std::optional<uint64_t>{3600});
	CHECK_FALSE(pacer.fail(start + 61s).has_value());
	// One that comes back after a quiet while is said at once.
	CHECK(pacer.fail(start + 10min) == std::optional<uint64_t>{1});
	CHECK(pacer.fail(start + 20min) == std::optional<uint64_t>{0});
}

TEST_CASE("A failure is said again when the clock goes back")
{
	FailurePacer pacer(1s);
	CHECK(pacer.fail(10s) == std::optional<uint64_t>{0});
	CHECK_FALSE(pacer.fail(10s + 500ms).has_value());
	CHECK(pacer.fail(2s) == std::optional<uint64_t>{1});
	CHECK_FALSE(pacer.fail(2s + 999ms).has_value());
	CHECK(pacer.fail(3s) == std::optional<uint64_t>{1});
}

TEST_CASE("A failure paced over no interval is always said")
{
	FailurePacer pacer(0ns);
	CHECK(pacer.fail(1s) == std::optional<uint64_t>{0});
	CHECK(pacer.fail(1s) == std::optional<uint64_t>{0});
	CHECK(pacer.fail(Nanoseconds{std::numeric_limits<int64_t>::min()}) == std::optional<uint64_t>{0});
}
