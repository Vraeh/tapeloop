// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

// The replay storage as a whole: what a crash or a damaged file leaves readable, and what
// a capture costs.

#include "core/FileIo.hpp"
#include "core/GopReader.hpp"
#include "core/ReplayWriter.hpp"

#include "StoredReplays.hpp"
#include "SyntheticEncoder.hpp"
#include "TempDirectory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using tapeloop::CaptureSource;
using tapeloop::Clip;
using tapeloop::GopReader;
using tapeloop::ReplayCapture;
using tapeloop::ReplayIndex;
using tapeloop::ReplayWriter;
using tapeloop::ReplayWriterConfig;
using tapeloop::StoredSource;
using tapeloop::test::captureOf;
using tapeloop::test::Gops;
using tapeloop::test::slice;
using tapeloop::test::SyntheticEncoder;
using tapeloop::test::TempDirectory;

namespace fs = std::filesystem;

namespace {

// GOPs of thirty frames at 60 fps, from one encoder run.
Gops encode(size_t count, size_t keyframeSize, size_t frameSize)
{
	SyntheticEncoder::Config settings;
	settings.gopLength = 30;
	settings.keyframeSize = keyframeSize;
	settings.frameSize = frameSize;
	SyntheticEncoder encoder(settings);
	tapeloop::GopBuilder builder(encoder.frameDuration());
	builder.setCodecConfig(tapeloop::VideoCodec::H264, tapeloop::test::h264Config());
	Gops gops;
	for (size_t gop = 0; gop < count; ++gop) {
		for (int frame = 0; frame < 30; ++frame) {
			builder.append(encoder.next());
		}
		gops.push_back(builder.seal());
	}
	return gops;
}

ReplayCapture captureOfSources(const fs::path &folder, const Gops &gops, size_t sources, const std::string &stem)
{
	std::vector<CaptureSource> captured;
	for (size_t source = 0; source < sources; ++source) {
		captured.push_back(tapeloop::test::sourceOf("source-" + std::to_string(source), gops));
	}
	return captureOf(folder, std::move(captured), stem);
}

// Every GOP of the replay reads back whole, with the bytes the encoder gave it.
bool readsBackWhole(const fs::path &manifest)
{
	const std::optional<ReplayIndex> index = tapeloop::readReplayIndex(manifest);
	if (!index) {
		return false;
	}
	GopReader reader(0);
	for (const StoredSource &source : index->sources) {
		for (size_t gop = 0; gop < source.gops.size(); ++gop) {
			const auto read = reader.read(manifest, source, gop);
			if (!read || !tapeloop::test::hasExpectedBytes(*read)) {
				return false;
			}
		}
	}
	return true;
}

} // namespace

