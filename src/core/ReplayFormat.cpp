// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/ReplayFormat.hpp"

#include "core/Crc32c.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace tapeloop {
namespace {

constexpr std::array<uint8_t, 8> kSegmentMagic = {0x54, 0x50, 0x4C, 0x53, 0x0D, 0x0A, 0x1A, 0x0A};
constexpr std::array<uint8_t, 8> kManifestMagic = {0x54, 0x50, 0x4C, 0x50, 0x0D, 0x0A, 0x1A, 0x0A};
constexpr std::array<uint8_t, 8> kFooterMagic = {'T', 'P', 'L', 'P', 'T', 'A', 'I', 'L'};
constexpr size_t kSegmentStringsOffset = 64;
constexpr size_t kManifestHeaderSize = 64;
constexpr size_t kGopHeadSize = 32;
constexpr size_t kPacketEntrySize = 32;
constexpr size_t kRunHeadSize = 24;
constexpr size_t kIndexHeadSize = 40;
constexpr size_t kSourceEntrySize = 48;
constexpr size_t kRunEntrySize = 32;
constexpr size_t kGopEntrySize = 40;
constexpr size_t kTagSlotHeadSize = 32;
constexpr size_t kTagRecordHeadSize = 8;
constexpr uint16_t kTagRecord = 1;
// The manifest refers to segments for its GOPs; an exported replay that holds them
// itself will clear it.
constexpr uint32_t kFlagSegments = 1;
// The configuration as the encoder gave it (Annex B for H.264 and HEVC).
constexpr uint32_t kConfigAsGiven = 1;
constexpr uint16_t kPacketKeyframe = 1;
constexpr uint32_t kNoSource = 0xFFFFFFFF;

constexpr uint32_t fourcc(std::string_view text)
{
	return uint32_t{static_cast<uint8_t>(text[0])} | uint32_t{static_cast<uint8_t>(text[1])} << 8 |
	       uint32_t{static_cast<uint8_t>(text[2])} << 16 | uint32_t{static_cast<uint8_t>(text[3])} << 24;
}

constexpr uint32_t kRunChunk = fourcc("RUN ");
constexpr uint32_t kGopChunk = fourcc("GOP ");
constexpr uint32_t kIndexChunk = fourcc("INDX");
constexpr uint32_t kTagMagic = fourcc("TPMD");

constexpr uint64_t aligned(uint64_t size)
{
	return (size + kReplayAlignment - 1) / kReplayAlignment * kReplayAlignment;
}

void put16(std::span<uint8_t> bytes, size_t at, uint16_t value)
{
	bytes[at] = static_cast<uint8_t>(value);
	bytes[at + 1] = static_cast<uint8_t>(value >> 8);
}

void put32(std::span<uint8_t> bytes, size_t at, uint32_t value)
{
	for (size_t i = 0; i < 4; ++i) {
		bytes[at + i] = static_cast<uint8_t>(value >> (8 * i));
	}
}

void put64(std::span<uint8_t> bytes, size_t at, uint64_t value)
{
	for (size_t i = 0; i < 8; ++i) {
		bytes[at + i] = static_cast<uint8_t>(value >> (8 * i));
	}
}

void putTime(std::span<uint8_t> bytes, size_t at, Nanoseconds value)
{
	put64(bytes, at, static_cast<uint64_t>(value.count()));
}

void putBytes(std::span<uint8_t> bytes, size_t at, std::span<const uint8_t> value)
{
	std::copy(value.begin(), value.end(), bytes.subspan(at, value.size()).begin());
}

std::vector<uint8_t> bytesOf(const std::string &text)
{
	return {text.begin(), text.end()};
}

uint16_t get16(std::span<const uint8_t> bytes, size_t at)
{
	return static_cast<uint16_t>(bytes[at] | bytes[at + 1] << 8);
}

uint32_t get32(std::span<const uint8_t> bytes, size_t at)
{
	uint32_t value = 0;
	for (size_t i = 0; i < 4; ++i) {
		value |= uint32_t{bytes[at + i]} << (8 * i);
	}
	return value;
}

uint64_t get64(std::span<const uint8_t> bytes, size_t at)
{
	uint64_t value = 0;
	for (size_t i = 0; i < 8; ++i) {
		value |= uint64_t{bytes[at + i]} << (8 * i);
	}
	return value;
}

Nanoseconds getTime(std::span<const uint8_t> bytes, size_t at)
{
	return Nanoseconds{static_cast<int64_t>(get64(bytes, at))};
}

std::string getString(std::span<const uint8_t> bytes, size_t at, size_t size)
{
	const std::span<const uint8_t> part = bytes.subspan(at, size);
	return {part.begin(), part.end()};
}

bool hasMagic(std::span<const uint8_t> bytes, const std::array<uint8_t, 8> &magic)
{
	return bytes.size() >= magic.size() && std::equal(magic.begin(), magic.end(), bytes.begin());
}

uint32_t codecNumber(VideoCodec codec)
{
	return codec == VideoCodec::H264 ? 1 : 2;
}

std::optional<VideoCodec> codecOfNumber(uint32_t number)
{
	switch (number) {
	case 1:
		return VideoCodec::H264;
	case 2:
		return VideoCodec::Hevc;
	default:
		return std::nullopt;
	}
}

// Writes the header of the chunk whose payload is already in place at offset + 32, and
// returns where the chunk is.
ChunkPlace sealChunk(std::span<uint8_t> bytes, uint64_t offset, uint32_t type, uint32_t sequence, uint64_t payloadSize)
{
	const std::span<uint8_t> header = bytes.subspan(offset, kChunkHeaderSize);
	put32(header, 0, type);
	put32(header, 4, kChunkHeaderSize);
	put64(header, 8, payloadSize);
	put32(header, 16, kNoSource);
	put32(header, 20, sequence);
	put32(header, 24, crc32c(bytes.subspan(offset + kChunkHeaderSize, payloadSize)));
	const uint32_t headerCrc = crc32c(header.first(28));
	put32(header, 28, headerCrc);
	return {offset, kChunkHeaderSize + payloadSize, headerCrc};
}

// The payload of an intact chunk of this type, or nothing.
std::optional<std::span<const uint8_t>> chunkPayload(std::span<const uint8_t> chunk, uint32_t type)
{
	if (chunk.size() < kChunkHeaderSize || get32(chunk, 0) != type || get32(chunk, 4) != kChunkHeaderSize ||
	    crc32c(chunk.first(28)) != get32(chunk, 28)) {
		return std::nullopt;
	}
	const uint64_t payloadSize = get64(chunk, 8);
	if (payloadSize > chunk.size() - kChunkHeaderSize) {
		return std::nullopt;
	}
	const std::span<const uint8_t> payload = chunk.subspan(kChunkHeaderSize, payloadSize);
	if (crc32c(payload) != get32(chunk, 24)) {
		return std::nullopt;
	}
	return payload;
}

// Makes room for one chunk with this payload size at the end of out, padded.
uint64_t growForChunk(std::vector<uint8_t> &out, uint64_t payloadSize)
{
	if (out.size() % kReplayAlignment != 0) {
		throw std::logic_error("chunks start at aligned offsets");
	}
	const uint64_t offset = out.size();
	out.resize(offset + aligned(kChunkHeaderSize + payloadSize));
	return offset;
}

std::span<const uint8_t> gopBytes(const Gop &gop)
{
	const std::span<const PacketRecord> packets = gop.packets();
	const PacketRecord &last = packets.back();
	// Packets lie back to back from the first one's offset.
	const std::span<const uint8_t> first = gop.packetData(0);
	return {first.data(), last.offset + last.size - packets.front().offset};
}

} // namespace

