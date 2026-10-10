// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/ReplayFormat.hpp"

#include "SyntheticEncoder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

using tapeloop::ChunkPlace;
using tapeloop::CodecConfig;
using tapeloop::Gop;
using tapeloop::GopBuilder;
using tapeloop::GopKey;
using tapeloop::Nanoseconds;
using tapeloop::ReplayIndex;
using tapeloop::StoredGop;
using tapeloop::StoredRun;
using tapeloop::StoredSource;
using tapeloop::VideoCodec;
using tapeloop::test::SyntheticEncoder;

namespace {

using Gops = std::vector<std::shared_ptr<const Gop>>;

SyntheticEncoder::Config smallFrames(int64_t firstFrame = 0)
{
	SyntheticEncoder::Config config;
	config.gopLength = 5;
	config.keyframeSize = 300;
	config.frameSize = 100;
	config.firstFrame = firstFrame;
	return config;
}

Gops makeGops(size_t count, VideoCodec codec, std::shared_ptr<const CodecConfig> config, int64_t firstFrame = 0)
{
	SyntheticEncoder encoder(smallFrames(firstFrame));
	GopBuilder builder(encoder.frameDuration());
	builder.setCodecConfig(codec, std::move(config));
	Gops gops;
	for (size_t gop = 0; gop < count; ++gop) {
		for (int frame = 0; frame < 5; ++frame) {
			builder.append(encoder.next());
		}
		gops.push_back(builder.seal());
	}
	return gops;
}

StoredRun runOf(const Gop &gop)
{
	return {gop.codec(), gop.frameDuration(),
		gop.codecConfig() ? std::make_shared<const CodecConfig>(*gop.codecConfig()) : nullptr,
		tapeloop::runKeyOf(gop.codec(), gop.codecConfig())};
}

void requireSame(const Gop &read, const Gop &written)
{
	REQUIRE(read.packets().size() == written.packets().size());
	for (size_t i = 0; i < read.packets().size(); ++i) {
		const tapeloop::PacketRecord &a = read.packets()[i];
		const tapeloop::PacketRecord &b = written.packets()[i];
		CHECK(a.pts == b.pts);
		CHECK(a.dts == b.dts);
		CHECK(a.time == b.time);
		CHECK(a.keyframe == b.keyframe);
		const std::span<const uint8_t> readData = read.packetData(i);
		const std::span<const uint8_t> writtenData = written.packetData(i);
		CHECK(std::equal(readData.begin(), readData.end(), writtenData.begin(), writtenData.end()));
	}
	CHECK(read.codec() == written.codec());
	CHECK(read.frameDuration() == written.frameDuration());
	REQUIRE((read.codecConfig() == nullptr) == (written.codecConfig() == nullptr));
	if (read.codecConfig()) {
		CHECK(*read.codecConfig() == *written.codecConfig());
	}
}

// A source of the index for these GOPs, as if they were written at these places of
// segment 0.
StoredSource sourceOf(const std::string &key, const Gops &gops, const std::vector<ChunkPlace> &places)
{
	StoredSource source;
	source.key = key;
	source.name = "Camera " + key;
	for (size_t i = 0; i < gops.size(); ++i) {
		const Gop &gop = *gops[i];
		const uint32_t runKey = tapeloop::runKeyOf(gop.codec(), gop.codecConfig());
		if (source.runs.empty() || source.runs.back().key != runKey) {
			source.runs.push_back(runOf(gop));
		}
		source.gops.push_back({0, static_cast<uint32_t>(gop.packets().size()), places[i].offset, places[i].size,
				       places[i].headerCrc, static_cast<uint32_t>(source.runs.size() - 1)});
		for (const tapeloop::PacketRecord &packet : gop.packets()) {
			source.frameTimes.push_back(packet.time);
		}
	}
	source.in = source.frameTimes.at(1);
	source.out = source.frameTimes.back();
	return source;
}

ReplayIndex twoSources()
{
	const auto config = std::make_shared<const CodecConfig>(CodecConfig{0, 0, 0, 1, 0x67, 0x42});
	Gops first = makeGops(2, VideoCodec::H264, config);
	const Gops hevc = makeGops(2, VideoCodec::Hevc, nullptr, 10);
	first.insert(first.end(), hevc.begin(), hevc.end());
	const Gops second = makeGops(3, VideoCodec::H264, config);
	std::vector<ChunkPlace> places;
	for (size_t i = 0; i < 4; ++i) {
		places.push_back({4096 * (i + 1), 1000 + i, static_cast<uint32_t>(i * 77)});
	}
	ReplayIndex index;
	index.id = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
	index.capturedAtUtc = 1'791'000'000'000'000'000;
	index.capturedAtClock = Nanoseconds{123'456'789};
	index.sources.push_back(sourceOf("a", first, places));
	index.sources.push_back(sourceOf("b", second, places));
	index.start = index.sources[0].in;
	index.end = index.sources[0].out;
	return index;
}

void requireSame(const ReplayIndex &a, const ReplayIndex &b)
{
	CHECK(a.id == b.id);
	CHECK(a.capturedAtUtc == b.capturedAtUtc);
	CHECK(a.capturedAtClock == b.capturedAtClock);
	CHECK(a.start == b.start);
	CHECK(a.end == b.end);
	REQUIRE(a.sources.size() == b.sources.size());
	for (size_t s = 0; s < a.sources.size(); ++s) {
		const StoredSource &x = a.sources[s];
		const StoredSource &y = b.sources[s];
		CHECK(x.key == y.key);
		CHECK(x.name == y.name);
		CHECK(x.in == y.in);
		CHECK(x.out == y.out);
		CHECK(x.frameTimes == y.frameTimes);
		REQUIRE(x.runs.size() == y.runs.size());
		for (size_t r = 0; r < x.runs.size(); ++r) {
			CHECK(x.runs[r].codec == y.runs[r].codec);
			CHECK(x.runs[r].frameDuration == y.runs[r].frameDuration);
			CHECK(x.runs[r].key == y.runs[r].key);
			REQUIRE((x.runs[r].config == nullptr) == (y.runs[r].config == nullptr));
			if (x.runs[r].config) {
				CHECK(*x.runs[r].config == *y.runs[r].config);
			}
		}
		REQUIRE(x.gops.size() == y.gops.size());
		for (size_t g = 0; g < x.gops.size(); ++g) {
			CHECK(x.gops[g].segment == y.gops[g].segment);
			CHECK(x.gops[g].packetCount == y.gops[g].packetCount);
			CHECK(x.gops[g].offset == y.gops[g].offset);
			CHECK(x.gops[g].size == y.gops[g].size);
			CHECK(x.gops[g].headerCrc == y.gops[g].headerCrc);
			CHECK(x.gops[g].run == y.gops[g].run);
		}
	}
}

} // namespace