#ifndef _WIN32
TEST_CASE("a writer killed at any point leaves every replay it finished whole")
{
	// Captures of three sources over a window of ten GOPs that moves two GOPs at a time,
	// so each appends some GOPs and finds the others on disk. The parent kills the child
	// at a random moment and reads back every replay the child had reported.
	const Gops gops = encode(200, 40000, 9000);
	constexpr size_t kCaptures = 95;
	const uint32_t seed = std::random_device{}();
	CAPTURE(seed);
	std::mt19937 random(seed);
	ReplayWriterConfig config;
	config.segmentBytes = size_t{4} << 20;
	config.writeBytes = size_t{1} << 20;
	for (int round = 0; round < 20; ++round) {
		CAPTURE(round);
		TempDirectory dir;
		const fs::path folder = dir.path() / "Liga 2026-10-09 21-00";
		int finished[2] = {-1, -1};
		REQUIRE(::pipe(finished) == 0);
		const pid_t child = ::fork();
		REQUIRE(child >= 0);
		if (child == 0) {
			::close(finished[0]);
			try {
				ReplayWriter writer(config);
				for (size_t i = 0; i < kCaptures; ++i) {
					writer.write(captureOfSources(folder, slice(gops, 2 * i, 2 * i + 10), 3,
								      "replay " + std::to_string(i)));
					const auto done = static_cast<unsigned char>(i);
					if (::write(finished[1], &done, 1) != 1) {
						::_exit(3);
					}
				}
			} catch (...) {
				::_exit(2);
			}
			::_exit(0);
		}
		::close(finished[1]);
		std::this_thread::sleep_for(std::chrono::microseconds(random() % 150000));
		::kill(child, SIGKILL);
		int status = 0;
		REQUIRE(::waitpid(child, &status, 0) == child);
		REQUIRE((WIFSIGNALED(status) || (WIFEXITED(status) && WEXITSTATUS(status) == 0)));
		size_t reported = 0;
		unsigned char done = 0;
		while (::read(finished[0], &done, 1) == 1) {
			++reported;
		}
		::close(finished[0]);

		for (size_t i = 0; i < reported; ++i) {
			CAPTURE(i);
			CHECK(readsBackWhole(folder / ("replay " + std::to_string(i) + ".tplp")));
		}
		// What the crash left is a half written manifest at most, and every manifest there
		// is whole, the one the child had not reported yet included.
		const tapeloop::ReplayScan scan = tapeloop::scanReplays(dir.path());
		CHECK(scan.removed.size() <= 1);
		CHECK(scan.errors.empty());
		CHECK(scan.replays.size() >= reported);
		CHECK(scan.replays.size() <= reported + 1);
		for (const tapeloop::FoundReplay &found : scan.replays) {
			CHECK(found.intact);
		}
		// OBS starting again writes on next to what the crash left.
		ReplayWriter after(config);
		const tapeloop::WrittenReplay next =
			after.write(captureOfSources(folder, slice(gops, 190, 200), 3, "after"));
		CHECK(readsBackWhole(next.manifest));
		for (size_t i = 0; i < reported; ++i) {
			CHECK(readsBackWhole(folder / ("replay " + std::to_string(i) + ".tplp")));
		}
	}
}
#endif

TEST_CASE("a segment cut short loses only the GOPs past the cut")
{
	const Gops gops = encode(40, 20000, 4000);
	const uint32_t seed = std::random_device{}();
	CAPTURE(seed);
	std::mt19937 random(seed);
	ReplayWriterConfig config;
	config.segmentBytes = size_t{1} << 20;
	for (int round = 0; round < 30; ++round) {
		CAPTURE(round);
		TempDirectory dir;
		ReplayWriter writer(config);
		std::vector<fs::path> manifests;
		for (size_t i = 0; i < 4; ++i) {
			manifests.push_back(writer.write(captureOfSources(dir.path(), slice(gops, 8 * i, 8 * i + 16), 2,
									  std::to_string(i)))
						    .manifest);
		}
		std::vector<fs::path> segments;
		for (const auto &entry : fs::directory_iterator(dir.path() / "data")) {
			segments.push_back(entry.path());
		}
		std::sort(segments.begin(), segments.end());
		const fs::path cut = segments[random() % segments.size()];
		const uint64_t at = random() % (fs::file_size(cut) + 1);
		CAPTURE(tapeloop::utf8FromPath(cut.filename()), at);
		fs::resize_file(cut, at);

		for (const fs::path &manifest : manifests) {
			const std::optional<ReplayIndex> index = tapeloop::readReplayIndex(manifest);
			REQUIRE(index);
			GopReader reader(0);
			for (const StoredSource &source : index->sources) {
				for (size_t gop = 0; gop < source.gops.size(); ++gop) {
					const tapeloop::StoredGop &stored = source.gops[gop];
					const bool lost = tapeloop::segmentPath(manifest, source, stored.segment) ==
								  cut &&
							  stored.offset + stored.size > at;
					const auto read = reader.read(manifest, source, gop);
					CHECK(static_cast<bool>(read) != lost);
					if (read) {
						CHECK(tapeloop::test::hasExpectedBytes(*read));
					}
				}
			}
		}
	}
}

