// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/ParameterSets.hpp"

#include <algorithm>
#include <optional>

namespace tapeloop {
namespace {

// Finds the start code at or after from: where it begins, a zero byte before 00 00 01
// counting as its own, and its length.
bool findStartCode(std::span<const uint8_t> data, size_t from, size_t &at, size_t &length) noexcept
{
	for (size_t i = from; i + 3 <= data.size(); ++i) {
		if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
			at = i > from && data[i - 1] == 0 ? i - 1 : i;
			length = i + 3 - at;
			return true;
		}
	}
	return false;
}

bool startsWithStartCode(std::span<const uint8_t> data, size_t &length) noexcept
{
	size_t at = 0;
	return findStartCode(data.first(std::min<size_t>(data.size(), 4)), 0, at, length) && at == 0;
}

// A NAL unit, from after its start code to the next start code or the end, without the
// zero bytes that may follow it there, which a NAL unit never ends with, and where that
// next start code is.
struct Nal {
	std::span<const uint8_t> bytes;
	size_t next = 0;
	size_t nextLength = 0;
};

Nal nalAfter(std::span<const uint8_t> data, size_t start, size_t length) noexcept
{
	const size_t begin = start + length;
	Nal nal;
	if (!findStartCode(data, begin, nal.next, nal.nextLength)) {
		nal.next = data.size();
	}
	nal.bytes = data.subspan(begin, nal.next - begin);
	while (!nal.bytes.empty() && nal.bytes.back() == 0) {
		nal.bytes = nal.bytes.first(nal.bytes.size() - 1);
	}
	return nal;
}

bool isSlice(VideoCodec codec, uint8_t header) noexcept
{
	if (codec == VideoCodec::H264) {
		const uint8_t type = header & 0x1F;
		return type >= 1 && type <= 5;
	}
	return ((header >> 1) & 0x3F) <= 31;
}

bool isParameterSet(VideoCodec codec, uint8_t header) noexcept
{
	if (codec == VideoCodec::H264) {
		const uint8_t type = header & 0x1F;
		return type == 7 || type == 8;
	}
	const uint8_t type = (header >> 1) & 0x3F;
	return type >= 32 && type <= 34;
}

// The NAL unit after the start code at start, or nothing when it is a slice. A slice is
// most of a packet, so it is told from its first byte without looking for its end; a
// zero there may be the start of the next start code, and then the end is found first.
std::optional<Nal> nalBeforeSlice(VideoCodec codec, std::span<const uint8_t> data, size_t start, size_t length) noexcept
{
	const size_t begin = start + length;
	if (begin < data.size() && data[begin] != 0 && isSlice(codec, data[begin])) {
		return std::nullopt;
	}
	const Nal nal = nalAfter(data, start, length);
	if (!nal.bytes.empty() && isSlice(codec, nal.bytes.front())) {
		return std::nullopt;
	}
	return nal;
}

bool configHolds(std::span<const uint8_t> config, std::span<const uint8_t> wanted) noexcept
{
	size_t length = 0;
	if (!startsWithStartCode(config, length)) {
		return false;
	}
	for (size_t start = 0; start < config.size();) {
		const Nal nal = nalAfter(config, start, length);
		if (std::equal(nal.bytes.begin(), nal.bytes.end(), wanted.begin(), wanted.end())) {
			return true;
		}
		start = nal.next;
		length = nal.nextLength;
	}
	return false;
}

// Whether the parameter sets before the packet's first slice can all go: there is one,
// the configuration holds each, and something else is left.
bool canDropSets(VideoCodec codec, std::span<const uint8_t> config, std::span<const uint8_t> packet,
		 size_t length) noexcept
{
	bool sets = false;
	bool kept = false;
	for (size_t start = 0; start < packet.size();) {
		const std::optional<Nal> nal = nalBeforeSlice(codec, packet, start, length);
		if (!nal) {
			return sets;
		}
		if (nal->bytes.empty() || !isParameterSet(codec, nal->bytes.front())) {
			kept = true;
		} else if (configHolds(config, nal->bytes)) {
			sets = true;
		} else {
			return false;
		}
		start = nal->next;
		length = nal->nextLength;
	}
	return sets && kept;
}

} // namespace

size_t copyWithoutKnownParameterSets(VideoCodec codec, std::span<const uint8_t> config, std::span<const uint8_t> packet,
				     std::span<uint8_t> out) noexcept
{
	size_t written = 0;
	const auto copy = [&](size_t from, size_t to) {
		const std::span<const uint8_t> piece = packet.subspan(from, to - from);
		std::copy(piece.begin(), piece.end(), out.subspan(written).begin());
		written += piece.size();
	};
	size_t length = 0;
	if (config.empty() || !startsWithStartCode(packet, length) || !canDropSets(codec, config, packet, length)) {
		copy(0, packet.size());
		return written;
	}

	for (size_t start = 0; start < packet.size();) {
		const std::optional<Nal> nal = nalBeforeSlice(codec, packet, start, length);
		if (!nal) {
			copy(start, packet.size());
			return written;
		}
		if (nal->bytes.empty() || !isParameterSet(codec, nal->bytes.front())) {
			copy(start, nal->next);
		}
		start = nal->next;
		length = nal->nextLength;
	}
	return written;
}

} // namespace tapeloop
