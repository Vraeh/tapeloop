// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/GopReader.hpp"

#include "core/FileIo.hpp"
#include "core/ReplayWriter.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <utility>

namespace tapeloop {

GopReader::GopReader(size_t cacheBytes) : limit_(cacheBytes) {}

std::shared_ptr<const Gop> GopReader::read(const std::filesystem::path &manifest, const StoredSource &source,
					   size_t gop, const std::weak_ptr<const Gop> &live)
{
	if (std::shared_ptr<const Gop> held = live.lock()) {
		++stats_.live;
		return held;
	}
	if (gop >= source.gops.size() || source.gops[gop].run >= source.runs.size()) {
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
		++stats_.cached;
		return found->second->gop;
	}
	std::shared_ptr<const Gop> read = readSegment(segment, stored, source.runs[stored.run]);
	if (!read) {
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

} // namespace tapeloop
