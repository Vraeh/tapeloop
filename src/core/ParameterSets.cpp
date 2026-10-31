// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/ParameterSets.hpp"

#include <algorithm>
#include <array>
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

// The bits of a NAL unit after its header, without the bytes that keep it from holding a
// start code, read as FFmpeg's muxer reads them. Reading past the end gives zeros and
// marks the reader failed.
class RbspReader {
public:
	explicit RbspReader(std::span<const uint8_t> payload) noexcept : data_(payload) {}

	bool failed() const noexcept { return failed_; }

	uint32_t bit() noexcept
	{
		if (byte_ >= data_.size()) {
			failed_ = true;
			return 0;
		}
		const uint32_t value = (static_cast<uint32_t>(data_[byte_]) >> (7 - bit_)) & 1U;
		if (++bit_ == 8) {
			bit_ = 0;
			++byte_;
			if (byte_ >= 2 && byte_ < data_.size() && data_[byte_] == 3 && data_[byte_ - 1] == 0 &&
			    data_[byte_ - 2] == 0) {
				++byte_;
			}
		}
		return value;
	}

	uint32_t bits(uint32_t count) noexcept
	{
		uint32_t value = 0;
		for (uint32_t i = 0; i < count; ++i) {
			value = (value << 1) | bit();
		}
		return value;
	}

	void skip(uint32_t count) noexcept
	{
		for (uint32_t i = 0; i < count; ++i) {
			bit();
		}
	}

	// An Exp-Golomb code. One of 32 leading zeros or more has no value FFmpeg reads the
	// same way, so it fails.
	uint32_t ue() noexcept
	{
		uint32_t zeros = 0;
		while (bit() == 0) {
			if (failed_ || ++zeros == 32) {
				failed_ = true;
				return 0;
			}
		}
		return ((1U << zeros) - 1) + bits(zeros);
	}

	int64_t se() noexcept
	{
		const uint32_t code = ue();
		const auto half = static_cast<int64_t>(code >> 1);
		return (code & 1U) != 0 ? half + 1 : -half;
	}

private:
	std::span<const uint8_t> data_;
	size_t byte_ = 0;
	uint32_t bit_ = 0;
	bool failed_ = false;
};

void skipHevcProfileTierLevel(RbspReader &reader, uint32_t subLayers) noexcept
{
	reader.skip(96);
	std::array<bool, 8> profiles{};
	std::array<bool, 8> levels{};
	for (uint32_t i = 0; i < subLayers; ++i) {
		profiles[i] = reader.bit() != 0;
		levels[i] = reader.bit() != 0;
	}
	if (subLayers > 0) {
		reader.skip(2 * (8 - subLayers));
	}
	for (uint32_t i = 0; i < subLayers; ++i) {
		reader.skip((profiles[i] ? 88U : 0U) + (levels[i] ? 8U : 0U));
	}
}

void skipHevcScalingLists(RbspReader &reader) noexcept
{
	for (uint32_t size = 0; size < 4; ++size) {
		for (uint32_t matrix = 0; matrix < (size == 3 ? 2U : 6U); ++matrix) {
			if (reader.bit() == 0) {
				reader.ue();
				continue;
			}
			const uint32_t coefficients = std::min(64U, 1U << (4 + 2 * size)) + (size > 1 ? 1U : 0U);
			for (uint32_t i = 0; i < coefficients; ++i) {
				reader.ue();
			}
		}
	}
}

