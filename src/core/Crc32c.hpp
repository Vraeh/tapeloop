// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include <cstdint>
#include <span>

namespace tapeloop {

// CRC-32C (Castagnoli), the checksum of every chunk of a stored replay. A previous
// result passed as seed continues it over more data.
uint32_t crc32c(std::span<const uint8_t> data, uint32_t seed = 0) noexcept;

} // namespace tapeloop
