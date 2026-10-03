// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "AllocationCounter.hpp"

#include <cstdlib>
#include <new>

namespace tapeloop::test {
namespace {

thread_local size_t *activeCount = nullptr;
thread_local AllocationFailure *activeFailure = nullptr;

} // namespace

void *allocate(std::size_t size)
{
	if (activeCount)
		++*activeCount;
	if (activeFailure && !activeFailure->triggered_) {
		if (activeFailure->remaining_ == 0) {
			activeFailure->triggered_ = true;
			throw std::bad_alloc();
		}
		--activeFailure->remaining_;
	}
	if (void *memory = std::malloc(size == 0 ? 1 : size))
		return memory;
	throw std::bad_alloc();
}

AllocationCounter::AllocationCounter() : previous_(activeCount)
{
	activeCount = &count_;
}

AllocationCounter::~AllocationCounter()
{
	activeCount = previous_;
}

AllocationFailure::AllocationFailure(size_t skip) : remaining_(skip), previous_(activeFailure)
{
	activeFailure = this;
}

AllocationFailure::~AllocationFailure()
{
	activeFailure = previous_;
}

} // namespace tapeloop::test

#if TAPELOOP_TEST_ALLOCATION_HOOKS

// The aligned forms are left to the runtime; nothing under test over-aligns. The
// nothrow forms are replaced as well: the standard library takes temporary buffers
// from them, and a runtime nothrow new freed by the delete below is a mismatch that
// ASan reports.
void *operator new(std::size_t size)
{
	return tapeloop::test::allocate(size);
}

void *operator new[](std::size_t size)
{
	return tapeloop::test::allocate(size);
}

void *operator new(std::size_t size, const std::nothrow_t &) noexcept
{
	try {
		return tapeloop::test::allocate(size);
	} catch (const std::bad_alloc &) {
		return nullptr;
	}
}

void *operator new[](std::size_t size, const std::nothrow_t &) noexcept
{
	try {
		return tapeloop::test::allocate(size);
	} catch (const std::bad_alloc &) {
		return nullptr;
	}
}

void operator delete(void *memory) noexcept
{
	std::free(memory);
}

void operator delete[](void *memory) noexcept
{
	std::free(memory);
}

void operator delete(void *memory, std::size_t) noexcept
{
	std::free(memory);
}

void operator delete[](void *memory, std::size_t) noexcept
{
	std::free(memory);
}

void operator delete(void *memory, const std::nothrow_t &) noexcept
{
	std::free(memory);
}

void operator delete[](void *memory, const std::nothrow_t &) noexcept
{
	std::free(memory);
}

#endif
