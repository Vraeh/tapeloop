// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/ReplayFormat.hpp"

#include "core/Crc32c.hpp"

#include "../core/SyntheticEncoder.hpp"
#include "FuzzInput.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

using tapeloop::ChunkPlace;
using tapeloop::CodecConfig;
using tapeloop::Gop;
using tapeloop::GopBuilder;
using tapeloop::Nanoseconds;
using tapeloop::ReplayIndex;
using tapeloop::StoredRun;
using tapeloop::StoredSource;
using tapeloop::VideoCodec;
using tapeloop::fuzz::FuzzInput;
using tapeloop::fuzz::require;
using tapeloop::test::SyntheticEncoder;

namespace {

// Flips a few bytes the input chooses; the rest of the input is read afterwards.
void damage(FuzzInput &input, std::vector<uint8_t> &bytes)
{
	for (int flips = input.byte() % 4; flips > 0 && !bytes.empty(); --flips) {
		const size_t at = static_cast<size_t>(input.u64() % bytes.size());
		bytes[at] ^= static_cast<uint8_t>(input.byte() | 1);
	}
}

uint64_t load64(std::span<const uint8_t> bytes, size_t at)
{
	uint64_t value = 0;
	for (size_t i = 0; i < 8; ++i) {
		value |= uint64_t{bytes[at + i]} << (8 * i);
	}
	return value;
}

void store32(std::span<uint8_t> bytes, size_t at, uint32_t value)
{
	for (size_t i = 0; i < 4; ++i) {
		bytes[at + i] = static_cast<uint8_t>(value >> (8 * i));
	}
}

// Computes the CRCs of a chunk again over what its header and payload hold now, so that
// damage gets past them to the checks behind; returns the new header CRC.
uint32_t resealChunk(std::span<uint8_t> chunk)
{
	if (chunk.size() < tapeloop::kChunkHeaderSize) {
		return 0;
	}
	const uint64_t payloadSize = std::min<uint64_t>(load64(chunk, 8), chunk.size() - tapeloop::kChunkHeaderSize);
	store32(chunk, 24, tapeloop::crc32c(chunk.subspan(tapeloop::kChunkHeaderSize, payloadSize)));
	const uint32_t headerCrc = tapeloop::crc32c(chunk.first(28));
	store32(chunk, 28, headerCrc);
	return headerCrc;
}

// The manifest's header, index chunk and footer CRCs computed again.
void resealManifest(std::vector<uint8_t> &manifest)
{
	const std::span<uint8_t> bytes(manifest);
	if (bytes.size() < tapeloop::kManifestIndexOffset + tapeloop::kManifestFooterSize) {
		return;
	}
	store32(bytes, 60, tapeloop::crc32c(bytes.first(60)));
	resealChunk(bytes.subspan(tapeloop::kManifestIndexOffset,
				  bytes.size() - tapeloop::kManifestIndexOffset - tapeloop::kManifestFooterSize));
	const std::span<uint8_t> footer = bytes.last(tapeloop::kManifestFooterSize);
	store32(footer, 60, tapeloop::crc32c(footer.first(60)));
}

std::vector<std::shared_ptr<const Gop>> readGops(FuzzInput &input, int64_t &frame)
{
	SyntheticEncoder::Config config;
	config.gopLength = 1 + input.byte() % 6;
	config.keyframeSize = 1 + input.byte() % 64;
	config.frameSize = input.byte() % 32;
	config.firstFrame = frame;
	SyntheticEncoder encoder(config);
	GopBuilder builder(encoder.frameDuration());
	const std::span<const uint8_t> bytes = input.bytes(input.byte() % 6);
	builder.setCodecConfig(input.flag() ? VideoCodec::Hevc : VideoCodec::H264,
			       bytes.empty() ? nullptr
					     : std::make_shared<const CodecConfig>(bytes.begin(), bytes.end()));
	std::vector<std::shared_ptr<const Gop>> gops;
	for (int gop = 1 + input.byte() % 3; gop > 0; --gop) {
		for (int64_t i = 0; i < config.gopLength; ++i) {
			builder.append(encoder.next());
		}
		gops.push_back(builder.seal());
	}
	frame = encoder.nextFrame();
	return gops;
}

StoredRun runOf(const Gop &gop)
{
	return {gop.codec(), gop.frameDuration(),
		gop.codecConfig() ? std::make_shared<const CodecConfig>(*gop.codecConfig()) : nullptr,
		tapeloop::runKeyOf(gop.codec(), gop.codecConfig())};
}

bool sameIndex(const ReplayIndex &a, const ReplayIndex &b)
{
	if (a.id != b.id || a.capturedAtUtc != b.capturedAtUtc || a.capturedAtClock != b.capturedAtClock ||
	    a.start != b.start || a.end != b.end || a.sources.size() != b.sources.size()) {
		return false;
	}
	for (size_t s = 0; s < a.sources.size(); ++s) {
		const StoredSource &x = a.sources[s];
		const StoredSource &y = b.sources[s];
		if (x.key != y.key || x.name != y.name || x.in != y.in || x.out != y.out ||
		    x.frameTimes != y.frameTimes || x.runs.size() != y.runs.size() || x.gops.size() != y.gops.size()) {
			return false;
		}
		for (size_t r = 0; r < x.runs.size(); ++r) {
			const bool configs = x.runs[r].config && y.runs[r].config
						     ? *x.runs[r].config == *y.runs[r].config
						     : x.runs[r].config == y.runs[r].config;
			if (x.runs[r].codec != y.runs[r].codec || x.runs[r].key != y.runs[r].key || !configs ||
			    x.runs[r].frameDuration != y.runs[r].frameDuration) {
				return false;
			}
		}
		for (size_t g = 0; g < x.gops.size(); ++g) {
			const tapeloop::StoredGop &p = x.gops[g];
			const tapeloop::StoredGop &q = y.gops[g];
			if (p.segment != q.segment || p.packetCount != q.packetCount || p.offset != q.offset ||
			    p.size != q.size || p.headerCrc != q.headerCrc || p.run != q.run) {
				return false;
			}
		}
	}
	return true;
}

// A manifest of real GOP chunks, damaged a little: it either decodes to what was written
// or is refused, and every GOP it lists reads back or is refused. Resealed, the damage
// passes the CRCs: the manifest is then refused or decodes to an index of its own, and
// the GOPs at the places it gives are read the way the reader reads them.
void writtenThenDamaged(FuzzInput &input)
{
	const bool reseal = input.flag();
	ReplayIndex index;
	index.capturedAtUtc = static_cast<int64_t>(input.u64());
	index.capturedAtClock = Nanoseconds{static_cast<int64_t>(input.u64() >> 1)};
	std::vector<uint8_t> segment(tapeloop::kReplayAlignment, 0);
	int64_t frame = 0;
	for (int sources = 1 + input.byte() % 3; sources > 0; --sources) {
		StoredSource source;
		source.key = std::to_string(sources);
		for (int runs = 1 + input.byte() % 2; runs > 0; --runs) {
			for (const std::shared_ptr<const Gop> &gop : readGops(input, frame)) {
				const StoredRun run = runOf(*gop);
				if (source.runs.empty() || source.runs.back().key != run.key) {
					source.runs.push_back(run);
				}
				const ChunkPlace place = tapeloop::appendGopChunk(segment, 0, *gop);
				source.gops.push_back({0, static_cast<uint32_t>(gop->packets().size()), place.offset,
						       place.size, place.headerCrc,
						       static_cast<uint32_t>(source.runs.size() - 1)});
				for (const tapeloop::PacketRecord &packet : gop->packets()) {
					source.frameTimes.push_back(packet.time);
				}
			}
		}
		source.in = source.frameTimes[input.byte() % source.frameTimes.size()];
		source.out = std::max(source.in, source.frameTimes.back());
		index.sources.push_back(std::move(source));
	}
	index.start = index.sources.front().in;
	index.end = index.sources.front().out;

	std::vector<uint8_t> manifest = tapeloop::encodeManifest(index, {});
	damage(input, manifest);
	if (reseal) {
		resealManifest(manifest);
	}
	const std::optional<ReplayIndex> read = tapeloop::decodeManifest(manifest);
	if (read && reseal) {
		const std::optional<ReplayIndex> again = tapeloop::decodeManifest(tapeloop::encodeManifest(*read, {}));
		require(again && sameIndex(*again, *read));
	} else if (read) {
		require(sameIndex(*read, index));
	}
	(void)tapeloop::decodeTags(manifest);
	(void)tapeloop::nextTagWrite(manifest);

	damage(input, segment);
	const ReplayIndex &placed = read && reseal ? *read : index;
	for (const StoredSource &source : placed.sources) {
		for (const tapeloop::StoredGop &stored : source.gops) {
			if (stored.offset > segment.size() || stored.size > segment.size() - stored.offset) {
				continue;
			}
			const std::span<uint8_t> chunk =
				std::span<uint8_t>(segment).subspan(stored.offset, stored.size);
			const uint32_t headerCrc = reseal ? resealChunk(chunk) : stored.headerCrc;
			const std::shared_ptr<const Gop> gop =
				tapeloop::decodeGopChunk(chunk, headerCrc, source.runs[stored.run]);
			if (gop && !reseal) {
				require(gop->packets().size() == stored.packetCount);
			}
			(void)tapeloop::peekGopKey(chunk);
		}
	}
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	FuzzInput input(data, size);
	switch (input.byte() % 4) {
	case 0: {
		const std::span<const uint8_t> bytes = input.bytes(size);
		if (const std::optional<ReplayIndex> index = tapeloop::decodeManifest(bytes)) {
			// What decodes encodes again to a manifest that decodes the same.
			const std::optional<ReplayIndex> again =
				tapeloop::decodeManifest(tapeloop::encodeManifest(*index, {}));
			require(again && sameIndex(*again, *index));
		}
		(void)tapeloop::decodeTags(bytes);
		(void)tapeloop::decodeTagSlot(bytes);
		break;
	}
	case 1: {
		const std::span<const uint8_t> bytes = input.bytes(size);
		(void)tapeloop::decodeSegmentHeader(bytes);
		(void)tapeloop::peekGopKey(bytes);
		StoredRun run;
		run.key = tapeloop::runKeyOf(run.codec, nullptr);
		uint32_t headerCrc = 0;
		for (size_t i = 0; i < 4 && 28 + i < bytes.size(); ++i) {
			headerCrc |= uint32_t{bytes[28 + i]} << (8 * i);
		}
		(void)tapeloop::decodeGopChunk(bytes, headerCrc, run);
		break;
	}
	default:
		writtenThenDamaged(input);
		break;
	}
	return 0;
}
