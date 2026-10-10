// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/Crc32c.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace {

std::span<const uint8_t> bytesOf(std::string_view text)
{
	return {reinterpret_cast<const uint8_t *>(text.data()), text.size()};
}

uint32_t bitwise(std::span<const uint8_t> data)
{
	uint32_t crc = ~0u;
	for (const uint8_t byte : data) {
		crc ^= byte;
		for (int bit = 0; bit < 8; ++bit) {
			crc = (crc & 1) != 0 ? (crc >> 1) ^ 0x82F63B78u : crc >> 1;
		}
	}
	return ~crc;
}

} // namespace

TEST_CASE("crc32c gives the published check values")
{
	CHECK(tapeloop::crc32c({}) == 0);
	CHECK(tapeloop::crc32c(bytesOf("123456789")) == 0xE3069283u);
	const std::vector<uint8_t> zeros(32, 0);
	CHECK(tapeloop::crc32c(zeros) == 0x8A9136AAu);
	const std::vector<uint8_t> ones(32, 0xFF);
	CHECK(tapeloop::crc32c(ones) == 0x62A8AB43u);
}

TEST_CASE("crc32c matches the bitwise definition at every length and alignment")
{
	std::vector<uint8_t> data(300);
	for (size_t i = 0; i < data.size(); ++i) {
		data[i] = static_cast<uint8_t>(i * 131 + 7);
	}
	for (size_t start = 0; start < 9; ++start) {
		for (size_t length = 0; start + length <= data.size(); length += 13) {
			const std::span<const uint8_t> part(data.data() + start, length);
			CAPTURE(start, length);
			CHECK(tapeloop::crc32c(part) == bitwise(part));
		}
	}
}

TEST_CASE("crc32c continues from a previous result")
{
	const std::span<const uint8_t> whole = bytesOf("The quick brown fox jumps over the lazy dog");
	for (size_t split = 0; split <= whole.size(); ++split) {
		const uint32_t first = tapeloop::crc32c(whole.first(split));
		CHECK(tapeloop::crc32c(whole.subspan(split), first) == tapeloop::crc32c(whole));
	}
}
