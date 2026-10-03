// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include <cstddef>

namespace tapeloop::test {

// Clang's thread sanitizer runtime defines the global operator new itself, so the
// test binary cannot replace it there and the hooks below do nothing.
#if defined(__SANITIZE_THREAD__)
#define TAPELOOP_TEST_ALLOCATION_HOOKS 0
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define TAPELOOP_TEST_ALLOCATION_HOOKS 0
#endif
#endif
#ifndef TAPELOOP_TEST_ALLOCATION_HOOKS
#define TAPELOOP_TEST_ALLOCATION_HOOKS 1
#endif

inline constexpr bool kAllocationHooks = TAPELOOP_TEST_ALLOCATION_HOOKS == 1;

// MSVC's debug iterators give every container a proxy object that is allocated on
// construction and on move, so exact bounds on allocations only hold without them.
// The moves are noexcept, so a failure injected there terminates the program.
#if defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL > 0
inline constexpr bool kExactAllocationCounts = false;
#else
inline constexpr bool kExactAllocationCounts = true;
#endif

inline constexpr bool kAllocationFailures = kAllocationHooks && kExactAllocationCounts;

// Counts the calls to the global operator new made by the current thread while the
// counter is alive. The test binary replaces operator new to make this possible.
class AllocationCounter {
public:
	AllocationCounter();
	~AllocationCounter();

	AllocationCounter(const AllocationCounter &) = delete;
	AllocationCounter &operator=(const AllocationCounter &) = delete;

	size_t count() const noexcept { return count_; }

private:
	size_t count_ = 0;
	size_t *previous_ = nullptr;
};

// While alive, lets `skip` allocations of the current thread through and makes the
// next one throw std::bad_alloc. Only one allocation fails.
class AllocationFailure {
public:
	explicit AllocationFailure(size_t skip = 0);
	~AllocationFailure();

	AllocationFailure(const AllocationFailure &) = delete;
	AllocationFailure &operator=(const AllocationFailure &) = delete;

	bool triggered() const noexcept { return triggered_; }

private:
	friend void *allocate(std::size_t size);

	size_t remaining_;
	bool triggered_ = false;
	AllocationFailure *previous_ = nullptr;
};

} // namespace tapeloop::test
