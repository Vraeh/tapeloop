// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/GopReader.hpp"

#include "core/FileIo.hpp"
#include "core/ReplayWriter.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <span>
#include <utility>

namespace tapeloop {
namespace {

// The index times and cuts the replay by its frame times, so a GOP whose own times differ
// is not the one it lists, whatever its CRCs say.
bool matchesIndex(const Gop &gop, const StoredSource &source, size_t index) noexcept
{
	const std::span<const PacketRecord> packets = gop.packets();
	const size_t first = firstFrameOf(source, index);
	if (packets.size() != source.gops[index].packetCount || first > source.frameTimes.size() ||
	    packets.size() > source.frameTimes.size() - first) {
		return false;
	}
	for (size_t i = 0; i < packets.size(); ++i) {
		if (packets[i].time != source.frameTimes[first + i]) {
			return false;
		}
	}
	return true;
}

} // namespace

GopReader::GopReader(size_t cacheBytes) : limit_(cacheBytes) {}

std::shared_ptr<const Gop> GopReader::read(const std::filesystem::path &manifest, const StoredSource &source,
					   size_t gop, const std::weak_ptr<const Gop> &live)
{
	if (std::shared_ptr<const Gop> held = live.lock()) {
		++stats_.live;
		return held;
	}
	// Without a manifest, the segments' paths would be taken as relative to the working
	// folder.
	if (manifest.empty() || gop >= source.gops.size() || source.gops[gop].run >= source.runs.size()) {
		++stats_.failed;
		return nullptr;
	}
	const StoredGop &stored = source.gops[gop];
	const std::filesystem::path segment = segmentPath(manifest, source, stored.segment);
	// The header CRC tells apart chunks at one offset of segments that were written again.
	std::string key = utf8FromPath(segment);
	key += ':';
	key += std::to_string(stored.offset);
	key += ':';
	key += std::to_string(stored.headerCrc);

	if (const auto found = byKey_.find(key); found != byKey_.end()) {
		entries_.splice(entries_.begin(), entries_, found->second);
		// Another replay's index may have listed the same chunk.
		if (!matchesIndex(*found->second->gop, source, gop)) {
			++stats_.failed;
			return nullptr;
		}
		++stats_.cached;
		return found->second->gop;
	}
	std::shared_ptr<const Gop> read = readSegment(segment, stored, source.runs[stored.run]);
	if (!read || !matchesIndex(*read, source, gop)) {
		++stats_.failed;
		return nullptr;
	}
	++stats_.read;
	remember(std::move(key), read);
	return read;
}

std::shared_ptr<const Gop> GopReader::readSegment(const std::filesystem::path &segment, const StoredGop &stored,
						  const StoredRun &run)
{
	try {
		File file(segment, File::Mode::ReadOnly);
		// The size comes from the manifest, and the buffer keeps what it grows to, so it
		// is taken only when the chunk's own header, which the manifest's CRC names, has
		// it too, and the chunk lies inside the segment.
		std::array<uint8_t, kChunkHeaderSize> header{};
		if (file.readAt(stored.offset, header) != header.size() ||
		    gopChunkSizeFromHeader(header, stored.headerCrc) != stored.size) {
			return nullptr;
		}
		const uint64_t fileSize = file.size();
		if (stored.offset > fileSize || stored.size > fileSize - stored.offset) {
			return nullptr;
		}
		chunk_.resize(static_cast<size_t>(stored.size));
		if (file.readAt(stored.offset, chunk_) != chunk_.size()) {
			return nullptr;
		}
		std::shared_ptr<const Gop> gop = decodeGopChunk(chunk_, stored.headerCrc, run);
		if (!gop || gop->packets().size() != stored.packetCount) {
			return nullptr;
		}
		return gop;
	} catch (const std::exception &) {
		// A segment that is gone or cannot be read holds no GOP to play.
		return nullptr;
	}
}

void GopReader::remember(std::string key, std::shared_ptr<const Gop> gop)
{
	const size_t size = gop->byteSize();
	entries_.push_front({key, std::move(gop)});
	try {
		byKey_.emplace(std::move(key), entries_.begin());
	} catch (...) {
		entries_.pop_front();
		throw;
	}
	bytes_ += size;
	while (bytes_ > limit_ && entries_.size() > 1) {
		const Entry &oldest = entries_.back();
		bytes_ -= oldest.gop->byteSize();
		byKey_.erase(oldest.key);
		entries_.pop_back();
	}
}

void ReplayFiles::set(std::filesystem::path manifest, std::shared_ptr<const ReplayIndex> index)
{
	std::shared_ptr<const Files> files = std::make_shared<Files>(Files{std::move(manifest), std::move(index)});
	const std::lock_guard lock(mutex_);
	files_ = std::move(files);
}

std::shared_ptr<const ReplayFiles::Files> ReplayFiles::get() const
{
	const std::lock_guard lock(mutex_);
	return files_;
}

StoredGops::StoredGops(std::filesystem::path manifest, StoredSource source, std::vector<std::weak_ptr<const Gop>> live,
		       size_t cacheBytes)
	: manifest_(std::move(manifest)),
	  source_(std::move(source)),
	  live_(std::move(live)),
	  reader_(cacheBytes)
{
}

StoredGops::StoredGops(std::shared_ptr<const ReplayFiles> files, StoredSource source,
		       std::vector<std::weak_ptr<const Gop>> live, size_t cacheBytes)
	: source_(std::move(source)),
	  live_(std::move(live)),
	  reader_(cacheBytes),
	  files_(std::move(files))
{
}

std::vector<Nanoseconds> StoredGops::frameTimes() const
{
	std::vector<Nanoseconds> times;
	for (const Nanoseconds time : source_.frameTimes) {
		if (time >= source_.in && time <= source_.out) {
			times.push_back(time);
		}
	}
	return times;
}

std::shared_ptr<const Gop> StoredGops::read(size_t gop)
{
	if (files_) {
		takeFiles();
	}
	return reader_.read(written_ ? written_->manifest : manifest_, stored_ ? *stored_ : source_, gop,
			    gop < live_.size() ? live_[gop] : std::weak_ptr<const Gop>());
}

void StoredGops::takeFiles()
{
	std::shared_ptr<const ReplayFiles::Files> files = files_->get();
	if (!files) {
		return;
	}
	files_.reset();
	for (const StoredSource &source : files->index->sources) {
		if (source.key == source_.key) {
			stored_ = &source;
			written_ = std::move(files);
			return;
		}
	}
}

} // namespace tapeloop
