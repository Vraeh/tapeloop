// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/ReplaySequence.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace tapeloop {

void ReplaySequence::setEntries(std::vector<SequenceEntry> entries)
{
	std::vector<size_t> byKey;
	byKey.reserve(entries.size());
	for (size_t i = 0; i < entries.size(); ++i) {
		if (!entries[i].sourceKey.empty()) {
			byKey.push_back(i);
		}
	}
	// Ties go by place, so the first entry for a source leads its run and is the one kept.
	std::sort(byKey.begin(), byKey.end(), [&entries](size_t a, size_t b) {
		const int order = entries[a].sourceKey.compare(entries[b].sourceKey);
		return order < 0 || (order == 0 && a < b);
	});
	byKey.erase(
		std::unique(byKey.begin(), byKey.end(),
			    [&entries](size_t a, size_t b) { return entries[a].sourceKey == entries[b].sourceKey; }),
		byKey.end());

	constexpr size_t kDropped = std::numeric_limits<size_t>::max();
	std::vector<size_t> placeOf(entries.size(), kDropped);
	for (const size_t index : byKey) {
		placeOf[index] = 0;
	}
	std::vector<SequenceEntry> unique;
	unique.reserve(byKey.size());
	for (size_t i = 0; i < entries.size(); ++i) {
		if (placeOf[i] != kDropped) {
			placeOf[i] = unique.size();
			unique.push_back(std::move(entries[i]));
		}
	}
	for (size_t &index : byKey) {
		index = placeOf[index];
	}
	entries_ = std::move(unique);
	byKey_ = std::move(byKey);
}

std::optional<size_t> ReplaySequence::indexOf(std::string_view sourceKey) const noexcept
{
	const auto found =
		std::lower_bound(byKey_.begin(), byKey_.end(), sourceKey, [this](size_t index, std::string_view key) {
			return std::string_view(entries_[index].sourceKey) < key;
		});
	if (found == byKey_.end() || entries_[*found].sourceKey != sourceKey) {
		return std::nullopt;
	}
	return *found;
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
	// The keys keep their order; only the places between the two indexes shift by one.
	for (size_t &index : byKey_) {
		if (index == from) {
			index = to;
		} else if (from < to && index > from && index <= to) {
			--index;
		} else if (to < from && index >= to && index < from) {
			++index;
		}
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
