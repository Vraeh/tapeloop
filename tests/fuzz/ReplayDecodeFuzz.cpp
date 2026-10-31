// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/DecodePlanner.hpp"
#include "core/GopReader.hpp"
#include "core/ReplayWriter.hpp"
#include "decode/FFmpegDecoder.hpp"
#include "decode/Picture.hpp"

#include "../core/TempDirectory.hpp"
#include "FuzzInput.hpp"
#include "StoredReplay.hpp"

extern "C" {
#include <libavutil/log.h>
}

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

using tapeloop::DecodePlanner;
using tapeloop::DecodePlannerConfig;
using tapeloop::DecodeResult;
using tapeloop::DecodeStatus;
using tapeloop::Nanoseconds;
using tapeloop::PlayDirection;
using tapeloop::ReplayIndex;
using tapeloop::StoredGops;
using tapeloop::decode::FFmpegDecoder;
using tapeloop::decode::Picture;
using tapeloop::decode::PixelLayout;
using tapeloop::fuzz::FuzzInput;
using tapeloop::fuzz::require;
using tapeloop::test::TempDirectory;

namespace {

// Every byte of the picture's planes copied out, as the player copies a picture decoded
// in memory, so that a size or stride the planes do not have reads outside them.
void copyPicture(const Picture &picture, std::vector<uint8_t> &copy)
{
	require(picture.texture == nullptr);
	if (picture.width == 0 || picture.height == 0) {
		return;
	}
	const bool nv12 = picture.layout == PixelLayout::Nv12;
	const uint32_t chromaWidth = (picture.width + 1) / 2;
	const uint32_t chromaHeight = (picture.height + 1) / 2;
	const std::array<uint32_t, 3> rowBytes{picture.width, nv12 ? chromaWidth * 2 : chromaWidth,
					       nv12 ? 0 : chromaWidth};
	const std::array<uint32_t, 3> rows{picture.height, chromaHeight, nv12 ? 0 : chromaHeight};
	for (size_t plane = 0; plane < 3; ++plane) {
		require(rows[plane] == 0 || (picture.planes[plane] && picture.strides[plane] >= rowBytes[plane]));
		copy.resize(rowBytes[plane]);
		for (uint32_t row = 0; row < rows[plane]; ++row) {
			std::memcpy(copy.data(), picture.planes[plane] + size_t{row} * picture.strides[plane],
				    rowBytes[plane]);
		}
	}
}

// A source of the replay played the way the player plays it: frames asked for in order,
// stepped through either way, jumped to, or at times between and beyond them, each one
// copied out when it decodes, and the GOPs ahead decoded after it.
void play(FuzzInput &input, const std::filesystem::path &manifest, const tapeloop::StoredSource &source)
{
	FFmpegDecoder decoder;
	{
		DecodePlannerConfig config;
		// Room for a few frames of the largest pictures the decoders take, so that
		// frames are also dropped and decoded again.
		config.maxBytes = size_t{256} << 20;
		DecodePlanner planner(decoder, config);
		auto gops = std::make_unique<StoredGops>(
			manifest, source, std::vector<std::weak_ptr<const tapeloop::Gop>>{}, size_t{8} << 20);
		const std::vector<Nanoseconds> times = gops->frameTimes();
		planner.load(std::move(gops));
		if (times.empty()) {
			return;
		}
		std::vector<uint8_t> copy;
		size_t frame = 0;
		PlayDirection direction = PlayDirection::Forward;
		// Without moves in the input, the replay plays through once.
		const size_t moves = input.empty() ? times.size() : input.byte();
		for (size_t move = 0; move < moves; ++move) {
			Nanoseconds time = times[frame];
			switch (input.byte() % 4) {
			case 0:
				frame = std::min(frame + 1, times.size() - 1);
				direction = PlayDirection::Forward;
				time = times[frame];
				break;
			case 1:
				frame = frame > 0 ? frame - 1 : 0;
				direction = PlayDirection::Backward;
				time = times[frame];
				break;
			case 2:
				frame = static_cast<size_t>(input.u64() % times.size());
				direction = input.flag() ? PlayDirection::Backward : PlayDirection::Forward;
				time = times[frame];
				break;
			default:
				time = Nanoseconds{input.i64()};
				break;
			}
			const DecodeResult result = planner.frameAt(time);
			if (result.status == DecodeStatus::Ok) {
				if (const std::optional<Picture> picture = decoder.picture(result.frame)) {
					copyPicture(*picture, copy);
				}
			}
			planner.prefetch(direction);
			require(planner.heldBytes() <= config.maxBytes || planner.heldFrames() <= 1);
		}
	}
	// The planner gave back every frame it kept.
	require(decoder.heldFrames() == 0);
}

} // namespace

extern "C" int LLVMFuzzerInitialize(int *, char ***)
{
	// The decoders report every broken bitstream, which would bury what the fuzzer prints.
	av_log_set_level(AV_LOG_QUIET);
	return 0;
}

// A replay written from the input, its bitstreams the input's own, damaged and its CRCs
// computed again as the input says, read back through the reader the player uses and
// decoded by FFmpeg. Whatever the files hold, nothing reads or writes outside its memory
// and every decoded frame is given back.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	FuzzInput input(data, size);
	const TempDirectory folder;
	const std::filesystem::path manifest = tapeloop::fuzz::writeStoredReplay(input, folder.path());
	const std::optional<ReplayIndex> index = tapeloop::readReplayIndex(manifest);
	if (!index || index->sources.empty()) {
		return 0;
	}
	const size_t source = input.byte() % index->sources.size();
	play(input, manifest, index->sources[source]);
	return 0;
}
