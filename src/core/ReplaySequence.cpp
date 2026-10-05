// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/ReplaySequence.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

namespace tapeloop {

void ReplaySequence::setEntries(std::vector<SequenceEntry> entries)
{
	std::vector<SequenceEntry> unique;
	unique.reserve(entries.size());
	for (SequenceEntry &entry : entries) {
		const bool seen = std::any_of(unique.begin(), unique.end(), [&entry](const SequenceEntry &kept) {
			return kept.sourceKey == entry.sourceKey;
		});
		if (!seen) {
			unique.push_back(std::move(entry));
		}
	}
	entries_ = std::move(unique);
}

std::optional<size_t> ReplaySequence::indexOf(std::string_view sourceKey) const noexcept
{
	const auto found = std::find_if(entries_.begin(), entries_.end(), [sourceKey](const SequenceEntry &entry) {
		return entry.sourceKey == sourceKey;
	});
	if (found == entries_.end()) {
		return std::nullopt;
	}
	return static_cast<size_t>(std::distance(entries_.begin(), found));
}

bool ReplaySequence::move(size_t from, size_t to) noexcept
{
	if (from >= entries_.size() || to >= entries_.size()) {
		return false;
	}
	const auto first = entries_.begin();
	if (from < to) {
		std::rotate(first + static_cast<ptrdiff_t>(from), first + static_cast<ptrdiff_t>(from) + 1,
			    first + static_cast<ptrdiff_t>(to) + 1);
	} else if (to < from) {
		std::rotate(first + static_cast<ptrdiff_t>(to), first + static_cast<ptrdiff_t>(from),
			    first + static_cast<ptrdiff_t>(from) + 1);
	}
	return true;
}

bool ReplaySequence::setShown(std::string_view sourceKey, bool shown) noexcept
{
	const std::optional<size_t> index = indexOf(sourceKey);
	if (!index) {
		return false;
	}
	entries_[*index].shown = shown;
	return true;
}

std::optional<size_t> ReplaySequence::nextShown(size_t from, std::span<const std::string> available) const noexcept
{
	for (size_t i = from; i < entries_.size(); ++i) {
		const SequenceEntry &entry = entries_[i];
		if (entry.shown && std::binary_search(available.begin(), available.end(), entry.sourceKey)) {
			return i;
		}
	}
	return std::nullopt;
}

} // namespace tapeloop
