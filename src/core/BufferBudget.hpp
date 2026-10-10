// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace tapeloop {

// The memory every buffer may hold together: a quarter of the computer's memory unless
// the user sets another amount, in whole MiB, zero meaning automatic. With the computer's
// memory unknown, the automatic budget sets no limit.
uint64_t bufferBudget(int64_t settingMiB, uint64_t physicalMemory) noexcept;
uint64_t automaticBufferBudget(uint64_t physicalMemory) noexcept;

// Above half of the computer's memory a budget leaves OBS and everything else running
// short, and the setting says so.
bool budgetAboveSafeShare(uint64_t budget, uint64_t physicalMemory) noexcept;

// What each buffer may hold: all it needs while the buffers need no more than the budget
// together, and otherwise a share of the budget in proportion to its need, so that each
// holds about the same part of its length.
std::vector<size_t> shareBudget(std::span<const size_t> needs, uint64_t budget);

} // namespace tapeloop