TEST_CASE("a GOP chunk gives back the GOP it was written from")
{
	const auto config = std::make_shared<const CodecConfig>(CodecConfig{0, 0, 0, 1, 0x67, 0x64, 0x00, 0x28});
	const Gops gops = makeGops(3, VideoCodec::Hevc, config);
	std::vector<uint8_t> segment(tapeloop::kReplayAlignment, 0);
	const ChunkPlace run = tapeloop::appendRunChunk(segment, 0, *gops[0]);
	CHECK(run.offset == tapeloop::kReplayAlignment);
	std::vector<ChunkPlace> places;
	for (size_t i = 0; i < gops.size(); ++i) {
		places.push_back(tapeloop::appendGopChunk(segment, static_cast<uint32_t>(i + 1), *gops[i]));
	}
	CHECK(segment.size() % tapeloop::kReplayAlignment == 0);
	for (size_t i = 0; i < gops.size(); ++i) {
		const ChunkPlace &place = places[i];
		CHECK(place.offset % tapeloop::kReplayAlignment == 0);
		const std::span<const uint8_t> chunk =
			std::span<const uint8_t>(segment).subspan(place.offset, place.size);
		const std::shared_ptr<const Gop> read =
			tapeloop::decodeGopChunk(chunk, place.headerCrc, runOf(*gops[i]));
		REQUIRE(read);
		requireSame(*read, *gops[i]);
		const std::optional<GopKey> key = tapeloop::peekGopKey(chunk);
		REQUIRE(key);
		CHECK(*key == tapeloop::gopKeyOf(*gops[i]));
		CHECK(tapeloop::gopChunkSizeFromHeader(chunk.first(tapeloop::kChunkHeaderSize), place.headerCrc) ==
		      place.size);
	}
	// The chunk read can be longer than the chunk, padding included.
	const std::span<const uint8_t> padded = std::span<const uint8_t>(segment).subspan(places[0].offset, 8192);
	CHECK(tapeloop::decodeGopChunk(padded, places[0].headerCrc, runOf(*gops[0])));
	// Reading a RUN chunk where a GOP was meant finds nothing.
	CHECK_FALSE(tapeloop::decodeGopChunk(std::span<const uint8_t>(segment).subspan(run.offset, run.size),
					     run.headerCrc, runOf(*gops[0])));
	CHECK_FALSE(tapeloop::gopChunkSizeFromHeader(std::span<const uint8_t>(segment).subspan(run.offset, run.size),
						     run.headerCrc));
}

