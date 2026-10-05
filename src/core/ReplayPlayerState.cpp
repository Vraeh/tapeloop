// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/ReplayPlayerState.hpp"

#include <algorithm>
#include <optional>
#include <utility>

namespace tapeloop {

void ReplayPlayerState::setEntries(std::vector<SequenceEntry> entries)
{
	if (phase_ != AirPhase::Source) {
		sequence_.setEntries(std::move(entries));
		return;
	}
	// What followed the source on air, in case the new list drops it.
	std::vector<std::string> following;
	const std::span<const SequenceEntry> old = sequence_.entries();
	for (size_t i = sequence_.indexOf(source_).value_or(old.size()) + 1; i < old.size(); ++i) {
		following.push_back(old[i].sourceKey);
	}
	sequence_.setEntries(std::move(entries));

	if (const std::optional<size_t> now = sequence_.indexOf(source_)) {
		if (!sequence_.entries()[*now].shown) {
			advance(*now + 1);
		}
		return;
	}
	// Gone: the next one counts from the first source that followed it and is still
	// listed.
	for (const std::string &key : following) {
		if (const std::optional<size_t> index = sequence_.indexOf(key)) {
			advance(*index);
			return;
		}
	}
	advance(sequence_.entries().size());
}

bool ReplayPlayerState::move(size_t from, size_t to) noexcept
{
	return sequence_.move(from, to);
}

bool ReplayPlayerState::setShown(std::string_view sourceKey, bool shown)
{
	const std::optional<size_t> index = sequence_.indexOf(sourceKey);
	if (!index) {
		return false;
	}
	sequence_.setShown(sourceKey, shown);
	if (!shown && phase_ == AirPhase::Source && sourceKey == source_) {
		advance(*index + 1);
	}
	return true;
}

bool ReplayPlayerState::start(std::vector<std::string> sources)
{
	if (phase_ != AirPhase::Live) {
		return false;
	}
	std::sort(sources.begin(), sources.end());
	const std::optional<size_t> first = sequence_.nextShown(0, sources);
	if (!first) {
		return false;
	}
	available_ = std::move(sources);
	airing_ = config_;
	speed_ = 1.0;
	if (airing_.intro) {
		enter(AirPhase::Intro);
	} else {
		enter(AirPhase::Source, sequence_.entries()[*first].sourceKey);
	}
	return true;
}

void ReplayPlayerState::ended(uint64_t token)
{
	if (token != token_) {
		return;
	}
	switch (phase_) {
	case AirPhase::Live:
		break;
	case AirPhase::Intro:
		advance(0);
		break;
	case AirPhase::Source: {
		// The source on air is always in the list: removing it moves on at once.
		const std::optional<size_t> playing = sequence_.indexOf(source_);
		advance(playing ? *playing + 1 : 0);
		break;
	}
	case AirPhase::Outro:
		enter(AirPhase::Live);
		break;
	}
}

void ReplayPlayerState::cutShort() noexcept
{
	if (phase_ == AirPhase::Live) {
		return;
	}
	if (phase_ != AirPhase::Outro && airing_.outro) {
		enter(AirPhase::Outro);
	} else {
		enter(AirPhase::Live);
	}
}

bool ReplayPlayerState::setSpeed(double speed) noexcept
{
	if (!(speed >= kMinSpeed && speed <= kMaxSpeed)) {
		return false;
	}
	speed_ = speed;
	return true;
}

void ReplayPlayerState::advance(size_t from)
{
	if (const std::optional<size_t> next = sequence_.nextShown(from, available_)) {
		enter(AirPhase::Source, sequence_.entries()[*next].sourceKey);
	} else if (airing_.outro) {
		enter(AirPhase::Outro);
	} else {
		enter(AirPhase::Live);
	}
}

void ReplayPlayerState::enter(AirPhase phase, std::string source)
{
	phase_ = phase;
	source_ = std::move(source);
	++token_;
	if (phase == AirPhase::Live) {
		available_.clear();
	}
}

} // namespace tapeloop
