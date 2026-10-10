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

} // namespace

// The configuration comes first, as long as its first byte says, then the packet.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	FuzzInput input(data, size);
	const VideoCodec codec = input.flag() ? VideoCodec::Hevc : VideoCodec::H264;
	const std::span<const uint8_t> config = input.bytes(input.byte());
	const std::span<const uint8_t> packet = input.bytes(size);

	const std::vector<uint8_t> out = filtered(codec, config, packet);
	// Never empty, which would end the stream.
	require(out.empty() == packet.empty());
	// Whole pieces of the packet are left out and the rest keeps its order.
	auto at = packet.begin();
	for (const uint8_t byte : out) {
		at = std::find(at, packet.end(), byte);
		require(at != packet.end());
		++at;
	}
	if (config.empty()) {
		require(std::equal(out.begin(), out.end(), packet.begin(), packet.end()));
	}
	// What was taken out is gone: filtering again changes nothing.
	require(filtered(codec, config, out) == out);
	return 0;
}
