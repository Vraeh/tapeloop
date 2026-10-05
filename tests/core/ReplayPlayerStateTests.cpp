// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/Playhead.hpp"
#include "core/ReplayPlayerState.hpp"
#include "core/ReplaySequence.hpp"

#include "AllocationCounter.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <vector>

using tapeloop::AirConfig;
using tapeloop::AirPhase;
using tapeloop::ReplayPlayerState;
using tapeloop::ReplaySequence;
using tapeloop::SequenceEntry;

namespace {

std::vector<SequenceEntry> entries(std::initializer_list<std::string_view> keys)
{
	std::vector<SequenceEntry> list;
	for (const std::string_view key : keys) {
		list.push_back({std::string(key), true});
	}
	return list;
}

std::vector<std::string> keysOf(const ReplaySequence &sequence)
{
	std::vector<std::string> keys;
	for (const SequenceEntry &entry : sequence.entries()) {
		keys.push_back(entry.sourceKey);
	}
	return keys;
}

// Ends whatever is on air, as the caller would once it has played.
void endPart(ReplayPlayerState &player)
{
	player.ended(player.token());
}

} // namespace

TEST_CASE("ReplaySequence keeps an order that can change")
{
	ReplaySequence sequence;
	sequence.setEntries(entries({"a", "b", "c", "a", ""}));
	CHECK(keysOf(sequence) == std::vector<std::string>{"a", "b", "c"});
	// The first of two entries for one source keeps its flag too.
	std::vector<SequenceEntry> twice = entries({"a", "a"});
	twice[1].shown = false;
	ReplaySequence first;
	first.setEntries(twice);
	REQUIRE(first.entries().size() == 1);
	CHECK(first.entries()[0].shown);
	CHECK(sequence.move(0, 2));
	CHECK(keysOf(sequence) == std::vector<std::string>{"b", "c", "a"});
	CHECK(sequence.move(2, 0));
	CHECK(keysOf(sequence) == std::vector<std::string>{"a", "b", "c"});
	CHECK(sequence.move(1, 1));
	CHECK_FALSE(sequence.move(3, 0));
	CHECK_FALSE(sequence.move(0, 3));
	CHECK(sequence.indexOf("c") == 2u);
	CHECK_FALSE(sequence.indexOf("d"));

	CHECK(sequence.setShown("b", false));
	CHECK_FALSE(sequence.setShown("d", false));
	const std::vector<std::string> available{"a", "b", "c"};
	CHECK(sequence.nextShown(1, available) == 2u);
	CHECK(sequence.nextShown(0, std::vector<std::string>{"c"}) == 2u);
	CHECK_FALSE(sequence.nextShown(3, available));
}

TEST_CASE("ReplayPlayerState plays every shown source with a clip, in list order")
{
	ReplayPlayerState player;
	player.setEntries(entries({"a", "b", "c", "d"}));
	REQUIRE(player.setShown("b", false));
	// The replay has no clip of d.
	REQUIRE(player.start({"c", "b", "a"}));
	CHECK(player.phase() == AirPhase::Source);
	CHECK(player.source() == "a");
	endPart(player);
	CHECK(player.source() == "c");
	endPart(player);
	CHECK(player.phase() == AirPhase::Live);
	CHECK(player.source().empty());
}

TEST_CASE("ReplayPlayerState plays the intro and the outro around the sources")
{
	ReplayPlayerState player(AirConfig{true, true});
	player.setEntries(entries({"a", "b"}));
	REQUIRE(player.start({"a", "b"}));
	CHECK(player.phase() == AirPhase::Intro);
	endPart(player);
	CHECK(player.source() == "a");
	endPart(player);
	CHECK(player.source() == "b");
	endPart(player);
	CHECK(player.phase() == AirPhase::Outro);
	endPart(player);
	CHECK(player.phase() == AirPhase::Live);
}

