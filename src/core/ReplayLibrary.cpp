// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/ReplayLibrary.hpp"

#include <algorithm>
#include <ranges>
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

char lower(char c) noexcept
{
	return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool sameIgnoringCase(std::string_view a, std::string_view b) noexcept
{
	return std::ranges::equal(a, b, [](char x, char y) { return lower(x) == lower(y); });
}

} // namespace

ReplayLibrary::ReplayLibrary(MomentListConfig limits) : moments_(limits) {}

uint64_t ReplayLibrary::add(Moment moment, std::chrono::system_clock::time_point capturedAt)
{
	// The entry is made before the moment is stored and given its id after, which
	// cannot fail, so a replay is never stored without it.
	auto entry = info_.extract(info_.try_emplace(0).first);
	entry.mapped().capturedAt = capturedAt;
	const uint64_t id = moments_.add(std::move(moment));
	if (id != 0) {
		entry.key() = id;
		info_.insert(std::move(entry));
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
	info_.erase(id);
	if (picked_ == id) {
		picked_ = 0;
	}
	return true;
}

void ReplayLibrary::clear()
{
	moments_.clear();
	info_.clear();
	picked_ = 0;
}

std::vector<uint64_t> ReplayLibrary::list(std::string_view tag) const
{
	const std::string name = tag.empty() ? std::string() : canonical(tag);
	const auto carries = [&](uint64_t id) {
		const auto found = info_.find(id);
		return found != info_.end() && containsSorted(found->second.tags, name);
	};
	std::vector<uint64_t> ids;
	ids.reserve(moments_.size());
	for (const Moment &moment : std::views::reverse(moments_.moments())) {
		if (tag.empty() || carries(moment.id)) {
			ids.push_back(moment.id);
		}
	}
	return ids;
}

const Moment *ReplayLibrary::find(uint64_t id) const
{
	return moments_.find(id);
}

std::chrono::system_clock::time_point ReplayLibrary::capturedAt(uint64_t id) const
{
	const auto found = info_.find(id);
	return found != info_.end() ? found->second.capturedAt : std::chrono::system_clock::time_point{};
}

bool ReplayLibrary::createTag(std::string_view tag)
{
	const std::string name = canonical(tag);
	return !name.empty() && insertSorted(tags_, name);
}

bool ReplayLibrary::deleteTag(std::string_view tag)
{
	const std::string name = canonical(tag);
	if (!eraseSorted(tags_, name)) {
		return false;
	}
	for (auto &[id, info] : info_) {
		eraseSorted(info.tags, name);
	}
	return true;
}

bool ReplayLibrary::addTag(uint64_t id, std::string_view tag)
{
	const std::string name = canonical(tag);
	if (name.empty() || !moments_.find(id)) {
		return false;
	}
	// The tag list first: should the replay's own list then fail to grow, the tag is at
	// least known.
	insertSorted(tags_, name);
	return insertSorted(info_[id].tags, name);
}

bool ReplayLibrary::removeTag(uint64_t id, std::string_view tag)
{
	const auto found = info_.find(id);
	return found != info_.end() && eraseSorted(found->second.tags, canonical(tag));
}

std::span<const std::string> ReplayLibrary::tagsOf(uint64_t id) const
{
	const auto found = info_.find(id);
	if (found == info_.end()) {
		return {};
	}
	return found->second.tags;
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

std::string ReplayLibrary::canonical(std::string_view tag) const
{
	const auto blank = [](char c) {
		return c == ' ' || c == '\t';
	};
	while (!tag.empty() && blank(tag.front())) {
		tag.remove_prefix(1);
	}
	while (!tag.empty() && blank(tag.back())) {
		tag.remove_suffix(1);
	}
	const bool control = std::ranges::any_of(tag, [](char c) {
		const auto code = static_cast<unsigned char>(c);
		return code < 0x20 || code == 0x7f;
	});
	if (tag.empty() || control) {
		return {};
	}
	const auto known =
		std::ranges::find_if(tags_, [tag](const std::string &name) { return sameIgnoringCase(name, tag); });
	return known != tags_.end() ? *known : std::string(tag);
}

void ReplayLibrary::forgetDropped()
{
	std::erase_if(info_, [this](const auto &entry) { return moments_.find(entry.first) == nullptr; });
}

} // namespace tapeloop
