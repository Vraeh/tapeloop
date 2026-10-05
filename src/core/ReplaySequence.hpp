// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tapeloop {

struct SequenceEntry {
	// Chosen by the caller to tell sources apart, as for MomentClip.
	std::string sourceKey;
	bool shown = true;
};

// The sources a replay shows, in the order it shows them. A hidden source keeps its place,
// so the order survives hiding it for a while.
class ReplaySequence {
public:
	std::span<const SequenceEntry> entries() const noexcept { return entries_; }
	// A key listed twice keeps its first place and flag; an empty key is left out.
	void setEntries(std::vector<SequenceEntry> entries);
	std::optional<size_t> indexOf(std::string_view sourceKey) const noexcept;
	// Moves an entry so that it ends up at index `to`. False for an index out of range.
	bool move(size_t from, size_t to) noexcept;
	// False for an unknown source.
	bool setShown(std::string_view sourceKey, bool shown) noexcept;
	// The first shown entry at or after `from` whose source is in `available`, which is
	// sorted.
	std::optional<size_t> nextShown(size_t from, std::span<const std::string> available) const noexcept;

private:
	std::vector<SequenceEntry> entries_;
};

} // namespace tapeloop