uint32_t runKeyOf(VideoCodec codec, const CodecConfig *config) noexcept
{
	const std::array<uint8_t, 1> codecByte = {static_cast<uint8_t>(codecNumber(codec))};
	const uint32_t crc = crc32c(codecByte);
	return config ? crc32c(*config, crc) : crc;
}

GopKey gopKeyOf(const Gop &gop) noexcept
{
	return {runKeyOf(gop.codec(), gop.codecConfig()), gop.packets().front().pts, gop.startTime(),
		static_cast<uint32_t>(gop.packets().size())};
}

std::vector<uint8_t> encodeSegmentHeader(const SegmentHeader &header)
{
	if (header.sourceKey.size() > 0xFFFF || header.sourceName.size() > 0xFFFF ||
	    kSegmentStringsOffset + header.sourceKey.size() + header.sourceName.size() > kReplayAlignment) {
		throw std::length_error("segment header strings too long");
	}
	std::vector<uint8_t> bytes(kReplayAlignment, 0);
	putBytes(bytes, 0, kSegmentMagic);
	put16(bytes, 8, kReplayFormatMajor);
	put16(bytes, 10, kReplayFormatMinor);
	put32(bytes, 12, static_cast<uint32_t>(kReplayAlignment));
	put32(bytes, 16, header.sequence);
	put16(bytes, 20, static_cast<uint16_t>(header.sourceKey.size()));
	put16(bytes, 22, static_cast<uint16_t>(header.sourceName.size()));
	putBytes(bytes, 24, header.broadcast);
	putBytes(bytes, kSegmentStringsOffset, bytesOf(header.sourceKey));
	putBytes(bytes, kSegmentStringsOffset + header.sourceKey.size(), bytesOf(header.sourceName));
	const size_t end = kSegmentStringsOffset + header.sourceKey.size() + header.sourceName.size();
	// Every byte up to the end of the strings but the CRC itself.
	const uint32_t crc = crc32c(std::span<const uint8_t>(bytes).subspan(60, end - 60),
				    crc32c(std::span<const uint8_t>(bytes).first(56)));
	put32(bytes, 56, crc);
	return bytes;
}