// Reads the fields in the order of hvcc_parse_sps in FFmpeg's libavformat/hevc.c, and
// ends after the short-term reference picture sets: FFmpeg bounds every count it reads
// after them. The muxer skips the sets of most further layers; they are read all the same.
bool hevcSpsIsWhole(std::span<const uint8_t> nal) noexcept
{
	if (nal.size() < 2) {
		return false;
	}
	const uint32_t layer = ((nal[0] & 1U) << 5) | (nal[1] >> 3);
	RbspReader reader(nal.subspan(2));
	reader.skip(4);
	const uint32_t subLayers = reader.bits(3);
	// A further layer's set that takes its sub-layers from the video parameter set leaves
	// out the fields it shares with it.
	const bool shared = layer != 0 && subLayers == 7;
	if (!shared) {
		reader.skip(1);
		skipHevcProfileTierLevel(reader, subLayers);
	}
	reader.ue();
	if (shared) {
		if (reader.bit() != 0) {
			reader.skip(8);
		}
	} else {
		// FFmpeg keeps chroma_format_idc in a byte.
		if (static_cast<uint8_t>(reader.ue()) == 3) {
			reader.skip(1);
		}
		reader.ue();
		reader.ue();
		if (reader.bit() != 0) {
			for (int i = 0; i < 4; ++i) {
				reader.ue();
			}
		}
		reader.ue();
		reader.ue();
	}
	reader.ue();
	if (!shared) {
		for (uint32_t i = reader.bit() != 0 ? 0 : subLayers; i <= subLayers; ++i) {
			reader.ue();
			reader.ue();
			reader.ue();
		}
	}
	for (int i = 0; i < 6; ++i) {
		reader.ue();
	}
	if (reader.bit() != 0) {
		if (shared && reader.bit() != 0) {
			reader.skip(6);
		} else if (reader.bit() != 0) {
			skipHevcScalingLists(reader);
		}
	}
	reader.skip(2);
	if (reader.bit() != 0) {
		reader.skip(8);
		reader.ue();
		reader.ue();
		reader.skip(1);
	}

	constexpr uint32_t kMaxShortTermSets = 64;
	const uint32_t sets = reader.ue();
	if (sets > kMaxShortTermSets) {
		return false;
	}
	// Each set gives the number of pictures it refers to, which a set predicted from it
	// counts through. The loops end at the end of the data, as every round reads a bit.
	std::array<uint64_t, kMaxShortTermSets> pictures{};
	for (uint32_t i = 0; i < sets && !reader.failed(); ++i) {
		if (i > 0 && reader.bit() != 0) {
			reader.skip(1);
			reader.ue();
			for (uint64_t j = 0; j <= pictures[i - 1] && !reader.failed(); ++j) {
				if (reader.bit() != 0 || reader.bit() != 0) {
					++pictures[i];
				}
			}
		} else {
			const uint64_t negative = reader.ue();
			pictures[i] = negative + reader.ue();
			for (uint64_t j = 0; j < pictures[i] && !reader.failed(); ++j) {
				reader.ue();
				reader.skip(1);
			}
		}
	}
	return !reader.failed();
}

bool isHighProfile(uint32_t profile) noexcept
{
	constexpr std::array<uint32_t, 12> kHigh{100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134};
	return std::find(kHigh.begin(), kHigh.end(), profile) != kHigh.end();
}

// Reads the fields in the order of ff_avc_decode_sps in FFmpeg's libavformat/avc.c, and
// ends after the picture order count cycle. The muxer reads only the first set, and only
// for some profiles, but a set cut short has no place in a configuration either way.
bool h264SpsIsWhole(std::span<const uint8_t> nal) noexcept
{
	RbspReader reader(nal.subspan(1));
	const uint32_t profile = reader.bits(8);
	reader.skip(16);
	reader.ue();
	if (isHighProfile(profile)) {
		const auto chroma = static_cast<uint8_t>(reader.ue());
		if (chroma == 3) {
			reader.skip(1);
		}
		reader.ue();
		reader.ue();
		reader.skip(1);
		if (reader.bit() != 0) {
			for (int i = 0; i < (chroma != 3 ? 8 : 12); ++i) {
				if (reader.bit() == 0) {
					continue;
				}
				// A list ends early at a scale of zero.
				int64_t last = 8;
				int64_t next = 8;
				for (int j = 0; j < (i < 6 ? 16 : 64) && next != 0; ++j) {
					next = (last + reader.se()) & 0xFF;
					last = next != 0 ? next : last;
				}
			}
		}
	}
	reader.ue();
	const uint32_t order = reader.ue();
	if (order == 0) {
		reader.ue();
	} else if (order == 1) {
		reader.skip(1);
		reader.ue();
		reader.ue();
		const uint32_t cycle = reader.ue();
		for (uint32_t i = 0; i < cycle && !reader.failed(); ++i) {
			reader.ue();
		}
	}
	return !reader.failed();
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

bool sequenceParameterSetsAreWhole(VideoCodec codec, std::span<const uint8_t> data) noexcept
{
	size_t length = 0;
	if (!startsWithStartCode(data, length)) {
		return true;
	}
	for (size_t start = 0; start < data.size();) {
		const Nal nal = nalAfter(data, start, length);
		if (!nal.bytes.empty()) {
			const uint8_t header = nal.bytes.front();
			if (codec == VideoCodec::H264 ? (header & 0x1F) == 7 && !h264SpsIsWhole(nal.bytes)
						      : ((header >> 1) & 0x3F) == 33 && !hevcSpsIsWhole(nal.bytes)) {
				return false;
			}
		}
		start = nal.next;
		length = nal.nextLength;
	}
	return true;
}

} // namespace tapeloop
