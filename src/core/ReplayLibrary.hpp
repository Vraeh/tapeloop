// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/Gop.hpp"
#include "core/ReplayFormat.hpp"
#include "core/ReplayState.hpp"
#include "core/ReplayWriter.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tapeloop {

// Tags come from files anyone can hand over as well as from the dock, so they have
// limits. A tag's length counts Unicode code points, which mean the same in every script
// where bytes do not: a Latin letter takes one byte, a Chinese character three.
inline constexpr size_t kMaxTagsPerReplay = 32;
inline constexpr size_t kMaxTags = 2000;
inline constexpr size_t kMaxTagLength = 64;

// A replay as the library keeps it: what a list of replays shows, and, for one captured
// since OBS started, where each of its frames is and the buffers' own GOPs for as long as
// the buffers hold them.
struct Replay {
	uint64_t id = 0;
	// The random id its files carry.
	ReplayId uuid{};
	std::chrono::system_clock::time_point capturedAt;
	// The name of its broadcast folder.
	std::string broadcast;
	ReplayState state = ReplayState::Writing;
	// Known once it is stored.
	std::filesystem::path manifest;
	std::vector<ReplaySource> sources;
	// For a replay captured since OBS started, its frames, and once stored where its GOPs
	// are. Null for one found on disk: readReplayIndex reads it when it is played.
	std::shared_ptr<const ReplayIndex> index;
	// Per source of the index and per GOP, the buffer's own GOP until the buffer lets go of
	// it. Empty for a replay found on disk.
	std::vector<std::vector<std::weak_ptr<const Gop>>> live;
	// For a replay captured since OBS started, until it lets go of live: where its files
	// are, set by the store's thread as soon as they are written, for the sources playing
	// it to switch to before the buffers let go of its GOPs. Null for one found on disk.
	std::shared_ptr<ReplayFiles> files;
	// Sorted.
	std::vector<std::string> tags;
	// The tags its file had that the library left out, past a limit or not valid, as the
	// file spells them and in its order, for its file to keep them. Other spellings of
	// its tags are not kept.
	std::vector<std::string> fileOnlyTags;
	// Why it was not saved.
	std::string error;
};

// The tags a replay's file is to hold: the library's, then those it left out.
std::vector<std::string> tagsToWrite(const Replay &replay);

// How many tags of replays found on disk the library left out for being past one of its
// limits.
struct LeftOutTags {
	size_t pastReplayLimit = 0;
	size_t pastLibraryLimit = 0;
};

// The replays of every broadcast, as an index: their pictures live in the buffers while
// the buffers hold them and on disk, never in the library. It adds tags, which exist on
// their own so that one can be made before any replay carries it, and the replay that
// goes on air next. Tag names are trimmed of spaces and tabs and told apart regardless of
// ASCII case: the first spelling stays. A name is not valid when it is empty, not UTF-8,
// longer than kMaxTagLength, or has a control character or one isLayoutControl names. Not
// thread-safe: the caller owns the synchronization.
class ReplayLibrary {
public:
	// A replay just captured, which the store is now writing. It becomes the current
	// one, dropping any pick: the replay just captured is the one to show next. Zero for a
	// capture without frames, which is not kept. Nothing changes when it throws.
	uint64_t addCaptured(const ReplayCapture &capture, std::string broadcast);
	// A replay a scan found, stored or damaged as the scan says, with its tags joining the
	// tag list. Zero when the library has it already, as when a scan comes upon a replay
	// captured since OBS started. Nothing changes when it throws. Its tags that are not
	// valid go to fileOnlyTags, as do, in the file's order, those past kMaxTagsPerReplay
	// on it or kMaxTags in the library; the second form adds how many were past a limit
	// to leftOut.
	uint64_t addFound(const FoundReplay &found);
	uint64_t addFound(const FoundReplay &found, LeftOutTags &leftOut);
	// What became of a replay being written. False for one that is not being written.
	// Its files take the manifest and index unless the store has set them already.
	bool stored(uint64_t id, std::filesystem::path manifest, ReplayIndex index);
	bool notSaved(uint64_t id, std::string error);
	bool remove(uint64_t id);
	void clear();
	// Lets go of what a stored or unsaved replay keeps for playing from the buffers once
	// they hold none of its GOPs: its frame index, the references and its files. A stored
	// replay's manifest has the index again. Returns how many replays it let go of.
	size_t releaseExpired();

	// Newest first, by capture time; with a tag, only the replays that carry it.
	std::vector<uint64_t> list(std::string_view tag = {}) const;
	// Null when absent. The pointer is valid until the library next changes.
	const Replay *find(uint64_t id) const;
	size_t size() const noexcept { return replays_.size(); }

	// Every tag, sorted.
	std::span<const std::string> tags() const noexcept { return tags_; }
	// False for a name that is not valid or exists already, or with kMaxTags already.
	bool createTag(std::string_view tag);
	// Also takes it off every replay.
	bool deleteTag(std::string_view tag);
	// Creates the tag if it is new. False when there is no such replay or it is damaged,
	// or the tag is not valid, already on it, or more than its file has room for, or past
	// a limit.
	bool addTag(uint64_t id, std::string_view tag);
	bool removeTag(uint64_t id, std::string_view tag);
	// Sorted; empty for an unknown replay.
	std::span<const std::string> tagsOf(uint64_t id) const;

	// The replay that "Play replay" puts on air: the one picked, or else the one captured
	// last, or else the newest that is not damaged. Zero when there is none.
	uint64_t current() const noexcept;
	// False for an unknown or damaged replay.
	bool pick(uint64_t id);
	// The replay captured last, whatever has been picked or found since; zero before the
	// first capture.
	uint64_t lastCaptured() const noexcept { return lastCaptured_; }
	void unpick() noexcept { picked_ = 0; }
	bool picked() const noexcept { return picked_ != 0; }

#ifdef TAPELOOP_COUNT_COMPARISONS
	// The names compared while looking tags up, so that a test can bound what a lookup
	// costs without timing it. Only the test project builds it.
	size_t tagComparisons() const noexcept { return tagComparisons_; }
#endif

private:
	uint64_t add(Replay replay);
	// Where name is, or would go, in names ordered ignoring ASCII case.
	size_t indexIgnoringCase(const std::vector<std::string> &names, std::string_view name) const noexcept;
	// The library's spelling of a valid name, or null when it has no such tag.
	const std::string *spellingOf(std::string_view name) const noexcept;
	// The library's spelling of the tag, or else the tag trimmed; empty when it is not
	// valid.
	std::string canonical(std::string_view tag) const;
	// A valid name new to the library.
	void insertTag(std::string name);
#ifdef TAPELOOP_COUNT_COMPARISONS
	void countComparison() const noexcept { ++tagComparisons_; }
#else
	void countComparison() const noexcept {}
#endif

	std::map<uint64_t, Replay> replays_;
	std::vector<std::string> tags_;
	// The names of tags_ ordered ignoring ASCII case, to find a name's spelling.
	std::vector<std::string> tagIndex_;
	uint64_t nextId_ = 1;
	uint64_t picked_ = 0;
	uint64_t lastCaptured_ = 0;
#ifdef TAPELOOP_COUNT_COMPARISONS
	mutable size_t tagComparisons_ = 0;
#endif
};

} // namespace tapeloop