TEST_CASE("ReplayPlayerState stays live with nothing to show")
{
	ReplayPlayerState player(AirConfig{true, true});
	player.setEntries(entries({"a", "b"}));
	REQUIRE(player.setShown("a", false));
	CHECK_FALSE(player.start({"a"}));
	CHECK_FALSE(player.start({}));
	CHECK(player.phase() == AirPhase::Live);

	// One replay at a time.
	REQUIRE(player.start({"b"}));
	CHECK_FALSE(player.start({"b"}));
}

TEST_CASE("ReplayPlayerState ignores the end of a part that is no longer on air")
{
	ReplayPlayerState player;
	player.setEntries(entries({"a", "b", "c"}));
	REQUIRE(player.start({"a", "b", "c"}));
	const uint64_t first = player.token();
	endPart(player);
	REQUIRE(player.source() == "b");
	// A late report for a.
	player.ended(first);
	CHECK(player.source() == "b");
	player.ended(player.token() + 1);
	CHECK(player.source() == "b");
}

TEST_CASE("ReplayPlayerState follows changes to the list while it plays")
{
	ReplayPlayerState player;
	player.setEntries(entries({"a", "b", "c", "d"}));
	REQUIRE(player.start({"a", "b", "c", "d"}));
	REQUIRE(player.source() == "a");

	SECTION("a source moved below the one playing comes next")
	{
		REQUIRE(player.move(3, 1));
		endPart(player);
		CHECK(player.source() == "d");
	}
	SECTION("a source hidden before its turn is skipped")
	{
		REQUIRE(player.setShown("b", false));
		CHECK(player.source() == "a");
		endPart(player);
		CHECK(player.source() == "c");
	}
	SECTION("hiding the source playing ends it at once")
	{
		const uint64_t token = player.token();
		REQUIRE(player.setShown("a", false));
		CHECK(player.source() == "b");
		CHECK(player.token() != token);
	}
	SECTION("hiding the source playing goes on below it, never back to the top")
	{
		endPart(player);
		endPart(player);
		REQUIRE(player.source() == "c");
		REQUIRE(player.setShown("c", false));
		CHECK(player.source() == "d");
	}
	SECTION("showing the source playing again changes nothing")
	{
		const uint64_t token = player.token();
		REQUIRE(player.setShown("a", true));
		CHECK(player.source() == "a");
		CHECK(player.token() == token);
	}
	SECTION("a source shown above the one playing waits for the next replay")
	{
		endPart(player);
		REQUIRE(player.source() == "b");
		REQUIRE(player.setShown("a", false));
		REQUIRE(player.setShown("a", true));
		endPart(player);
		CHECK(player.source() == "c");
	}
	SECTION("the source playing removed from the list is followed by the next one")
	{
		player.setEntries(entries({"b", "c", "d"}));
		CHECK(player.source() == "b");
	}
	SECTION("a new list without the source playing goes on from what followed it")
	{
		endPart(player);
		endPart(player);
		REQUIRE(player.source() == "c");
		player.setEntries(entries({"d", "a", "b"}));
		CHECK(player.source() == "d");
	}
	SECTION("a new list that hides the source playing goes on below its new place")
	{
		endPart(player);
		REQUIRE(player.source() == "b");
		std::vector<SequenceEntry> reordered = entries({"c", "a", "b"});
		reordered[2].shown = false;
		player.setEntries(reordered);
		CHECK(player.phase() == AirPhase::Live);
	}
	SECTION("a new list that keeps the source playing changes nothing")
	{
		const uint64_t token = player.token();
		player.setEntries(entries({"c", "a", "b"}));
		CHECK(player.source() == "a");
		CHECK(player.token() == token);
		endPart(player);
		CHECK(player.source() == "b");
	}
}

