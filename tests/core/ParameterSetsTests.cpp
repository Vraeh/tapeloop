// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "NalUnitWriter.hpp"

#include "core/ParameterSets.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

using tapeloop::VideoCodec;
using tapeloop::test::NalUnit;
using tapeloop::test::NalUnitWriter;

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

namespace {

// The general profile, tier and level of HEVC Main at level 3.1, with zeros that need
// emulation prevention bytes.
void writeHevcProfile(NalUnitWriter &writer)
{
	writer.bits(0x01, 8);
	writer.bits(0x60000000, 32);
	writer.bits(0x900000000000, 48);
	writer.bits(93, 8);
}

// What a test changes in an HEVC SPS, which is written up to its short-term reference
// picture sets, the last fields the check reads.
struct HevcSps {
	uint8_t layer = 0;
	// sps_max_sub_layers_minus1, or for a further layer sps_ext_or_max_sub_layers_minus1,
	// where 7 takes the rest from the video parameter set.
	uint32_t subLayers = 0;
	bool subLayerProfiles = false;
	bool orderingForEach = false;
	uint64_t chroma = 1;
	bool window = false;
	bool scaling = false;
	bool pcm = false;
	uint64_t sets = 1;
	bool predicted = false;
};

NalUnit hevcSps(const HevcSps &sps)
{
	NalUnitWriter writer(
		{static_cast<uint8_t>(0x42 | (sps.layer >> 5)), static_cast<uint8_t>((sps.layer << 3) | 1)});
	writer.bits(0, 4);
	writer.bits(sps.subLayers, 3);
	const bool shared = sps.layer != 0 && sps.subLayers == 7;
	if (!shared) {
		writer.bit(true);
		writeHevcProfile(writer);
		for (uint32_t i = 0; i < sps.subLayers; ++i) {
			writer.bit(sps.subLayerProfiles);
			writer.bit(i % 2 == 0);
		}
		if (sps.subLayers > 0) {
			writer.bits(0, static_cast<int>(2 * (8 - sps.subLayers)));
		}
		for (uint32_t i = 0; i < sps.subLayers; ++i) {
			if (sps.subLayerProfiles) {
				writer.bits(0x01, 8);
				writer.bits(0x60000000, 32);
				writer.bits(0x900000000000, 48);
			}
			if (i % 2 == 0) {
				writer.bits(93, 8);
			}
		}
	}
	writer.ue(0);
	if (shared) {
		writer.bit(true);
		writer.bits(0, 8);
	} else {
		writer.ue(sps.chroma);
		// FFmpeg keeps chroma_format_idc in a byte.
		if (static_cast<uint8_t>(sps.chroma) == 3) {
			writer.bit(false);
		}
		writer.ue(1920);
		writer.ue(1080);
		writer.bit(sps.window);
		if (sps.window) {
			writer.ue(0);
			writer.ue(0);
			writer.ue(0);
			writer.ue(4);
		}
		writer.ue(0);
		writer.ue(0);
	}
	writer.ue(4);
	if (!shared) {
		writer.bit(sps.orderingForEach);
		for (uint32_t i = sps.orderingForEach ? 0 : sps.subLayers; i <= sps.subLayers; ++i) {
			writer.ue(4);
			writer.ue(2);
			writer.ue(0);
		}
	}
	for (const uint64_t value : std::initializer_list<uint64_t>{0, 2, 0, 3, 3, 3}) {
		writer.ue(value);
	}
	writer.bit(sps.scaling);
	if (sps.scaling && shared) {
		// Inferred from another layer.
		writer.bit(true);
		writer.bits(5, 6);
	} else if (sps.scaling) {
		writer.bit(true);
		for (int size = 0; size < 4; ++size) {
			for (int matrix = 0; matrix < (size == 3 ? 2 : 6); ++matrix) {
				// The first of each size is listed, the rest copy another.
				writer.bit(matrix == 0);
				if (matrix != 0) {
					writer.ue(1);
					continue;
				}
				if (size > 1) {
					writer.se(8);
				}
				for (int i = 0; i < std::min(64, 1 << (4 + 2 * size)); ++i) {
					writer.se(i % 3 - 1);
				}
			}
		}
	}
	writer.bits(0, 2);
	writer.bit(sps.pcm);
	if (sps.pcm) {
		writer.bits(0x77, 8);
		writer.ue(0);
		writer.ue(1);
		writer.bit(true);
	}
	writer.ue(sps.sets);
	uint64_t pictures = 0;
	for (uint64_t i = 0; i < sps.sets; ++i) {
		if (i > 0) {
			writer.bit(sps.predicted);
		}
		if (i > 0 && sps.predicted) {
			// From the set before, with one picture more: every picture of that set
			// and the set itself are used.
			writer.bit(false);
			writer.ue(0);
			for (uint64_t j = 0; j <= pictures; ++j) {
				writer.bit(true);
			}
			++pictures;
		} else {
			writer.ue(2);
			writer.ue(1);
			for (int j = 0; j < 3; ++j) {
				writer.ue(0);
				writer.bit(true);
			}
			pictures = 3;
		}
	}
	return writer.finish();
}

// What a test changes in an H.264 SPS, which is written up to its picture order count
// cycle, the last field the check reads.
struct H264Sps {
	uint8_t profile = 100;
	uint64_t chroma = 1;
	bool scaling = false;
	uint64_t order = 0;
	uint64_t cycle = 0;
};

NalUnit h264Sps(const H264Sps &sps)
{
	NalUnitWriter writer({0x67});
	writer.bits(sps.profile, 8);
	writer.bits(0, 8);
	writer.bits(40, 8);
	writer.ue(0);
	const std::vector<uint8_t> high = {100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134};
	if (std::find(high.begin(), high.end(), sps.profile) != high.end()) {
		writer.ue(sps.chroma);
		if (sps.chroma == 3) {
			writer.bit(false);
		}
		writer.ue(0);
		writer.ue(0);
		writer.bit(false);
		writer.bit(sps.scaling);
		for (int i = 0; sps.scaling && i < (sps.chroma != 3 ? 8 : 12); ++i) {
			// Every other list is there. Of those, the third ends early: a scale of zero
			// leaves the rest as they are.
			writer.bit(i % 2 == 0);
			if (i == 2) {
				writer.se(0);
				writer.se(-8);
			} else if (i % 2 == 0) {
				for (int j = 0; j < (i < 6 ? 16 : 64); ++j) {
					writer.se(1);
				}
			}
		}
	}
	writer.ue(0);
	writer.ue(sps.order);
	if (sps.order == 0) {
		writer.ue(2);
	} else if (sps.order == 1) {
		writer.bit(false);
		writer.se(-1);
		writer.se(2);
		writer.ue(sps.cycle);
		for (uint64_t i = 0; i < sps.cycle; ++i) {
			writer.se(i % 2 == 0 ? 3 : -3);
		}
	}
	return writer.finish();
}

bool whole(VideoCodec codec, const Bytes &data)
{
	return tapeloop::sequenceParameterSetsAreWhole(codec, data);
}

// The unit passes whole and fails cut anywhere before the end of its fields, down to its
// header.
void checkCuts(VideoCodec codec, const NalUnit &unit)
{
	CHECK(whole(codec, join({kLong, unit.bytes})));
	for (size_t size = 1; size < unit.fields; ++size) {
		CAPTURE(size);
		const Bytes cut(unit.bytes.begin(), unit.bytes.begin() + static_cast<std::ptrdiff_t>(size));
		CHECK_FALSE(whole(codec, join({kLong, cut})));
	}
}

const Bytes kHevcVps = {0x40, 0x01, 0x0C, 0x01, 0xFF, 0xFF, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00,
			0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x5D, 0x95, 0x98, 0x09};
const Bytes kHevcPps = {0x44, 0x01, 0xC1, 0x72, 0xB4, 0x62, 0x40};

} // namespace

