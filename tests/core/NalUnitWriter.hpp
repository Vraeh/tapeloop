// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace tapeloop::test {

// A NAL unit as an encoder writes it: the fields, a stop bit, and a 3 wherever two zeros
// come before a byte below 4, which would otherwise read as a start code.
struct NalUnit {
	std::vector<uint8_t> bytes;
	// How many bytes from the start hold every field written.
	size_t fields = 0;
};

class NalUnitWriter {
public:
	explicit NalUnitWriter(std::vector<uint8_t> header) : header_(std::move(header)) {}

	void bit(bool value)
	{
		if (used_ % 8 == 0) {
			payload_.push_back(0);
		}
		if (value) {
			payload_.back() |= static_cast<uint8_t>(0x80U >> (used_ % 8));
		}
		++used_;
	}

	void bits(uint64_t value, int count)
	{
		for (int i = count - 1; i >= 0; --i) {
			bit(((value >> i) & 1U) != 0);
		}
	}

	void ue(uint64_t value)
	{
		const uint64_t code = value + 1;
		int length = 0;
		while ((code >> (length + 1)) != 0) {
			++length;
		}
		bits(0, length);
		bits(code, length + 1);
	}

	void se(int64_t value)
	{
		ue(value > 0 ? 2 * static_cast<uint64_t>(value) - 1 : 2 * static_cast<uint64_t>(-value));
	}

	NalUnit finish() const
	{
		const size_t fieldBytes = (used_ + 7) / 8;
		std::vector<uint8_t> raw = payload_;
		if (used_ % 8 == 0) {
			raw.push_back(0x80);
		} else {
			raw.back() |= static_cast<uint8_t>(0x80U >> (used_ % 8));
		}
		NalUnit unit;
		unit.bytes = header_;
		unit.fields = header_.size();
		int zeros = 0;
		for (size_t i = 0; i < raw.size(); ++i) {
			if (zeros >= 2 && raw[i] <= 3) {
				unit.bytes.push_back(3);
				zeros = 0;
			}
			unit.bytes.push_back(raw[i]);
			zeros = raw[i] == 0 ? zeros + 1 : 0;
			if (i < fieldBytes) {
				unit.fields = unit.bytes.size();
			}
		}
		return unit;
	}

private:
	std::vector<uint8_t> header_;
	std::vector<uint8_t> payload_;
	size_t used_ = 0;
};

} // namespace tapeloop::test
