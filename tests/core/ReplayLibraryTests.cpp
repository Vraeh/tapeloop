// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/ReplayLibrary.hpp"

#include "AllocationCounter.hpp"
#include "StoredReplays.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <vector>

using tapeloop::Clip;
using tapeloop::FoundReplay;
using tapeloop::Gop;
using tapeloop::Replay;
using tapeloop::ReplayCapture;
using tapeloop::ReplayLibrary;
using tapeloop::ReplayState;
using tapeloop::test::captureOf;
using tapeloop::test::Gops;
using tapeloop::test::makeGops;
using tapeloop::test::sourceOf;

namespace fs = std::filesystem;

namespace {

constexpr int64_t kSecond = 1'000'000'000;
constexpr int64_t kBase = int64_t{1'791'600'000} * kSecond;

std::chrono::system_clock::time_point wallClock(int64_t seconds)
{
	return std::chrono::system_clock::time_point(std::chrono::duration_cast<std::chrono::system_clock::duration>(
		std::chrono::nanoseconds(kBase + seconds * kSecond)));
}

// A capture of one GOP of a fresh source, made `seconds` after the base time.
ReplayCapture captureAt(int64_t seconds, const Gops &gops = makeGops(1))
{
	ReplayCapture capture = captureOf("Liga 2026-10-09 21-00", {sourceOf("camera", gops)});
	capture.capturedAtUtc = kBase + seconds * kSecond;
	return capture;
}

FoundReplay foundAt(int64_t seconds, std::vector<std::string> tags = {})
{
	FoundReplay found;
	found.manifest = fs::path("Copa") / (std::to_string(seconds) + ".tplp");
	found.broadcast = "Copa";
	found.intact = true;
	found.id = tapeloop::newReplayId();
	found.capturedAtUtc = kBase + seconds * kSecond;
	found.sources = {{"wide", "Wide"}, {"close", "Close"}};
	found.tags = std::move(tags);
	return found;
}

FoundReplay damaged(const std::string &name)
{
	FoundReplay found;
	found.manifest = fs::path("Copa") / name;
	found.broadcast = "Copa";
	return found;
}

} // namespace

TEST_CASE("ReplayLibrary lists replays newest first and plays the newest unless one is picked")
{
	ReplayLibrary library;
	CHECK(library.current() == 0);
	const uint64_t first = library.addCaptured(captureAt(10), "Liga");
	const uint64_t second = library.addCaptured(captureAt(20), "Liga");
	REQUIRE(first != 0);
	REQUIRE(second != 0);
	CHECK(library.list() == std::vector<uint64_t>{second, first});
	CHECK(library.current() == second);
	REQUIRE(library.find(first));
	CHECK(library.find(first)->capturedAt == wallClock(10));
	CHECK(library.find(first)->broadcast == "Liga");
	CHECK(library.find(99) == nullptr);

	CHECK(library.pick(first));
	CHECK(library.picked());
	CHECK(library.current() == first);
	CHECK_FALSE(library.pick(99));
	CHECK(library.current() == first);
	library.unpick();
	CHECK(library.current() == second);

	// A replay just captured is the one to show next, whatever was picked.
	REQUIRE(library.pick(first));
	const uint64_t third = library.addCaptured(captureAt(30), "Liga");
	CHECK_FALSE(library.picked());
	CHECK(library.current() == third);
}

TEST_CASE("ReplayLibrary is unchanged when adding fails")
{
	if (!tapeloop::test::kAllocationFailures) {
		SKIP("allocation failures cannot be injected in this configuration");
	}
	const ReplayCapture capture = captureAt(20);
	const FoundReplay found = foundAt(30, {"goals", "fouls"});
	for (size_t skip = 0; skip < 40; ++skip) {
		CAPTURE(skip);
		ReplayLibrary library;
		const uint64_t kept = library.addCaptured(captureAt(10), "Liga");
		REQUIRE(library.pick(kept));
		REQUIRE(library.createTag("saves"));
		uint64_t captured = 0;
		uint64_t added = 0;
		bool failed = false;
		{
			tapeloop::test::AllocationFailure failure(skip);
			try {
				captured = library.addCaptured(capture, "Liga");
				added = library.addFound(found);
			} catch (const std::bad_alloc &) {
				failed = true;
			}
		}
		if (failed && captured == 0) {
			CHECK(library.list() == std::vector<uint64_t>{kept});
			CHECK(library.current() == kept);
		} else if (failed) {
			CHECK(library.list() == std::vector<uint64_t>{captured, kept});
			CHECK(library.tags().size() == 1);
		} else {
			REQUIRE(added != 0);
			CHECK(library.find(added)->capturedAt == wallClock(30));
			CHECK(library.tags().size() == 3);
		}
		CHECK(library.find(kept)->capturedAt == wallClock(10));
	}
}

TEST_CASE("ReplayLibrary keeps no replay without frames")
{
	ReplayLibrary library;
	ReplayCapture capture = captureAt(1);
	capture.sources.front().clip = Clip();
	CHECK(library.addCaptured(capture, "Liga") == 0);
	capture.sources.clear();
	CHECK(library.addCaptured(capture, "Liga") == 0);
	CHECK(library.size() == 0);
	CHECK(library.list().empty());
}

TEST_CASE("ReplayLibrary falls back to the newest when the picked replay goes")
{
	ReplayLibrary library;
	const uint64_t first = library.addCaptured(captureAt(10), "Liga");
	const uint64_t second = library.addCaptured(captureAt(20), "Liga");
	REQUIRE(library.pick(first));
	CHECK(library.remove(first));
	CHECK_FALSE(library.remove(first));
	CHECK_FALSE(library.picked());
	CHECK(library.current() == second);
	library.clear();
	CHECK(library.current() == 0);
	CHECK(library.size() == 0);
}

TEST_CASE("ReplayLibrary tags replays and filters by tag")
{
	ReplayLibrary library;
	CHECK(library.createTag("goals"));
	CHECK_FALSE(library.createTag("goals"));
	CHECK_FALSE(library.createTag(""));
	const uint64_t goal = library.addCaptured(captureAt(10), "Liga");
	const uint64_t foul = library.addCaptured(captureAt(20), "Liga");
	const uint64_t both = library.addCaptured(captureAt(30), "Liga");

	CHECK(library.addTag(goal, "goals"));
	CHECK_FALSE(library.addTag(goal, "goals"));
	CHECK(library.addTag(foul, "fouls"));
	CHECK(library.addTag(both, "goals"));
	CHECK(library.addTag(both, "fouls"));
	CHECK_FALSE(library.addTag(99, "goals"));
	CHECK_FALSE(library.addTag(goal, ""));

	CHECK(library.tags().size() == 2);
	CHECK(library.tags()[0] == "fouls");
	CHECK(library.tags()[1] == "goals");
	CHECK(library.list("goals") == std::vector<uint64_t>{both, goal});
	CHECK(library.list("fouls") == std::vector<uint64_t>{both, foul});
	CHECK(library.list("saves").empty());
	REQUIRE(library.tagsOf(both).size() == 2);
	CHECK(library.tagsOf(99).empty());

	CHECK(library.removeTag(both, "goals"));
	CHECK_FALSE(library.removeTag(both, "goals"));
	CHECK(library.list("goals") == std::vector<uint64_t>{goal});

	// Deleting a tag takes it off every replay; a tag with no replay stays until then.
	CHECK(library.deleteTag("fouls"));
	CHECK_FALSE(library.deleteTag("fouls"));
	CHECK(library.list("fouls").empty());
	CHECK(library.tagsOf(both).empty());
	REQUIRE(library.tags().size() == 1);
	CHECK(library.tags()[0] == "goals");
}

TEST_CASE("ReplayLibrary puts on a replay only the tags its file has room for")
{
	ReplayLibrary library;
	const uint64_t replay = library.addCaptured(captureAt(10), "Liga");
	for (char name = 'a'; name < 'e'; ++name) {
		CHECK(library.addTag(replay, std::string(992, name)));
	}
	CHECK_FALSE(library.addTag(replay, std::string(992, 'e')));
	CHECK(library.tagsOf(replay).size() == 4);
	CHECK(library.tags().size() == 4);
	CHECK(library.addTag(replay, "short"));
	// Another replay has room of its own.
	const uint64_t other = library.addCaptured(captureAt(20), "Liga");
	CHECK(library.addTag(other, std::string(992, 'e')));
}

TEST_CASE("ReplayLibrary keeps tag names tidy and tells them apart regardless of case")
{
	ReplayLibrary library;
	const uint64_t replay = library.addCaptured(captureAt(10), "Liga");
	CHECK(library.createTag("  Goals "));
	CHECK_FALSE(library.createTag("goals"));
	CHECK_FALSE(library.createTag(" "));
	CHECK_FALSE(library.createTag(std::string("a\0b", 3)));
	CHECK_FALSE(library.createTag("tab\tinside"));
	REQUIRE(library.tags().size() == 1);
	CHECK(library.tags()[0] == "Goals");

	// Any spelling reaches the tag as it was first written.
	CHECK(library.addTag(replay, "GOALS"));
	CHECK_FALSE(library.addTag(replay, "goals"));
	REQUIRE(library.tagsOf(replay).size() == 1);
	CHECK(library.tagsOf(replay)[0] == "Goals");
	CHECK(library.list("goals") == std::vector<uint64_t>{replay});
	CHECK(library.removeTag(replay, " goals"));
	CHECK(library.deleteTag("gOaLs"));
	CHECK(library.tags().empty());
}

TEST_CASE("ReplayLibrary forgets the pick and the tags of what it lets go of")
{
	ReplayLibrary library;
	const uint64_t first = library.addCaptured(captureAt(10), "Liga");
	library.addCaptured(captureAt(20), "Liga");
	REQUIRE(library.addTag(first, "goals"));
	REQUIRE(library.pick(first));

	REQUIRE(library.remove(first));
	CHECK(library.list("goals").empty());
	CHECK(library.tagsOf(first).empty());

	const uint64_t third = library.addCaptured(captureAt(30), "Liga");
	REQUIRE(library.pick(third));
	library.clear();
	CHECK_FALSE(library.picked());
	CHECK(library.tagsOf(third).empty());
	CHECK(library.find(third) == nullptr);
	// Tags outlive their replays.
	CHECK(library.tags().size() == 1);
}

TEST_CASE("ReplayLibrary follows a captured replay until it is written")
{
	ReplayLibrary library;
	const ReplayCapture capture = captureAt(10, makeGops(3));
	const uint64_t id = library.addCaptured(capture, "Liga");
	const Replay *replay = library.find(id);
	REQUIRE(replay);
	CHECK(replay->state == ReplayState::Writing);
	CHECK(replay->uuid == capture.id);
	CHECK(replay->manifest.empty());
	CHECK(replay->sources == std::vector<tapeloop::ReplaySource>{{"camera", "Camera camera"}});
	REQUIRE(replay->index);
	CHECK(replay->index->sources[0].frameTimes.size() == 3 * tapeloop::test::kGopLength);

	tapeloop::ReplayIndex written = tapeloop::indexOf(capture);
	written.sources[0].gops[0].offset = 4096;
	CHECK(library.stored(id, "Liga/one.tplp", written));
	replay = library.find(id);
	CHECK(replay->state == ReplayState::Stored);
	CHECK(replay->manifest == fs::path("Liga/one.tplp"));
	CHECK(replay->index->sources[0].gops[0].offset == 4096);
	CHECK_FALSE(library.stored(id, "Liga/again.tplp", written));
	CHECK_FALSE(library.notSaved(id, "too late"));
	CHECK_FALSE(library.stored(99, "x.tplp", written));

	const uint64_t failed = library.addCaptured(captureAt(20), "Liga");
	CHECK(library.notSaved(failed, "disk full"));
	CHECK(library.find(failed)->state == ReplayState::NotSaved);
	CHECK(library.find(failed)->error == "disk full");
	CHECK_FALSE(library.notSaved(99, "unknown"));
	// Not saved, it still plays from the buffers, and is tagged in memory.
	CHECK(library.current() == failed);
	CHECK(library.pick(failed));
	CHECK(library.addTag(failed, "goals"));
}

TEST_CASE("ReplayLibrary points at the buffers' GOPs only while they live")
{
	ReplayLibrary library;
	uint64_t id = 0;
	{
		const Gops gops = makeGops(2);
		id = library.addCaptured(captureAt(10, gops), "Liga");
		const Replay *replay = library.find(id);
		REQUIRE(replay->live.size() == 1);
		REQUIRE(replay->live[0].size() == 2);
		CHECK(replay->live[0][0].lock() == gops[0]);
		CHECK(replay->live[0][1].lock() == gops[1]);
	}
	for (const std::weak_ptr<const Gop> &gop : library.find(id)->live[0]) {
		CHECK(gop.expired());
	}
}

TEST_CASE("ReplayLibrary forgets the frames of a replay the buffers no longer hold")
{
	ReplayLibrary library;
	Gops kept = makeGops(3);
	uint64_t stored = 0;
	uint64_t unsaved = 0;
	uint64_t writing = 0;
	uint64_t held = 0;
	{
		const Gops dropped = makeGops(2);
		stored = library.addCaptured(captureAt(10, dropped), "Liga");
		unsaved = library.addCaptured(captureAt(20, dropped), "Liga");
		writing = library.addCaptured(captureAt(30, dropped), "Liga");
		held = library.addCaptured(captureAt(40, kept), "Liga");
	}
	REQUIRE(library.stored(stored, "Liga/10.tplp", tapeloop::indexOf(captureAt(10, kept))));
	REQUIRE(library.notSaved(unsaved, "disk full"));
	REQUIRE(library.stored(held, "Liga/40.tplp", tapeloop::indexOf(captureAt(40, kept))));
	// Only the newest GOP of the one the buffers still hold is gone.
	kept.pop_back();

	CHECK(library.releaseExpired() == 2);
	CHECK(library.find(stored)->index == nullptr);
	CHECK(library.find(stored)->live.empty());
	CHECK(library.find(stored)->state == ReplayState::Stored);
	CHECK(library.find(stored)->sources.size() == 1);
	CHECK(library.find(unsaved)->index == nullptr);
	// Still being written, it needs its index when the store reports it.
	CHECK(library.find(writing)->index != nullptr);
	CHECK(library.find(held)->index != nullptr);
	CHECK(library.find(held)->live[0].size() == 3);
	CHECK(library.releaseExpired() == 0);
	kept.clear();
	CHECK(library.releaseExpired() == 1);
	CHECK(library.find(held)->index == nullptr);
}

TEST_CASE("ReplayLibrary takes replays found on disk with their tags")
{
	ReplayLibrary library;
	REQUIRE(library.createTag("Goals"));
	const uint64_t id = library.addFound(foundAt(10, {"goals", "Saves", "saves", " "}));
	const Replay *replay = library.find(id);
	REQUIRE(replay);
	CHECK(replay->state == ReplayState::Stored);
	CHECK(replay->capturedAt == wallClock(10));
	CHECK(replay->broadcast == "Copa");
	CHECK(replay->manifest == fs::path("Copa") / "10.tplp");
	CHECK(replay->sources.size() == 2);
	CHECK(replay->index == nullptr);
	CHECK(replay->live.empty());
	CHECK(replay->tags == std::vector<std::string>{"Goals", "Saves"});
	CHECK(std::vector<std::string>(library.tags().begin(), library.tags().end()) ==
	      std::vector<std::string>{"Goals", "Saves"});
	CHECK(library.list("saves") == std::vector<uint64_t>{id});
	CHECK(library.current() == id);
	CHECK(library.pick(id));
}

TEST_CASE("ReplayLibrary lists a damaged replay but never plays it")
{
	ReplayLibrary library;
	const uint64_t broken = library.addFound(damaged("broken.tplp"));
	const Replay *replay = library.find(broken);
	REQUIRE(replay);
	CHECK(replay->state == ReplayState::Damaged);
	CHECK(library.list() == std::vector<uint64_t>{broken});
	CHECK(library.current() == 0);
	CHECK_FALSE(library.pick(broken));
	CHECK_FALSE(library.addTag(broken, "goals"));
	const uint64_t good = library.addFound(foundAt(5));
	CHECK(library.current() == good);
}

TEST_CASE("ReplayLibrary does not take a replay twice")
{
	ReplayLibrary library;
	const ReplayCapture capture = captureAt(10);
	const uint64_t captured = library.addCaptured(capture, "Liga");
	FoundReplay found = foundAt(10);
	found.id = capture.id;
	CHECK(library.addFound(found) == 0);
	CHECK(library.addFound(damaged("broken.tplp")) != 0);
	CHECK(library.addFound(damaged("broken.tplp")) == 0);
	CHECK(library.addFound(damaged("other.tplp")) != 0);
	CHECK(library.size() == 3);
	CHECK(library.current() == captured);
}

TEST_CASE("ReplayLibrary knows the replay captured last")
{
	ReplayLibrary library;
	CHECK(library.lastCaptured() == 0);
	library.addFound(foundAt(5));
	CHECK(library.lastCaptured() == 0);
	const uint64_t first = library.addCaptured(captureAt(10), "Liga");
	CHECK(library.lastCaptured() == first);
	const uint64_t found = library.addFound(foundAt(20));
	REQUIRE(library.pick(found));
	CHECK(library.lastCaptured() == first);
	const uint64_t second = library.addCaptured(captureAt(30), "Liga");
	CHECK(library.lastCaptured() == second);
	ReplayCapture empty = captureAt(40);
	empty.sources.clear();
	library.addCaptured(empty, "Liga");
	CHECK(library.lastCaptured() == second);
}

TEST_CASE("ReplayLibrary orders replays by when they were captured")
{
	ReplayLibrary library;
	const uint64_t captured = library.addCaptured(captureAt(100), "Liga");
	const uint64_t older = library.addFound(foundAt(50));
	CHECK(library.list() == std::vector<uint64_t>{captured, older});
	CHECK(library.current() == captured);
	// A replay with a later capture time, from a clock set ahead: listed first, but the
	// one captured last is still the one to play.
	const uint64_t later = library.addFound(foundAt(200));
	CHECK(library.list() == std::vector<uint64_t>{later, captured, older});
	CHECK(library.current() == captured);
	// With the capture gone, the newest is.
	REQUIRE(library.remove(captured));
	CHECK(library.current() == later);
}
