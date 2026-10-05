// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/ReplayLibrary.hpp"

#include "SyntheticEncoder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using tapeloop::Gop;
using tapeloop::Moment;
using tapeloop::MomentListConfig;
using tapeloop::Nanoseconds;
using tapeloop::ReplayLibrary;
using tapeloop::test::SyntheticEncoder;

namespace {

constexpr size_t kUnlimited = std::numeric_limits<size_t>::max();

// A replay with one clip of one GOP of a fresh source, captured at `capturedAt` seconds.
Moment replayAt(int64_t capturedAt)
{
	SyntheticEncoder encoder(SyntheticEncoder::Config{});
	tapeloop::GopBuilder builder(encoder.frameDuration());
	for (int frame = 0; frame < 30; ++frame) {
		builder.append(encoder.next());
	}
	const std::vector<std::shared_ptr<const Gop>> gops{builder.seal()};
	Moment moment;
	moment.end = Nanoseconds{capturedAt * 1'000'000'000};
	moment.start = moment.end - Nanoseconds{1'000'000'000};
	moment.clips.push_back({"camera", tapeloop::Clip(gops, encoder.timeOf(0), encoder.timeOf(29))});
	return moment;
}

ReplayLibrary unlimited()
{
	return ReplayLibrary(MomentListConfig{kUnlimited, kUnlimited});
}

} // namespace

TEST_CASE("ReplayLibrary lists replays newest first and plays the newest unless one is picked")
{
	ReplayLibrary library = unlimited();
	CHECK(library.current() == 0);
	const uint64_t first = library.add(replayAt(10));
	const uint64_t second = library.add(replayAt(20));
	REQUIRE(first != 0);
	REQUIRE(second != 0);
	CHECK(library.list() == std::vector<uint64_t>{second, first});
	CHECK(library.current() == second);
	REQUIRE(library.find(first));
	CHECK(library.find(first)->end == Nanoseconds{10'000'000'000});

	CHECK(library.pick(first));
	CHECK(library.picked());
	CHECK(library.current() == first);
	CHECK_FALSE(library.pick(99));
	CHECK(library.current() == first);
	library.unpick();
	CHECK(library.current() == second);

	// A replay just captured is the one to show next, whatever was picked.
	REQUIRE(library.pick(first));
	const uint64_t third = library.add(replayAt(30));
	CHECK_FALSE(library.picked());
	CHECK(library.current() == third);
}

TEST_CASE("ReplayLibrary stores no replay without clips")
{
	ReplayLibrary library = unlimited();
	CHECK(library.add(Moment{}) == 0);
	CHECK(library.size() == 0);
	CHECK(library.list().empty());
}

TEST_CASE("ReplayLibrary falls back to the newest when the picked replay goes")
{
	ReplayLibrary library = unlimited();
	const uint64_t first = library.add(replayAt(10));
	const uint64_t second = library.add(replayAt(20));
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
	ReplayLibrary library = unlimited();
	CHECK(library.createTag("goals"));
	CHECK_FALSE(library.createTag("goals"));
	CHECK_FALSE(library.createTag(""));
	const uint64_t goal = library.add(replayAt(10));
	const uint64_t foul = library.add(replayAt(20));
	const uint64_t both = library.add(replayAt(30));

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

TEST_CASE("ReplayLibrary forgets the tags of replays dropped for room")
{
	ReplayLibrary library(MomentListConfig{2, kUnlimited});
	const uint64_t first = library.add(replayAt(10));
	REQUIRE(library.addTag(first, "goals"));
	library.add(replayAt(20));
	library.add(replayAt(30));
	CHECK_FALSE(library.find(first));
	CHECK(library.size() == 2);
	CHECK(library.tagsOf(first).empty());
	CHECK(library.list("goals").empty());
	// The tag itself stays for the replays to come.
	CHECK(library.tags().size() == 1);
}
