// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "../core/StoredReplays.hpp"
#include "ObsFixture.hpp"
#include "UiTasks.hpp"

#include "core/ReplayLibrary.hpp"
#include "core/ReplayPlayerState.hpp"
#include "core/ReplaySequence.hpp"
#include "obs/ReplayPlayer.hpp"
#include "obs/ReplaySource.hpp"
#include "obs/SequencePlayer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <obs.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using tapeloop::AirPhase;
using tapeloop::Nanoseconds;
using tapeloop::Replay;
using tapeloop::ReplayLibrary;
using tapeloop::SequenceEntry;
using tapeloop::obs::PlayerState;
using tapeloop::obs::registerReplaySource;
using tapeloop::obs::SequencePlayer;
using tapeloop::test::captureOf;
using tapeloop::test::Gops;
using tapeloop::test::makeGops;
using tapeloop::test::ObsFixture;
using tapeloop::test::sourceOf;
using tapeloop::test::UiTasks;

namespace {

// A replay just captured, with a clip of every source the GOPs given: the buffer's GOPs,
// which the test holds, so nothing is read from disk.
class Captured {
public:
	Captured(const std::vector<std::string> &keys, size_t gops) : gops_(makeGops(gops))
	{
		std::vector<tapeloop::CaptureSource> sources;
		for (const std::string &key : keys) {
			sources.push_back(sourceOf(key, gops_));
		}
		id_ = library_.addCaptured(captureOf("unused", std::move(sources)), "Broadcast");
		REQUIRE(id_ != 0);
	}

	const Replay &replay() const { return *library_.find(id_); }
	Nanoseconds lastFrame() const { return gops_.back()->lastTime(); }

private:
	Gops gops_;
	ReplayLibrary library_;
	uint64_t id_ = 0;
};

// Runs the UI thread's tasks until the condition holds, as OBS ticks on its own.
bool runUntil(const std::function<bool()> &condition)
{
	const auto deadline = std::chrono::steady_clock::now() + 10s;
	while (std::chrono::steady_clock::now() < deadline) {
		UiTasks::run();
		if (condition()) {
			return true;
		}
		std::this_thread::sleep_for(2ms);
	}
	return false;
}

// Waits, without running the UI thread's tasks, until the player's state holds.
bool playerUntil(const SequencePlayer &sequence, const std::function<bool(const PlayerState &)> &condition)
{
	const auto deadline = std::chrono::steady_clock::now() + 10s;
	while (std::chrono::steady_clock::now() < deadline) {
		if (condition(sequence.player()->state())) {
			return true;
		}
		std::this_thread::sleep_for(2ms);
	}
	return false;
}

// The source on air once the player has its clip and plays it.
std::string playing(SequencePlayer &sequence)
{
	const uint64_t token = sequence.state().token();
	REQUIRE(sequence.state().phase() == AirPhase::Source);
	REQUIRE(playerUntil(sequence, [](const PlayerState &state) { return state.loaded && state.playing; }));
	CHECK(sequence.state().token() == token);
	return std::string(sequence.state().source());
}

std::vector<std::string> keysOf(const SequencePlayer &sequence)
{
	std::vector<std::string> keys;
	for (const SequenceEntry &entry : sequence.state().sequence().entries()) {
		keys.push_back(entry.sourceKey + (entry.shown ? "" : "-"));
	}
	return keys;
}

} // namespace

TEST_CASE_METHOD(ObsFixture, "a sequence plays the shown sources of a replay in list order, then goes back to live",
		 "[obs][sequence]")
{
	registerReplaySource({});
	const UiTasks ui;
	std::optional<SequencePlayer> sequence(std::in_place);
	REQUIRE(sequence->source());
	REQUIRE(sequence->player());

	// Clips of a sixth of a second, which play out by themselves.
	const Captured captured({"b", "a", "c", "d"}, 2);
	sequence->setEntries({{"c", true}, {"d", false}, {"a", true}});
	REQUIRE(sequence->putOnAir(captured.replay()));
	// The source the list did not have joins it at the end, shown.
	CHECK(keysOf(*sequence) == std::vector<std::string>{"c", "d-", "a", "b"});

	std::vector<std::string> order;
	uint64_t token = 0;
	REQUIRE(runUntil([&] {
		if (sequence->state().token() != token) {
			token = sequence->state().token();
			if (sequence->state().phase() == AirPhase::Source) {
				order.push_back(std::string(sequence->state().source()));
			}
		}
		return sequence->state().phase() == AirPhase::Live;
	}));
	CHECK(order == std::vector<std::string>{"c", "a", "b"});
	REQUIRE(playerUntil(*sequence, [](const PlayerState &state) { return !state.loaded; }));

	// Another goes on air from the start of the list.
	REQUIRE(sequence->putOnAir(captured.replay()));
	CHECK(sequence->state().source() == "c");
	sequence.reset();
	obs_wait_for_destroy_queue();
}

