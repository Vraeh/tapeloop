// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/ReplayLibrary.hpp"

#include <algorithm>
#include <utility>

namespace tapeloop {
namespace {

// Inserts value into a sorted vector unless it is there. Returns whether it was added.
bool insertSorted(std::vector<std::string> &values, std::string_view value)
{
	const auto at = std::lower_bound(values.begin(), values.end(), value);
	if (at != values.end() && *at == value) {
		return false;
	}
	values.insert(at, std::string(value));
	return true;
}

bool eraseSorted(std::vector<std::string> &values, std::string_view value)
{
	const auto at = std::lower_bound(values.begin(), values.end(), value);
	if (at == values.end() || *at != value) {
		return false;
	}
	values.erase(at);
	return true;
}

bool containsSorted(const std::vector<std::string> &values, std::string_view value)
{
	return std::binary_search(values.begin(), values.end(), value);
}

} // namespace

ReplayLibrary::ReplayLibrary(MomentListConfig limits) : moments_(limits) {}

uint64_t ReplayLibrary::add(Moment moment)
{
	const uint64_t id = moments_.add(std::move(moment));
	if (id != 0) {
		picked_ = 0;
		forgetDropped();
	}
	return id;
}

bool ReplayLibrary::remove(uint64_t id)
{
	if (!moments_.remove(id)) {
		return false;
	}
	tagsOf_.erase(id);
	if (picked_ == id) {
		picked_ = 0;
	}
	return true;
}

void ReplayLibrary::clear()
{
	moments_.clear();
	tagsOf_.clear();
	picked_ = 0;
}

std::vector<uint64_t> ReplayLibrary::list(std::string_view tag) const
{
	std::vector<uint64_t> ids;
	ids.reserve(moments_.size());
	const std::span<const Moment> moments = moments_.moments();
	for (auto moment = moments.rbegin(); moment != moments.rend(); ++moment) {
		if (tag.empty()) {
			ids.push_back(moment->id);
		} else if (const auto found = tagsOf_.find(moment->id);
			   found != tagsOf_.end() && containsSorted(found->second, tag)) {
			ids.push_back(moment->id);
		}
	}
	return ids;
}

const Moment *ReplayLibrary::find(uint64_t id) const
{
	return moments_.find(id);
}

bool ReplayLibrary::createTag(std::string_view tag)
{
	return !tag.empty() && insertSorted(tags_, tag);
}

bool ReplayLibrary::deleteTag(std::string_view tag)
{
	if (!eraseSorted(tags_, tag)) {
		return false;
	}
	for (auto &[id, tags] : tagsOf_) {
		eraseSorted(tags, tag);
	}
	return true;
}

bool ReplayLibrary::addTag(uint64_t id, std::string_view tag)
{
	if (tag.empty() || !moments_.find(id)) {
		return false;
	}
	if (!insertSorted(tagsOf_[id], tag)) {
		return false;
	}
	insertSorted(tags_, tag);
	return true;
}

bool ReplayLibrary::removeTag(uint64_t id, std::string_view tag)
{
	const auto found = tagsOf_.find(id);
	return found != tagsOf_.end() && eraseSorted(found->second, tag);
}

std::span<const std::string> ReplayLibrary::tagsOf(uint64_t id) const
{
	const auto found = tagsOf_.find(id);
	if (found == tagsOf_.end()) {
		return {};
	}
	return found->second;
}

uint64_t ReplayLibrary::current() const noexcept
{
	if (picked_ != 0) {
		return picked_;
	}
	const std::span<const Moment> moments = moments_.moments();
	return moments.empty() ? 0 : moments.back().id;
}

bool ReplayLibrary::pick(uint64_t id)
{
	if (!moments_.find(id)) {
		return false;
	}
	picked_ = id;
	return true;
}

void ReplayLibrary::forgetDropped()
{
	std::erase_if(tagsOf_, [this](const auto &entry) { return moments_.find(entry.first) == nullptr; });
}

} // namespace tapeloop
