// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/ReplayWriter.hpp"

#include "core/FileIo.hpp"

#include "StoredReplays.hpp"
#include "SyntheticEncoder.hpp"
#include "TempDirectory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

using tapeloop::CaptureSource;
using tapeloop::Clip;
using tapeloop::File;
using tapeloop::GopBuilder;
using tapeloop::ReplayCapture;
using tapeloop::ReplayIndex;
using tapeloop::ReplayWriter;
using tapeloop::ReplayWriterConfig;
using tapeloop::StoredSource;
using tapeloop::VideoCodec;
using tapeloop::WrittenReplay;
using tapeloop::test::captureOf;
using tapeloop::test::fileBytes;
using tapeloop::test::Gops;
using tapeloop::test::h264Config;
using tapeloop::test::kGopLength;
using tapeloop::test::makeGops;
using tapeloop::test::readManifest;
using tapeloop::test::requireReadsBack;
using tapeloop::test::segmentsOf;
using tapeloop::test::slice;
using tapeloop::test::sourceOf;
using tapeloop::test::SyntheticEncoder;
using tapeloop::test::TempDirectory;

namespace fs = std::filesystem;

TEST_CASE("a capture is written as segments and a manifest")
{
	TempDirectory dir;
	const fs::path folder = dir.path() / "Liga 2026-10-09 21-00";
	const Gops a = makeGops(4);
	const Gops b = makeGops(3, 100, VideoCodec::Hevc, nullptr);
	ReplayCapture capture = captureOf(folder, {sourceOf("a", a), sourceOf("b", b)});
	capture.tags = {"Goal"};

	ReplayWriter writer;
	const WrittenReplay written = writer.write(capture);
	CHECK(written.manifest == folder / "2026-10-09 21-05-42.tplp");
	CHECK(written.gopsAppended == 7);
	CHECK(written.gopsShared == 0);
	CHECK(segmentsOf(folder) == std::vector<std::string>{"a-000001.tpls", "b-000001.tpls"});
	CHECK(written.bytesAppended ==
	      fs::file_size(folder / "data" / "a-000001.tpls") + fs::file_size(folder / "data" / "b-000001.tpls"));
	CHECK_FALSE(fs::exists(folder / "2026-10-09 21-05-42.tplp.part"));

	const std::optional<ReplayIndex> index = readManifest(written.manifest);
	REQUIRE(index);
	CHECK(index->id == capture.id);
	CHECK(index->capturedAtUtc == capture.capturedAtUtc);
	CHECK(index->start == capture.start);
	CHECK(index->end == capture.end);
	REQUIRE(index->sources.size() == 2);
	CHECK(index->sources[0].key == "a");
	CHECK(index->sources[0].name == "Camera a");
	CHECK(index->sources[1].runs.front().codec == VideoCodec::Hevc);
	CHECK(index->sources[1].runs.front().config == nullptr);
	CHECK(index->sources[0].frameTimes.size() == 4 * kGopLength);
	CHECK(index->sources[0].in == a.front()->startTime());
	CHECK(index->sources[0].out == a.back()->lastTime());
	requireReadsBack(written.manifest, index->sources[0], a);
	requireReadsBack(written.manifest, index->sources[1], b);
	CHECK(tapeloop::decodeTags(fileBytes(written.manifest)) == std::vector<std::string>{"Goal"});
}

TEST_CASE("a capture's index before writing has everything but the places of its GOPs")
{
	TempDirectory dir;
	ReplayCapture capture = captureOf(dir.path(), {sourceOf("a", makeGops(3)), sourceOf("b", makeGops(2, 40))});
	capture.sources.push_back({"empty", "Empty", Clip()});
	const ReplayIndex pending = tapeloop::indexOf(capture);
	const WrittenReplay written = ReplayWriter().write(capture);
	CHECK(pending.id == written.index.id);
	CHECK(pending.start == written.index.start);
	CHECK(pending.capturedAtClock == written.index.capturedAtClock);
	REQUIRE(pending.sources.size() == 2);
	for (size_t s = 0; s < pending.sources.size(); ++s) {
		const StoredSource &before = pending.sources[s];
		const StoredSource &after = written.index.sources[s];
		CHECK(before.key == after.key);
		CHECK(before.name == after.name);
		CHECK(before.in == after.in);
		CHECK(before.out == after.out);
		CHECK(before.frameTimes == after.frameTimes);
		REQUIRE(before.runs.size() == after.runs.size());
		REQUIRE(before.gops.size() == after.gops.size());
		for (size_t g = 0; g < before.gops.size(); ++g) {
			CHECK(before.gops[g].packetCount == after.gops[g].packetCount);
			CHECK(before.gops[g].run == after.gops[g].run);
			CHECK(before.gops[g].size == 0);
			CHECK(after.gops[g].size > 0);
		}
	}
}