TEST_CASE("an HEVC SPS passes with every field the muxer counts through, and fails cut")
{
	std::vector<HevcSps> variants(9);
	variants[1].subLayers = 2;
	variants[1].subLayerProfiles = true;
	variants[1].orderingForEach = true;
	variants[2].subLayers = 7;
	variants[3].chroma = 3;
	variants[3].window = true;
	variants[3].pcm = true;
	variants[4].scaling = true;
	variants[5].sets = 4;
	variants[5].predicted = true;
	variants[6].sets = 0;
	// A further layer with sub-layers of its own reads like the base layer; one that
	// takes them from the video parameter set leaves fields out.
	variants[7].layer = 1;
	variants[7].subLayers = 3;
	variants[8].layer = 1;
	variants[8].subLayers = 7;
	variants[8].scaling = true;
	for (size_t i = 0; i < variants.size(); ++i) {
		CAPTURE(i);
		checkCuts(VideoCodec::Hevc, hevcSps(variants[i]));
	}
}

TEST_CASE("an H.264 SPS passes with every field the muxer counts through, and fails cut")
{
	std::vector<H264Sps> variants(6);
	variants[1].scaling = true;
	variants[2].profile = 244;
	variants[2].chroma = 3;
	variants[2].scaling = true;
	variants[3].profile = 66;
	variants[3].order = 2;
	variants[4].profile = 77;
	variants[4].order = 1;
	variants[4].cycle = 3;
	variants[5].order = 1;
	for (size_t i = 0; i < variants.size(); ++i) {
		CAPTURE(i);
		checkCuts(VideoCodec::H264, h264Sps(variants[i]));
	}
}