TEST_CASE("a chunk's size is known before it is appended")
{
	// A configuration that takes the RUN chunk past one aligned block.
	const auto large = std::make_shared<const CodecConfig>(CodecConfig(5000, 0x42));
	for (const auto &config : {std::shared_ptr<const CodecConfig>(), large}) {
		const Gops gops = makeGops(2, VideoCodec::H264, config);
		std::vector<uint8_t> segment(tapeloop::kReplayAlignment, 0);
		size_t before = segment.size();
		tapeloop::appendRunChunk(segment, 0, *gops[0]);
		CHECK(segment.size() - before == tapeloop::runChunkSize(*gops[0]));
		before = segment.size();
		tapeloop::appendGopChunk(segment, 1, *gops[1]);
		CHECK(segment.size() - before == tapeloop::gopChunkSize(*gops[1]));
	}
	CHECK(tapeloop::runChunkSize(*makeGops(1, VideoCodec::H264, large)[0]) == 2 * tapeloop::kReplayAlignment);
}

TEST_CASE("a GOP chunk that is damaged, cut short or not the one meant is refused")
{
	const Gops gops = makeGops(1, VideoCodec::H264, nullptr);
	std::vector<uint8_t> segment;
	const ChunkPlace place = tapeloop::appendGopChunk(segment, 0, *gops[0]);
	const StoredRun run = runOf(*gops[0]);
	const auto decode = [&](std::span<const uint8_t> chunk) {
		return tapeloop::decodeGopChunk(chunk, place.headerCrc, run);
	};
	const std::span<const uint8_t> chunk = std::span<const uint8_t>(segment).first(place.size);
	REQUIRE(decode(chunk));
	for (size_t at = 0; at < place.size; at += 7) {
		std::vector<uint8_t> damaged(chunk.begin(), chunk.end());
		damaged[at] ^= 0x10;
		CAPTURE(at);
		CHECK_FALSE(decode(damaged));
	}
	for (size_t size = 0; size < place.size; size += 11) {
		CHECK_FALSE(decode(chunk.first(size)));
	}
	CHECK_FALSE(tapeloop::decodeGopChunk(chunk, place.headerCrc + 1, run));
	CHECK_FALSE(tapeloop::gopChunkSizeFromHeader(chunk, place.headerCrc + 1));
	CHECK_FALSE(tapeloop::gopChunkSizeFromHeader(chunk.first(tapeloop::kChunkHeaderSize - 1), place.headerCrc));
	std::vector<uint8_t> header(chunk.begin(), chunk.begin() + tapeloop::kChunkHeaderSize);
	header[8] ^= 0x01;
	CHECK_FALSE(tapeloop::gopChunkSizeFromHeader(header, place.headerCrc));
	StoredRun other = run;
	other.key += 1;
	CHECK_FALSE(tapeloop::decodeGopChunk(chunk, place.headerCrc, other));
}

TEST_CASE("the GOP still being encoded has another key than the GOP it becomes")
{
	SyntheticEncoder encoder(smallFrames());
	GopBuilder builder(encoder.frameDuration());
	for (int frame = 0; frame < 3; ++frame) {
		builder.append(encoder.next());
	}
	const std::shared_ptr<const Gop> open = builder.snapshot();
	builder.append(encoder.next());
	const std::shared_ptr<const Gop> sealed = builder.seal();
	const GopKey a = tapeloop::gopKeyOf(*open);
	const GopKey b = tapeloop::gopKeyOf(*sealed);
	CHECK(a.runKey == b.runKey);
	CHECK(a.firstTime == b.firstTime);
	CHECK(a.firstPts == b.firstPts);
	CHECK(a.packetCount == 3);
	CHECK(b.packetCount == 4);
	CHECK_FALSE(a == b);
	// The run key tells codecs and configurations apart.
	const CodecConfig config{1, 2, 3};
	CHECK(tapeloop::runKeyOf(VideoCodec::H264, nullptr) != tapeloop::runKeyOf(VideoCodec::Hevc, nullptr));
	CHECK(tapeloop::runKeyOf(VideoCodec::H264, nullptr) != tapeloop::runKeyOf(VideoCodec::H264, &config));
}

