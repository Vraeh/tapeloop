// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include <cstddef>
#include <string_view>

namespace tapeloop {

// The length of the valid UTF-8 sequence that starts at text[at], or zero.
size_t utf8SequenceLength(std::string_view text, size_t at) noexcept;

// The code point of one valid UTF-8 sequence, as utf8SequenceLength measures them.
char32_t utf8CodePoint(std::string_view sequence) noexcept;

// The line and paragraph separators and the bidirectional marks, embeddings, overrides
// and isolates. Text with them shows on more lines, or in another order, than it is
// stored in, so one row of a list can pass for two rows or for another one.
bool isLayoutControl(char32_t code) noexcept;

} // namespace tapeloop
