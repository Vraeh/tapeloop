// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/Crc32c.hpp"

#include <array>
#include <cstddef>

namespace tapeloop {
namespace {

using Tables = std::array<std::array<uint32_t, 256>, 8>;

// Slicing by eight: table k gives the effect of a byte followed by k zero bytes, so the
// loop takes eight bytes per step. Several times faster than one table, and portable.
constexpr Tables makeTables()
{
	Tables tables{};
	for (uint32_t byte = 0; byte < 256; ++byte) {
		uint32_t crc = byte;
		for (int bit = 0; bit < 8; ++bit) {
			crc = (crc & 1) != 0 ? (crc >> 1) ^ 0x82F63B78u : crc >> 1;
		}
		tables[0][byte] = crc;
	}
	for (size_t k = 1; k < tables.size(); ++k) {
		for (size_t byte = 0; byte < 256; ++byte) {
			const uint32_t previous = tables[k - 1][byte];
			tables[k][byte] = (previous >> 8) ^ tables[0][previous & 0xFF];
		}
	}
	return tables;
}

constexpr Tables kTables = makeTables();

} // namespace

uint32_t crc32c(std::span<const uint8_t> data, uint32_t seed) noexcept
{
	uint32_t crc = ~seed;
	size_t i = 0;
	for (; i + 8 <= data.size(); i += 8) {
		const uint32_t low = crc ^ (uint32_t{data[i]} | uint32_t{data[i + 1]} << 8 |
					    uint32_t{data[i + 2]} << 16 | uint32_t{data[i + 3]} << 24);
		crc = kTables[7][low & 0xFF] ^ kTables[6][(low >> 8) & 0xFF] ^ kTables[5][(low >> 16) & 0xFF] ^
		      kTables[4][low >> 24] ^ kTables[3][data[i + 4]] ^ kTables[2][data[i + 5]] ^
		      kTables[1][data[i + 6]] ^ kTables[0][data[i + 7]];
	}
	for (; i < data.size(); ++i) {
		crc = (crc >> 8) ^ kTables[0][(crc ^ data[i]) & 0xFF];
	}
	return ~crc;
}

} // namespace tapeloop