TEST_CASE("a segment header names its broadcast, sequence and source")
{
	tapeloop::SegmentHeader header;
	header.broadcast = {9, 8, 7};
	header.sequence = 12;
	header.sourceKey = "0d6a8f2c-uuid";
	header.sourceName = "Cámara tribuna";
	const std::vector<uint8_t> bytes = tapeloop::encodeSegmentHeader(header);
	REQUIRE(bytes.size() == tapeloop::kReplayAlignment);
	const std::optional<tapeloop::SegmentHeader> read = tapeloop::decodeSegmentHeader(bytes);
	REQUIRE(read);
	CHECK(read->broadcast == header.broadcast);
	CHECK(read->sequence == 12);
	CHECK(read->sourceKey == header.sourceKey);
	CHECK(read->sourceName == header.sourceName);
	for (size_t at = 0; at < 64 + header.sourceKey.size() + header.sourceName.size(); ++at) {
		std::vector<uint8_t> damaged = bytes;
		damaged[at] ^= 0x01;
		CAPTURE(at);
		CHECK_FALSE(tapeloop::decodeSegmentHeader(damaged));
	}
	header.sourceName.assign(5000, 'x');
	CHECK_THROWS_AS(tapeloop::encodeSegmentHeader(header), std::length_error);
}

TEST_CASE("a manifest gives back its replay index and tags")
{
	const ReplayIndex index = twoSources();
	const std::vector<std::string> tags = {"goal", "Gol de visita"};
	const std::vector<uint8_t> file = tapeloop::encodeManifest(index, tags);
	const std::optional<ReplayIndex> read = tapeloop::decodeManifest(file);
	REQUIRE(read);
	requireSame(*read, index);
	CHECK(tapeloop::decodeTags(file) == tags);
	// Two runs of the first source, one of the second.
	CHECK(read->sources[0].runs.size() == 2);
	CHECK(read->sources[0].gops[2].run == 1);
	CHECK(read->sources[1].runs.size() == 1);
}

TEST_CASE("a manifest cut short or damaged where it is checked is refused")
{
	const ReplayIndex index = twoSources();
	const std::vector<uint8_t> file = tapeloop::encodeManifest(index, {});
	for (size_t size = 0; size < file.size(); size += 97) {
		CHECK_FALSE(tapeloop::decodeManifest(std::span<const uint8_t>(file).first(size)));
	}
	const auto flipFails = [&](size_t at) {
		std::vector<uint8_t> damaged = file;
		damaged[at] ^= 0x04;
		return !tapeloop::decodeManifest(damaged).has_value();
	};
	for (size_t at = 0; at < 64; ++at) {
		CAPTURE(at);
		CHECK(flipFails(at));
	}
	for (size_t at = tapeloop::kManifestIndexOffset; at < file.size(); at += 5) {
		CAPTURE(at);
		CHECK(flipFails(at));
	}
	// The padding after the header and the tag slots are not part of the index.
	CHECK_FALSE(flipFails(100));
	CHECK_FALSE(flipFails(tapeloop::kTagSlotA + 40));
	// Something longer than the manifest, as a file that grew, is refused too.
	std::vector<uint8_t> longer = file;
	longer.push_back(0);
	CHECK_FALSE(tapeloop::decodeManifest(longer));
}

