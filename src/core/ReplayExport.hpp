// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/Gop.hpp"
#include "core/GopReader.hpp"
#include "core/MediaTime.hpp"
#include "core/ReplayFormat.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace tapeloop {

enum class ExportFormat {
	// One MP4 per source by remux, and one more each time the encoder of a source fell back
	// from HEVC to H.264 or changed its picture size.
	Mp4,
	// One self-contained .tplp, which plays without the broadcast folder.
	Replay,
};

// Writes the packets of one source to a video file; the decode module writes MP4 with
// FFmpeg. Each call throws std::runtime_error, naming what failed, when it cannot do its
// part, and the caller then deletes the file, except when open fails: open creates the
// file only where none is, and removes it again itself if a later step of it fails.
class VideoFileWriter {
public:
	VideoFileWriter() = default;
	virtual ~VideoFileWriter() = default;

	VideoFileWriter(const VideoFileWriter &) = delete;
	VideoFileWriter &operator=(const VideoFileWriter &) = delete;
	VideoFileWriter(VideoFileWriter &&) = delete;
	VideoFileWriter &operator=(VideoFileWriter &&) = delete;

	// Starts a file at path for GOPs of first's codec and configuration.
	virtual void open(const std::filesystem::path &path, const Gop &first) = 0;
	// Whether gop, of the codec of the file, can go on in it. One that cannot starts a file
	// of its own.
	virtual bool fits(const Gop &gop) const = 0;
	// A packet of gop, in decode order, at time from the file's start, which is negative
	// for the frames before it that only serve decoding. A GOP of another configuration of
	// the same codec carries it along.
	virtual void write(const Gop &gop, size_t packet, Nanoseconds time) = 0;
	virtual void finish() = 0;
};

// The GOPs of the MP4 files of a source with one codec: those with a frame between its in
// and out points, as long as they keep that codec. A part goes on in a new file from a GOP
// that does not fit the one open (VideoFileWriter::fits).
struct Mp4Part {
	size_t firstGop = 0;
	size_t endGop = 0;
};
std::vector<Mp4Part> mp4Parts(const StoredSource &source);

// "<name> - <source>" for the MP4 files of a replay whose manifest is named <name>.tplp.
std::string mp4Stem(const std::filesystem::path &manifest, const std::string &sourceName);

// Where the exports of the replay with this manifest go: the Export folder of its
// broadcast folder.
std::filesystem::path exportFolderOf(const std::filesystem::path &manifest);

// The first of stem + extension, stem (2) + extension and so on that is neither in the
// folder nor among taken. Throws std::runtime_error when a thousand are.
std::filesystem::path freeExportPath(const std::filesystem::path &folder, const std::string &stem,
				     const std::string &extension, const std::vector<std::filesystem::path> &taken);

// Deletes the file at path if it is one an export stopped by a crash leaves, named for
// the export's file with .tapeloop-part after it, and no export of this process is
// writing it. Returns whether it did; error says why it could not.
bool removeStaleExportPart(const std::filesystem::path &path, std::error_code &error);

struct ExportRequest {
	ExportFormat format = ExportFormat::Mp4;
	std::filesystem::path manifest;
	// Read from the manifest when null.
	std::shared_ptr<const ReplayIndex> index;
	// Per source and per GOP of the index, the buffers' own GOPs, read instead of the
	// segments while they last. May be empty.
	std::vector<std::vector<std::weak_ptr<const Gop>>> live;
	// What a self-contained replay carries.
	std::vector<std::string> tags;
};

struct ExportResult {
	uint64_t ticket = 0;
	// What it wrote, in order.
	std::vector<std::filesystem::path> files;
	// Why it failed, empty when it did not. A failed export leaves no file behind.
	std::string error;
};

struct ExportProgress {
	uint64_t ticket = 0;
	uint64_t packetsDone = 0;
	uint64_t packets = 0;
};

// Exports, on a thread of their own at low I/O priority, one at a time in the order they
// were asked for. Each writes its files under a temporary name and renames them when
// they are complete, so a file with its final name is always whole. Safe to call from
// any thread.
class ReplayExporter {
public:
	using WriterFactory = std::function<std::unique_ptr<VideoFileWriter>()>;

	// Without a factory, MP4 exports fail.
	explicit ReplayExporter(WriterFactory mp4 = {});
	// Stops, as stop() does.
	~ReplayExporter();

	ReplayExporter(const ReplayExporter &) = delete;
	ReplayExporter &operator=(const ReplayExporter &) = delete;
	ReplayExporter(ReplayExporter &&) = delete;
	ReplayExporter &operator=(ReplayExporter &&) = delete;

	bool canExport(ExportFormat format) const noexcept;
	// Returns the ticket its result will carry, from 1 up. Once stopped, the export is
	// dropped and no result carries it.
	uint64_t exportReplay(ExportRequest request);
	// The results of the exports finished since the last call, in the order they finished.
	std::vector<ExportResult> poll();
	// The export running; nothing while none runs.
	std::optional<ExportProgress> progress() const;
	// Exports asked for that have not finished.
	size_t pending() const;
	// Returns once every export asked for so far has finished.
	void waitUntilIdle();
	// Stops the export running, which then leaves no file and reports why, and drops the
	// others, which report nothing. Returns once the export running has stopped.
	void stop();

private:
	struct Job {
		uint64_t ticket = 0;
		ExportRequest request;
	};

	void run() noexcept;
	ExportResult execute(const Job &job) noexcept;
	std::vector<std::filesystem::path> exportMp4(const ExportRequest &request, const ReplayIndex &index,
						     GopReader &reader);
	std::vector<std::filesystem::path> exportReplay(const ExportRequest &request, const ReplayIndex &index,
							GopReader &reader);
	std::shared_ptr<const Gop> readGop(const ExportRequest &request, const ReplayIndex &index, GopReader &reader,
					   size_t source, size_t gop);
	void setProgress(const ExportProgress &progress);
	void setPacketCount(uint64_t packets);
	void countDone(uint64_t packets);

	WriterFactory mp4_;
	mutable std::mutex mutex_;
	std::condition_variable wake_;
	std::condition_variable idle_;
	// The export running is the first, until it has finished.
	std::deque<Job> jobs_;
	std::list<ExportResult> results_;
	uint64_t nextTicket_ = 1;
	bool stopping_ = false;
	std::atomic<bool> stop_{false};
	// Under a lock of its own, so that progress() reads it whole.
	mutable std::mutex progressMutex_;
	ExportProgress progress_;
	// So that only one stop() joins the worker.
	std::mutex joinMutex_;
	// Last, so that it starts after everything it uses.
	std::thread worker_;
};

} // namespace tapeloop