TEST_CASE("a replay's index is read back from its manifest")
{
	TempDirectory dir;
	const WrittenReplay written = ReplayWriter().write(captureOf(dir.path(), {sourceOf("a", makeGops(2))}));
	const std::optional<ReplayIndex> index = tapeloop::readReplayIndex(written.manifest);
	REQUIRE(index);
	CHECK(index->id == written.index.id);
	CHECK(index->sources[0].frameTimes == written.index.sources[0].frameTimes);
	CHECK_FALSE(tapeloop::readReplayIndex(dir.path() / "missing.tplp"));
	fs::resize_file(written.manifest, 100);
	CHECK_FALSE(tapeloop::readReplayIndex(written.manifest));
}

TEST_CASE("a GOP already written is stored once")
{
	TempDirectory dir;
	const Gops gops = makeGops(15);
	ReplayWriter writer;
	const WrittenReplay first = writer.write(captureOf(dir.path(), {sourceOf("a", slice(gops, 0, 10))}, "one"));
	const uint64_t afterFirst = fs::file_size(dir.path() / "data" / "a-000001.tpls");
	const WrittenReplay second = writer.write(captureOf(dir.path(), {sourceOf("a", slice(gops, 5, 15))}, "two"));
	CHECK(second.gopsShared == 5);
	CHECK(second.gopsAppended == 5);
	CHECK(second.bytesAppended == fs::file_size(dir.path() / "data" / "a-000001.tpls") - afterFirst);
	CHECK(second.bytesAppended < first.bytesAppended);

	const std::optional<ReplayIndex> one = readManifest(first.manifest);
	const std::optional<ReplayIndex> two = readManifest(second.manifest);
	REQUIRE(one);
	REQUIRE(two);
	for (size_t i = 0; i < 5; ++i) {
		CHECK(two->sources[0].gops[i].offset == one->sources[0].gops[i + 5].offset);
	}
	requireReadsBack(first.manifest, one->sources[0], slice(gops, 0, 10));
	requireReadsBack(second.manifest, two->sources[0], slice(gops, 5, 15));

	// Nothing new at all: only the manifest is written.
	const WrittenReplay again = writer.write(captureOf(dir.path(), {sourceOf("a", slice(gops, 3, 12))}, "three"));
	CHECK(again.gopsAppended == 0);
	CHECK(again.bytesAppended == 0);
}

TEST_CASE("a GOP captured while it was still growing is written again once complete")
{
	TempDirectory dir;
	SyntheticEncoder::Config settings;
	settings.gopLength = kGopLength;
	SyntheticEncoder encoder(settings);
	GopBuilder builder(encoder.frameDuration());
	builder.setCodecConfig(VideoCodec::H264, h264Config());
	for (int frame = 0; frame < 3; ++frame) {
		builder.append(encoder.next());
	}
	const Gops partial{builder.snapshot()};
	for (int frame = 3; frame < kGopLength; ++frame) {
		builder.append(encoder.next());
	}
	const Gops complete{builder.seal()};

	ReplayWriter writer;
	writer.write(captureOf(dir.path(), {sourceOf("a", partial)}, "one"));
	const WrittenReplay second = writer.write(captureOf(dir.path(), {sourceOf("a", complete)}, "two"));
	CHECK(second.gopsAppended == 1);
	const std::optional<ReplayIndex> index = readManifest(second.manifest);
	REQUIRE(index);
	requireReadsBack(second.manifest, index->sources[0], complete);
}