std::optional<SegmentHeader> decodeSegmentHeader(std::span<const uint8_t> bytes)
{
	if (bytes.size() < kReplayAlignment || !hasMagic(bytes, kSegmentMagic) ||
	    get16(bytes, 8) != kReplayFormatMajor || get32(bytes, 12) != kReplayAlignment) {
		return std::nullopt;
	}
	const size_t keySize = get16(bytes, 20);
	const size_t nameSize = get16(bytes, 22);
	if (kSegmentStringsOffset + keySize + nameSize > kReplayAlignment) {
		return std::nullopt;
	}
	const uint32_t crc =
		crc32c(bytes.subspan(60, kSegmentStringsOffset - 60 + keySize + nameSize), crc32c(bytes.first(56)));
	if (crc != get32(bytes, 56)) {
		return std::nullopt;
	}
	SegmentHeader header;
	std::copy_n(bytes.subspan(24, header.broadcast.size()).begin(), header.broadcast.size(),
		    header.broadcast.begin());
	header.sequence = get32(bytes, 16);
	header.sourceKey = getString(bytes, kSegmentStringsOffset, keySize);
	header.sourceName = getString(bytes, kSegmentStringsOffset + keySize, nameSize);
	return header;
}

ChunkPlace appendRunChunk(std::vector<uint8_t> &out, uint32_t sequence, const Gop &gop)
{
	const CodecConfig *config = gop.codecConfig();
	const size_t configSize = config ? config->size() : 0;
	const uint64_t offset = growForChunk(out, kRunHeadSize + configSize);
	const std::span<uint8_t> payload =
		std::span<uint8_t>(out).subspan(offset + kChunkHeaderSize, kRunHeadSize + configSize);
	put32(payload, 0, codecNumber(gop.codec()));
	put32(payload, 4, kConfigAsGiven);
	putTime(payload, 8, gop.frameDuration());
	put32(payload, 16, runKeyOf(gop.codec(), config));
	put32(payload, 20, static_cast<uint32_t>(configSize));
	if (config) {
		putBytes(payload, kRunHeadSize, *config);
	}
	return sealChunk(out, offset, kRunChunk, sequence, payload.size());
}

ChunkPlace appendGopChunk(std::vector<uint8_t> &out, uint32_t sequence, const Gop &gop)
{
	const std::span<const PacketRecord> packets = gop.packets();
	const std::span<const uint8_t> data = gopBytes(gop);
	const uint64_t payloadSize = kGopHeadSize + kPacketEntrySize * packets.size() + data.size();
	const uint64_t offset = growForChunk(out, payloadSize);
	const std::span<uint8_t> payload = std::span<uint8_t>(out).subspan(offset + kChunkHeaderSize, payloadSize);
	const GopKey key = gopKeyOf(gop);
	put32(payload, 0, key.packetCount);
	put32(payload, 4, key.runKey);
	put64(payload, 8, data.size());
	put64(payload, 16, static_cast<uint64_t>(key.firstPts));
	putTime(payload, 24, key.firstTime);
	for (size_t i = 0; i < packets.size(); ++i) {
		const size_t at = kGopHeadSize + kPacketEntrySize * i;
		put64(payload, at, static_cast<uint64_t>(packets[i].pts));
		put64(payload, at + 8, static_cast<uint64_t>(packets[i].dts));
		putTime(payload, at + 16, packets[i].time);
		put32(payload, at + 24, static_cast<uint32_t>(packets[i].size));
		put32(payload, at + 28, packets[i].keyframe ? kPacketKeyframe : 0);
	}
	putBytes(payload, kGopHeadSize + kPacketEntrySize * packets.size(), data);
	return sealChunk(out, offset, kGopChunk, sequence, payloadSize);
}

