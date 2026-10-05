// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/ReplaySequence.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tapeloop {

enum class AirPhase { Live, Intro, Source, Outro };

struct AirConfig {
	bool intro = false;
	bool outro = false;
};

// One replay on air: its intro, then every shown source it has a clip of, in list order,
// then its outro, then back to live. It only decides: the caller plays each part and
// reports when it ends, passing back the token of that part, so that a late report for a
// part already replaced changes nothing.
//
// Reordering, showing and hiding while a replay plays act on what has not played yet:
// the source after the one playing is the next shown one below it in the list when it
// ends. Hiding or removing the source playing ends it at once, and the next one counts
// from where it is, or, when it is gone, from the first source that followed it. A source
// shown above the one playing waits for the next replay.
class ReplayPlayerState {
public:
	explicit ReplayPlayerState(AirConfig config = {}) : config_(config) {}

	const ReplaySequence &sequence() const noexcept { return sequence_; }
	void setEntries(std::vector<SequenceEntry> entries);
	bool move(size_t from, size_t to) noexcept;
	bool setShown(std::string_view sourceKey, bool shown);
	// Applies to the next replay put on air.
	void setConfig(AirConfig config) noexcept { config_ = config; }

	// Puts a replay with clips of these sources on air, from its intro or else its first
	// shown source with a clip, at 1x. False, staying live, while another replay is on
	// air or when no shown source has a clip.
	bool start(std::vector<std::string> sources);
	// The part with this token ended. Any other token is ignored.
	void ended(uint64_t token);
	// Takes the replay off air early: through the outro when there is one and it is not
	// on air yet, otherwise straight back to live.
	void cutShort() noexcept;

	AirPhase phase() const noexcept { return phase_; }
	// The source on air during the Source phase, empty otherwise.
	std::string_view source() const noexcept { return source_; }
	// Changes every time a part goes on air or the replay goes back to live.
	uint64_t token() const noexcept { return token_; }
	// The speed the sources play at. Each replay starts at 1x, and the speed chosen
	// stays for the next source.
	double speed() const noexcept { return speed_; }
	// False, changing nothing, for a speed outside the bounds below, NaN included.
	bool setSpeed(double speed) noexcept;
	static constexpr double kMinSpeed = 1.0 / 16;
	static constexpr double kMaxSpeed = 16;

private:
	// Puts on air the first shown source at or after `from`, or else what follows the
	// sources.
	void advance(size_t from);
	void enter(AirPhase phase, std::string source = {});

	AirConfig config_;
	// The configuration the replay on air started with.
	AirConfig airing_;
	ReplaySequence sequence_;
	// Sorted keys of the sources the replay on air has clips of.
	std::vector<std::string> available_;
	AirPhase phase_ = AirPhase::Live;
	std::string source_;
	uint64_t token_ = 0;
	double speed_ = 1.0;
};

} // namespace tapeloop
