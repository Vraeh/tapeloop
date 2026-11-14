// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/ReplayLibrary.hpp"

#include "core/Text.hpp"

#include <algorithm>
#include <memory>
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

bool lessIgnoringCase(std::string_view a, std::string_view b) noexcept
{
	return std::ranges::lexicographical_compare(a, b, [](char x, char y) {
		return static_cast<unsigned char>(lower(x)) < static_cast<unsigned char>(lower(y));
	});
}

// Room for extra more names, growing as push_back would, so that inserting them throws
// nothing.
void makeRoom(std::vector<std::string> &names, size_t extra)
{
	const size_t needed = names.size() + extra;
	if (names.capacity() < needed) {
		names.reserve(std::max(needed, names.capacity() * 2));
	}
}

std::chrono::system_clock::time_point fromUnixNanoseconds(int64_t nanoseconds)
{
	return std::chrono::system_clock::time_point(
		std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::nanoseconds(nanoseconds)));
}

// The tag trimmed, or empty when it is not valid.
std::string_view validTag(std::string_view tag) noexcept
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
	size_t length = 0;
	for (size_t at = 0; at < tag.size();) {
		const size_t bytes = utf8SequenceLength(tag, at);
		if (bytes == 0) {
			return {};
		}
		const char32_t code = utf8CodePoint(tag.substr(at, bytes));
		if (code < 0x20 || code == 0x7F || isLayoutControl(code) || ++length > kMaxTagLength) {
			return {};
		}
		at += bytes;
	}
	return tag;
}

// Newer first: by capture time, then by id, which follows the order replays came in.
bool newer(const Replay &a, const Replay &b) noexcept
{
	return a.capturedAt != b.capturedAt ? a.capturedAt > b.capturedAt : a.id > b.id;
}

} // namespace

std::vector<std::string> tagsToWrite(const Replay &replay)
{
	std::vector<std::string> tags;
	tags.reserve(replay.tags.size() + replay.fileOnlyTags.size());
	tags.insert(tags.end(), replay.tags.begin(), replay.tags.end());
	tags.insert(tags.end(), replay.fileOnlyTags.begin(), replay.fileOnlyTags.end());
	return tags;
}

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
	replay.files = std::make_shared<ReplayFiles>();
	const uint64_t id = add(std::move(replay));
	picked_ = 0;
	lastCaptured_ = id;
	return id;
}

uint64_t ReplayLibrary::addFound(const FoundReplay &found)
{
	LeftOutTags leftOut;
	return addFound(found, leftOut);
}

uint64_t ReplayLibrary::addFound(const FoundReplay &found, LeftOutTags &leftOut)
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
	// The names new to the library, ordered as tagIndex_.
	std::vector<std::string> added;
	LeftOutTags past;
	for (const std::string &tag : found.tags) {
		const std::string_view name = validTag(tag);
		if (name.empty()) {
			replay.fileOnlyTags.push_back(tag);
			continue;
		}
		const std::string *spelling = spellingOf(name);
		size_t newAt = 0;
		if (!spelling) {
			newAt = indexIgnoringCase(added, name);
			if (newAt < added.size() && sameIgnoringCase(added[newAt], name)) {
				spelling = &added[newAt];
			}
		}
		const std::string_view kept = spelling ? std::string_view(*spelling) : name;
		if (containsSorted(replay.tags, kept)) {
			continue;
		}
		if (replay.tags.size() >= kMaxTagsPerReplay) {
			++past.pastReplayLimit;
			replay.fileOnlyTags.push_back(tag);
			continue;
		}
		if (!spelling && tags_.size() + added.size() >= kMaxTags) {
			++past.pastLibraryLimit;
			replay.fileOnlyTags.push_back(tag);
			continue;
		}
		insertSorted(replay.tags, kept);
		if (!spelling) {
			added.insert(added.begin() + static_cast<std::ptrdiff_t>(newAt), std::string(name));
		}
	}
	// Whatever can throw comes before the replay goes in.
	std::vector<std::string> sorted = added;
	makeRoom(tags_, added.size());
	makeRoom(tagIndex_, added.size());
	const uint64_t id = add(std::move(replay));
	for (std::string &name : sorted) {
		tags_.insert(std::lower_bound(tags_.begin(), tags_.end(), name), std::move(name));
	}
	for (std::string &name : added) {
		const size_t at = indexIgnoringCase(tagIndex_, name);
		tagIndex_.insert(tagIndex_.begin() + static_cast<std::ptrdiff_t>(at), std::move(name));
	}
	leftOut.pastReplayLimit += past.pastReplayLimit;
	leftOut.pastLibraryLimit += past.pastLibraryLimit;
	return id;
}