TEST_CASE("a manifest cut short anywhere is listed as damaged")
{
	const Gops gops = encode(6, 20000, 4000);
	TempDirectory dir;
	ReplayWriter writer;
	const fs::path kept = writer.write(captureOfSources(dir.path() / "Liga", gops, 2, "kept")).manifest;
	const fs::path cut = writer.write(captureOfSources(dir.path() / "Liga", gops, 2, "cut")).manifest;
	const uint64_t size = fs::file_size(cut);
	const std::vector<uint8_t> whole = tapeloop::test::fileBytes(cut);
	const uint32_t seed = std::random_device{}();
	CAPTURE(seed);
	std::mt19937 random(seed);
	for (int round = 0; round < 50; ++round) {
		const uint64_t at = random() % size;
		CAPTURE(at);
		fs::resize_file(cut, at);
		const tapeloop::ReplayScan scan = tapeloop::scanReplays(dir.path());
		REQUIRE(scan.replays.size() == 2);
		CHECK_FALSE(scan.replays[0].intact);
		CHECK(scan.replays[1].manifest == kept);
		CHECK(scan.replays[1].intact);
		tapeloop::File(cut, tapeloop::File::Mode::ReadWrite).writeAt(0, whole);
	}
}

// Writes 8 sources of 60 s at about 30 Mbps, as a capture of a full buffer does, then a
// capture 20 s later and one of the same window again, and reads GOPs back. Not run by
// default: it writes about 2.5 GB. Run on the machine to measure with
// tapeloop-core-tests "[.measure]", with TMPDIR on the disk to measure.
TEST_CASE("replay storage costs", "[.measure]")
{
	// Keyframes of 230 KB and frames of 55 KB, 29 Mbps at 60 fps.
	const Gops gops = encode(160, 230000, 55000);
	TempDirectory dir;
	const fs::path folder = dir.path() / "Liga 2026-10-09 21-00";
	ReplayWriter writer;
	const auto timed = [&](const Gops &window, const char *what) {
		const auto start = std::chrono::steady_clock::now();
		const tapeloop::WrittenReplay written = writer.write(captureOfSources(folder, window, 8, what));
		const std::chrono::duration<double> took = std::chrono::steady_clock::now() - start;
		std::printf("%-28s %8.1f MB written, %4zu GOPs new, %4zu on disk, %6.2f s\n", what,
			    static_cast<double>(written.bytesAppended) / 1e6, written.gopsAppended, written.gopsShared,
			    took.count());
		return written;
	};
	timed(slice(gops, 0, 120), "first capture, 8 x 60 s");
	timed(slice(gops, 40, 160), "20 s later");
	const tapeloop::WrittenReplay again = timed(slice(gops, 40, 160), "the same window again");

	// The manifests of a long broadcast, listed at load.
	for (int i = 0; i < 97; ++i) {
		writer.write(captureOfSources(folder, slice(gops, 40, 160), 8, "listed " + std::to_string(i)));
	}
	auto start = std::chrono::steady_clock::now();
	const tapeloop::ReplayScan scan = tapeloop::scanReplays(dir.path());
	std::chrono::duration<double> took = std::chrono::steady_clock::now() - start;
	std::printf("scan of %zu replays %25.3f s\n", scan.replays.size(), took.count());

	const ReplayIndex &index = again.index;
	std::mt19937 random(7);
	for (const char *what : {"cold", "warm"}) {
		double total = 0;
		constexpr int kReads = 40;
		for (int i = 0; i < kReads; ++i) {
			const StoredSource &source = index.sources[random() % index.sources.size()];
			const size_t gop = random() % source.gops.size();
#ifdef __linux__
			// Elsewhere the page cache stays, and "cold" reads it warm.
			if (std::string(what) == "cold") {
				const fs::path segment =
					tapeloop::segmentPath(again.manifest, source, source.gops[gop].segment);
				const int fd = ::open(segment.c_str(), O_RDONLY);
				::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
				::close(fd);
			}
#endif
			GopReader fresh(0);
			start = std::chrono::steady_clock::now();
			const auto read = fresh.read(again.manifest, source, gop);
			took = std::chrono::steady_clock::now() - start;
			REQUIRE(read);
			total += took.count();
		}
		std::printf("random GOP read, %s %18.2f ms\n", what, total / kReads * 1000);
	}
	CHECK(scan.replays.size() == 100);
}
