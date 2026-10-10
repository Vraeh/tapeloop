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

std::chrono::system_clock::time_point fromUnixNanoseconds(int64_t nanoseconds)
{
	return std::chrono::system_clock::time_point(
		std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::nanoseconds(nanoseconds)));
}

// The spelling a name has in known when it matches one ignoring ASCII case, else the
// name trimmed; empty for a name that is not valid.
std::string canonicalIn(const std::vector<std::string> &known, std::string_view tag)
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
	const auto found =
		std::ranges::find_if(known, [tag](const std::string &name) { return sameIgnoringCase(name, tag); });
	return found != known.end() ? *found : std::string(tag);
}

// Newer first: by capture time, then by id, which follows the order replays came in.
bool newer(const Replay &a, const Replay &b) noexcept
{
	return a.capturedAt != b.capturedAt ? a.capturedAt > b.capturedAt : a.id > b.id;
}

} // namespace

uint64_t ReplayLibrary::addCaptured(const ReplayCapture &capture, std::string broadcast)
{
	ReplayIndex index = indexOf(capture);
	if (index.sources.empty()) {
		return 0;
	}
	Replay replay;
	replay.uuid = capture.id;
	replay.capturedAt = fromUnixNanoseconds(capture.capturedAtUtc);
	replay.broadcast = std::move(broadcast);
	replay.state = ReplayState::Writing;
	for (const CaptureSource &source : capture.sources) {
		if (source.clip.empty()) {
			continue;
		}
		std::vector<std::weak_ptr<const Gop>> gops(source.clip.gops().begin(), source.clip.gops().end());
		replay.live.push_back(std::move(gops));
	}
	for (const StoredSource &source : index.sources) {
		replay.sources.push_back({source.key, source.name});
	}
	replay.index = std::make_shared<const ReplayIndex>(std::move(index));
	const uint64_t id = add(std::move(replay));
	picked_ = 0;
	lastCaptured_ = id;
	return id;
}

uint64_t ReplayLibrary::addFound(const FoundReplay &found)
{
	const bool known = std::ranges::any_of(replays_, [&](const auto &entry) {
		const Replay &replay = entry.second;
		return found.intact ? replay.uuid == found.id && replay.state != ReplayState::Damaged
				    : replay.manifest == found.manifest;
	});
	if (known) {
		return 0;
	}
	Replay replay;
	replay.state = found.intact ? ReplayState::Stored : ReplayState::Damaged;
	replay.uuid = found.id;
	replay.capturedAt = fromUnixNanoseconds(found.capturedAtUtc);
	replay.broadcast = found.broadcast;
	replay.manifest = found.manifest;
	replay.sources = found.sources;
	std::vector<std::string> tags = tags_;
	for (const std::string &tag : found.tags) {
		const std::string name = canonicalIn(tags, tag);
		if (!name.empty()) {
			insertSorted(replay.tags, name);
			insertSorted(tags, name);
		}
	}
	const uint64_t id = add(std::move(replay));
	tags_.swap(tags);
	return id;
}

bool ReplayLibrary::stored(uint64_t id, std::filesystem::path manifest, ReplayIndex index)
{
	const auto found = replays_.find(id);
	if (found == replays_.end() || found->second.state != ReplayState::Writing) {
		return false;
	}
	Replay &replay = found->second;
	replay.index = std::make_shared<const ReplayIndex>(std::move(index));
	replay.manifest = std::move(manifest);
	replay.state = ReplayState::Stored;
	return true;
}

bool ReplayLibrary::notSaved(uint64_t id, std::string error)
{
	const auto found = replays_.find(id);
	if (found == replays_.end() || found->second.state != ReplayState::Writing) {
		return false;
	}
	found->second.error = std::move(error);
	found->second.state = ReplayState::NotSaved;
	return true;
}

bool ReplayLibrary::remove(uint64_t id)
{
	if (replays_.erase(id) == 0) {
		return false;
	}
	if (picked_ == id) {
		picked_ = 0;
	}
	return true;
}