bool ReplayLibrary::stored(uint64_t id, std::filesystem::path manifest, ReplayIndex index)
{
	const auto found = replays_.find(id);
	if (found == replays_.end() || found->second.state != ReplayState::Writing) {
		return false;
	}
	Replay &replay = found->second;
	const std::shared_ptr<const ReplayFiles::Files> files = replay.files ? replay.files->get() : nullptr;
	std::shared_ptr<const ReplayIndex> shared;
	if (files) {
		// The store set them with the same index, which need not be kept twice.
		shared = files->index;
	} else {
		shared = std::make_shared<const ReplayIndex>(std::move(index));
		if (replay.files) {
			// The store could not set them.
			replay.files->set(manifest, shared);
		}
	}
	replay.index = std::move(shared);
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
			replay.files.reset();
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
	const std::string_view name = validTag(tag);
	if (name.empty() || spellingOf(name) || tags_.size() >= kMaxTags) {
		return false;
	}
	insertTag(std::string(name));
	return true;
}

bool ReplayLibrary::deleteTag(std::string_view tag)
{
	const std::string name = canonical(tag);
	if (!eraseSorted(tags_, name)) {
		return false;
	}
	tagIndex_.erase(tagIndex_.begin() + static_cast<std::ptrdiff_t>(indexIgnoringCase(tagIndex_, name)));
	for (auto &[id, replay] : replays_) {
		eraseSorted(replay.tags, name);
	}
	return true;
}

bool ReplayLibrary::addTag(uint64_t id, std::string_view tag)
{
	const std::string_view name = validTag(tag);
	const auto found = replays_.find(id);
	if (name.empty() || found == replays_.end() || found->second.state == ReplayState::Damaged) {
		return false;
	}
	Replay &replay = found->second;
	const std::string *spelling = spellingOf(name);
	if (replay.tags.size() >= kMaxTagsPerReplay || (!spelling && tags_.size() >= kMaxTags)) {
		return false;
	}
	const std::string kept(spelling ? std::string_view(*spelling) : name);
	std::vector<std::string> tags = replay.tags;
	if (!insertSorted(tags, kept)) {
		return false;
	}
	std::vector<std::string> written = tagsToWrite(replay);
	written.push_back(kept);
	if (!tagsFit(written)) {
		return false;
	}
	if (!spelling) {
		insertTag(kept);
	}
	replay.tags.swap(tags);
	return true;
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

size_t ReplayLibrary::indexIgnoringCase(const std::vector<std::string> &names, std::string_view name) const noexcept
{
	const auto at = std::lower_bound(names.begin(), names.end(), name,
					 [this](const std::string &known, std::string_view sought) {
						 countComparison();
						 return lessIgnoringCase(known, sought);
					 });
	return static_cast<size_t>(at - names.begin());
}

const std::string *ReplayLibrary::spellingOf(std::string_view name) const noexcept
{
	const size_t at = indexIgnoringCase(tagIndex_, name);
	return at < tagIndex_.size() && sameIgnoringCase(tagIndex_[at], name) ? &tagIndex_[at] : nullptr;
}

std::string ReplayLibrary::canonical(std::string_view tag) const
{
	const std::string_view name = validTag(tag);
	if (name.empty()) {
		return {};
	}
	const std::string *spelling = spellingOf(name);
	return std::string(spelling ? std::string_view(*spelling) : name);
}

void ReplayLibrary::insertTag(std::string name)
{
	// Whatever can throw comes before either list changes, so they never disagree.
	std::string copy = name;
	makeRoom(tags_, 1);
	makeRoom(tagIndex_, 1);
	tags_.insert(std::lower_bound(tags_.begin(), tags_.end(), copy), std::move(copy));
	const size_t at = indexIgnoringCase(tagIndex_, name);
	tagIndex_.insert(tagIndex_.begin() + static_cast<std::ptrdiff_t>(at), std::move(name));
}

} // namespace tapeloop
