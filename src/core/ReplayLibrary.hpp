// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/MomentList.hpp"

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tapeloop {

// The replays captured so far. A replay is a moment: its id, the range it covers, which
// ends when it was captured, and a clip of every source that held something. The library adds tags, which exist on their own so that one can be made
// before any replay carries it, and the replay that goes on air next. Not thread-safe:
// the caller owns the synchronization.
class ReplayLibrary {
public:
	explicit ReplayLibrary(MomentListConfig limits);

	// Stores a replay and makes it the current one, dropping any pick: the replay just
	// captured is the one to show next. Zero for a moment without clips, which is not
	// stored. The oldest replays go when a limit is exceeded, as in MomentList.
	uint64_t add(Moment moment);
	bool remove(uint64_t id);
	void clear();

	// Newest first; with a tag, only the replays that carry it.
	std::vector<uint64_t> list(std::string_view tag = {}) const;
	// Null when absent. The pointer is valid until the library next changes.
	const Moment *find(uint64_t id) const;
	size_t size() const noexcept { return moments_.size(); }

	// Every tag, sorted.
	std::span<const std::string> tags() const noexcept { return tags_; }
	// False for an empty name or one that exists already.
	bool createTag(std::string_view tag);
	// Also takes it off every replay.
	bool deleteTag(std::string_view tag);
	// Creates the tag if it is new. False when there is no such replay, or the tag is
	// empty or already on it.
	bool addTag(uint64_t id, std::string_view tag);
	bool removeTag(uint64_t id, std::string_view tag);
	// Sorted; empty for an unknown replay.
	std::span<const std::string> tagsOf(uint64_t id) const;

	// The replay that "Play replay" puts on air: the one picked, or else the newest.
	// Zero when there is none.
	uint64_t current() const noexcept;
	bool pick(uint64_t id);
	void unpick() noexcept { picked_ = 0; }
	bool picked() const noexcept { return picked_ != 0; }

private:
	// Forgets the tags of replays the moment list no longer has.
	void forgetDropped();

	MomentList moments_;
	std::map<uint64_t, std::vector<std::string>> tagsOf_;
	std::vector<std::string> tags_;
	uint64_t picked_ = 0;
};

} // namespace tapeloop
