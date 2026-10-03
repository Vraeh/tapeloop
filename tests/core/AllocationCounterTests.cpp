// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "AllocationCounter.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <new>

using tapeloop::test::AllocationCounter;
using tapeloop::test::AllocationFailure;

// Assertions wait until the hooks are released, since reporting them can allocate.
TEST_CASE("Allocation hooks count the nothrow operator new")
{
	if (!tapeloop::test::kAllocationHooks)
		SKIP("operator new cannot be replaced under this sanitizer");

	void *single = nullptr;
	void *array = nullptr;
	size_t count = 0;
	{
		AllocationCounter allocations;
		single = ::operator new(16, std::nothrow);
		array = ::operator new[](16, std::nothrow);
		count = allocations.count();
	}
	CHECK(count == 2);
	CHECK(single != nullptr);
	CHECK(array != nullptr);
	::operator delete(single, std::nothrow);
	::operator delete[](array, std::nothrow);
}

TEST_CASE("An injected failure makes the nothrow operator new return null")
{
	if (!tapeloop::test::kAllocationFailures)
		SKIP("allocation failures cannot be injected in this configuration");

	void *first = nullptr;
	void *second = nullptr;
	bool triggeredByFirst = false;
	bool triggeredBySecond = false;
	{
		AllocationFailure failure(1);
		first = ::operator new(16, std::nothrow);
		triggeredByFirst = failure.triggered();
		second = ::operator new[](16, std::nothrow);
		triggeredBySecond = failure.triggered();
	}
	CHECK_FALSE(triggeredByFirst);
	CHECK(triggeredBySecond);
	CHECK(first != nullptr);
	CHECK(second == nullptr);
	::operator delete(first);
	::operator delete[](second);
}