TEST_CASE("a self-contained replay holds its GOP chunks between its tag slots and its index")
{
	const auto config = std::make_shared<const CodecConfig>(CodecConfig{0, 0, 0, 1, 0x67, 0x42});
	Gops gops = makeGops(2, VideoCodec::H264, config);
	const Gops hevc = makeGops(1, VideoCodec::Hevc, nullptr, 10);
	gops.insert(gops.end(), hevc.begin(), hevc.end());
	std::vector<uint8_t> file(tapeloop::kManifestIndexOffset, 0);
	std::vector<ChunkPlace> places;
	for (const auto &gop : gops) {
		places.push_back(tapeloop::appendGopChunk(file, 0, *gop));
	}
	ReplayIndex index;
	index.id = {9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 1, 2, 3, 4, 5, 6};
	index.sources.push_back(sourceOf("a", gops, places));
	for (StoredGop &gop : index.sources[0].gops) {
		gop.segment = tapeloop::kOwnFile;
	}
	index.start = index.sources[0].in;
	index.end = index.sources[0].out;
	REQUIRE(tapeloop::selfContained(index));
	CHECK_THROWS_AS(tapeloop::encodeManifest(index, {}), std::invalid_argument);

	const std::vector<std::string> tags = {"save"};
	const std::vector<uint8_t> head = tapeloop::encodeManifestHead(index, tags);
	REQUIRE(head.size() == tapeloop::kManifestIndexOffset);
	std::copy(head.begin(), head.end(), file.begin());
	const uint64_t indexOffset = file.size();
	const std::vector<uint8_t> tail = tapeloop::encodeManifestTail(index, indexOffset);
	file.insert(file.end(), tail.begin(), tail.end());

	const std::optional<ReplayIndex> read = tapeloop::decodeManifest(file);
	REQUIRE(read);
	requireSame(*read, index);
	CHECK(tapeloop::selfContained(*read));
	CHECK(tapeloop::decodeTags(file) == tags);
	for (size_t g = 0; g < gops.size(); ++g) {
		const StoredGop &stored = read->sources[0].gops[g];
		const auto chunk = std::span<const uint8_t>(file).subspan(stored.offset, stored.size);
		const auto gop = tapeloop::decodeGopChunk(chunk, stored.headerCrc, read->sources[0].runs[stored.run]);
		REQUIRE(gop);
		requireSame(*gop, *gops[g]);
	}

	// Its index is found from its footer, without reading the GOPs.
	const std::span<const uint8_t> footer = std::span<const uint8_t>(file).last(tapeloop::kManifestFooterSize);
	const std::optional<tapeloop::IndexPlace> place = tapeloop::decodeManifestFooter(footer, file.size());
	REQUIRE(place);
	CHECK(place->offset == indexOffset);
	CHECK(tapeloop::decodeManifest(std::span<const uint8_t>(file).first(tapeloop::kManifestIndexOffset),
				       std::span<const uint8_t>(file).subspan(place->offset, place->size), footer,
				       file.size()));
	CHECK_FALSE(tapeloop::decodeManifestFooter(footer, file.size() + 1));
	CHECK_FALSE(tapeloop::decodeManifest(std::span<const uint8_t>(file).first(tapeloop::kManifestIndexOffset),
					     std::span<const uint8_t>(file).subspan(place->offset, place->size - 1),
					     footer, file.size()));
}

TEST_CASE("a replay is self-contained or uses segments, never both")
{
	const auto config = std::make_shared<const CodecConfig>(CodecConfig{0, 0, 0, 1, 0x67, 0x42});
	const Gops gops = makeGops(2, VideoCodec::H264, config);
	std::vector<uint8_t> file(tapeloop::kManifestIndexOffset, 0);
	std::vector<ChunkPlace> places;
	for (const auto &gop : gops) {
		places.push_back(tapeloop::appendGopChunk(file, 0, *gop));
	}
	ReplayIndex index;
	index.sources.push_back(sourceOf("a", gops, places));
	index.start = index.sources[0].in;
	index.end = index.sources[0].out;
	const auto build = [&](const ReplayIndex &written, uint64_t indexOffset) {
		std::vector<uint8_t> bytes = file;
		const std::vector<uint8_t> head = tapeloop::encodeManifestHead(written, {});
		std::copy(head.begin(), head.end(), bytes.begin());
		bytes.resize(indexOffset);
		const std::vector<uint8_t> tail = tapeloop::encodeManifestTail(written, indexOffset);
		bytes.insert(bytes.end(), tail.begin(), tail.end());
		return bytes;
	};

	// A manifest, which refers to segments, has its index right after its tag slots.
	CHECK(tapeloop::decodeManifest(build(index, tapeloop::kManifestIndexOffset)));
	CHECK_FALSE(tapeloop::decodeManifest(build(index, file.size())));

	index.sources[0].gops[1].segment = tapeloop::kOwnFile;
	CHECK_THROWS_AS(tapeloop::encodeManifestHead(index, {}), std::invalid_argument);
	index.sources[0].gops[0].segment = tapeloop::kOwnFile;
	CHECK(tapeloop::decodeManifest(build(index, file.size())));
	// Its own GOPs must lie between the tag slots and the index.
	CHECK_FALSE(tapeloop::decodeManifest(build(index, tapeloop::kManifestIndexOffset)));
	index.sources[0].gops[1].offset = file.size();
	CHECK_FALSE(tapeloop::decodeManifest(build(index, file.size())));
	index.sources[0].gops[1].offset = 0;
	CHECK_FALSE(tapeloop::decodeManifest(build(index, file.size())));
	CHECK_THROWS_AS(tapeloop::encodeManifestTail(index, file.size() + 1), std::invalid_argument);
	CHECK_THROWS_AS(tapeloop::encodeManifestTail(index, 4096), std::invalid_argument);
}

