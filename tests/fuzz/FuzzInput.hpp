// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <span>

namespace tapeloop::fuzz {

// Reads values from the front of a fuzzer input, integers little-endian. Once the
// input runs out every value is zero, so a harness can always finish its current
// step. Reading from the front keeps hand-made seed inputs easy to write.
class FuzzInput {
public:
	FuzzInput(const uint8_t *data, size_t size) : data_(data, size) {}

	bool empty() const noexcept { return data_.empty(); }

	uint8_t byte() noexcept
	{
		if (data_.empty())
			return 0;
		const uint8_t value = data_.front();
		data_ = data_.subspan(1);
		return value;
	}

	bool flag() noexcept { return (byte() & 1) != 0; }

	uint64_t u64() noexcept
	{
		uint64_t value = 0;
		for (int shift = 0; shift < 64; shift += 8)
			value |= uint64_t{byte()} << shift;
		return value;
	}

	int64_t i64() noexcept { return static_cast<int64_t>(u64()); }
	// Takes 8 bytes like every other integer and keeps the low 32 bits.
	int32_t i32() noexcept { return static_cast<int32_t>(static_cast<uint32_t>(u64())); }

	// A value in [0, limit). Limit must be positive.
	uint64_t below(uint64_t limit) noexcept { return u64() % limit; }

	std::span<const uint8_t> bytes(size_t count) noexcept
	{
		const std::span<const uint8_t> taken = data_.first(std::min(count, data_.size()));
		data_ = data_.subspan(taken.size());
		return taken;
	}

private:
	std::span<const uint8_t> data_;
};

// libFuzzer reports an abort as a crash and keeps the input that caused it; the line
// tells which check failed.
inline void require(bool condition, std::source_location location = std::source_location::current())
{
	if (!condition) {
		std::fprintf(stderr, "check failed at %s:%u\n", location.file_name(),
			     static_cast<unsigned>(location.line()));
		std::abort();
	}
}

} // namespace tapeloop::fuzz
