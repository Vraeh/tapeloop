// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/ReplayExport.hpp"

#include "core/FileIo.hpp"
#include "core/ReplayNames.hpp"
#include "core/ReplayWriter.hpp"

#include <algorithm>
#include <exception>
#include <iterator>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace tapeloop {
namespace {

constexpr std::string_view kPartSuffix = ".part";
constexpr int kMaxNameTries = 1000;

std::filesystem::path partPathOf(const std::filesystem::path &path)
{
	std::filesystem::path part = path;
	part += pathFromUtf8(kPartSuffix);
	return part;
}

bool pathTaken(const std::filesystem::path &path)
{
	std::error_code error;
	const bool found = std::filesystem::exists(path, error);
	// What cannot be checked is taken, so that nothing is written over it.
	return found || error;
}

// Files deleted when it goes, unless kept: what a failed export wrote.
class Leftovers {
public:
	Leftovers() = default;
	~Leftovers()
	{
		for (const std::filesystem::path &path : paths_) {
			std::error_code ignored;
			std::filesystem::remove(path, ignored);
		}
	}

	Leftovers(const Leftovers &) = delete;
	Leftovers &operator=(const Leftovers &) = delete;
	Leftovers(Leftovers &&) = delete;
	Leftovers &operator=(Leftovers &&) = delete;

	void add(std::filesystem::path path) { paths_.push_back(std::move(path)); }
	void forget(const std::filesystem::path &path)
	{
		paths_.erase(std::remove(paths_.begin(), paths_.end(), path), paths_.end());
	}
	void keepAll() noexcept { paths_.clear(); }

private:
	std::vector<std::filesystem::path> paths_;
};

// Where each GOP's frames start in frameTimes, and after the last GOP, the end.
std::vector<size_t> gopFrames(const StoredSource &source)
{
	std::vector<size_t> frames{0};
	for (const StoredGop &gop : source.gops) {
		frames.push_back(std::min<size_t>(frames.back() + gop.packetCount, source.frameTimes.size()));
	}
	return frames;
}

// The name the file is published under: the one it was written for, unless something
// took that one meanwhile.
std::filesystem::path publishedPath(const std::filesystem::path &target, const std::string &stem,
				    const std::string &extension, const std::vector<std::filesystem::path> &taken)
{
	return pathTaken(target) ? freeExportPath(target.parent_path(), stem, extension, taken) : target;
}

} // namespace

std::vector<Mp4Part> mp4Parts(const StoredSource &source)
{
	std::vector<Mp4Part> parts;
	const std::vector<size_t> frames = gopFrames(source);
	for (size_t gop = 0; gop < source.gops.size(); ++gop) {
		const size_t first = frames[gop];
		const size_t end = frames[gop + 1];
		if (first >= end || source.frameTimes[end - 1] < source.in || source.frameTimes[first] > source.out) {
			continue;
		}
		const VideoCodec codec = source.runs[source.gops[gop].run].codec;
		if (parts.empty() || parts.back().endGop != gop ||
		    source.runs[source.gops[parts.back().firstGop].run].codec != codec) {
			parts.push_back({gop, gop + 1});
		} else {
			parts.back().endGop = gop + 1;
		}
	}
	return parts;
}

std::string mp4Stem(const std::filesystem::path &manifest, const std::string &sourceName)
{
	return safeFileName(utf8FromPath(manifest.stem()) + " - " + sourceName);
}

std::filesystem::path exportFolderOf(const std::filesystem::path &manifest)
{
	return manifest.parent_path() / pathFromUtf8(kExportFolderName);
}

std::filesystem::path freeExportPath(const std::filesystem::path &folder, const std::string &stem,
				     const std::string &extension, const std::vector<std::filesystem::path> &taken)
{
	for (int number = 1; number <= kMaxNameTries; ++number) {
		std::string name = stem;
		if (number > 1) {
			name += " (";
			name += std::to_string(number);
			name += ")";
		}
		name += extension;
		const std::filesystem::path path = folder / pathFromUtf8(name);
		if (!pathTaken(path) && !pathTaken(partPathOf(path)) &&
		    std::find(taken.begin(), taken.end(), path) == taken.end()) {
			return path;
		}
	}
	throw std::runtime_error("no free name for " + stem + extension + " in " + utf8FromPath(folder));
}

// Every GOP is read once, so the reader keeps only the last.
ReplayExporter::ReplayExporter(WriterFactory mp4) : mp4_(std::move(mp4)), reader_(0), worker_([this] { run(); }) {}