TEST_CASE("a tag edit goes to the older slot, and a torn edit leaves the previous tags")
{
	const std::vector<std::string> before = {"goal"};
	std::vector<uint8_t> file = tapeloop::encodeManifest(twoSources(), before);
	const auto head = [&] {
		return std::span<const uint8_t>(file).first(tapeloop::kManifestIndexOffset);
	};

	tapeloop::TagWrite write = tapeloop::nextTagWrite(head());
	CHECK(write.offset == tapeloop::kTagSlotB);
	CHECK(write.generation == 2);
	const std::vector<std::string> after = {"goal", "foul"};
	std::vector<uint8_t> slot = tapeloop::encodeTagSlot(after, write.generation);
	std::copy(slot.begin(), slot.end(), file.begin() + static_cast<ptrdiff_t>(write.offset));
	CHECK(tapeloop::decodeTags(head()) == after);
	CHECK(tapeloop::decodeManifest(file));

	write = tapeloop::nextTagWrite(head());
	CHECK(write.offset == tapeloop::kTagSlotA);
	CHECK(write.generation == 3);
	// Half of the next edit reaches the disk: the slot fails its check, and the tags of
	// the other slot stand.
	const std::vector<std::string> third = {"save"};
	slot = tapeloop::encodeTagSlot(third, write.generation);
	std::copy(slot.begin(), slot.begin() + 20, file.begin() + static_cast<ptrdiff_t>(write.offset));
	CHECK(tapeloop::decodeTags(head()) == after);
	// The torn slot is the one written next.
	CHECK(tapeloop::nextTagWrite(head()).offset == tapeloop::kTagSlotA);

	const std::vector<std::string> tooMany(400, std::string(20, 't'));
	CHECK_THROWS_AS(tapeloop::encodeTagSlot(tooMany, 1), std::length_error);
}

TEST_CASE("tags fit a slot exactly when they can be encoded")
{
	// A slot has 4096 bytes: a 32-byte head, then 8 bytes and the name per tag.
	const std::vector<std::string> fits{std::string(4056, 'a')};
	const std::vector<std::string> over{std::string(4057, 'a')};
	CHECK(tapeloop::tagsFit(fits));
	CHECK_NOTHROW(tapeloop::encodeTagSlot(fits, 1));
	CHECK_FALSE(tapeloop::tagsFit(over));
	CHECK_THROWS_AS(tapeloop::encodeTagSlot(over, 1), std::length_error);
	const std::vector<std::string> many(4064 / 9, "x");
	CHECK(tapeloop::tagsFit(many));
	std::vector<std::string> more = many;
	more.emplace_back("x");
	CHECK_FALSE(tapeloop::tagsFit(more));
	CHECK_THROWS_AS(tapeloop::encodeTagSlot(more, 1), std::length_error);
	CHECK(tapeloop::tagsFit({}));
}

TEST_CASE("a stored source locates frames as a clip does")
{
	const StoredSource source = twoSources().sources[1];
	// GOPs of five frames; the clip starts at the second frame.
	const tapeloop::FrameLocation first = tapeloop::locate(source, Nanoseconds{0});
	CHECK(first.gop == 0);
	CHECK(first.packet == 1);
	const tapeloop::FrameLocation middle = tapeloop::locate(source, source.frameTimes[7] + Nanoseconds{1});
	CHECK(middle.gop == 1);
	CHECK(middle.packet == 2);
	const tapeloop::FrameLocation last = tapeloop::locate(source, source.out + Nanoseconds{1'000'000'000});
	CHECK(last.gop == 2);
	CHECK(last.packet == 4);
	CHECK(tapeloop::firstFrameOf(source, 2) == 10);
}
