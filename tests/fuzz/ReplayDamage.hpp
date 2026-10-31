// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/Crc32c.hpp"
#include "core/ReplayFormat.hpp"

#include "FuzzInput.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace tapeloop::fuzz {

// Flips a few bytes the input chooses; the rest of the input is read afterwards.
inline void damage(FuzzInput &input, std::span<uint8_t> bytes)
{
	for (int flips = input.byte() % 4; flips > 0 && !bytes.empty(); --flips) {
		const size_t at = static_cast<size_t>(input.u64() % bytes.size());
		bytes[at] ^= static_cast<uint8_t>(input.byte() | 1);
	}
}

inline uint64_t load64(std::span<const uint8_t> bytes, size_t at)
{
	uint64_t value = 0;
	for (size_t i = 0; i < 8; ++i) {
		value |= uint64_t{bytes[at + i]} << (8 * i);
	}
	return value;
}

inline void store32(std::span<uint8_t> bytes, size_t at, uint32_t value)
{
	for (size_t i = 0; i < 4; ++i) {
		bytes[at + i] = static_cast<uint8_t>(value >> (8 * i));
	}
}

// Computes the CRCs of a chunk again over what its header and payload hold now, so that
// damage gets past them to the checks behind; returns the new header CRC.
inline uint32_t resealChunk(std::span<uint8_t> chunk)
{
	if (chunk.size() < kChunkHeaderSize) {
		return 0;
	}
	const uint64_t payloadSize = std::min<uint64_t>(load64(chunk, 8), chunk.size() - kChunkHeaderSize);
	store32(chunk, 24, crc32c(chunk.subspan(kChunkHeaderSize, payloadSize)));
	const uint32_t headerCrc = crc32c(chunk.first(28));
	store32(chunk, 28, headerCrc);
	return headerCrc;
}

// The header, index chunk and footer CRCs computed again, for an index at indexOffset.
inline void resealManifest(std::vector<uint8_t> &manifest, uint64_t indexOffset)
{
	const std::span<uint8_t> bytes(manifest);
	if (bytes.size() < indexOffset + kManifestFooterSize) {
		return;
	}
	store32(bytes, 60, crc32c(bytes.first(60)));
	resealChunk(bytes.subspan(indexOffset, bytes.size() - indexOffset - kManifestFooterSize));
	const std::span<uint8_t> footer = bytes.last(kManifestFooterSize);
	store32(footer, 60, crc32c(footer.first(60)));
}

} // namespace tapeloop::fuzz
