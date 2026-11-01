// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/Gop.hpp"
#include "core/ReplayFormat.hpp"
#include "core/ReplayLibrary.hpp"
#include "core/ReplayPlayerState.hpp"
#include "core/ReplaySequence.hpp"
#include "obs/ReplayPlayer.hpp"

#include <obs.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace tapeloop::obs {

// Plays the shown sources of one replay after another through a replay source of its
// own, as a ReplayPlayerState decides: each source from the first frame of its clip at
// the speed the last one had, the next one once the clip played forward to its end, and
// back to live, with nothing loaded, after the last. Everything here runs on the UI
// thread except the player's controls, which may come from anywhere. A clip's end is
// seen on the graphics thread and handed to the UI thread, which ignores it once the
// source it was for is no longer on air or someone moved the playhead off the end.
class SequencePlayer {
public:
	// Creates the replay source; source() is null when that fails, and nothing plays.
	SequencePlayer();
	~SequencePlayer();

	SequencePlayer(const SequencePlayer &) = delete;
	SequencePlayer &operator=(const SequencePlayer &) = delete;
	SequencePlayer(SequencePlayer &&) = delete;
	SequencePlayer &operator=(SequencePlayer &&) = delete;

	// Private, valid while the sequence player is.
	obs_source_t *source() const noexcept { return source_; }
	// For the controls of the replay on air; null without a source.
	ReplayPlayer *player() const noexcept { return player_; }
	const ReplayPlayerState &state() const noexcept { return state_; }

	// As ReplayPlayerState's, live while a replay plays.
	void setEntries(std::vector<SequenceEntry> entries);
	bool move(size_t from, size_t to);
	bool setShown(std::string_view sourceKey, bool shown);

	// Puts the replay on air. Its sources the list does not have yet join it at the end,
	// shown, by name. A source with fewer than two frames has nothing to play and is left
	// out. False while closed or a replay is on air, for a damaged replay or one whose
	// index cannot be read, and when none of its shown sources has a clip.
	bool putOnAir(const Replay &replay);
	// Takes the replay off air early, as ReplayPlayerState's cutShort() says.
	void takeOffAir();
	// Ends the part on air as if it had played out.
	void skip();

	// For a scene collection that goes away: takes the replay off air at once, without
	// an outro, and forgets the list, whose sources were that collection's. Nothing goes
	// on air again until open().
	void close();
	void open() noexcept { closed_ = false; }
	bool closed() const noexcept { return closed_; }

private:
	// What follows the state's part on air, if it changed: the clip of a source, or else
	// nothing loaded.
	void follow();
	// Loads the source's clip and plays it; zero when it cannot be loaded.
	uint64_t play(std::string_view sourceKey);
	// Keeps the speed of the source on air for the next one, before anything that may
	// change sources.
	void keepRate() noexcept;
	void onEnded(uint64_t clip);
	static void handleTick(void *data, float seconds) noexcept;
	static void runEnded(void *param) noexcept;

	obs_source_t *source_ = nullptr;
	ReplayPlayer *player_ = nullptr;
	ReplayPlayerState state_;
	bool closed_ = false;
	// The token of the part follow() last set up, and the clip it loaded for a source.
	uint64_t token_ = 0;
	uint64_t clip_ = 0;
	// The replay on air.
	std::filesystem::path manifest_;
	std::shared_ptr<const ReplayIndex> index_;
	std::vector<std::vector<std::weak_ptr<const Gop>>> live_;
	// Set while a source plays, so that the graphics thread looks for its end only then.
	std::atomic<bool> watching_{false};
	// The graphics thread only: the clip and end it saw last.
	uint64_t seenClip_ = 0;
	bool seenEnded_ = false;
	std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

} // namespace tapeloop::obs
