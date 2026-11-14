// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/Text.hpp"

namespace tapeloop {
namespace {

bool isContinuation(unsigned char byte) noexcept
{
	return (byte & 0xC0) == 0x80;
}

} // namespace

size_t utf8SequenceLength(std::string_view text, size_t at) noexcept
{
	const auto lead = static_cast<unsigned char>(text[at]);
	size_t length = 0;
	if (lead < 0x80) {
		return 1;
	}
	if (lead >= 0xC2 && lead <= 0xDF) {
		length = 2;
	} else if (lead >= 0xE0 && lead <= 0xEF) {
		length = 3;
	} else if (lead >= 0xF0 && lead <= 0xF4) {
		length = 4;
	} else {
		return 0;
	}
	if (text.size() - at < length) {
		return 0;
	}
	for (size_t i = 1; i < length; ++i) {
		if (!isContinuation(static_cast<unsigned char>(text[at + i]))) {
			return 0;
		}
	}
	const auto second = static_cast<unsigned char>(text[at + 1]);
	// Overlong forms, UTF-16 surrogates, and code points past U+10FFFF.
	if ((lead == 0xE0 && second < 0xA0) || (lead == 0xED && second > 0x9F) || (lead == 0xF0 && second < 0x90) ||
	    (lead == 0xF4 && second > 0x8F)) {
		return 0;
	}
	return length;
}

char32_t utf8CodePoint(std::string_view sequence) noexcept
{
	if (sequence.empty()) {
		return 0;
	}
	const auto lead = static_cast<unsigned char>(sequence.front());
	if (sequence.size() == 1) {
		return lead;
	}
	// The lead byte of an n-byte sequence keeps 7 - n bits of the code point.
	auto code = static_cast<char32_t>(lead & (0x7Fu >> sequence.size()));
	for (const char c : sequence.substr(1)) {
		code = (code << 6) | (static_cast<unsigned char>(c) & 0x3Fu);
	}
	return code;
}

bool isLayoutControl(char32_t code) noexcept
{
	return code == 0x200E || code == 0x200F || code == 0x2028 || code == 0x2029 ||
	       (code >= 0x202A && code <= 0x202E) || (code >= 0x2066 && code <= 0x2069);
}

} // namespace tapeloop