TEST_CASE("a configuration passes when each SPS in it is whole")
{
	const NalUnit sps = hevcSps({});
	const Bytes cut(sps.bytes.begin(), sps.bytes.begin() + static_cast<std::ptrdiff_t>(sps.fields - 1));
	CHECK(whole(VideoCodec::Hevc, join({kLong, kHevcVps, kLong, sps.bytes, kShort, kHevcPps})));
	CHECK_FALSE(whole(VideoCodec::Hevc, join({kLong, kHevcVps, kLong, cut, kShort, kHevcPps})));
	// Every SPS counts, not only the first.
	CHECK_FALSE(whole(VideoCodec::Hevc, join({kLong, kHevcVps, kLong, sps.bytes, kLong, cut, kLong, kHevcPps})));
	// Zero bytes after a unit do not count as its fields.
	CHECK_FALSE(whole(VideoCodec::Hevc, join({kLong, kHevcVps, kLong, cut, {0, 0}, kLong, kHevcPps})));

	const NalUnit h264 = h264Sps({});
	const Bytes h264Cut(h264.bytes.begin(), h264.bytes.begin() + static_cast<std::ptrdiff_t>(h264.fields - 1));
	CHECK(whole(VideoCodec::H264, join({kLong, h264.bytes, kLong, kPps})));
	CHECK_FALSE(whole(VideoCodec::H264, join({kShort, h264Cut, kLong, kPps})));
	// Units of the other codec are not read as an SPS.
	CHECK(whole(VideoCodec::H264, join({kLong, kHevcVps, kLong, cut})));
	CHECK(whole(VideoCodec::Hevc, join({kLong, h264Cut, kLong, kPps})));
}

TEST_CASE("data the muxer takes as it is passes")
{
	const NalUnit sps = hevcSps({});
	const Bytes cut(sps.bytes.begin(), sps.bytes.begin() + static_cast<std::ptrdiff_t>(sps.fields - 1));
	CHECK(whole(VideoCodec::Hevc, {}));
	CHECK(whole(VideoCodec::H264, {}));
	// An hvcC or avcC record starts with its version, 1.
	CHECK(whole(VideoCodec::Hevc, join({{0x01}, kLong, cut})));
	CHECK(whole(VideoCodec::H264, join({{0x01, 0x00}, kLong, cut})));
	// A start code later than the first bytes does not make the data Annex B.
	CHECK(whole(VideoCodec::Hevc, join({{0x00, 0x00, 0x00, 0x00, 0x01}, cut})));
}

// What FFmpeg 8.1.3's hvcC writer took about a minute over: a video parameter set, then
// a sequence parameter set whose fields run out before its short-term reference picture
// sets.
TEST_CASE("the configuration that held up FFmpeg's hvcC writer fails")
{
	const Bytes config = {0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0C, 0x01, 0xFF, 0xFF, 0x01, 0x60, 0x00, 0x00,
			      0x03, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x3C, 0xBA, 0x02, 0x40,
			      0x00, 0x00, 0x00, 0x01, 0x42, 0x01, 0x01, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x90,
			      0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x3C, 0xA0, 0x0A, 0x08, 0x0C, 0x2F, 0x74,
			      0x6D, 0x70, 0x2F, 0x74, 0x61, 0x70, 0x65, 0x6C, 0x6F, 0x6F, 0x70, 0x2D, 0x74, 0x65,
			      0x73, 0x74, 0x2D, 0x34, 0x39, 0x31, 0x35, 0x34, 0x32, 0x39, 0x30};
	REQUIRE(config.size() == 81);
	CHECK_FALSE(whole(VideoCodec::Hevc, config));
}

// The same for FFmpeg's avcC writer, which took 13 seconds: a picture order count cycle
// of 2^31 - 1 offsets, from a code that runs past the end of the set.
TEST_CASE("the configuration that held up FFmpeg's avcC writer fails")
{
	const Bytes config = {0x00, 0x00, 0x00, 0x01, 0x67, 0x01, 0x00, 0x1F, 0xD3, 0x00, 0x00,
			      0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x68, 0xCE, 0x38, 0x80};
	CHECK_FALSE(whole(VideoCodec::H264, config));
}
