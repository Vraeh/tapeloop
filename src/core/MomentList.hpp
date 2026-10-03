// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/Clip.hpp"
#include "core/Gop.hpp"
#include "core/MediaTime.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace tapeloop {

struct MomentClip {
	// Chosen by the caller to tell sources apart; the core does not interpret it.
	std::string sourceKey;
	Clip clip;
};

// One marked instant, cut from one or more sources over the same range.
struct Moment {
	uint64_t id = 0;
	Nanoseconds start{0};
	Nanoseconds end{0};
	std::vector<MomentClip> clips;
};

struct MomentListConfig {
	// The newest moment is always kept, so zero behaves like one.
	size_t maxMoments = 0;
	// Counts each GOP once, however many moments share it.
	size_t maxBytes = 0;
};

// The moments marked so far, oldest first. Not thread-safe: the caller owns the
// synchronization.
class MomentList {
public:
	explicit MomentList(MomentListConfig config);

	MomentList(const MomentList &) = delete;
	MomentList &operator=(const MomentList &) = delete;

	// Gives the moment the next id, stores it, and drops the oldest moments while a
	// limit is exceeded. The new moment itself is always kept. Returns its id, or zero
	// for a moment without clips, which has nothing to replay and is not stored. If it
	// throws, the list is unchanged.
	uint64_t add(Moment moment);
	bool remove(uint64_t id);
	void clear();

	// Null when absent. The pointer is valid until the list next changes.
	const Moment *find(uint64_t id) const;
	std::span<const Moment> moments() const noexcept { return moments_; }
	size_t size() const noexcept { return moments_.size(); }
	size_t byteSize() const noexcept { return bytes_; }

private:
	void hold(const Moment &moment);
	// Releases the first `count` GOP references of the moment, all of them by default.
	void release(const Moment &moment, size_t count = SIZE_MAX) noexcept;
	void dropOldest();

	MomentListConfig config_;
	std::vector<Moment> moments_;
	uint64_t nextId_ = 1;
	// How many clips of the stored moments hold each GOP. The moments keep the GOPs
	// alive, so the pointers stay valid while they are keys here.
	std::unordered_map<const Gop *, size_t> gopHolders_;
	size_t bytes_ = 0;
};

} // namespace tapeloop