ReplayExporter::~ReplayExporter()
{
	{
		const std::lock_guard lock(mutex_);
		stopping_ = true;
	}
	stop_ = true;
	wake_.notify_all();
	worker_.join();
}

bool ReplayExporter::canExport(ExportFormat format) const noexcept
{
	return format == ExportFormat::Replay || static_cast<bool>(mp4_);
}

uint64_t ReplayExporter::exportReplay(ExportRequest request)
{
	uint64_t ticket = 0;
	{
		const std::lock_guard lock(mutex_);
		ticket = nextTicket_++;
		jobs_.push_back({ticket, std::move(request)});
	}
	wake_.notify_one();
	return ticket;
}

std::vector<ExportResult> ReplayExporter::poll()
{
	std::list<ExportResult> done;
	{
		const std::lock_guard lock(mutex_);
		done.swap(results_);
	}
	return {std::make_move_iterator(done.begin()), std::make_move_iterator(done.end())};
}

std::optional<ExportProgress> ReplayExporter::progress() const
{
	const uint64_t ticket = running_.load();
	if (ticket == 0) {
		return std::nullopt;
	}
	return ExportProgress{ticket, packetsDone_.load(), packets_.load()};
}

size_t ReplayExporter::pending() const
{
	const std::lock_guard lock(mutex_);
	return jobs_.size();
}

void ReplayExporter::waitUntilIdle()
{
	std::unique_lock lock(mutex_);
	idle_.wait(lock, [this] { return jobs_.empty(); });
}

void ReplayExporter::run() noexcept
{
	lowerThreadIoPriority();
	std::unique_lock lock(mutex_);
	while (true) {
		wake_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
		if (stopping_) {
			// What was not started is dropped; the one running has stopped already.
			jobs_.clear();
			idle_.notify_all();
			return;
		}
		const Job &job = jobs_.front();
		lock.unlock();
		packets_ = 0;
		packetsDone_ = 0;
		running_ = job.ticket;
		ExportResult result = execute(job);
		running_ = 0;
		std::list<ExportResult> slot;
		try {
			slot.push_back(std::move(result));
		} catch (...) {
			// Without memory for its result the export is not reported, which a poll that
			// waits on it notices from pending().
			slot.clear();
		}
		lock.lock();
		results_.splice(results_.end(), slot);
		jobs_.pop_front();
		idle_.notify_all();
	}
}

ExportResult ReplayExporter::execute(const Job &job) noexcept
{
	ExportResult result;
	result.ticket = job.ticket;
	try {
		std::shared_ptr<const ReplayIndex> index = job.request.index;
		if (!index) {
			std::optional<ReplayIndex> read = readReplayIndex(job.request.manifest);
			if (!read) {
				throw std::runtime_error("cannot read the replay " +
							 utf8FromPath(job.request.manifest));
			}
			index = std::make_shared<const ReplayIndex>(std::move(*read));
		}
		result.files = job.request.format == ExportFormat::Mp4 ? exportMp4(job.request, *index)
								       : exportReplay(job.request, *index);
	} catch (const std::exception &e) {
		result.files.clear();
		try {
			result.error = e.what();
		} catch (...) {
			// Short enough for the string's own buffer, so it needs no memory.
			result.error = "out of memory";
		}
		if (result.error.empty()) {
			result.error = "unknown error";
		}
	} catch (...) {
		result.files.clear();
		result.error = "unknown error";
	}
	return result;
}