uint64_t runChunkSize(const Gop &gop) noexcept
{
	const CodecConfig *config = gop.codecConfig();
	return aligned(kChunkHeaderSize + kRunHeadSize + (config ? config->size() : 0));
}

uint64_t gopChunkSize(const Gop &gop) noexcept
{
	return aligned(kChunkHeaderSize + kGopHeadSize + kPacketEntrySize * gop.packets().size() +
		       gopBytes(gop).size());
}

std::shared_ptr<const Gop> decodeGopChunk(std::span<const uint8_t> chunk, uint32_t headerCrc, const StoredRun &run)
{
	if (chunk.size() < kChunkHeaderSize || get32(chunk, 28) != headerCrc) {
		return nullptr;
	}
	const std::optional<std::span<const uint8_t>> payload = chunkPayload(chunk, kGopChunk);
	if (!payload || payload->size() < kGopHeadSize) {
		return nullptr;
	}
	const std::span<const uint8_t> bytes = *payload;
	const uint64_t count = get32(bytes, 0);
	const uint64_t dataSize = get64(bytes, 8);
	if (count == 0 || get32(bytes, 4) != run.key || count > (bytes.size() - kGopHeadSize) / kPacketEntrySize ||
	    dataSize != bytes.size() - kGopHeadSize - kPacketEntrySize * count) {
		return nullptr;
	}
	const std::span<const uint8_t> data = bytes.subspan(kGopHeadSize + kPacketEntrySize * count);
	GopBuilder builder(run.frameDuration);
	builder.setCodecConfig(run.codec, run.config);
	uint64_t offset = 0;
	Nanoseconds previous{0};
	for (uint64_t i = 0; i < count; ++i) {
		const size_t at = kGopHeadSize + kPacketEntrySize * i;
		EncodedPacket packet;
		packet.pts = static_cast<int64_t>(get64(bytes, at));
		packet.dts = static_cast<int64_t>(get64(bytes, at + 8));
		packet.time = getTime(bytes, at + 16);
		const uint64_t size = get32(bytes, at + 24);
		packet.keyframe = (get32(bytes, at + 28) & kPacketKeyframe) != 0;
		if ((i == 0 && !packet.keyframe) || (i > 0 && packet.time <= previous) || size > data.size() - offset) {
			return nullptr;
		}
		packet.data = data.subspan(offset, size);
		builder.append(packet);
		offset += size;
		previous = packet.time;
	}
	if (offset != data.size()) {
		return nullptr;
	}
	return builder.seal();
}

std::optional<uint64_t> gopChunkSizeFromHeader(std::span<const uint8_t> header, uint32_t headerCrc)
{
	if (header.size() < kChunkHeaderSize || get32(header, 0) != kGopChunk || get32(header, 4) != kChunkHeaderSize ||
	    get32(header, 28) != headerCrc || crc32c(header.first(28)) != headerCrc) {
		return std::nullopt;
	}
	const uint64_t payloadSize = get64(header, 8);
	if (payloadSize > std::numeric_limits<uint64_t>::max() - kChunkHeaderSize) {
		return std::nullopt;
	}
	return kChunkHeaderSize + payloadSize;
}

std::optional<GopKey> peekGopKey(std::span<const uint8_t> chunkStart)
{
	if (chunkStart.size() < kChunkHeaderSize + kGopHeadSize || get32(chunkStart, 0) != kGopChunk ||
	    crc32c(chunkStart.first(28)) != get32(chunkStart, 28) || get64(chunkStart, 8) < kGopHeadSize) {
		return std::nullopt;
	}
	const std::span<const uint8_t> head = chunkStart.subspan(kChunkHeaderSize, kGopHeadSize);
	return GopKey{get32(head, 4), static_cast<int64_t>(get64(head, 16)), getTime(head, 24), get32(head, 0)};
}

size_t firstFrameOf(const StoredSource &source, size_t gop) noexcept
{
	size_t frame = 0;
	for (size_t i = 0; i < gop && i < source.gops.size(); ++i) {
		frame += source.gops[i].packetCount;
	}
	return frame;
}