void ReplayLibrary::clear()
{
	replays_.clear();
	picked_ = 0;
}

size_t ReplayLibrary::releaseExpired()
{
	size_t released = 0;
	for (auto &[id, replay] : replays_) {
		if (replay.live.empty() || replay.state == ReplayState::Writing) {
			continue;
		}
		// The GOP still being encoded at the capture expires first, so a replay the
		// buffers still hold shows it a GOP or two from the end.
		const bool held = std::ranges::any_of(replay.live, [](const auto &gops) {
			return std::any_of(gops.rbegin(), gops.rend(), [](const auto &gop) { return !gop.expired(); });
		});
		if (!held) {
			replay.index.reset();
			replay.live = {};
			++released;
		}
	}
	return released;
}

std::vector<uint64_t> ReplayLibrary::list(std::string_view tag) const
{
	const std::string name = tag.empty() ? std::string() : canonical(tag);
	std::vector<const Replay *> shown;
	shown.reserve(replays_.size());
	for (const auto &[id, replay] : replays_) {
		if (tag.empty() || containsSorted(replay.tags, name)) {
			shown.push_back(&replay);
		}
	}
	std::sort(shown.begin(), shown.end(), [](const Replay *a, const Replay *b) { return newer(*a, *b); });
	std::vector<uint64_t> ids;
	ids.reserve(shown.size());
	for (const Replay *replay : shown) {
		ids.push_back(replay->id);
	}
	return ids;
}

const Replay *ReplayLibrary::find(uint64_t id) const
{
	const auto found = replays_.find(id);
	return found != replays_.end() ? &found->second : nullptr;
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
	for (auto &[id, replay] : replays_) {
		eraseSorted(replay.tags, name);
	}
	return true;
}

bool ReplayLibrary::addTag(uint64_t id, std::string_view tag)
{
	const std::string name = canonical(tag);
	const auto found = replays_.find(id);
	if (name.empty() || found == replays_.end() || found->second.state == ReplayState::Damaged) {
		return false;
	}
	std::vector<std::string> tags = found->second.tags;
	if (!insertSorted(tags, name) || !tagsFit(tags)) {
		return false;
	}
	// The tag list first: should the replay's own list then fail to grow, the tag is at
	// least known.
	insertSorted(tags_, name);
	return insertSorted(found->second.tags, name);
}

bool ReplayLibrary::removeTag(uint64_t id, std::string_view tag)
{
	const auto found = replays_.find(id);
	return found != replays_.end() && eraseSorted(found->second.tags, canonical(tag));
}

std::span<const std::string> ReplayLibrary::tagsOf(uint64_t id) const
{
	const auto found = replays_.find(id);
	if (found == replays_.end()) {
		return {};
	}
	return found->second.tags;
}

uint64_t ReplayLibrary::current() const noexcept
{
	if (picked_ != 0) {
		return picked_;
	}
	// The last capture wins over a replay found on disk with a later time, from a clock
	// that was ahead.
	if (replays_.contains(lastCaptured_)) {
		return lastCaptured_;
	}
	const Replay *newest = nullptr;
	for (const auto &[id, replay] : replays_) {
		if (replay.state != ReplayState::Damaged && (!newest || newer(replay, *newest))) {
			newest = &replay;
		}
	}
	return newest ? newest->id : 0;
}

bool ReplayLibrary::pick(uint64_t id)
{
	const auto found = replays_.find(id);
	if (found == replays_.end() || found->second.state == ReplayState::Damaged) {
		return false;
	}
	picked_ = id;
	return true;
}

uint64_t ReplayLibrary::add(Replay replay)
{
	const uint64_t id = nextId_;
	replay.id = id;
	replays_.emplace(id, std::move(replay));
	++nextId_;
	return id;
}

std::string ReplayLibrary::canonical(std::string_view tag) const
{
	return canonicalIn(tags_, tag);
}

} // namespace tapeloop