std::vector<std::filesystem::path> ReplayExporter::exportMp4(const ExportRequest &request, const ReplayIndex &index)
{
	if (!mp4_) {
		throw std::runtime_error("MP4 export is not available here");
	}
	std::vector<std::vector<Mp4Part>> parts;
	uint64_t packets = 0;
	for (const StoredSource &source : index.sources) {
		parts.push_back(mp4Parts(source));
		const std::vector<size_t> frames = gopFrames(source);
		for (const Mp4Part &part : parts.back()) {
			for (size_t frame = frames[part.firstGop];
			     frame < frames[part.endGop] && source.frameTimes[frame] <= source.out; ++frame) {
				++packets;
			}
		}
	}
	packets_ = packets;

	const std::filesystem::path folder = exportFolderOf(request.manifest);
	std::filesystem::create_directories(folder);
	Leftovers leftovers;
	std::vector<std::filesystem::path> files;
	for (size_t s = 0; s < index.sources.size(); ++s) {
		const StoredSource &source = index.sources[s];
		const std::string stem = mp4Stem(request.manifest, source.name);
		for (const Mp4Part &part : parts[s]) {
			const std::filesystem::path target = freeExportPath(folder, stem, ".mp4", files);
			const std::filesystem::path partPath = partPathOf(target);
			leftovers.add(partPath);
			std::unique_ptr<VideoFileWriter> writer = mp4_();
			const std::shared_ptr<const Gop> first = readGop(request, index, s, part.firstGop);
			writer->open(partPath, *first);
			// A later part starts at its own keyframe; the first, at the in point, after
			// the frames before it that decoding needs.
			const Nanoseconds origin = std::max(source.in, first->startTime());
			bool past = false;
			for (size_t gop = part.firstGop; gop < part.endGop && !past; ++gop) {
				const std::shared_ptr<const Gop> read =
					gop == part.firstGop ? first : readGop(request, index, s, gop);
				const std::span<const PacketRecord> records = read->packets();
				for (size_t packet = 0; packet < records.size(); ++packet) {
					if (records[packet].time > source.out) {
						past = true;
						break;
					}
					writer->write(*read, packet, records[packet].time - origin);
					++packetsDone_;
				}
			}
			writer->finish();
			writer.reset();
			File(partPath, File::Mode::ReadWrite).flush();
			const std::filesystem::path path = publishedPath(target, stem, ".mp4", files);
			renameFile(partPath, path);
			leftovers.forget(partPath);
			leftovers.add(path);
			files.push_back(path);
		}
	}
	leftovers.keepAll();
	return files;
}

std::vector<std::filesystem::path> ReplayExporter::exportReplay(const ExportRequest &request, const ReplayIndex &index)
{
	uint64_t packets = 0;
	for (const StoredSource &source : index.sources) {
		packets += source.frameTimes.size();
	}
	packets_ = packets;

	const std::filesystem::path folder = exportFolderOf(request.manifest);
	std::filesystem::create_directories(folder);
	const std::string stem = utf8FromPath(request.manifest.stem());
	Leftovers leftovers;
	const std::filesystem::path target = freeExportPath(folder, stem, ".tplp", {});
	const std::filesystem::path partPath = partPathOf(target);
	File file(partPath, File::Mode::CreateNew);
	leftovers.add(partPath);

	ReplayIndex copy = index;
	std::vector<uint8_t> chunks;
	uint64_t offset = kManifestIndexOffset;
	for (size_t s = 0; s < index.sources.size(); ++s) {
		std::optional<uint32_t> run;
		for (size_t g = 0; g < index.sources[s].gops.size(); ++g) {
			const std::shared_ptr<const Gop> gop = readGop(request, index, s, g);
			StoredGop &stored = copy.sources[s].gops[g];
			chunks.clear();
			// As in a segment, a run's description comes before its first GOP, for a
			// tool that reads the chunks without the index.
			if (run != stored.run) {
				appendRunChunk(chunks, 0, *gop);
				run = stored.run;
			}
			const ChunkPlace place = appendGopChunk(chunks, 0, *gop);
			file.writeAt(offset, chunks);
			stored = {kOwnFile,   stored.packetCount, offset + place.offset,
				  place.size, place.headerCrc,    stored.run};
			offset += chunks.size();
			packetsDone_ += gop->packets().size();
		}
	}
	file.writeAt(offset, encodeManifestTail(copy, offset));
	file.writeAt(0, encodeManifestHead(copy, request.tags));
	file.flush();
	file.close();
	const std::filesystem::path path = publishedPath(target, stem, ".tplp", {});
	renameFile(partPath, path);
	leftovers.keepAll();
	return {path};
}

std::shared_ptr<const Gop> ReplayExporter::readGop(const ExportRequest &request, const ReplayIndex &index,
						   size_t source, size_t gop)
{
	if (stop_) {
		throw std::runtime_error("stopped, OBS is closing");
	}
	const std::weak_ptr<const Gop> live = source < request.live.size() && gop < request.live[source].size()
						      ? request.live[source][gop]
						      : std::weak_ptr<const Gop>();
	std::shared_ptr<const Gop> read = reader_.read(request.manifest, index.sources[source], gop, live);
	if (!read) {
		throw std::runtime_error("cannot read the pictures of " + index.sources[source].name + " from " +
					 utf8FromPath(request.manifest.parent_path()));
	}
	return read;
}

} // namespace tapeloop
