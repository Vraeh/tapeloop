// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/Clip.hpp"
#include "core/FileIo.hpp"
#include "core/MediaTime.hpp"
#include "core/ReplayFormat.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tapeloop {

// A random id for a replay or a broadcast.
ReplayId newReplayId();

// One source of a capture. The clip keeps its GOPs alive until they are written.
struct CaptureSource {
	// Tells the sources of a broadcast apart from one capture to the next: the OBS source
	// UUID.
	std::string key;
	// As the source was named at the capture, for showing.
	std::string name;
	Clip clip;
};

struct ReplayCapture {
	ReplayId id{};
	// The broadcast folder, created if it does not exist.
	std::filesystem::path folder;
	// The manifest's file name without its extension. When a replay of the folder has it
	// already, " (2)", " (3)" and so on are added.
	std::string stem;
	// Unix time in nanoseconds, and the OBS clock the packets are stamped with.
	int64_t capturedAtUtc = 0;
	Nanoseconds capturedAtClock{0};
	Nanoseconds start{0};
	Nanoseconds end{0};
	// Sources with an empty clip are left out.
	std::vector<CaptureSource> sources;
	std::vector<std::string> tags;
};

// The index of a capture before it is written: everything but where its GOPs are, which
// only writing tells. Sources with an empty clip are left out.
ReplayIndex indexOf(const ReplayCapture &capture);

struct WrittenReplay {
	std::filesystem::path manifest;
	ReplayIndex index;
	// What the capture appended to segments, and how many of its GOPs were there already.
	uint64_t bytesAppended = 0;
	size_t gopsAppended = 0;
	size_t gopsShared = 0;
};

struct ReplayWriterConfig {
	// A segment rolls over before a chunk would take it past this size; a larger chunk
	// gets a segment of its own.
	uint64_t segmentBytes = uint64_t{1} << 30;
	// Chunks are gathered into writes of about this size.
	size_t writeBytes = size_t{8} << 20;
};

// Writes captured replays into their broadcast folder: per source, the GOPs no segment of
// the folder holds yet are appended to the source's open segment, every segment written
// to is flushed, and only then is the manifest written as `<stem>.tplp.part`, flushed and
// renamed. A crash loses at most the capture being written. GOPs are known by their key,
// so one shared by several replays is stored once. A writer appends only to segments it
// created: one from before OBS restarted cannot hold a GOP of the buffers, which start
// empty. Not thread-safe; one thread does all the writing.
class ReplayWriter {
public:
	explicit ReplayWriter(ReplayWriterConfig config = {});

	// Throws std::invalid_argument for a capture with no frames, and std::system_error
	// or std::length_error when it cannot be written; the replay is then not saved, and
	// the later captures of its sources write their GOPs again, into new segments.
	WrittenReplay write(const ReplayCapture &capture);

	// Writes the tags into the manifest's older tag slot, so that a torn write leaves the
	// slot with the previous ones (see nextTagWrite). Throws std::system_error when the
	// manifest cannot be written, std::invalid_argument when it is not an intact one, and
	// std::length_error when the tags do not fit their slot.
	void writeTags(const std::filesystem::path &manifest, std::span<const std::string> tags);

private:
	struct Place {
		uint32_t segment = 0;
		uint64_t offset = 0;
		uint64_t size = 0;
		uint32_t headerCrc = 0;
	};

	struct GopKeyHash {
		size_t operator()(const GopKey &key) const noexcept;
	};

	struct Segment {
		File file;
		uint32_t sequence = 0;
		uint64_t end = 0;
		uint32_t chunks = 0;
		bool unflushed = false;
		// The runs with a RUN chunk in this segment: their key and frame duration.
		std::vector<std::pair<uint32_t, Nanoseconds>> runs;
	};

	struct SourceState {
		uint32_t nextSequence = 1;
		std::optional<Segment> open;
		std::unordered_map<GopKey, Place, GopKeyHash> written;
	};

	void prepareFolder();
	SourceState &stateOf(const std::string &key);
	// Fills in where the GOPs of the source landed.
	void writeSource(const CaptureSource &source, StoredSource &stored, WrittenReplay &written);
	Place append(SourceState &state, const CaptureSource &source, const Gop &gop, const GopKey &key);
	void writeBatch(SourceState &state);
	Segment &openSegment(SourceState &state, const CaptureSource &source);
	void retire(SourceState &state);
	void flushSegments();
	std::filesystem::path publishManifest(const std::string &stem, std::span<const uint8_t> bytes);

	ReplayWriterConfig config_;
	std::filesystem::path folder_;
	ReplayId broadcast_{};
	std::map<std::string, SourceState> sources_;
	// Segments rolled over during the capture being written, still to be flushed.
	std::vector<File> retired_;
	bool newSegments_ = false;
	// The chunks gathered for the next write, and the GOPs they hold.
	std::vector<uint8_t> batch_;
	std::vector<std::pair<GopKey, Place>> batchGops_;
	uint64_t appended_ = 0;
};

struct ReplaySource {
	std::string key;
	std::string name;

	bool operator==(const ReplaySource &) const = default;
};

// A replay found in a broadcast folder: what a list of replays shows, without the frame
// times, which only playing it needs (see readReplayIndex).
struct FoundReplay {
	std::filesystem::path manifest;
	// The folder's name.
	std::string broadcast;
	// False for a manifest that is damaged or refers to a segment that is gone; what
	// follows is then empty.
	bool intact = false;
	ReplayId id{};
	// Unix time in nanoseconds.
	int64_t capturedAtUtc = 0;
	std::vector<ReplaySource> sources;
	std::vector<std::string> tags;
};

struct ReplayScan {
	std::vector<FoundReplay> replays;
	// Manifests a crash left half written, which the scan deleted.
	std::vector<std::filesystem::path> removed;
	// What could not be read or deleted.
	std::vector<std::string> errors;
};

// Reads every broadcast folder under base, in name order; reports what it cannot read
// instead of throwing. Nothing for a base that does not exist.
ReplayScan scanReplays(const std::filesystem::path &base);

// The index of the replay whose manifest this is. Nothing when it cannot be read or is
// not an intact manifest.
std::optional<ReplayIndex> readReplayIndex(const std::filesystem::path &manifest);

// Where the segment of a source that a manifest refers to lives.
std::filesystem::path segmentPath(const std::filesystem::path &manifest, const StoredSource &source, uint32_t segment);

} // namespace tapeloop