TEST_CASE("ReplayPlayerState cuts a replay short through its outro")
{
	ReplayPlayerState player(AirConfig{true, true});
	player.setEntries(entries({"a", "b"}));
	REQUIRE(player.start({"a", "b"}));
	player.cutShort();
	CHECK(player.phase() == AirPhase::Outro);
	player.cutShort();
	CHECK(player.phase() == AirPhase::Live);
	player.cutShort();
	CHECK(player.phase() == AirPhase::Live);

	ReplayPlayerState plain;
	plain.setEntries(entries({"a", "b"}));
	REQUIRE(plain.start({"a", "b"}));
	plain.cutShort();
	CHECK(plain.phase() == AirPhase::Live);
}

TEST_CASE("ReplayPlayerState keeps the rate from one source to the next, not to the next replay")
{
	ReplayPlayerState player;
	player.setEntries(entries({"a", "b"}));
	REQUIRE(player.start({"a", "b"}));
	CHECK(player.rate() == 1000);
	CHECK(player.setRate(500));
	// The Playhead's limits, so that the rate chosen is the rate played.
	CHECK(ReplayPlayerState::kMinRate == tapeloop::PlayheadConfig{}.minRate);
	CHECK(ReplayPlayerState::kMaxRate == tapeloop::PlayheadConfig{}.maxRate);
	CHECK_FALSE(player.setRate(0));
	CHECK_FALSE(player.setRate(-1000));
	CHECK_FALSE(player.setRate(ReplayPlayerState::kMinRate - 1));
	CHECK_FALSE(player.setRate(ReplayPlayerState::kMaxRate + 1));
	CHECK_FALSE(player.setRate(std::numeric_limits<int32_t>::min()));
	CHECK(player.rate() == 500);
	CHECK(player.setRate(ReplayPlayerState::kMinRate));
	CHECK(player.setRate(ReplayPlayerState::kMaxRate));
	CHECK(player.setRate(500));
	endPart(player);
	CHECK(player.source() == "b");
	CHECK(player.rate() == 500);
	endPart(player);
	REQUIRE(player.start({"a"}));
	CHECK(player.rate() == 1000);
}

TEST_CASE("ReplayPlayerState leaves no hidden or removed source on air when an edit fails")
{
	if (!tapeloop::test::kAllocationFailures) {
		SKIP("allocation failures cannot be injected in this configuration");
	}
	// Keys too long to be stored inline, so that putting one on air allocates.
	const std::string first(40, 'a');
	const std::string second(40, 'b');
	const std::string third(40, 'c');
	for (const bool hide : {true, false}) {
		bool wentLive = false;
		for (size_t skip = 0; skip < 8; ++skip) {
			CAPTURE(hide, skip);
			ReplayPlayerState player;
			player.setEntries(entries({first, second, third}));
			REQUIRE(player.start({first, second, third}));
			std::vector<SequenceEntry> without = entries({second, third});
			bool failed = false;
			{
				tapeloop::test::AllocationFailure failure(skip);
				try {
					if (hide) {
						player.setShown(first, false);
					} else {
						player.setEntries(std::move(without));
					}
				} catch (const std::bad_alloc &) {
					failed = true;
				}
			}
			if (!failed) {
				CHECK(player.phase() == AirPhase::Source);
				CHECK(player.source() == second);
			} else if (player.phase() == AirPhase::Source) {
				// It failed before the list changed, so nothing did.
				CHECK(player.source() == first);
				REQUIRE(player.sequence().indexOf(first) == 0);
				CHECK(player.sequence().entries()[0].shown);
			} else {
				CHECK(player.phase() == AirPhase::Live);
				wentLive = true;
			}
		}
		CHECK(wentLive);
	}
}

TEST_CASE("ReplayPlayerState plays a replay with the configuration it started with")
{
	ReplayPlayerState player;
	player.setEntries(entries({"a"}));
	REQUIRE(player.start({"a"}));
	player.setConfig(AirConfig{true, true});
	endPart(player);
	CHECK(player.phase() == AirPhase::Live);
	REQUIRE(player.start({"a"}));
	CHECK(player.phase() == AirPhase::Intro);
}