TEST_CASE_METHOD(ObsFixture, "a sequence follows the list as it changes while a replay plays", "[obs][sequence]")
{
	registerReplaySource({});
	const UiTasks ui;
	std::optional<SequencePlayer> sequence(std::in_place);
	REQUIRE(sequence->player());

	// Clips of ten seconds, which move on only when told.
	const Captured captured({"a", "b", "c", "d"}, 120);
	sequence->setEntries({{"a", true}, {"b", true}, {"c", true}, {"d", true}});
	REQUIRE(sequence->putOnAir(captured.replay()));
	CHECK(playing(*sequence) == "a");

	REQUIRE(sequence->move(3, 1));
	REQUIRE(sequence->setShown("b", false));
	CHECK(keysOf(*sequence) == std::vector<std::string>{"a", "d", "b-", "c"});
	CHECK(playing(*sequence) == "a");
	sequence->skip();
	CHECK(playing(*sequence) == "d");

	REQUIRE(sequence->setShown("b", true));
	// Shown above the source on air, it waits for the next replay.
	REQUIRE(sequence->move(2, 0));
	CHECK(keysOf(*sequence) == std::vector<std::string>{"b", "a", "d", "c"});
	CHECK(playing(*sequence) == "d");

	// Hiding the source on air ends it at once.
	REQUIRE(sequence->setShown("d", false));
	CHECK(playing(*sequence) == "c");

	// So does a list without it; nothing followed it, so the replay is over.
	sequence->setEntries({{"b", true}, {"a", true}, {"d", false}});
	CHECK(sequence->state().phase() == AirPhase::Live);
	REQUIRE(playerUntil(*sequence, [](const PlayerState &state) { return !state.loaded; }));
	CHECK_FALSE(sequence->setShown("c", true));

	sequence.reset();
	obs_wait_for_destroy_queue();
}

TEST_CASE_METHOD(ObsFixture, "a sequence keeps the speed for the next source and plays it forward", "[obs][sequence]")
{
	registerReplaySource({});
	const UiTasks ui;
	std::optional<SequencePlayer> sequence(std::in_place);
	REQUIRE(sequence->player());

	const Captured captured({"a", "b", "c"}, 120);
	REQUIRE(sequence->putOnAir(captured.replay()));
	CHECK(playing(*sequence) == "a");
	CHECK(sequence->player()->state().rate == 1000);

	sequence->player()->faster();
	sequence->player()->reverse();
	REQUIRE(playerUntil(*sequence, [](const PlayerState &state) { return state.rate == -2000; }));
	sequence->skip();
	CHECK(playing(*sequence) == "b");
	REQUIRE(playerUntil(*sequence, [](const PlayerState &state) { return state.rate == 2000; }));

	sequence->player()->slower();
	sequence->player()->slower();
	REQUIRE(playerUntil(*sequence, [](const PlayerState &state) { return state.rate == 750; }));
	sequence->skip();
	CHECK(playing(*sequence) == "c");
	REQUIRE(playerUntil(*sequence, [](const PlayerState &state) { return state.rate == 750; }));

	// Each replay starts at real time.
	sequence->takeOffAir();
	REQUIRE(sequence->putOnAir(captured.replay()));
	CHECK(playing(*sequence) == "a");
	REQUIRE(playerUntil(*sequence, [](const PlayerState &state) { return state.rate == 1000; }));

	sequence.reset();
	obs_wait_for_destroy_queue();
}

TEST_CASE_METHOD(ObsFixture, "a sequence goes back to live when cut short, and not before", "[obs][sequence]")
{
	registerReplaySource({});
	const UiTasks ui;
	std::optional<SequencePlayer> sequence(std::in_place);
	REQUIRE(sequence->player());

	const Captured captured({"a", "b"}, 120);
	REQUIRE(sequence->putOnAir(captured.replay()));
	CHECK(playing(*sequence) == "a");
	// One replay at a time.
	CHECK_FALSE(sequence->putOnAir(captured.replay()));

	// Someone who takes the clip off its end as it gets there keeps it on air.
	sequence->player()->seek(captured.lastFrame() - 50ms);
	REQUIRE(playerUntil(*sequence, [](const PlayerState &state) { return state.ended; }));
	sequence->player()->step(-1);
	REQUIRE(playerUntil(*sequence, [](const PlayerState &state) { return !state.ended; }));
	REQUIRE(UiTasks::pending() > 0);
	UiTasks::run();
	CHECK(sequence->state().phase() == AirPhase::Source);
	CHECK(sequence->state().source() == "a");

	// Played to the end again, it moves on.
	sequence->player()->play();
	REQUIRE(runUntil([&] { return sequence->state().source() == "b"; }));
	CHECK(playing(*sequence) == "b");

	sequence->takeOffAir();
	CHECK(sequence->state().phase() == AirPhase::Live);
	REQUIRE(playerUntil(*sequence, [](const PlayerState &state) { return !state.loaded; }));
	sequence->skip();
	CHECK(sequence->state().phase() == AirPhase::Live);

	sequence.reset();
	obs_wait_for_destroy_queue();
}

TEST_CASE_METHOD(ObsFixture, "a sequence leaves out what has nothing to play", "[obs][sequence]")
{
	registerReplaySource({});
	const UiTasks ui;
	std::optional<SequencePlayer> sequence(std::in_place);
	REQUIRE(sequence->player());

	const Captured captured({"a", "b"}, 2);
	sequence->setEntries({{"a", false}, {"b", false}});
	CHECK_FALSE(sequence->putOnAir(captured.replay()));
	CHECK(sequence->state().phase() == AirPhase::Live);

	// Found on disk without a manifest that can be read.
	Replay found = captured.replay();
	found.index.reset();
	found.manifest = "missing.tlr";
	REQUIRE(sequence->setShown("a", true));
	CHECK_FALSE(sequence->putOnAir(found));
	found.state = tapeloop::ReplayState::Damaged;
	found.index = captured.replay().index;
	CHECK_FALSE(sequence->putOnAir(found));
	CHECK(sequence->state().phase() == AirPhase::Live);

	sequence.reset();
	obs_wait_for_destroy_queue();
}