TEST_CASE("segments roll over at their bound and each decodes on its own")
{
	TempDirectory dir;
	const Gops first = makeGops(6);
	const Gops second = makeGops(6, 30, VideoCodec::Hevc, nullptr);
	Gops gops = first;
	gops.insert(gops.end(), second.begin(), second.end());
	const uint64_t chunk = tapeloop::gopChunkSize(*gops.front());
	ReplayWriterConfig config;
	// The header, a RUN chunk and three GOP chunks.
	config.segmentBytes = tapeloop::kReplayAlignment + tapeloop::runChunkSize(*gops.front()) + 3 * chunk;
	config.writeBytes = 1;
	ReplayWriter writer(config);
	const WrittenReplay written = writer.write(captureOf(dir.path(), {sourceOf("a", gops)}));
	CHECK(segmentsOf(dir.path()) ==
	      std::vector<std::string>{"a-000001.tpls", "a-000002.tpls", "a-000003.tpls", "a-000004.tpls"});
	for (const std::string &segment : segmentsOf(dir.path())) {
		CHECK(fs::file_size(dir.path() / "data" / segment) <= config.segmentBytes);
	}

	const std::optional<ReplayIndex> index = readManifest(written.manifest);
	REQUIRE(index);
	const StoredSource &source = index->sources[0];
	std::vector<uint32_t> segments;
	for (const tapeloop::StoredGop &gop : source.gops) {
		segments.push_back(gop.segment);
	}
	CHECK(segments == std::vector<uint32_t>{1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4, 4});
	CHECK(source.runs.size() == 2);
	requireReadsBack(written.manifest, source, gops);
	// A segment that starts in the middle of a run still names the run first.
	for (const std::string &segment : segmentsOf(dir.path())) {
		const std::vector<uint8_t> bytes = fileBytes(dir.path() / "data" / segment);
		const auto header = tapeloop::decodeSegmentHeader(bytes);
		REQUIRE(header);
		CHECK(header->sourceKey == "a");
		CHECK(header->sourceName == "Camera a");
		CHECK_FALSE(tapeloop::peekGopKey(std::span<const uint8_t>(bytes).subspan(tapeloop::kReplayAlignment)));
	}
}

TEST_CASE("a GOP larger than a segment gets one of its own")
{
	TempDirectory dir;
	const Gops gops = makeGops(3);
	ReplayWriterConfig config;
	config.segmentBytes = tapeloop::kReplayAlignment;
	ReplayWriter writer(config);
	const WrittenReplay written = writer.write(captureOf(dir.path(), {sourceOf("a", gops)}));
	CHECK(segmentsOf(dir.path()).size() == 3);
	const std::optional<ReplayIndex> index = readManifest(written.manifest);
	REQUIRE(index);
	requireReadsBack(written.manifest, index->sources[0], gops);
}

TEST_CASE("a replay whose name is taken gets a number")
{
	TempDirectory dir;
	const Gops gops = makeGops(2);
	ReplayWriter writer;
	CHECK(writer.write(captureOf(dir.path(), {sourceOf("a", gops)}, "same")).manifest == dir.path() / "same.tplp");
	CHECK(writer.write(captureOf(dir.path(), {sourceOf("a", gops)}, "same")).manifest ==
	      dir.path() / "same (2).tplp");
	// A manifest a crash left half written keeps its name until a scan deletes it.
	File(dir.path() / "same (3).tplp.part", File::Mode::CreateNew).close();
	CHECK(writer.write(captureOf(dir.path(), {sourceOf("a", gops)}, "same")).manifest ==
	      dir.path() / "same (4).tplp");
	CHECK(writer.write(captureOf(dir.path(), {sourceOf("a", gops)}, "a/b")).manifest == dir.path() / "a_b.tplp");
	// A stem at the length limit still gets its number.
	const std::string longStem(200, 's');
	CHECK(writer.write(captureOf(dir.path(), {sourceOf("a", gops)}, longStem)).manifest ==
	      dir.path() / (std::string(120, 's') + ".tplp"));
	CHECK(writer.write(captureOf(dir.path(), {sourceOf("a", gops)}, longStem)).manifest ==
	      dir.path() / (std::string(116, 's') + " (2).tplp"));
}

