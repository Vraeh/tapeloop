// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/ParameterSets.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <initializer_list>
#include <vector>

using tapeloop::VideoCodec;

namespace {

using Bytes = std::vector<uint8_t>;

Bytes join(std::initializer_list<Bytes> parts)
{
	Bytes all;
	for (const Bytes &part : parts) {
		all.insert(all.end(), part.begin(), part.end());
	}
	return all;
}

Bytes filtered(VideoCodec codec, const Bytes &config, const Bytes &packet)
{
	Bytes out(packet.size() + 1, 0xEE);
	const size_t written = tapeloop::copyWithoutKnownParameterSets(codec, config, packet, out);
	REQUIRE(written <= packet.size());
	CHECK(out[packet.size()] == 0xEE);
	out.resize(written);
	return out;
}

const Bytes kLong = {0, 0, 0, 1};
const Bytes kShort = {0, 0, 1};
// H.264 NAL units: a sequence and a picture parameter set, an access unit delimiter, an
// SEI and the slices of an IDR picture, with an emulation prevention byte.
const Bytes kSps = {0x67, 0x64, 0x00, 0x28, 0xAC, 0xD9};
const Bytes kPps = {0x68, 0xEB, 0xE3, 0xCB};
const Bytes kAud = {0x09, 0xF0};
const Bytes kSei = {0x06, 0x05, 0x02, 0xAA, 0xBB, 0x80};
const Bytes kIdr = {0x65, 0x88, 0x84, 0x00, 0x00, 0x03, 0x01, 0x20};
const Bytes kSlice = {0x41, 0x9A, 0x02, 0x00, 0x00, 0x00, 0x00, 0x01, 0x07};

} // namespace

TEST_CASE("parameter sets the configuration holds go, everything else stays")
{
	const Bytes config = join({kLong, kSps, kLong, kPps});
	const Bytes keyframe = join({kLong, kAud, kLong, kSps, kLong, kPps, kShort, kSei, kShort, kIdr});
	CHECK(filtered(VideoCodec::H264, config, keyframe) == join({kLong, kAud, kShort, kSei, kShort, kIdr}));
	// Three-byte start codes in the configuration or the packet change nothing.
	CHECK(filtered(VideoCodec::H264, join({kShort, kSps, kShort, kPps}), keyframe) ==
	      join({kLong, kAud, kShort, kSei, kShort, kIdr}));

	// From the first slice on, the packet is copied as it is, start codes in its data
	// included. An IDR slice is one.
	const Bytes inside = join({kLong, kSps, kLong, kSlice, kLong, kSps});
	CHECK(filtered(VideoCodec::H264, config, inside) == join({kLong, kSlice, kLong, kSps}));
	CHECK(filtered(VideoCodec::H264, config, join({kLong, kSps, kLong, kIdr, kLong, kPps})) ==
	      join({kLong, kIdr, kLong, kPps}));

	// A parameter set the configuration does not hold, as a new one in the stream, keeps
	// the known ones that come with it: they refer to it.
	const Bytes otherSps = {0x67, 0x64, 0x00, 0x1F, 0xAC, 0xD9};
	const Bytes changed = join({kLong, otherSps, kLong, kPps, kLong, kIdr});
	CHECK(filtered(VideoCodec::H264, config, changed) == changed);
	CHECK(filtered(VideoCodec::H264, config, join({kLong, kSps, kLong, otherSps, kLong, kIdr})) ==
	      join({kLong, kSps, kLong, otherSps, kLong, kIdr}));
	// Zero bytes between a parameter set and the next start code are not part of it; the
	// last of them belongs to that start code.
	CHECK(filtered(VideoCodec::H264, config,
		       join({kLong, kSps, Bytes{0, 0}, kLong, kPps, Bytes{0}, kShort, kIdr})) == join({kLong, kIdr}));
	// One that only begins as a known one does too.
	const Bytes longer = join({kSps, Bytes{0x01}});
	CHECK(filtered(VideoCodec::H264, config, join({kLong, longer, kLong, kIdr})) ==
	      join({kLong, longer, kLong, kIdr}));
}

TEST_CASE("HEVC parameter sets go as H.264 ones do")
{
	const Bytes vps = {0x40, 0x01, 0x0C, 0x01, 0xFF};
	const Bytes sps = {0x42, 0x01, 0x01, 0x01, 0x60};
	const Bytes pps = {0x44, 0x01, 0xC1, 0x72};
	const Bytes idr = {0x26, 0x01, 0xAF, 0x13};
	const Bytes config = join({kLong, vps, kLong, sps, kLong, pps});
	CHECK(filtered(VideoCodec::Hevc, config, join({kLong, vps, kLong, sps, kLong, pps, kLong, idr})) ==
	      join({kLong, idr}));
	// A new SPS keeps the known VPS and PPS with it.
	const Bytes otherSps = {0x42, 0x01, 0x01, 0x02, 0x60};
	const Bytes changed = join({kLong, vps, kLong, otherSps, kLong, pps, kLong, idr});
	CHECK(filtered(VideoCodec::Hevc, config, changed) == changed);
	// Every IRAP and trailing picture type is a slice, after which nothing goes.
	for (const int type : {0, 1, 16, 19, 20, 21, 31}) {
		const Bytes slice = {static_cast<uint8_t>(type << 1), 0x01, 0xAF};
		CHECK(filtered(VideoCodec::Hevc, config, join({kLong, vps, kLong, slice, kLong, pps})) ==
		      join({kLong, slice, kLong, pps}));
	}
	// HEVC's slice types are not H.264's: as HEVC, 0x26 is a slice; as H.264 a slice
	// would be 0x65.
	CHECK(filtered(VideoCodec::H264, config, join({kLong, vps, kLong, idr})) == join({kLong, vps, kLong, idr}));
}

TEST_CASE("a packet the filter cannot read is copied whole")
{
	const Bytes config = join({kLong, kSps, kLong, kPps});
	// Length-prefixed, as in MP4.
	const Bytes lengthPrefixed = {0x00, 0x00, 0x00, 0x06, 0x67, 0x64, 0x00, 0x28, 0xAC, 0xD9};
	CHECK(filtered(VideoCodec::H264, config, lengthPrefixed) == lengthPrefixed);
	// A start code that does not open the packet.
	const Bytes late = join({Bytes{0xAA}, kShort, kSps, kLong, kIdr});
	CHECK(filtered(VideoCodec::H264, config, late) == late);
	const Bytes padded = join({Bytes{0, 0}, kLong, kSps, kLong, kIdr});
	CHECK(filtered(VideoCodec::H264, config, padded) == padded);
	// No configuration to compare with.
	const Bytes keyframe = join({kLong, kSps, kLong, kPps, kLong, kIdr});
	CHECK(filtered(VideoCodec::H264, {}, keyframe) == keyframe);
	// A configuration that is not Annex B holds no parameter set the filter can find.
	CHECK(filtered(VideoCodec::H264, Bytes{0x01, 0x64, 0x00, 0x28}, keyframe) == keyframe);
	// Nothing but known parameter sets: an empty packet would end the stream.
	const Bytes setsOnly = join({kLong, kSps, kLong, kPps});
	CHECK(filtered(VideoCodec::H264, config, setsOnly) == setsOnly);
	CHECK(filtered(VideoCodec::H264, config, {}).empty());
	CHECK(filtered(VideoCodec::H264, config, Bytes{0, 0}) == Bytes({0, 0}));
	CHECK(filtered(VideoCodec::H264, config, kLong) == kLong);
}