FrameLocation locate(const StoredSource &source, Nanoseconds t) noexcept
{
	if (source.frameTimes.empty()) {
		return {};
	}
	t = std::clamp(t, source.in, source.out);
	const auto after = std::upper_bound(source.frameTimes.begin(), source.frameTimes.end(), t);
	size_t frame = after == source.frameTimes.begin() ? 0
							  : static_cast<size_t>(after - source.frameTimes.begin()) - 1;
	for (size_t gop = 0; gop < source.gops.size(); ++gop) {
		if (frame < source.gops[gop].packetCount) {
			return {gop, frame};
		}
		frame -= source.gops[gop].packetCount;
	}
	return {};
}

bool tagsFit(std::span<const std::string> tags) noexcept
{
	size_t size = kTagSlotHeadSize;
	for (const std::string &tag : tags) {
		if (tag.size() > kTagSlotSize || size + kTagRecordHeadSize + tag.size() > kTagSlotSize) {
			return false;
		}
		size += kTagRecordHeadSize + tag.size();
	}
	return true;
}

std::vector<uint8_t> encodeTagSlot(std::span<const std::string> tags, uint64_t generation)
{
	if (!tagsFit(tags)) {
		throw std::length_error("tags do not fit their slot");
	}
	std::vector<uint8_t> slot(kTagSlotSize, 0);
	size_t at = kTagSlotHeadSize;
	for (const std::string &tag : tags) {
		put16(slot, at, kTagRecord);
		put32(slot, at + 4, static_cast<uint32_t>(tag.size()));
		putBytes(slot, at + kTagRecordHeadSize, bytesOf(tag));
		at += kTagRecordHeadSize + tag.size();
	}
	const size_t payloadSize = at - kTagSlotHeadSize;
	put32(slot, 0, kTagMagic);
	put32(slot, 4, static_cast<uint32_t>(payloadSize));
	put64(slot, 8, generation);
	put32(slot, 16, crc32c(std::span<const uint8_t>(slot).subspan(kTagSlotHeadSize, payloadSize)));
	put32(slot, 28, crc32c(std::span<const uint8_t>(slot).first(28)));
	return slot;
}

std::optional<TagSlot> decodeTagSlot(std::span<const uint8_t> slot)
{
	if (slot.size() < kTagSlotSize || get32(slot, 0) != kTagMagic || crc32c(slot.first(28)) != get32(slot, 28)) {
		return std::nullopt;
	}
	const size_t payloadSize = get32(slot, 4);
	if (payloadSize > kTagSlotSize - kTagSlotHeadSize ||
	    crc32c(slot.subspan(kTagSlotHeadSize, payloadSize)) != get32(slot, 16)) {
		return std::nullopt;
	}
	TagSlot result;
	result.generation = get64(slot, 8);
	const size_t end = kTagSlotHeadSize + payloadSize;
	size_t at = kTagSlotHeadSize;
	while (at < end) {
		if (end - at < kTagRecordHeadSize) {
			return std::nullopt;
		}
		const uint16_t type = get16(slot, at);
		const size_t length = get32(slot, at + 4);
		if (length > end - at - kTagRecordHeadSize) {
			return std::nullopt;
		}
		// Records of other types come from a newer minor version; this one has none to
		// keep, so they are skipped.
		if (type == kTagRecord) {
			result.tags.push_back(getString(slot, at + kTagRecordHeadSize, length));
		}
		at += kTagRecordHeadSize + length;
	}
	return result;
}

std::vector<std::string> decodeTags(std::span<const uint8_t> manifestHead)
{
	if (manifestHead.size() < kManifestIndexOffset) {
		return {};
	}
	std::optional<TagSlot> a = decodeTagSlot(manifestHead.subspan(kTagSlotA, kTagSlotSize));
	std::optional<TagSlot> b = decodeTagSlot(manifestHead.subspan(kTagSlotB, kTagSlotSize));
	if (a && (!b || a->generation >= b->generation)) {
		return std::move(a->tags);
	}
	if (b) {
		return std::move(b->tags);
	}
	return {};
}

TagWrite nextTagWrite(std::span<const uint8_t> manifestHead)
{
	const auto generationAt = [&](uint64_t offset) -> uint64_t {
		if (manifestHead.size() < offset + kTagSlotSize) {
			return 0;
		}
		const std::optional<TagSlot> slot = decodeTagSlot(manifestHead.subspan(offset, kTagSlotSize));
		return slot ? slot->generation : 0;
	};
	const uint64_t a = generationAt(kTagSlotA);
	const uint64_t b = generationAt(kTagSlotB);
	return {a < b ? kTagSlotA : kTagSlotB, std::max(a, b) + 1};
}

