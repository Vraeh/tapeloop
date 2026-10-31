// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/ReplayExport.hpp"

#include "core/FileIo.hpp"
#include "core/GopReader.hpp"
#include "core/ReplayLibrary.hpp"
#include "core/ReplayWriter.hpp"

#include "StoredReplays.hpp"
#include "TempDirectory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
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

// Holds an export at one of its packets until the test lets it go on.
class Pause {
public:
	explicit Pause(int at) : at_(at) {}

	void reach(int packet)
	{
		if (packet != at_) {
			return;
		}
		std::unique_lock lock(mutex_);
		reached_ = true;
		changed_.notify_all();
		changed_.wait(lock, [this] { return released_; });
	}
	void waitUntilReached()
	{
		std::unique_lock lock(mutex_);
		changed_.wait(lock, [this] { return reached_; });
	}
	void release()
	{
		{
			const std::lock_guard lock(mutex_);
			released_ = true;
		}
		changed_.notify_all();
	}

private:
	std::mutex mutex_;
	std::condition_variable changed_;
	int at_;
	bool reached_ = false;
	bool released_ = false;
};

// Records what an MP4 export hands over, and writes a byte so the file exists. The
// packet at failAt, counted over every file of the export, fails, and the GOP starting at
// pts splitAt does not fit the file before it.
class RecordingWriter final : public tapeloop::VideoFileWriter {
public:
	RecordingWriter(std::vector<WrittenFile> &files, int &written, int failAt, Pause *pause, int64_t splitAt)
		: files_(files),
		  written_(written),
		  failAt_(failAt),
		  pause_(pause),
		  splitAt_(splitAt)
	{
	}

