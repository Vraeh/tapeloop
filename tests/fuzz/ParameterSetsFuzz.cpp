// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/ParameterSets.hpp"

#include "FuzzInput.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

using tapeloop::VideoCodec;
using tapeloop::fuzz::FuzzInput;
using tapeloop::fuzz::require;

namespace {

std::vector<uint8_t> filtered(VideoCodec codec, std::span<const uint8_t> config, std::span<const uint8_t> packet)
{
	// One byte more than the packet, to catch a write past what the filter may use.
	std::vector<uint8_t> out(packet.size() + 1, 0xEE);
	const size_t written = tapeloop::copyWithoutKnownParameterSets(codec, config, packet, out);
	require(written <= packet.size());
	require(out[packet.size()] == 0xEE);
	out.resize(written);
	return out;
}

// A NAL unit with its start code: the bytes from start to end, the unit itself from
// payload on, without the zero bytes before the next start code.
struct Unit {
	size_t start = 0;
	size_t end = 0;
	std::span<const uint8_t> nal;
};

// The model the filter is checked against: the data cut at every 00 00 01, a zero byte
// right before one belonging to it. Empty when the data does not open with a start code.
std::vector<Unit> unitsOf(std::span<const uint8_t> data)
{
	size_t payload = 0;
	if (data.size() >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1) {
		payload = 3;
	} else if (data.size() >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) {
		payload = 4;
	} else {
		return {};
	}
	std::vector<Unit> units;
	size_t start = 0;
	for (;;) {
		size_t code = payload;
		while (code + 3 <= data.size() && !(data[code] == 0 && data[code + 1] == 0 && data[code + 2] == 1)) {
			++code;
		}
		const bool last = code + 3 > data.size();
		const size_t end = last ? data.size() : (code > payload && data[code - 1] == 0 ? code - 1 : code);
		size_t nalEnd = end;
		while (nalEnd > payload && data[nalEnd - 1] == 0) {
			--nalEnd;
		}
		units.push_back({start, end, data.subspan(payload, nalEnd - payload)});
		if (last) {
			return units;
		}
		start = end;
		payload = code + 3;
	}
}

bool slice(VideoCodec codec, std::span<const uint8_t> nal)
{
	if (nal.empty()) {
		return false;
	}
	const int type = codec == VideoCodec::H264 ? nal[0] & 0x1F : (nal[0] >> 1) & 0x3F;
	return codec == VideoCodec::H264 ? type >= 1 && type <= 5 : type <= 31;
}

bool parameterSet(VideoCodec codec, std::span<const uint8_t> nal)
{
	if (nal.empty()) {
		return false;
	}
	const int type = codec == VideoCodec::H264 ? nal[0] & 0x1F : (nal[0] >> 1) & 0x3F;
	return codec == VideoCodec::H264 ? type == 7 || type == 8 : type >= 32 && type <= 34;
}

// What the filter has to give: the packet without the parameter sets before its first
// slice when the configuration holds each of them and something else is left, and the
// whole packet otherwise.
std::vector<uint8_t> expected(VideoCodec codec, std::span<const uint8_t> config, std::span<const uint8_t> packet)
{
	const std::vector<uint8_t> whole(packet.begin(), packet.end());
	const std::vector<Unit> known = unitsOf(config);
	const std::vector<Unit> units = unitsOf(packet);
	if (known.empty() || units.empty()) {
		return whole;
	}
	size_t firstSlice = 0;
	while (firstSlice < units.size() && !slice(codec, units[firstSlice].nal)) {
		++firstSlice;
	}
	bool sets = false;
	bool kept = firstSlice < units.size();
	for (size_t i = 0; i < firstSlice; ++i) {
		const std::span<const uint8_t> nal = units[i].nal;
		if (!parameterSet(codec, nal)) {
			kept = true;
			continue;
		}
		const bool held = std::any_of(known.begin(), known.end(), [&](const Unit &unit) {
			return std::equal(unit.nal.begin(), unit.nal.end(), nal.begin(), nal.end());
		});
		if (!held) {
			return whole;
		}
		sets = true;
	}
	if (!sets || !kept) {
		return whole;
	}
	std::vector<uint8_t> out;
	for (size_t i = 0; i < units.size(); ++i) {
		if (i < firstSlice && parameterSet(codec, units[i].nal)) {
			continue;
		}
		out.insert(out.end(), packet.begin() + static_cast<std::ptrdiff_t>(units[i].start),
			   packet.begin() + static_cast<std::ptrdiff_t>(units[i].end));
	}
	return out;
}

} // namespace

// The configuration comes first, as long as its first byte says, then the packet.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	FuzzInput input(data, size);
	const VideoCodec codec = input.flag() ? VideoCodec::Hevc : VideoCodec::H264;
	const std::span<const uint8_t> config = input.bytes(input.byte());
	const std::span<const uint8_t> packet = input.bytes(size);

	const std::vector<uint8_t> out = filtered(codec, config, packet);
	require(out == expected(codec, config, packet));
	// Never empty, which would end the stream.
	require(out.empty() == packet.empty());
	// What was taken out is gone: filtering again changes nothing.
	require(filtered(codec, config, out) == out);
	return 0;
}