std::vector<uint8_t> encodeManifest(const ReplayIndex &index, std::span<const std::string> tags)
{
	uint64_t runCount = 0;
	uint64_t gopCount = 0;
	uint64_t frameCount = 0;
	uint64_t blobSize = 0;
	for (const StoredSource &source : index.sources) {
		if (source.key.size() > 0xFFFF || source.name.size() > 0xFFFF) {
			throw std::length_error("source key or name too long");
		}
		runCount += source.runs.size();
		gopCount += source.gops.size();
		frameCount += source.frameTimes.size();
		blobSize += source.key.size() + source.name.size();
		for (const StoredRun &run : source.runs) {
			blobSize += run.config ? run.config->size() : 0;
		}
	}
	constexpr uint64_t kCountLimit = std::numeric_limits<uint32_t>::max();
	if (index.sources.size() > kCountLimit || runCount > kCountLimit || gopCount > kCountLimit ||
	    frameCount > kCountLimit || blobSize > kCountLimit) {
		throw std::length_error("replay index too large");
	}
	const uint64_t sourcesAt = kIndexHeadSize;
	const uint64_t runsAt = sourcesAt + kSourceEntrySize * index.sources.size();
	const uint64_t gopsAt = runsAt + kRunEntrySize * runCount;
	const uint64_t framesAt = gopsAt + kGopEntrySize * gopCount;
	const uint64_t blobAt = framesAt + 8 * frameCount;
	const uint64_t payloadSize = blobAt + blobSize;
	const uint64_t fileSize = kManifestIndexOffset + kChunkHeaderSize + payloadSize + kManifestFooterSize;

	std::vector<uint8_t> file(fileSize, 0);
	const std::span<uint8_t> bytes(file);
	putBytes(bytes, 0, kManifestMagic);
	put16(bytes, 8, kReplayFormatMajor);
	put16(bytes, 10, kReplayFormatMinor);
	put32(bytes, 12, kManifestHeaderSize);
	put32(bytes, 16, kFlagSegments);
	put32(bytes, 20, static_cast<uint32_t>(kReplayAlignment));
	putBytes(bytes, 24, index.id);
	put64(bytes, 40, static_cast<uint64_t>(index.capturedAtUtc));
	putTime(bytes, 48, index.capturedAtClock);
	put32(bytes, 56, static_cast<uint32_t>(kTagSlotSize));
	put32(bytes, 60, crc32c(bytes.first(60)));

	const std::vector<uint8_t> slot = encodeTagSlot(tags, 1);
	putBytes(bytes, kTagSlotA, slot);
	putBytes(bytes, kTagSlotB, slot);

	const std::span<uint8_t> payload = bytes.subspan(kManifestIndexOffset + kChunkHeaderSize, payloadSize);
	put32(payload, 0, static_cast<uint32_t>(index.sources.size()));
	put32(payload, 4, static_cast<uint32_t>(runCount));
	put32(payload, 8, static_cast<uint32_t>(gopCount));
	put32(payload, 12, static_cast<uint32_t>(frameCount));
	put32(payload, 16, static_cast<uint32_t>(blobSize));
	putTime(payload, 24, index.start);
	putTime(payload, 32, index.end);
	uint64_t run = 0;
	uint64_t gop = 0;
	uint64_t frame = 0;
	uint64_t blob = 0;
	const auto addToBlob = [&](std::span<const uint8_t> value) {
		const uint64_t at = blob;
		putBytes(payload, blobAt + blob, value);
		blob += value.size();
		return static_cast<uint32_t>(at);
	};
	for (size_t s = 0; s < index.sources.size(); ++s) {
		const StoredSource &source = index.sources[s];
		const uint64_t entry = sourcesAt + kSourceEntrySize * s;
		put32(payload, entry, static_cast<uint32_t>(run));
		put32(payload, entry + 4, static_cast<uint32_t>(source.runs.size()));
		put32(payload, entry + 8, static_cast<uint32_t>(gop));
		put32(payload, entry + 12, static_cast<uint32_t>(source.gops.size()));
		putTime(payload, entry + 16, source.in);
		putTime(payload, entry + 24, source.out);
		put32(payload, entry + 32, addToBlob(bytesOf(source.key)));
		put16(payload, entry + 36, static_cast<uint16_t>(source.key.size()));
		put16(payload, entry + 38, static_cast<uint16_t>(source.name.size()));
		addToBlob(bytesOf(source.name));
		put32(payload, entry + 40, static_cast<uint32_t>(frame));
		put32(payload, entry + 44, static_cast<uint32_t>(source.frameTimes.size()));
		for (const StoredRun &stored : source.runs) {
			const uint64_t at = runsAt + kRunEntrySize * run++;
			put32(payload, at, codecNumber(stored.codec));
			put32(payload, at + 4, kConfigAsGiven);
			putTime(payload, at + 8, stored.frameDuration);
			put32(payload, at + 16, stored.key);
			const std::span<const uint8_t> config = stored.config ? std::span<const uint8_t>(*stored.config)
									      : std::span<const uint8_t>();
			put32(payload, at + 20, addToBlob(config));
			put32(payload, at + 24, static_cast<uint32_t>(config.size()));
		}
		for (const StoredGop &stored : source.gops) {
			const uint64_t at = gopsAt + kGopEntrySize * gop++;
			put32(payload, at, stored.segment);
			put32(payload, at + 4, stored.packetCount);
			put64(payload, at + 8, stored.offset);
			put64(payload, at + 16, stored.size);
			put32(payload, at + 24, stored.headerCrc);
			put32(payload, at + 28, stored.run);
		}
		for (const Nanoseconds time : source.frameTimes) {
			putTime(payload, framesAt + 8 * frame++, time);
		}
	}
	sealChunk(bytes, kManifestIndexOffset, kIndexChunk, 0, payloadSize);

	const std::span<uint8_t> footer = bytes.last(kManifestFooterSize);
	putBytes(footer, 0, kFooterMagic);
	put16(footer, 8, kReplayFormatMajor);
	put16(footer, 10, kReplayFormatMinor);
	put64(footer, 16, kManifestIndexOffset);
	put64(footer, 24, kChunkHeaderSize + payloadSize);
	putBytes(footer, 32, index.id);
	put64(footer, 48, fileSize);
	put32(footer, 60, crc32c(footer.first(60)));
	return file;
}

