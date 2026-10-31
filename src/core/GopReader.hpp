// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/Gop.hpp"
#include "core/GopSource.hpp"
#include "core/ReplayFormat.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace tapeloop {

inline constexpr size_t kDefaultGopCacheBytes = size_t{64} << 20;

// Where the GOPs of a stored replay come from: the buffer, while it still holds the GOP,
// and otherwise the segment on disk, through a small cache of the GOPs read last.
// Not thread-safe: whoever plays a replay owns one.
class GopReader {
public:
	struct Stats {
		uint64_t live = 0;
		uint64_t cached = 0;
		uint64_t read = 0;
		uint64_t failed = 0;
	};

	// The cache holds at most cacheBytes of GOPs, but always the last one read.
	explicit GopReader(size_t cacheBytes = kDefaultGopCacheBytes);

	// The GOP at index gop of the source in the replay whose manifest this is: live when
	// it has not expired, else from the cache or the segment. Null when the segment
	// cannot be read or does not hold that GOP intact.
	std::shared_ptr<const Gop> read(const std::filesystem::path &manifest, const StoredSource &source, size_t gop,
					const std::weak_ptr<const Gop> &live = {});

	size_t cachedBytes() const noexcept { return bytes_; }
	const Stats &stats() const noexcept { return stats_; }

private:
	struct Entry {
		std::string key;
		std::shared_ptr<const Gop> gop;
	};

	std::shared_ptr<const Gop> readSegment(const std::filesystem::path &segment, const StoredGop &stored,
					       const StoredRun &run);
	void remember(std::string key, std::shared_ptr<const Gop> gop);

	size_t limit_;
	size_t bytes_ = 0;
	// The GOP read last first.
	std::list<Entry> entries_;
	std::unordered_map<std::string, std::list<Entry>::iterator> byKey_;
	// Kept from one read to the next, so reading does not allocate for it.
	std::vector<uint8_t> chunk_;
	Stats stats_;
};

// The GOPs of one source of a stored replay, for a decode planner: the buffer's own while
// it holds them, else read through a reader of its own.
class StoredGops final : public GopSource {
public:
	// live holds the buffer's GOPs by index, and may be shorter than the source's GOPs.
	StoredGops(std::filesystem::path manifest, StoredSource source, std::vector<std::weak_ptr<const Gop>> live,
		   size_t cacheBytes = kDefaultGopCacheBytes);

	size_t gopCount() const noexcept override { return source_.gops.size(); }
	size_t packetCount(size_t gop) const noexcept override { return source_.gops[gop].packetCount; }
	FrameLocation locate(Nanoseconds t) const noexcept override { return tapeloop::locate(source_, t); }
	std::vector<Nanoseconds> frameTimes() const override;
	std::shared_ptr<const Gop> read(size_t gop) override;

	const GopReader::Stats &stats() const noexcept { return reader_.stats(); }

private:
	std::filesystem::path manifest_;
	StoredSource source_;
	std::vector<std::weak_ptr<const Gop>> live_;
	GopReader reader_;
};

} // namespace tapeloop
