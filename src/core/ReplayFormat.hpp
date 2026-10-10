// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/Clip.hpp"
#include "core/Gop.hpp"
#include "core/MediaTime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace tapeloop {

// The bytes of stored replays. A broadcast folder holds, per captured source, append-only
// segments `data/<source key>-<sequence>.tpls` of GOP chunks, and per replay a small
// manifest `<name>.tplp` that lists the GOPs it uses with their frame times, and its
// tags. Everything is little-endian, chunks start at multiples of kReplayAlignment, and
// every chunk header and payload carries a CRC-32C. These functions only encode and
// decode bytes; ReplayWriter and GopReader read and write the files.

inline constexpr size_t kReplayAlignment = 4096;
inline constexpr uint16_t kReplayFormatMajor = 1;
inline constexpr uint16_t kReplayFormatMinor = 0;
inline constexpr size_t kChunkHeaderSize = 32;
// Where the tag slots and the index chunk of a manifest start.
inline constexpr uint64_t kTagSlotA = 4096;
inline constexpr uint64_t kTagSlotB = 8192;
inline constexpr size_t kTagSlotSize = 4096;
inline constexpr uint64_t kManifestIndexOffset = 12288;
inline constexpr size_t kManifestFooterSize = 64;

using ReplayId = std::array<uint8_t, 16>;

// A GOP's identity, the same for every copy of it: the run it belongs to, its first
// packet, and how many packets it has, since the GOP still being encoded can be captured
// and written before it is complete.
struct GopKey {
	uint32_t runKey = 0;
	int64_t firstPts = 0;
	Nanoseconds firstTime{0};
	uint32_t packetCount = 0;

	bool operator==(const GopKey &) const = default;
};

// CRC-32C of the codec and the configuration bytes, the same for every GOP of a run.
uint32_t runKeyOf(VideoCodec codec, const CodecConfig *config) noexcept;
GopKey gopKeyOf(const Gop &gop) noexcept;

struct SegmentHeader {
	ReplayId broadcast{};
	uint32_t sequence = 0;
	std::string sourceKey;
	std::string sourceName;
};

// One aligned block, kReplayAlignment bytes. Throws std::length_error when the key and
// name do not fit.
std::vector<uint8_t> encodeSegmentHeader(const SegmentHeader &header);
std::optional<SegmentHeader> decodeSegmentHeader(std::span<const uint8_t> bytes);

// Where a chunk landed in the bytes it was appended to: its offset there, its size
// without the padding that follows it, and the CRC of its header, which a manifest keeps
// to check that a read finds the chunk it meant.
struct ChunkPlace {
	uint64_t offset = 0;
	uint64_t size = 0;
	uint32_t headerCrc = 0;
};

// Each appends one chunk, padded to the alignment, to out, which must hold a whole
// number of aligned blocks.
ChunkPlace appendRunChunk(std::vector<uint8_t> &out, uint32_t sequence, const Gop &gop);
ChunkPlace appendGopChunk(std::vector<uint8_t> &out, uint32_t sequence, const Gop &gop);
// How many bytes appendRunChunk and appendGopChunk add to out for this GOP, padding
// included.
uint64_t runChunkSize(const Gop &gop) noexcept;
uint64_t gopChunkSize(const Gop &gop) noexcept;

struct StoredRun {
	VideoCodec codec = VideoCodec::H264;
	Nanoseconds frameDuration{0};
	// Null when the encoder reported none.
	std::shared_ptr<const CodecConfig> config;
	uint32_t key = 0;
};

// Rebuilds a GOP from its chunk, as read from a segment, for the run the manifest names.
// Null when the chunk is not the one meant (another header CRC), fails a CRC, or does
// not describe a valid GOP of that run.
std::shared_ptr<const Gop> decodeGopChunk(std::span<const uint8_t> chunk, uint32_t headerCrc, const StoredRun &run);

// The size of the GOP chunk this header starts, header included, when the header is
// intact, belongs to a GOP chunk and has this CRC. The caller reads kChunkHeaderSize
// bytes.
std::optional<uint64_t> gopChunkSizeFromHeader(std::span<const uint8_t> header, uint32_t headerCrc);

// Reads the GOP key of a GOP chunk header and the start of its payload, without checking
// the payload CRC; the caller reads at least kChunkHeaderSize + 32 bytes. Null when the
// header fails its CRC or is not a GOP chunk.
std::optional<GopKey> peekGopKey(std::span<const uint8_t> chunkStart);

struct StoredGop {
	uint32_t segment = 0;
	uint32_t packetCount = 0;
	uint64_t offset = 0;
	uint64_t size = 0;
	uint32_t headerCrc = 0;
	// Index into the source's runs.
	uint32_t run = 0;
};

struct StoredSource {
	std::string key;
	std::string name;
	// The first and last frame of the clip; frames of its GOPs outside them only serve
	// decoding.
	Nanoseconds in{0};
	Nanoseconds out{0};
	std::vector<StoredRun> runs;
	std::vector<StoredGop> gops;
	// The time of every packet of every GOP, in order: replays have no B-frames, so decode
	// order is presentation order.
	std::vector<Nanoseconds> frameTimes;
};

// Everything a manifest says about a replay but its tags.
struct ReplayIndex {
	ReplayId id{};
	// Unix time in nanoseconds, and the OBS clock the packets are stamped with.
	int64_t capturedAtUtc = 0;
	Nanoseconds capturedAtClock{0};
	// The range the capture asked for, back from the capture as far as the longest window
	// of the buffers that held anything; what each source gives is its in and out.
	Nanoseconds start{0};
	Nanoseconds end{0};
	std::vector<StoredSource> sources;
};

// The first frame of GOP gop of the source, an index into frameTimes.
size_t firstFrameOf(const StoredSource &source, size_t gop) noexcept;
// As Clip::locate: the frame on screen at t, clamped to [in, out].
FrameLocation locate(const StoredSource &source, Nanoseconds t) noexcept;

// The whole manifest, both tag slots holding these tags at generation 1.
std::vector<uint8_t> encodeManifest(const ReplayIndex &index, std::span<const std::string> tags);
// Null for a file that is not a complete and intact manifest of this major version.
std::optional<ReplayIndex> decodeManifest(std::span<const uint8_t> file);

struct TagSlot {
	uint64_t generation = 0;
	std::vector<std::string> tags;
};

// Whether the tags fit one tag slot; encodeTagSlot throws std::length_error for those
// that do not.
bool tagsFit(std::span<const std::string> tags) noexcept;
std::vector<uint8_t> encodeTagSlot(std::span<const std::string> tags, uint64_t generation);
// Null for a slot that is torn or was never written.
std::optional<TagSlot> decodeTagSlot(std::span<const uint8_t> slot);
// The tags of the newer intact slot, from the bytes of a manifest that hold both slots.
std::vector<std::string> decodeTags(std::span<const uint8_t> manifestHead);

// Where the next tag edit goes: the slot with the older generation, B on a tie, so that
// a torn write leaves the other slot with the previous tags.
struct TagWrite {
	uint64_t offset = 0;
	uint64_t generation = 0;
};
TagWrite nextTagWrite(std::span<const uint8_t> manifestHead);

} // namespace tapeloop
