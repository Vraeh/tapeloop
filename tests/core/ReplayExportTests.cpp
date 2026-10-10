// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/ReplayExport.hpp"

#include "core/FileIo.hpp"
#include "core/GopReader.hpp"
#include "core/ReplayWriter.hpp"

#include "StoredReplays.hpp"
#include "TempDirectory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using tapeloop::CaptureSource;
using tapeloop::Clip;
using tapeloop::ExportFormat;
using tapeloop::ExportRequest;
using tapeloop::ExportResult;
using tapeloop::File;
using tapeloop::Gop;
using tapeloop::GopReader;
using tapeloop::Nanoseconds;
using tapeloop::ReplayExporter;
using tapeloop::ReplayIndex;
using tapeloop::ReplayWriter;
using tapeloop::StoredSource;
using tapeloop::VideoCodec;
using tapeloop::WrittenReplay;
using tapeloop::test::captureOf;
using tapeloop::test::Gops;
using tapeloop::test::makeGops;
using tapeloop::test::sameGop;
using tapeloop::test::sourceOf;
using tapeloop::test::TempDirectory;

namespace fs = std::filesystem;

namespace {

struct WrittenFile {
	fs::path path;
	VideoCodec codec = VideoCodec::H264;
	std::vector<int64_t> pts;
	std::vector<Nanoseconds> times;
	bool finished = false;
};

// Records what an MP4 export hands over, and writes a byte so the file exists. The
// packet at failAt, counted over every file of the export, fails.
class RecordingWriter final : public tapeloop::VideoFileWriter {
public:
	RecordingWriter(std::vector<WrittenFile> &files, int &written, int failAt)
		: files_(files),
		  written_(written),
		  failAt_(failAt)
	{
	}

	void open(const fs::path &path, const Gop &first) override
	{
		files_.push_back({path, first.codec(), {}, {}, false});
		const uint8_t byte = 1;
		File(path, File::Mode::CreateNew).writeAt(0, {&byte, 1});
	}
	void write(const Gop &gop, size_t packet, Nanoseconds time) override
	{
		if (written_++ == failAt_) {
			throw std::runtime_error("no space left on the disk");
		}
		files_.back().pts.push_back(gop.packets()[packet].pts);
		files_.back().times.push_back(time);
	}
	void finish() override { files_.back().finished = true; }

private:
	std::vector<WrittenFile> &files_;
	int &written_;
	int failAt_;
};

ReplayExporter::WriterFactory recording(std::vector<WrittenFile> &files, int failAt = -1)
{
	auto written = std::make_shared<int>(0);
	return [&files, written, failAt] {
		return std::make_unique<RecordingWriter>(files, *written, failAt);
	};
}

ExportResult exportOnce(ReplayExporter &exporter, ExportRequest request)
{
	const uint64_t ticket = exporter.exportReplay(std::move(request));
	exporter.waitUntilIdle();
	std::vector<ExportResult> results = exporter.poll();
	REQUIRE(results.size() == 1);
	CHECK(results.front().ticket == ticket);
	CHECK_FALSE(exporter.progress());
	return results.front();
}

ExportRequest requestFor(const WrittenReplay &written, ExportFormat format)
{
	ExportRequest request;
	request.format = format;
	request.manifest = written.manifest;
	return request;
}

std::vector<std::string> namesIn(const fs::path &folder)
{
	std::vector<std::string> names;
	for (const auto &entry : fs::directory_iterator(folder)) {
		names.push_back(tapeloop::utf8FromPath(entry.path().filename()));
	}
	std::sort(names.begin(), names.end());
	return names;
}

// A source whose clip starts at the third frame of its first GOP and ends at the fourth
// of its last.
CaptureSource trimmed(const std::string &key, const Gops &gops)
{
	return {key, "Camera " + key, Clip(gops, gops.front()->packets()[2].time, gops.back()->packets()[3].time)};
}

} // namespace