TEST_CASE("a writer appends only to segments it created")
{
	TempDirectory dir;
	const Gops gops = makeGops(4);
	const WrittenReplay before = ReplayWriter().write(captureOf(dir.path(), {sourceOf("a", gops)}, "before"));
	// As after OBS restarts with the broadcast folder of the same minute.
	ReplayWriter writer;
	const WrittenReplay after = writer.write(captureOf(dir.path(), {sourceOf("a", gops)}, "after"));
	CHECK(after.gopsAppended == 4);
	CHECK(segmentsOf(dir.path()) == std::vector<std::string>{"a-000001.tpls", "a-000002.tpls"});
	const std::optional<ReplayIndex> first = readManifest(before.manifest);
	const std::optional<ReplayIndex> second = readManifest(after.manifest);
	REQUIRE(first);
	REQUIRE(second);
	requireReadsBack(before.manifest, first->sources[0], gops);
	requireReadsBack(after.manifest, second->sources[0], gops);
}

TEST_CASE("a writer starts over in another broadcast folder")
{
	TempDirectory dir;
	const Gops gops = makeGops(4);
	ReplayWriter writer;
	writer.write(captureOf(dir.path() / "one", {sourceOf("a", gops)}));
	const WrittenReplay other = writer.write(captureOf(dir.path() / "two", {sourceOf("a", gops)}));
	CHECK(other.gopsAppended == 4);
	CHECK(segmentsOf(dir.path() / "two") == std::vector<std::string>{"a-000001.tpls"});
	const WrittenReplay back = writer.write(captureOf(dir.path() / "one", {sourceOf("a", gops)}, "again"));
	CHECK(back.gopsAppended == 4);
	CHECK(segmentsOf(dir.path() / "one") == std::vector<std::string>{"a-000001.tpls", "a-000002.tpls"});
}

TEST_CASE("a capture without frames is refused")
{
	TempDirectory dir;
	ReplayCapture capture = captureOf(dir.path(), {sourceOf("a", makeGops(1))});
	capture.sources.front().clip = Clip();
	ReplayWriter writer;
	CHECK_THROWS_AS(writer.write(capture), std::invalid_argument);
	capture.sources.clear();
	CHECK_THROWS_AS(writer.write(capture), std::invalid_argument);
	CHECK_FALSE(fs::exists(dir.path() / "data"));
}

TEST_CASE("a source without frames is left out of the replay")
{
	TempDirectory dir;
	ReplayCapture capture = captureOf(dir.path(), {sourceOf("a", makeGops(2)), sourceOf("b", makeGops(2))});
	capture.sources.front().clip = Clip();
	const WrittenReplay written = ReplayWriter().write(capture);
	REQUIRE(written.index.sources.size() == 1);
	CHECK(written.index.sources.front().key == "b");
	CHECK(segmentsOf(dir.path()) == std::vector<std::string>{"b-000001.tpls"});
}

TEST_CASE("a failed capture leaves earlier replays whole and the next one writes again")
{
	TempDirectory dir;
	const Gops a = makeGops(6);
	const Gops b = makeGops(6, 50);
	ReplayWriter writer;
	const WrittenReplay first = writer.write(captureOf(dir.path(), {sourceOf("a", slice(a, 0, 3))}, "first"));

	// A key too long for a segment header fails the capture after a and b were written.
	const CaptureSource broken = sourceOf(std::string(3500, 'x'), makeGops(1, 200));
	CHECK_THROWS_AS(writer.write(captureOf(dir.path(), {sourceOf("a", a), sourceOf("b", b), broken}, "second")),
			std::length_error);
	CHECK_FALSE(fs::exists(dir.path() / "second.tplp"));

	const WrittenReplay third = writer.write(captureOf(dir.path(), {sourceOf("a", a), sourceOf("b", b)}, "third"));
	// a's earlier GOPs are written again, into a segment of their own.
	CHECK(third.gopsAppended == 12);
	const std::vector<std::string> segments = segmentsOf(dir.path());
	CHECK(std::find(segments.begin(), segments.end(), "a-000002.tpls") != segments.end());
	CHECK(std::find(segments.begin(), segments.end(), "b-000002.tpls") != segments.end());

	const std::optional<ReplayIndex> before = readManifest(first.manifest);
	const std::optional<ReplayIndex> after = readManifest(third.manifest);
	REQUIRE(before);
	REQUIRE(after);
	requireReadsBack(first.manifest, before->sources[0], slice(a, 0, 3));
	requireReadsBack(third.manifest, after->sources[0], a);
	requireReadsBack(third.manifest, after->sources[1], b);
}

