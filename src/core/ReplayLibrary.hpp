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
	// Sorted.
	std::vector<std::string> tags;
	// Why it was not saved.
	std::string error;
};

// The replays of every broadcast, as an index: their pictures live in the buffers while
// the buffers hold them and on disk, never in the library. It adds tags, which exist on
// their own so that one can be made before any replay carries it, and the replay that
// goes on air next. Tag names are trimmed, never empty nor with control characters, and
// told apart regardless of ASCII case: the first spelling stays. Not thread-safe: the
// caller owns the synchronization.
class ReplayLibrary {
public:
	// A replay just captured, which the store is now writing. It becomes the current
	// one, dropping any pick: the replay just captured is the one to show next. Zero for a
	// capture without frames, which is not kept. Nothing changes when it throws.
	uint64_t addCaptured(const ReplayCapture &capture, std::string broadcast);
	// A replay a scan found, stored or damaged as the scan says, with its tags joining the
	// tag list. Zero when the library has it already, as when a scan comes upon a replay
	// captured since OBS started. Nothing changes when it throws.
	uint64_t addFound(const FoundReplay &found);
	// What became of a replay being written. False for one that is not being written.
	bool stored(uint64_t id, std::filesystem::path manifest, ReplayIndex index);
	bool notSaved(uint64_t id, std::string error);
	bool remove(uint64_t id);
	void clear();
	// Lets go of what a stored or unsaved replay keeps for playing from the buffers once
	// they hold none of its GOPs: its frame index and the references. A stored replay's
	// manifest has the index again. Returns how many replays it let go of.
	size_t releaseExpired();

	// Newest first, by capture time; with a tag, only the replays that carry it.
	std::vector<uint64_t> list(std::string_view tag = {}) const;
	// Null when absent. The pointer is valid until the library next changes.
	const Replay *find(uint64_t id) const;
	size_t size() const noexcept { return replays_.size(); }

	// Every tag, sorted.
	std::span<const std::string> tags() const noexcept { return tags_; }
	// False for a name that is not valid or exists already.
	bool createTag(std::string_view tag);
	// Also takes it off every replay.
	bool deleteTag(std::string_view tag);
	// Creates the tag if it is new. False when there is no such replay or it is damaged,
	// or the tag is not valid, already on it, or more than its file has room for.
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

private:
	uint64_t add(Replay replay);
	// The tag's spelling in tags_, see canonicalIn.
	std::string canonical(std::string_view tag) const;

	std::map<uint64_t, Replay> replays_;
	std::vector<std::string> tags_;
	uint64_t nextId_ = 1;
	uint64_t picked_ = 0;
	uint64_t lastCaptured_ = 0;
};

} // namespace tapeloop