std::optional<ReplayIndex> decodeManifest(std::span<const uint8_t> file)
{
	if (file.size() < kManifestIndexOffset + kChunkHeaderSize + kIndexHeadSize + kManifestFooterSize ||
	    !hasMagic(file, kManifestMagic) || get16(file, 8) != kReplayFormatMajor ||
	    get32(file, 12) != kManifestHeaderSize || (get32(file, 16) & kFlagSegments) == 0 ||
	    get32(file, 20) != kReplayAlignment || get32(file, 56) != kTagSlotSize ||
	    crc32c(file.first(60)) != get32(file, 60)) {
		return std::nullopt;
	}
	const std::span<const uint8_t> footer = file.last(kManifestFooterSize);
	if (!hasMagic(footer, kFooterMagic) || get16(footer, 8) != kReplayFormatMajor ||
	    crc32c(footer.first(60)) != get32(footer, 60) || get64(footer, 48) != file.size() ||
	    get64(footer, 16) != kManifestIndexOffset ||
	    get64(footer, 24) != file.size() - kManifestIndexOffset - kManifestFooterSize ||
	    !std::equal(footer.begin() + 32, footer.begin() + 48, file.begin() + 24)) {
		return std::nullopt;
	}
	const std::optional<std::span<const uint8_t>> found =
		chunkPayload(file.subspan(kManifestIndexOffset, get64(footer, 24)), kIndexChunk);
	if (!found || found->size() != get64(footer, 24) - kChunkHeaderSize) {
		return std::nullopt;
	}
	const std::span<const uint8_t> payload = *found;
	const uint64_t sourceCount = get32(payload, 0);
	const uint64_t runCount = get32(payload, 4);
	const uint64_t gopCount = get32(payload, 8);
	const uint64_t frameCount = get32(payload, 12);
	const uint64_t blobSize = get32(payload, 16);
	const uint64_t runsAt = kIndexHeadSize + kSourceEntrySize * sourceCount;
	const uint64_t gopsAt = runsAt + kRunEntrySize * runCount;
	const uint64_t framesAt = gopsAt + kGopEntrySize * gopCount;
	const uint64_t blobAt = framesAt + 8 * frameCount;
	if (blobAt + blobSize != payload.size()) {
		return std::nullopt;
	}
	const std::span<const uint8_t> blob = payload.subspan(blobAt, blobSize);
	const auto inBlob = [&](uint64_t at, uint64_t size) {
		return at <= blob.size() && size <= blob.size() - at;
	};

	ReplayIndex index;
	std::copy_n(file.subspan(24, index.id.size()).begin(), index.id.size(), index.id.begin());
	index.capturedAtUtc = static_cast<int64_t>(get64(file, 40));
	index.capturedAtClock = getTime(file, 48);
	index.start = getTime(payload, 24);
	index.end = getTime(payload, 32);
	uint64_t run = 0;
	uint64_t gop = 0;
	uint64_t frame = 0;
	for (uint64_t s = 0; s < sourceCount; ++s) {
		const uint64_t entry = kIndexHeadSize + kSourceEntrySize * s;
		const uint64_t sourceRuns = get32(payload, entry + 4);
		const uint64_t sourceGops = get32(payload, entry + 12);
		const uint64_t keyAt = get32(payload, entry + 32);
		const uint64_t keySize = get16(payload, entry + 36);
		const uint64_t nameSize = get16(payload, entry + 38);
		const uint64_t sourceFrames = get32(payload, entry + 44);
		// Sources follow each other in every table, with nothing left over.
		if (get32(payload, entry) != run || get32(payload, entry + 8) != gop ||
		    get32(payload, entry + 40) != frame || sourceRuns == 0 || sourceGops == 0 ||
		    sourceRuns > runCount - run || sourceGops > gopCount - gop || sourceFrames > frameCount - frame ||
		    !inBlob(keyAt, keySize + nameSize)) {
			return std::nullopt;
		}
		StoredSource source;
		source.key = getString(blob, keyAt, keySize);
		source.name = getString(blob, keyAt + keySize, nameSize);
		source.in = getTime(payload, entry + 16);
		source.out = getTime(payload, entry + 24);
		for (uint64_t r = 0; r < sourceRuns; ++r, ++run) {
			const uint64_t at = runsAt + kRunEntrySize * run;
			const std::optional<VideoCodec> codec = codecOfNumber(get32(payload, at));
			const uint64_t configAt = get32(payload, at + 20);
			const uint64_t configSize = get32(payload, at + 24);
			if (!codec || get32(payload, at + 4) != kConfigAsGiven || !inBlob(configAt, configSize)) {
				return std::nullopt;
			}
			StoredRun stored;
			stored.codec = *codec;
			stored.frameDuration = getTime(payload, at + 8);
			stored.key = get32(payload, at + 16);
			if (configSize > 0) {
				const std::span<const uint8_t> config = blob.subspan(configAt, configSize);
				stored.config = std::make_shared<const CodecConfig>(config.begin(), config.end());
			}
			if (stored.key != runKeyOf(stored.codec, stored.config.get())) {
				return std::nullopt;
			}
			source.runs.push_back(std::move(stored));
		}
		uint64_t packets = 0;
		for (uint64_t g = 0; g < sourceGops; ++g, ++gop) {
			const uint64_t at = gopsAt + kGopEntrySize * gop;
			StoredGop stored;
			stored.segment = get32(payload, at);
			stored.packetCount = get32(payload, at + 4);
			stored.offset = get64(payload, at + 8);
			stored.size = get64(payload, at + 16);
			stored.headerCrc = get32(payload, at + 24);
			stored.run = get32(payload, at + 28);
			if (stored.packetCount == 0 || stored.run >= source.runs.size() ||
			    stored.size < kChunkHeaderSize || stored.offset % kReplayAlignment != 0) {
				return std::nullopt;
			}
			packets += stored.packetCount;
			source.gops.push_back(stored);
		}
		if (packets != sourceFrames) {
			return std::nullopt;
		}
		source.frameTimes.reserve(sourceFrames);
		for (uint64_t f = 0; f < sourceFrames; ++f, ++frame) {
			const Nanoseconds time = getTime(payload, framesAt + 8 * frame);
			if (!source.frameTimes.empty() && time <= source.frameTimes.back()) {
				return std::nullopt;
			}
			source.frameTimes.push_back(time);
		}
		if (source.in > source.out || source.in < source.frameTimes.front() ||
		    source.out > source.frameTimes.back()) {
			return std::nullopt;
		}
		index.sources.push_back(std::move(source));
	}
	if (run != runCount || gop != gopCount || frame != frameCount) {
		return std::nullopt;
	}
	return index;
}

} // namespace tapeloop