TEST_CASE("a broadcast folder that cannot be made fails the capture")
{
	TempDirectory dir;
	File(dir.path() / "taken", File::Mode::CreateNew).close();
	ReplayWriter writer;
	CHECK_THROWS_AS(writer.write(captureOf(dir.path() / "taken", {sourceOf("a", makeGops(1))})), std::system_error);
}

TEST_CASE("a capture whose tags do not fit is refused before anything is written")
{
	TempDirectory dir;
	ReplayCapture capture = captureOf(dir.path() / "Liga", {sourceOf("a", makeGops(2))});
	capture.tags = std::vector<std::string>(600, "Penalty");
	CHECK_THROWS_AS(ReplayWriter().write(capture), std::length_error);
	CHECK_FALSE(fs::exists(dir.path() / "Liga"));
}

TEST_CASE("tags go to the older slot of a manifest")
{
	TempDirectory dir;
	ReplayWriter writer;
	ReplayCapture capture = captureOf(dir.path(), {sourceOf("a", makeGops(2))});
	capture.tags = {"Goal"};
	const fs::path manifest = writer.write(capture).manifest;

	writer.writeTags(manifest, std::vector<std::string>{"Goal", "Foul"});
	CHECK(tapeloop::decodeTags(fileBytes(manifest)) == std::vector<std::string>{"Goal", "Foul"});
	writer.writeTags(manifest, std::vector<std::string>{"Save"});
	std::vector<uint8_t> bytes = fileBytes(manifest);
	CHECK(tapeloop::decodeTags(bytes) == std::vector<std::string>{"Save"});
	CHECK(tapeloop::nextTagWrite(bytes).generation == 4);
	CHECK(tapeloop::decodeManifest(bytes));

	// A torn write of the newer slot leaves the tags before it.
	const tapeloop::TagWrite next = tapeloop::nextTagWrite(bytes);
	const uint64_t newer = next.offset == tapeloop::kTagSlotA ? tapeloop::kTagSlotB : tapeloop::kTagSlotA;
	File(manifest, File::Mode::ReadWrite).writeAt(newer, std::vector<uint8_t>(64, 0xEE));
	CHECK(tapeloop::decodeTags(fileBytes(manifest)) == std::vector<std::string>{"Goal", "Foul"});
}

TEST_CASE("tags are written only into an intact manifest and when they fit")
{
	TempDirectory dir;
	ReplayWriter writer;
	const fs::path manifest = writer.write(captureOf(dir.path(), {sourceOf("a", makeGops(2))})).manifest;
	CHECK_THROWS_AS(writer.writeTags(manifest, std::vector<std::string>(600, "Penalty")), std::length_error);
	CHECK(tapeloop::decodeTags(fileBytes(manifest)).empty());

	const fs::path other = dir.path() / "other.tplp";
	File(other, File::Mode::CreateNew).writeAt(0, std::vector<uint8_t>(20000, 1));
	CHECK_THROWS_AS(writer.writeTags(other, std::vector<std::string>{"Goal"}), std::invalid_argument);
	CHECK(fileBytes(other) == std::vector<uint8_t>(20000, 1));
	CHECK_THROWS_AS(writer.writeTags(dir.path() / "missing.tplp", std::vector<std::string>{"Goal"}),
			std::system_error);
}