TEST_CASE("an MP4 export has a part for each codec of a source, from its in point to its out point")
{
	TempDirectory dir;
	Gops gops = makeGops(2);
	const Gops hevc = makeGops(2, 10, VideoCodec::Hevc, nullptr);
	gops.insert(gops.end(), hevc.begin(), hevc.end());
	const WrittenReplay written = ReplayWriter().write(captureOf(dir.path(), {trimmed("a", gops)}));
	const StoredSource &source = written.index.sources.front();
	const std::vector<tapeloop::Mp4Part> parts = tapeloop::mp4Parts(source);
	REQUIRE(parts.size() == 2);
	CHECK(parts[0].firstGop == 0);
	CHECK(parts[0].endGop == 2);
	CHECK(parts[1].firstGop == 2);
	CHECK(parts[1].endGop == 4);

	// A GOP wholly before the in point or after the out point has no part.
	StoredSource narrow = source;
	narrow.in = narrow.frameTimes[5];
	narrow.out = narrow.frameTimes[9];
	REQUIRE(tapeloop::mp4Parts(narrow).size() == 1);
	CHECK(tapeloop::mp4Parts(narrow).front().firstGop == 1);
	CHECK(tapeloop::mp4Parts(narrow).front().endGop == 2);
}

TEST_CASE("an MP4 export writes each source from its in point, in the Export folder")
{
	TempDirectory dir;
	const Gops first = makeGops(3);
	const Gops second = makeGops(2);
	const WrittenReplay written =
		ReplayWriter().write(captureOf(dir.path(), {trimmed("a", first), sourceOf("b", second)}));
	std::vector<WrittenFile> files;
	ReplayExporter exporter(recording(files));
	REQUIRE(exporter.canExport(ExportFormat::Mp4));
	const ExportResult result = exportOnce(exporter, requestFor(written, ExportFormat::Mp4));
	REQUIRE(result.error.empty());

	const fs::path folder = dir.path() / "Export";
	REQUIRE(result.files.size() == 2);
	CHECK(result.files[0] == folder / "2026-10-09 21-05-42 - Camera a.mp4");
	CHECK(result.files[1] == folder / "2026-10-09 21-05-42 - Camera b.mp4");
	CHECK(namesIn(folder) ==
	      std::vector<std::string>{"2026-10-09 21-05-42 - Camera a.mp4", "2026-10-09 21-05-42 - Camera b.mp4"});
	REQUIRE(files.size() == 2);
	CHECK(files[0].finished);
	CHECK(files[0].path.filename() == "2026-10-09 21-05-42 - Camera a.mp4.part");

	// The first source keeps the two frames before its in point for decoding, before
	// zero, and nothing after its out point.
	const WrittenFile &a = files[0];
	REQUIRE(a.times.size() == 14);
	const Nanoseconds in = first.front()->packets()[2].time;
	CHECK(a.times[0] == first.front()->packets()[0].time - in);
	CHECK(a.times[0] < Nanoseconds{0});
	CHECK(a.times[2] == Nanoseconds{0});
	CHECK(a.times.back() == first.back()->packets()[3].time - in);
	CHECK(a.pts.front() == first.front()->packets()[0].pts);
	const WrittenFile &b = files[1];
	CHECK(b.times.size() == 10);
	CHECK(b.times.front() == Nanoseconds{0});
}

TEST_CASE("an export that fails leaves no file, and one that is done again gets a new name")
{
	TempDirectory dir;
	const WrittenReplay written =
		ReplayWriter().write(captureOf(dir.path(), {sourceOf("a", makeGops(2)), sourceOf("b", makeGops(2))}));
	const fs::path folder = dir.path() / "Export";
	{
		// The second source fails after the first one's file is complete.
		std::vector<WrittenFile> files;
		ReplayExporter exporter(recording(files, 13));
		const ExportResult result = exportOnce(exporter, requestFor(written, ExportFormat::Mp4));
		CHECK(result.error == "no space left on the disk");
		CHECK(result.files.empty());
		CHECK(namesIn(folder).empty());
	}
	std::vector<WrittenFile> files;
	ReplayExporter exporter(recording(files));
	REQUIRE(exportOnce(exporter, requestFor(written, ExportFormat::Mp4)).error.empty());
	const ExportResult again = exportOnce(exporter, requestFor(written, ExportFormat::Mp4));
	REQUIRE(again.error.empty());
	REQUIRE(again.files.size() == 2);
	CHECK(again.files[0].filename() == "2026-10-09 21-05-42 - Camera a (2).mp4");
	CHECK(again.files[1].filename() == "2026-10-09 21-05-42 - Camera b (2).mp4");
}

