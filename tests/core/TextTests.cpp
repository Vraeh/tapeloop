// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/Text.hpp"

#include <catch2/catch_test_macros.hpp>

using tapeloop::isLayoutControl;
using tapeloop::utf8CodePoint;
using tapeloop::utf8SequenceLength;

TEST_CASE("a UTF-8 sequence is measured from its lead byte")
{
	CHECK(utf8SequenceLength("a", 0) == 1);
	CHECK(utf8SequenceLength("a\xC3\xB1", 1) == 2);
	CHECK(utf8SequenceLength("\xE2\x80\xA8", 0) == 3);
	CHECK(utf8SequenceLength("\xF0\x9F\x8E\xA5", 0) == 4);
	// A continuation byte, a sequence cut short, an overlong form and a surrogate.
	CHECK(utf8SequenceLength("\xB1", 0) == 0);
	CHECK(utf8SequenceLength("\xF0\x9F\x8E", 0) == 0);
	CHECK(utf8SequenceLength("\xC1\xBF", 0) == 0);
	CHECK(utf8SequenceLength("\xED\xA0\x80", 0) == 0);
}

TEST_CASE("a UTF-8 sequence decodes to its code point")
{
	CHECK(utf8CodePoint("") == 0);
	CHECK(utf8CodePoint("A") == 0x41);
	CHECK(utf8CodePoint("\x7F") == 0x7F);
	CHECK(utf8CodePoint("\xC2\x80") == 0x80);
	CHECK(utf8CodePoint("\xC3\xB1") == 0xF1);
	CHECK(utf8CodePoint("\xDF\xBF") == 0x7FF);
	CHECK(utf8CodePoint("\xE0\xA0\x80") == 0x800);
	CHECK(utf8CodePoint("\xE2\x80\xA8") == 0x2028);
	CHECK(utf8CodePoint("\xEF\xBF\xBF") == 0xFFFF);
	CHECK(utf8CodePoint("\xF0\x90\x80\x80") == 0x10000);
	CHECK(utf8CodePoint("\xF0\x9F\x8E\xA5") == 0x1F3A5);
	CHECK(utf8CodePoint("\xF4\x8F\xBF\xBF") == 0x10FFFF);
}

TEST_CASE("the characters that change how text is laid out are told apart from their neighbors")
{
	const char32_t layout[] = {0x200E, 0x200F, 0x2028, 0x2029, 0x202A, 0x202B, 0x202C,
				   0x202D, 0x202E, 0x2066, 0x2067, 0x2068, 0x2069};
	for (const char32_t code : layout) {
		CAPTURE(static_cast<unsigned>(code));
		CHECK(isLayoutControl(code));
	}
	// The joiners stay: emoji and some scripts are written with them.
	const char32_t others[] = {0x20, 0x41, 0x200C, 0x200D, 0x2010, 0x2027, 0x202F, 0x2065, 0x206A, 0xFFFD, 0x1F3A5};
	for (const char32_t code : others) {
		CAPTURE(static_cast<unsigned>(code));
		CHECK_FALSE(isLayoutControl(code));
	}
}