TEST_CASE("a scan finds every replay of every broadcast")
{
	TempDirectory dir;
	const Gops gops = makeGops(6);
	ReplayWriter writer;
	ReplayCapture tagged = captureOf(dir.path() / "Liga 2026-10-09 21-00", {sourceOf("a", slice(gops, 0, 3))}, "1");
	tagged.tags = {"Goal"};
	const WrittenReplay first = writer.write(tagged);
	const WrittenReplay second =
		writer.write(captureOf(dir.path() / "Liga 2026-10-09 21-00", {sourceOf("a", slice(gops, 2, 6))}, "2"));
	const WrittenReplay third =
		writer.write(captureOf(dir.path() / "Copa 2026-10-10 18-30", {sourceOf("a", gops)}, "1"));
	File(dir.path() / "loose.tplp", File::Mode::CreateNew).close();

	const tapeloop::ReplayScan scan = tapeloop::scanReplays(dir.path());
	CHECK(scan.errors.empty());
	CHECK(scan.removed.empty());
	REQUIRE(scan.replays.size() == 3);
	CHECK(scan.replays[0].manifest == third.manifest);
	CHECK(scan.replays[0].broadcast == "Copa 2026-10-10 18-30");
	CHECK(scan.replays[1].manifest == first.manifest);
	CHECK(scan.replays[1].broadcast == "Liga 2026-10-09 21-00");
	CHECK(scan.replays[1].tags == std::vector<std::string>{"Goal"});
	CHECK(scan.replays[2].manifest == second.manifest);
	CHECK(scan.replays[2].tags.empty());
	for (const tapeloop::FoundReplay &found : scan.replays) {
		CHECK(found.intact);
		CHECK(found.sources == std::vector<tapeloop::ReplaySource>{{"a", "Camera a"}});
		CHECK(found.capturedAtUtc == tagged.capturedAtUtc);
	}
	CHECK(scan.replays[1].id == tagged.id);
	const std::optional<ReplayIndex> index = tapeloop::readReplayIndex(scan.replays[2].manifest);
	REQUIRE(index);
	CHECK(index->sources[0].gops.size() == 4);
}

TEST_CASE("a scan deletes half written manifests and lists damaged ones")
{
	TempDirectory dir;
	const fs::path folder = dir.path() / "Liga 2026-10-09 21-00";
	ReplayWriter writer;
	const WrittenReplay kept = writer.write(captureOf(folder, {sourceOf("a", makeGops(2))}, "kept"));
	const WrittenReplay cut = writer.write(captureOf(folder, {sourceOf("b", makeGops(2))}, "cut"));
	const WrittenReplay orphan = writer.write(captureOf(folder, {sourceOf("c", makeGops(2))}, "orphan"));
	File(folder / "torn.tplp.part", File::Mode::CreateNew).writeAt(0, std::vector<uint8_t>(5000, 7));
	fs::resize_file(cut.manifest, fs::file_size(cut.manifest) - 1);
	fs::remove(folder / "data" / "c-000001.tpls");

	const tapeloop::ReplayScan scan = tapeloop::scanReplays(dir.path());
	CHECK(scan.removed == std::vector<fs::path>{folder / "torn.tplp.part"});
	CHECK_FALSE(fs::exists(folder / "torn.tplp.part"));
	REQUIRE(scan.replays.size() == 3);
	CHECK(scan.replays[0].manifest == cut.manifest);
	CHECK_FALSE(scan.replays[0].intact);
	CHECK(scan.replays[0].broadcast == "Liga 2026-10-09 21-00");
	CHECK(scan.replays[0].sources.empty());
	CHECK(scan.replays[1].manifest == kept.manifest);
	CHECK(scan.replays[1].intact);
	CHECK(scan.replays[2].manifest == orphan.manifest);
	CHECK_FALSE(scan.replays[2].intact);
	CHECK(scan.replays[2].sources.empty());
	CHECK(scan.errors.empty());
}

TEST_CASE("a scan of a base that does not exist finds nothing")
{
	TempDirectory dir;
	const tapeloop::ReplayScan scan = tapeloop::scanReplays(dir.path() / "missing");
	CHECK(scan.replays.empty());
	CHECK(scan.removed.empty());
	CHECK(scan.errors.empty());
	CHECK(tapeloop::scanReplays(dir.path()).replays.empty());
}

TEST_CASE("replay ids are random")
{
	std::set<tapeloop::ReplayId> ids;
	for (int i = 0; i < 100; ++i) {
		ids.insert(tapeloop::newReplayId());
	}
	CHECK(ids.size() == 100);
}

TEST_CASE("long source names are cut to fit the segment header")
{
	TempDirectory dir;
	CaptureSource source = sourceOf("a", makeGops(2));
	source.name = std::string(1023, 'x') + "\xC3\xBA" + std::string(5000, 'y');
	const WrittenReplay written = ReplayWriter().write(captureOf(dir.path(), {source}));
	CHECK(written.index.sources[0].name == std::string(1023, 'x'));
	const auto header = tapeloop::decodeSegmentHeader(fileBytes(dir.path() / "data" / "a-000001.tpls"));
	REQUIRE(header);
	CHECK(header->sourceName == std::string(1023, 'x'));
}
