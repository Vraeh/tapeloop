// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/BufferBudget.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <vector>

using tapeloop::bufferBudget;
using tapeloop::shareBudget;

namespace {

constexpr uint64_t kMiB = uint64_t{1} << 20;
constexpr uint64_t kGiB = uint64_t{1} << 30;

} // namespace

TEST_CASE("the buffers get a quarter of the computer's memory unless set")
{
	CHECK(tapeloop::automaticBufferBudget(32 * kGiB) == 8 * kGiB);
	CHECK(bufferBudget(0, 16 * kGiB) == 4 * kGiB);
	CHECK(bufferBudget(-5, 16 * kGiB) == 4 * kGiB);
	CHECK(bufferBudget(6144, 16 * kGiB) == 6 * kGiB);
	// Unknown memory sets no limit until one is set.
	CHECK(bufferBudget(0, 0) == std::numeric_limits<uint64_t>::max());
	CHECK(bufferBudget(512, 0) == 512 * kMiB);
	CHECK(bufferBudget(std::numeric_limits<int64_t>::max(), 0) == (std::numeric_limits<uint64_t>::max() >> 20)
									      << 20);
}

TEST_CASE("a budget above half of the computer's memory is not safe")
{
	CHECK_FALSE(tapeloop::budgetAboveSafeShare(8 * kGiB, 16 * kGiB));
	CHECK(tapeloop::budgetAboveSafeShare(8 * kGiB + 1, 16 * kGiB));
	CHECK_FALSE(tapeloop::budgetAboveSafeShare(4 * kGiB, 0));
}

TEST_CASE("buffers that need more than the budget share it in proportion")
{
	const std::vector<size_t> needs = {300 * kMiB, 100 * kMiB, 600 * kMiB};
	// Within the budget, each holds all it needs.
	CHECK(shareBudget(needs, 1000 * kMiB) == needs);
	CHECK(shareBudget(needs, 2000 * kMiB) == needs);
	// Over it, each holds the same part of its need, and together no more than it.
	const std::vector<size_t> half = shareBudget(needs, 500 * kMiB);
	CHECK(half == std::vector<size_t>{150 * kMiB, 50 * kMiB, 300 * kMiB});
	const std::vector<size_t> odd = shareBudget(needs, 333);
	CHECK(std::accumulate(odd.begin(), odd.end(), uint64_t{0}) <= 333);
	CHECK(odd[2] > odd[0]);
	CHECK(shareBudget(needs, 0) == std::vector<size_t>{0, 0, 0});
	CHECK(shareBudget({}, 0).empty());
	// Needs that pass 64 bits together share the budget all the same.
	const size_t most = std::numeric_limits<size_t>::max();
	const std::vector<size_t> huge = shareBudget(std::vector<size_t>{most, most}, 4 * kGiB);
	CHECK(huge[0] == huge[1]);
	CHECK(huge[0] <= 2 * kGiB);
}
