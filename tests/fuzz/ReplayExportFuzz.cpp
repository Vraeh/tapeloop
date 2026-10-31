// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/GopReader.hpp"
#include "core/ReplayExport.hpp"
#include "core/ReplayWriter.hpp"
#include "decode/Mp4Writer.hpp"
#include "decode/FFmpegVersion.hpp"

#include "../core/TempDirectory.hpp"
#include "FuzzInput.hpp"
#include "StoredReplay.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <system_error>
#include <vector>

using tapeloop::ExportFormat;
using tapeloop::ExportRequest;
using tapeloop::ExportResult;
using tapeloop::GopReader;
using tapeloop::ReplayExporter;
using tapeloop::ReplayIndex;
using tapeloop::decode::Mp4Writer;
using tapeloop::fuzz::FuzzInput;
using tapeloop::fuzz::require;
using tapeloop::test::TempDirectory;

namespace {

// A replay exported again holds the frames of the one it came from, and all of them read.
void checkExportedReplay(const std::filesystem::path &exported, const ReplayIndex &original)
{
	const std::optional<ReplayIndex> index = tapeloop::readReplayIndex(exported);
	require(index && index->sources.size() == original.sources.size());
	GopReader reader(0);
	for (size_t s = 0; s < index->sources.size(); ++s) {
		const tapeloop::StoredSource &source = index->sources[s];
		require(source.frameTimes == original.sources[s].frameTimes && source.in == original.sources[s].in &&
			source.out == original.sources[s].out);
		for (size_t gop = 0; gop < source.gops.size(); ++gop) {
			require(reader.read(exported, source, gop, {}) != nullptr);
		}
	}
}

} // namespace

extern "C" int LLVMFuzzerInitialize(int *, char ***)
{
	// The parsers report every broken bitstream, which would bury what the fuzzer prints.
	tapeloop::decode::quietFFmpegLog();
	return 0;
}

// A replay written from the input, its bitstreams the input's own, damaged and its CRCs
// computed again as the input says, exported as a replay found on disk is: to MP4 files
// through FFmpeg's parsers and muxer, or to a self-contained replay. Whatever the files
// hold, nothing reads or writes outside its memory, an export that succeeds leaves the
// files it reports, and one that fails leaves none.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	FuzzInput input(data, size);
	const TempDirectory folder;
	ExportRequest request;
	request.manifest = tapeloop::fuzz::writeStoredReplay(input, folder.path());
	request.format = input.flag() ? ExportFormat::Replay : ExportFormat::Mp4;
	const std::optional<ReplayIndex> original = tapeloop::readReplayIndex(request.manifest);

	std::vector<ExportResult> results;
	{
		ReplayExporter exporter([] { return std::make_unique<Mp4Writer>(); });
		exporter.exportReplay(request);
		exporter.waitUntilIdle();
		results = exporter.poll();
	}
	require(results.size() == 1);
	const ExportResult &result = results.front();
	require(result.error.empty() || result.files.empty());
	// What cannot be read cannot be exported.
	require(original || !result.error.empty());

	std::error_code error;
	const std::filesystem::path exports = tapeloop::exportFolderOf(request.manifest);
	for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(exports, error)) {
		require(std::find(result.files.begin(), result.files.end(), entry.path()) != result.files.end());
	}
	for (const std::filesystem::path &file : result.files) {
		require(std::filesystem::file_size(file, error) > 0 && !error);
	}
	if (request.format == ExportFormat::Replay && result.error.empty()) {
		require(result.files.size() == 1);
		checkExportedReplay(result.files.front(), *original);
	}
	return 0;
}