	void open(const fs::path &path, const Gop &first) override
	{
		files_.push_back({path, first.codec(), {}, {}, false});
		const uint8_t byte = 1;
		File(path, File::Mode::CreateNew).writeAt(0, {&byte, 1});
	}
	bool fits(const Gop &gop) const override { return gop.packets().front().pts != splitAt_; }
	void write(const Gop &gop, size_t packet, Nanoseconds time) override
	{
		if (pause_ != nullptr) {
			pause_->reach(written_);
		}
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
	Pause *pause_;
	int64_t splitAt_;
};

// Writes a byte, and when the file is done takes the name it is meant for, as another
// program could just before the export publishes it.
class NameTakingWriter final : public tapeloop::VideoFileWriter {
public:
	void open(const fs::path &path, const Gop &) override
	{
		part_ = path;
		const uint8_t byte = 1;
		File(path, File::Mode::CreateNew).writeAt(0, {&byte, 1});
	}
	bool fits(const Gop &) const override { return true; }
	void write(const Gop &, size_t, Nanoseconds) override {}
	void finish() override
	{
		const uint8_t byte = 2;
		File(fs::path(part_).replace_extension(), File::Mode::CreateNew).writeAt(0, {&byte, 1});
	}

private:
	fs::path part_;
};

// Finds its path taken, as when another OBS exporting the same replay gets there first,
// and fails as a writer that creates its file only where none is.
class TakenPathWriter final : public tapeloop::VideoFileWriter {
public:
	void open(const fs::path &path, const Gop &) override
	{
		const uint8_t byte = 3;
		File(path, File::Mode::CreateNew).writeAt(0, {&byte, 1});
		throw std::runtime_error("a file is already at " + tapeloop::utf8FromPath(path));
	}
	bool fits(const Gop &) const override { return true; }
	void write(const Gop &, size_t, Nanoseconds) override {}
	void finish() override {}
};

ReplayExporter::WriterFactory recording(std::vector<WrittenFile> &files, int failAt = -1, Pause *pause = nullptr,
					int64_t splitAt = -1)
{
	auto written = std::make_shared<int>(0);
	return [&files, written, failAt, pause, splitAt] {
		return std::make_unique<RecordingWriter>(files, *written, failAt, pause, splitAt);
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

TEST_CASE("an MP4 part after a change of codec starts at its own first frame")
{
	TempDirectory dir;
	Gops gops = makeGops(2);
	const Gops hevc = makeGops(2, 10, VideoCodec::Hevc, nullptr);
	gops.insert(gops.end(), hevc.begin(), hevc.end());
	const WrittenReplay written = ReplayWriter().write(captureOf(dir.path(), {trimmed("a", gops)}));
	std::vector<WrittenFile> files;
	ReplayExporter exporter(recording(files));
	REQUIRE(exportOnce(exporter, requestFor(written, ExportFormat::Mp4)).error.empty());
	REQUIRE(files.size() == 2);
	CHECK(files[0].codec == VideoCodec::H264);
	CHECK(files[0].times[2] == Nanoseconds{0});
	CHECK(files[1].codec == VideoCodec::Hevc);
	REQUIRE(files[1].times.size() == 9);
	CHECK(files[1].times.front() == Nanoseconds{0});
	CHECK(files[1].times.back() == hevc.back()->packets()[3].time - hevc.front()->startTime());
}

TEST_CASE("an MP4 part goes on in a new file from a GOP that does not fit the one before")
{
	TempDirectory dir;
	const Gops gops = makeGops(4);
	const WrittenReplay written = ReplayWriter().write(captureOf(dir.path(), {trimmed("a", gops)}));
	REQUIRE(tapeloop::mp4Parts(written.index.sources.front()).size() == 1);
	std::vector<WrittenFile> files;
	ReplayExporter exporter(recording(files, -1, nullptr, gops[2]->packets().front().pts));
	const ExportResult result = exportOnce(exporter, requestFor(written, ExportFormat::Mp4));
	REQUIRE(result.error.empty());
	REQUIRE(files.size() == 2);
	REQUIRE(result.files.size() == 2);
	CHECK(files[0].finished);
	REQUIRE(files[0].times.size() == 10);
	CHECK(files[0].times[2] == Nanoseconds{0});
	// The next file starts at its own keyframe and goes on to the out point.
	CHECK(files[1].finished);
	REQUIRE(files[1].times.size() == 9);
	CHECK(files[1].times.front() == Nanoseconds{0});
	CHECK(files[1].times.back() == gops[3]->packets()[3].time - gops[2]->startTime());
	CHECK(namesIn(dir.path() / "Export") ==
	      std::vector<std::string>{"2026-10-09 21-05-42 - Camera a (2).mp4", "2026-10-09 21-05-42 - Camera a.mp4"});
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
	CHECK(files[0].path.filename() == "2026-10-09 21-05-42 - Camera a.mp4.tapeloop-part");

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
	File(dir.path() / "x (2).mp4.tapeloop-part", File::Mode::CreateNew).flush();
	CHECK(tapeloop::freeExportPath(dir.path(), "x", ".mp4", {}) == dir.path() / "x (3).mp4");
	CHECK(tapeloop::freeExportPath(dir.path(), "x", ".mp4", {dir.path() / "x (3).mp4"}) ==
	      dir.path() / "x (4).mp4");
	CHECK(tapeloop::freeExportPath(dir.path(), "y", ".tplp", {}) == dir.path() / "y.tplp");
	CHECK(tapeloop::mp4Stem(dir.path() / "2026-10-09 21-05-42.tplp", "Cam/1") == "2026-10-09 21-05-42 - Cam_1");
}

TEST_CASE("an export whose name is taken while it is written gets the next free one")
{
	TempDirectory dir;
	const WrittenReplay written = ReplayWriter().write(captureOf(dir.path(), {sourceOf("a", makeGops(1))}));
	ReplayExporter exporter([] { return std::make_unique<NameTakingWriter>(); });
	const ExportResult result = exportOnce(exporter, requestFor(written, ExportFormat::Mp4));
	REQUIRE(result.error.empty());
	const fs::path folder = dir.path() / "Export";
	REQUIRE(result.files.size() == 1);
	CHECK(result.files.front() == folder / "2026-10-09 21-05-42 - Camera a (2).mp4");
	CHECK(tapeloop::test::fileBytes(result.files.front()) == std::vector<uint8_t>{1});
	CHECK(tapeloop::test::fileBytes(folder / "2026-10-09 21-05-42 - Camera a.mp4") == std::vector<uint8_t>{2});
	CHECK(namesIn(folder).size() == 2);
}

TEST_CASE("an MP4 export that cannot open its file leaves the file at that path")
{
	TempDirectory dir;
	const WrittenReplay written = ReplayWriter().write(captureOf(dir.path(), {sourceOf("a", makeGops(1))}));
	ReplayExporter exporter([] { return std::make_unique<TakenPathWriter>(); });
	const ExportResult result = exportOnce(exporter, requestFor(written, ExportFormat::Mp4));
	CHECK_FALSE(result.error.empty());
	CHECK(result.files.empty());
	const fs::path folder = dir.path() / "Export";
	REQUIRE(namesIn(folder) == std::vector<std::string>{"2026-10-09 21-05-42 - Camera a.mp4.tapeloop-part"});
	CHECK(tapeloop::test::fileBytes(folder / "2026-10-09 21-05-42 - Camera a.mp4.tapeloop-part") ==
	      std::vector<uint8_t>{3});
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
	CHECK(index->id != written.index.id);
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

TEST_CASE("a self-contained export is listed next to the replay it came from")
{
	TempDirectory dir;
	const WrittenReplay written =
		ReplayWriter().write(captureOf(dir.path() / "Liga A", {sourceOf("a", makeGops(2))}));
	ReplayExporter exporter;
	const ExportResult result = exportOnce(exporter, requestFor(written, ExportFormat::Replay));
	REQUIRE(result.error.empty());
	fs::create_directories(dir.path() / "Liga B");
	fs::copy_file(result.files.front(), dir.path() / "Liga B" / "Imported.tplp");

	const tapeloop::ReplayScan scan = tapeloop::scanReplays(dir.path());
	REQUIRE(scan.replays.size() == 2);
	CHECK(scan.replays[0].intact);
	CHECK(scan.replays[1].intact);
	CHECK(scan.replays[0].id != scan.replays[1].id);
	tapeloop::ReplayLibrary library;
	CHECK(library.addFound(scan.replays[0]) != 0);
	CHECK(library.addFound(scan.replays[1]) != 0);
	CHECK(library.size() == 2);
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

TEST_CASE("an MP4 export refuses a GOP whose times are not the ones its index gives")
{
	TempDirectory dir;
	const Gops gops = makeGops(1);
	const WrittenReplay written = ReplayWriter().write(captureOf(dir.path(), {sourceOf("a", gops)}));
	// A self-contained file whose CRCs all hold, with its frames near the end of time in
	// its index and near the start in its GOP: farther apart than a count can hold.
	const Gop &original = *gops.front();
	tapeloop::GopBuilder builder(original.frameDuration());
	builder.setCodecConfig(original.codec(), tapeloop::test::h264Config());
	ReplayIndex index = written.index;
	StoredSource &source = index.sources.front();
	for (size_t i = 0; i < original.packets().size(); ++i) {
		const tapeloop::PacketRecord &packet = original.packets()[i];
		const Nanoseconds step = original.frameDuration() * static_cast<int64_t>(i);
		builder.append({original.packetData(i), packet.pts, packet.dts,
				Nanoseconds{-6'000'000'000'000'000'000} + step, packet.keyframe});
		source.frameTimes[i] = Nanoseconds{4'000'000'000'000'000'000} + step;
	}
	source.in = source.frameTimes.front();
	source.out = source.frameTimes.back();
	const std::shared_ptr<const Gop> crafted = builder.seal();
	ExportRequest request = requestFor(written, ExportFormat::Replay);
	request.index = std::make_shared<const ReplayIndex>(index);
	request.live = {{crafted}};
	std::vector<WrittenFile> files;
	ReplayExporter exporter(recording(files));
	const ExportResult copy = exportOnce(exporter, request);
	REQUIRE(copy.error.empty());
	REQUIRE(tapeloop::readReplayIndex(copy.files.front()));

	ExportRequest mp4;
	mp4.format = ExportFormat::Mp4;
	mp4.manifest = copy.files.front();
	CHECK_FALSE(exportOnce(exporter, mp4).error.empty());
	CHECK(files.empty());
	CHECK(namesIn(dir.path() / "Export" / "Export").empty());
}

TEST_CASE("an export's progress never goes past its end")
{
	TempDirectory dir;
	const Gops gops = makeGops(2);
	const WrittenReplay written = ReplayWriter().write(captureOf(dir.path(), {sourceOf("a", gops)}));
	// A first GOP from the buffers that holds all ten frames the index counts, so that
	// the export writes more packets than it counted.
	tapeloop::test::SyntheticEncoder::Config settings;
	settings.gopLength = 2 * tapeloop::test::kGopLength;
	tapeloop::test::SyntheticEncoder encoder(settings);
	tapeloop::GopBuilder builder(encoder.frameDuration());
	builder.setCodecConfig(VideoCodec::H264, tapeloop::test::h264Config());
	for (int64_t frame = 0; frame < settings.gopLength; ++frame) {
		builder.append(encoder.next());
	}
	const std::shared_ptr<const Gop> whole = builder.seal();
	ExportRequest request = requestFor(written, ExportFormat::Mp4);
	request.live = {{whole, gops[1]}};

	std::vector<WrittenFile> files;
	Pause pause(12);
	ReplayExporter exporter(recording(files, -1, &pause));
	const uint64_t ticket = exporter.exportReplay(request);
	pause.waitUntilReached();
	const std::optional<tapeloop::ExportProgress> progress = exporter.progress();
	pause.release();
	exporter.waitUntilIdle();
	REQUIRE(progress);
	CHECK(progress->ticket == ticket);
	CHECK(progress->packets == 10);
	CHECK(progress->packetsDone == 10);
	REQUIRE(exporter.poll().size() == 1);
	REQUIRE(files.size() == 1);
	CHECK(files.front().times.size() == 15);
}

TEST_CASE("an exporter keeps no GOP once its exports are done")
{
	TempDirectory dir;
	const WrittenReplay written = ReplayWriter().write(captureOf(dir.path(), {sourceOf("a", makeGops(2))}));
	std::optional<ReplayIndex> read = tapeloop::readReplayIndex(written.manifest);
	REQUIRE(read);
	const auto index = std::make_shared<const ReplayIndex>(std::move(*read));
	// Each GOP read from a segment holds the configuration of its run.
	const std::shared_ptr<const tapeloop::CodecConfig> &config = index->sources.front().runs.front().config;
	REQUIRE(config);
	REQUIRE(config.use_count() == 1);
	std::vector<WrittenFile> files;
	ReplayExporter exporter(recording(files));
	for (const ExportFormat format : {ExportFormat::Mp4, ExportFormat::Replay}) {
		ExportRequest request = requestFor(written, format);
		request.index = index;
		REQUIRE(exportOnce(exporter, request).error.empty());
		CHECK(config.use_count() == 1);
	}
}

TEST_CASE("a scan leaves alone the file an export is writing")
{
	TempDirectory dir;
	const fs::path folder = dir.path() / "Liga 2026-10-09 21-00";
	const WrittenReplay written = ReplayWriter().write(captureOf(folder, {sourceOf("a", makeGops(2))}));
	std::vector<WrittenFile> files;
	Pause pause(0);
	ReplayExporter exporter(recording(files, -1, &pause));
	exporter.exportReplay(requestFor(written, ExportFormat::Mp4));
	pause.waitUntilReached();
	const bool writing = fs::exists(folder / "Export" / "2026-10-09 21-05-42 - Camera a.mp4.tapeloop-part");
	const tapeloop::ReplayScan scan = tapeloop::scanReplays(dir.path());
	pause.release();
	exporter.waitUntilIdle();
	CHECK(writing);
	CHECK(scan.removed.empty());
	const std::vector<ExportResult> results = exporter.poll();
	REQUIRE(results.size() == 1);
	CHECK(results.front().error.empty());
	CHECK(namesIn(folder / "Export") == std::vector<std::string>{"2026-10-09 21-05-42 - Camera a.mp4"});
}

TEST_CASE("an exporter that goes during an export stops it and leaves no file")
{
	TempDirectory dir;
	const WrittenReplay written = ReplayWriter().write(captureOf(dir.path(), {sourceOf("a", makeGops(20))}));
	std::vector<WrittenFile> files;
	Pause pause(0);
	auto exporter = std::make_unique<ReplayExporter>(recording(files, -1, &pause));
	exporter->exportReplay(requestFor(written, ExportFormat::Mp4));
	pause.waitUntilReached();
	std::thread closing([&exporter] { exporter.reset(); });
	// Time for the exporter to ask the export to stop before the export goes on.
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	pause.release();
	closing.join();
	REQUIRE(files.size() == 1);
	CHECK(files.front().times.size() < static_cast<size_t>(20 * tapeloop::test::kGopLength));
	CHECK_FALSE(files.front().finished);
	CHECK(namesIn(dir.path() / "Export").empty());
}

TEST_CASE("a stopped exporter reports the export it stopped, and drops the others")
{
	TempDirectory dir;
	const WrittenReplay written = ReplayWriter().write(captureOf(dir.path(), {sourceOf("a", makeGops(20))}));
	std::vector<WrittenFile> files;
	Pause pause(0);
	ReplayExporter exporter(recording(files, -1, &pause));
	const uint64_t running = exporter.exportReplay(requestFor(written, ExportFormat::Mp4));
	exporter.exportReplay(requestFor(written, ExportFormat::Replay));
	pause.waitUntilReached();
	std::thread stopping([&exporter] { exporter.stop(); });
	// Time for the exporter to ask the export to stop before the export goes on.
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	pause.release();
	stopping.join();
	const std::vector<ExportResult> results = exporter.poll();
	REQUIRE(results.size() == 1);
	CHECK(results.front().ticket == running);
	CHECK(results.front().error == "stopped, OBS is closing");
	CHECK(results.front().files.empty());
	// One asked for later is dropped too, rather than waited for.
	exporter.exportReplay(requestFor(written, ExportFormat::Replay));
	CHECK(exporter.pending() == 0);
	exporter.waitUntilIdle();
	CHECK(exporter.poll().empty());
	CHECK(namesIn(dir.path() / "Export").empty());
	exporter.stop();
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
