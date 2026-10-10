// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

// Reads back the one video track of an MP4 the export wrote, box by box, since the
// plugin's FFmpeg has no demuxer. Only what the tests check is read; anything else is
// skipped.
namespace tapeloop::test {

struct Mp4SampleEntry {
	// "avc1" or "hvc1".
	std::string type;
	uint16_t width = 0;
	uint16_t height = 0;
	// The parameter sets of its avcC or hvcC, in Annex B, as a decoder takes them.
	std::vector<uint8_t> config;
	// The size of the length before each NAL unit of a sample.
	size_t lengthSize = 4;
};

struct Mp4Sample {
	// The sample in Annex B, its lengths turned into start codes.
	std::vector<uint8_t> data;
	// 1-based, as the sample-to-chunk table counts sample entries.
	uint32_t entry = 0;
	int64_t dts = 0;
	int64_t pts = 0;
	bool sync = false;
};

struct Mp4Edit {
	int64_t duration = 0;
	int64_t mediaTime = 0;
};

struct Mp4Track {
	uint32_t movieTimescale = 0;
	uint32_t timescale = 0;
	std::vector<Mp4SampleEntry> entries;
	std::vector<Mp4Sample> samples;
	std::vector<Mp4Edit> edits;
};

// Throws std::runtime_error for a file it cannot follow.
Mp4Track readMp4(std::span<const uint8_t> file);

// The NAL units of an Annex B packet, start codes left out.
std::vector<std::vector<uint8_t>> annexBUnits(std::span<const uint8_t> packet);

} // namespace tapeloop::test