TEST_CASE("a name an export would take, or its half-written file, is passed over")
{
	TempDirectory dir;
	File(dir.path() / "x.mp4", File::Mode::CreateNew).flush();
	File(dir.path() / "x (2).mp4.part", File::Mode::CreateNew).flush();
	CHECK(tapeloop::freeExportPath(dir.path(), "x", ".mp4", {}) == dir.path() / "x (3).mp4");
	CHECK(tapeloop::freeExportPath(dir.path(), "x", ".mp4", {dir.path() / "x (3).mp4"}) ==
	      dir.path() / "x (4).mp4");
	CHECK(tapeloop::freeExportPath(dir.path(), "y", ".tplp", {}) == dir.path() / "y.tplp");
	CHECK(tapeloop::mp4Stem(dir.path() / "2026-10-09 21-05-42.tplp", "Cam/1") == "2026-10-09 21-05-42 - Cam_1");
}

TEST_CASE("a self-contained export plays without the broadcast folder")
{
	TempDirectory dir;
	Gops first = makeGops(2);
	const Gops hevc = makeGops(2, 10, VideoCodec::Hevc, nullptr);
	first.insert(first.end(), hevc.begin(), hevc.end());
	const Gops second = makeGops(3);
	const WrittenReplay written =
		ReplayWriter().write(captureOf(dir.path(), {trimmed("a", first), sourceOf("b", second)}));
	ReplayExporter exporter;
	CHECK_FALSE(exporter.canExport(ExportFormat::Mp4));
	CHECK(exporter.canExport(ExportFormat::Replay));
	ExportRequest request = requestFor(written, ExportFormat::Replay);
	request.tags = {"goal", "Gol de visita"};
	const ExportResult result = exportOnce(exporter, request);
	REQUIRE(result.error.empty());
	REQUIRE(result.files.size() == 1);
	const fs::path exported = dir.path() / "Moved.tplp";
	CHECK(result.files.front() == dir.path() / "Export" / "2026-10-09 21-05-42.tplp");
	fs::rename(result.files.front(), exported);
	fs::remove_all(dir.path() / "data");

	const std::optional<ReplayIndex> index = tapeloop::readReplayIndex(exported);
	REQUIRE(index);
	CHECK(tapeloop::selfContained(*index));
	CHECK(index->id == written.index.id);
	REQUIRE(index->sources.size() == 2);
	CHECK(index->sources[0].in == written.index.sources[0].in);
	CHECK(index->sources[0].runs.size() == 2);
	CHECK(tapeloop::decodeTags(tapeloop::test::fileBytes(exported)) == request.tags);
	GopReader reader;
	const std::vector<const Gops *> gops = {&first, &second};
	for (size_t s = 0; s < 2; ++s) {
		REQUIRE(index->sources[s].gops.size() == gops[s]->size());
		for (size_t g = 0; g < gops[s]->size(); ++g) {
			const std::shared_ptr<const Gop> read = reader.read(exported, index->sources[s], g);
			REQUIRE(read);
			CHECK(sameGop(*read, *(*gops[s])[g]));
		}
	}
}

TEST_CASE("an export reads the buffers' own GOPs while they last")
{
	TempDirectory dir;
	const Gops gops = makeGops(2);
	const WrittenReplay written = ReplayWriter().write(captureOf(dir.path(), {sourceOf("a", gops)}));
	fs::remove_all(dir.path() / "data");
	ExportRequest request = requestFor(written, ExportFormat::Replay);
	request.index = std::make_shared<const ReplayIndex>(written.index);
	ReplayExporter exporter;
	// Without the segments, the GOPs the buffer let go of cannot be read.
	CHECK_FALSE(exportOnce(exporter, request).error.empty());
	request.live = {{gops[0], gops[1]}};
	const ExportResult result = exportOnce(exporter, request);
	REQUIRE(result.error.empty());
	CHECK(tapeloop::readReplayIndex(result.files.front()));
}

TEST_CASE("an MP4 export without an MP4 writer fails, and so does one of a replay that is gone")
{
	TempDirectory dir;
	const WrittenReplay written = ReplayWriter().write(captureOf(dir.path(), {sourceOf("a", makeGops(1))}));
	ReplayExporter exporter;
	CHECK(exportOnce(exporter, requestFor(written, ExportFormat::Mp4)).error == "MP4 export is not available here");
	ExportRequest gone = requestFor(written, ExportFormat::Replay);
	gone.manifest = dir.path() / "missing.tplp";
	CHECK_FALSE(exportOnce(exporter, gone).error.empty());
}
