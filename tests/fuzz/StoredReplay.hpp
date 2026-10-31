// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/FileIo.hpp"
#include "core/Gop.hpp"
#include "core/ReplayFormat.hpp"
#include "core/ReplayWriter.hpp"

#include "FuzzInput.hpp"
#include "ReplayDamage.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace tapeloop::fuzz {

inline size_t readSize(FuzzInput &input)
{
	const size_t low = input.byte();
	const size_t high = input.byte();
	return low | high << 8;
}

inline Nanoseconds readFrameDuration(FuzzInput &input)
{
	// The frame rates OBS runs at most often, or any other up to a frame a second.
	constexpr std::array<int64_t, 4> kCommon{16'683'333, 16'666'667, 20'000'000, 33'366'667};
	const size_t pick = input.byte() % (kCommon.size() + 1);
	if (pick < kCommon.size()) {
		return Nanoseconds{kCommon[pick]};
	}
	return Nanoseconds{1 + static_cast<int64_t>(input.u64() % 1'000'000'000)};
}

// The GOP chunks of one file, as a writer lays them out.
struct ChunkFile {
	std::vector<uint8_t> bytes;
	uint32_t chunks = 0;
};

// The runs, GOPs and frames of a source from the input, their chunks appended to the
// file the source's GOPs go to. Packets hold the input's bytes as they are, so that
// whatever reads them gets the input's own bitstreams.
inline StoredSource readSource(FuzzInput &input, size_t number, bool own, ChunkFile &file)
{
	StoredSource source;
	source.key = std::to_string(number);
	source.name = "Camera " + std::to_string(number + 1);
	int64_t frame = 0;
	// An hour into the OBS clock, as a replay of a match is.
	Nanoseconds time{3'600'000'000'000};
	for (int runs = 1 + input.byte() % 2; runs > 0; --runs) {
		const VideoCodec codec = input.flag() ? VideoCodec::Hevc : VideoCodec::H264;
		const Nanoseconds frameDuration = readFrameDuration(input);
		const std::span<const uint8_t> configBytes = input.bytes(readSize(input));
		std::shared_ptr<const CodecConfig> config =
			configBytes.empty()
				? nullptr
				: std::make_shared<const CodecConfig>(configBytes.begin(), configBytes.end());
		source.runs.push_back({codec, frameDuration, config, runKeyOf(codec, config.get())});
		GopBuilder builder(frameDuration);
		builder.setCodecConfig(codec, config);
		bool firstOfRun = true;
		for (int gops = 1 + input.byte() % 3; gops > 0; --gops) {
			for (int packets = 1 + input.byte() % 32, i = 0; i < packets; ++i) {
				const uint8_t flags = input.byte();
				EncodedPacket packet;
				packet.pts = frame;
				// Some frames are decoded up to three frames before they show.
				packet.dts = frame - (flags & 3);
				packet.time = time;
				packet.keyframe = i == 0 || (flags & 4) != 0;
				packet.data = input.bytes(readSize(input));
				builder.append(packet);
				source.frameTimes.push_back(time);
				++frame;
				time += frameDuration;
			}
			const std::shared_ptr<const Gop> gop = builder.seal();
			if (firstOfRun) {
				appendRunChunk(file.bytes, file.chunks++, *gop);
				firstOfRun = false;
			}
			const ChunkPlace place = appendGopChunk(file.bytes, file.chunks++, *gop);
			source.gops.push_back({own ? kOwnFile : 0, static_cast<uint32_t>(gop->packets().size()),
					       place.offset, place.size, place.headerCrc,
					       static_cast<uint32_t>(source.runs.size() - 1)});
		}
	}
	const size_t count = source.frameTimes.size();
	const size_t in = input.byte() % count;
	source.in = source.frameTimes[in];
	source.out = source.frameTimes[count - 1 - input.byte() % (count - in)];
	return source;
}

// A replay from the input, written into folder the way the plugin writes one: a manifest
// with a segment of GOP chunks for each source under data, or a self-contained replay
// holding them. Each file is then damaged a little, and when the input says so the CRCs
// are computed again, the index taking the chunks' new ones, which gets the damage past
// them to the checks behind. Returns the path of the manifest.
inline std::filesystem::path writeStoredReplay(FuzzInput &input, const std::filesystem::path &folder)
{
	const bool own = input.flag();
	const bool reseal = input.flag();
	ReplayIndex index;
	std::vector<ChunkFile> files;
	ChunkFile ownFile;
	// A self-contained replay's chunks start after its head.
	ownFile.bytes.resize(kManifestIndexOffset);
	for (size_t number = 0, sources = 1 + input.byte() % 2; number < sources; ++number) {
		ChunkFile segment;
		if (!own) {
			segment.bytes = encodeSegmentHeader({{}, 0, std::to_string(number), {}});
		}
		index.sources.push_back(readSource(input, number, own, own ? ownFile : segment));
		if (!own) {
			files.push_back(std::move(segment));
		}
	}
	index.start = index.sources.front().in;
	index.end = index.sources.front().out;

	if (own) {
		damage(input, std::span<uint8_t>(ownFile.bytes).subspan(kManifestIndexOffset));
	}
	for (ChunkFile &file : files) {
		damage(input, file.bytes);
	}
	if (reseal) {
		for (size_t s = 0; s < index.sources.size(); ++s) {
			std::vector<uint8_t> &bytes = own ? ownFile.bytes : files[s].bytes;
			for (StoredGop &stored : index.sources[s].gops) {
				if (stored.offset <= bytes.size() && stored.size <= bytes.size() - stored.offset) {
					stored.headerCrc = resealChunk(
						std::span<uint8_t>(bytes).subspan(stored.offset, stored.size));
				}
			}
		}
	}

	const std::filesystem::path manifestPath = folder / "replay.tplp";
	std::vector<uint8_t> manifest;
	const uint64_t indexOffset = own ? ownFile.bytes.size() : kManifestIndexOffset;
	if (own) {
		manifest = encodeManifestHead(index, {});
		manifest.insert(manifest.end(), ownFile.bytes.begin() + kManifestIndexOffset, ownFile.bytes.end());
		const std::vector<uint8_t> tail = encodeManifestTail(index, indexOffset);
		manifest.insert(manifest.end(), tail.begin(), tail.end());
	} else {
		manifest = encodeManifest(index, {});
	}
	damage(input, manifest);
	if (reseal) {
		resealManifest(manifest, indexOffset);
	}
	File(manifestPath, File::Mode::CreateNew).writeAt(0, manifest);
	for (size_t s = 0; s < files.size(); ++s) {
		const std::filesystem::path path = segmentPath(manifestPath, index.sources[s], 0);
		std::filesystem::create_directories(path.parent_path());
		File(path, File::Mode::CreateNew).writeAt(0, files[s].bytes);
	}
	return manifestPath;
}

} // namespace tapeloop::fuzz
