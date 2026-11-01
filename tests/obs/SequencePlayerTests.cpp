// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "../core/FakeDecoder.hpp"
#include "../core/StoredReplays.hpp"
#include "../core/TempDirectory.hpp"
#include "ObsFixture.hpp"
#include "UiTasks.hpp"

#include "core/ReplayLibrary.hpp"
#include "core/ReplayPlayerState.hpp"
#include "core/ReplaySequence.hpp"
#include "core/ReplayWriter.hpp"
#include "decode/PictureDecoder.hpp"
#include "obs/ReplayPlayer.hpp"
#include "obs/ReplaySource.hpp"
#include "obs/SequencePlayer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <obs.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using tapeloop::AirPhase;
using tapeloop::DecodedFrame;
using tapeloop::DecodeStatus;
using tapeloop::Nanoseconds;
using tapeloop::ReplayLibrary;
using tapeloop::SequenceEntry;
using tapeloop::VideoCodec;
using tapeloop::decode::Picture;
using tapeloop::decode::PictureDecoder;
using tapeloop::obs::PlayerState;
using tapeloop::obs::registerReplaySource;
using tapeloop::obs::SequencePlayer;
using tapeloop::test::captureOf;
using tapeloop::test::FakeDecoder;
using tapeloop::test::Gops;
using tapeloop::test::makeGops;
using tapeloop::test::ObsFixture;
using tapeloop::test::sourceOf;
using tapeloop::test::TempDirectory;
using tapeloop::test::UiTasks;
using tapeloop::test::waitFor;

namespace {

// A replay just captured, with a clip of every source the GOPs given: the buffer's GOPs,
// which the test holds, so nothing is read from disk.
class Captured {
public:
	Captured(ReplayLibrary &library, const std::vector<std::string> &keys, size_t gops) : gops_(makeGops(gops))
	{
		std::vector<tapeloop::CaptureSource> sources;
		for (const std::string &key : keys) {
			sources.push_back(sourceOf(key, gops_));
		}
		id_ = library.addCaptured(captureOf("unused", std::move(sources)), "Broadcast");
		REQUIRE(id_ != 0);
	}

	uint64_t id() const noexcept { return id_; }
	Nanoseconds lastFrame() const { return gops_.back()->lastTime(); }

private:
	Gops gops_;
	uint64_t id_ = 0;
};

// Decodes as a FakeDecoder does, and keeps the newest presentation time it was handed.
// Its frames have no picture, so the player shows nothing.
class NewestDecoder final : public PictureDecoder {
public:
	explicit NewestDecoder(std::atomic<int64_t> &newest) : newest_(newest) {}

	DecodeStatus open(VideoCodec codec, std::span<const uint8_t> config) noexcept override
	{
		return decoder_.open(codec, config);
	}

	DecodeStatus send(std::span<const uint8_t> data, int64_t pts, int64_t dts) noexcept override
	{
		if (pts > newest_.load()) {
			newest_.store(pts);
		}
		return decoder_.send(data, pts, dts);
	}

	DecodeStatus receive(DecodedFrame &frame) noexcept override { return decoder_.receive(frame); }
	DecodeStatus flush() noexcept override { return decoder_.flush(); }
	void reset() noexcept override { decoder_.reset(); }
	void release(const DecodedFrame &frame) noexcept override { decoder_.release(frame); }
	void close() noexcept override { decoder_.close(); }
	std::optional<Picture> picture(const DecodedFrame &) const noexcept override { return std::nullopt; }

private:
	std::atomic<int64_t> &newest_;
	FakeDecoder decoder_{[](int64_t pts) { return pts % tapeloop::test::kGopLength == 0; }, 0};
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
	ReplayLibrary library;
	std::optional<SequencePlayer> sequence(std::in_place, library);
	REQUIRE(sequence->source());
	REQUIRE(sequence->player());

	// Clips of a sixth of a second, which play out by themselves.
	const Captured captured(library, {"b", "a", "c", "d"}, 2);
	sequence->setEntries({{"c", true}, {"d", false}, {"a", true}});
	REQUIRE(sequence->putOnAir(captured.id()));
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
	REQUIRE(sequence->putOnAir(captured.id()));
	CHECK(sequence->state().source() == "c");
	sequence.reset();
	obs_wait_for_destroy_queue();
}

TEST_CASE_METHOD(ObsFixture, "a sequence follows the list as it changes while a replay plays", "[obs][sequence]")
{
	registerReplaySource({});
	const UiTasks ui;
	ReplayLibrary library;
	std::optional<SequencePlayer> sequence(std::in_place, library);
	REQUIRE(sequence->player());

	// Clips of ten seconds, which move on only when told.
	const Captured captured(library, {"a", "b", "c", "d"}, 120);
	sequence->setEntries({{"a", true}, {"b", true}, {"c", true}, {"d", true}});
	REQUIRE(sequence->putOnAir(captured.id()));
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
	ReplayLibrary library;
	std::optional<SequencePlayer> sequence(std::in_place, library);
	REQUIRE(sequence->player());

	const Captured captured(library, {"a", "b", "c"}, 120);
	REQUIRE(sequence->putOnAir(captured.id()));
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
	REQUIRE(sequence->putOnAir(captured.id()));
	CHECK(playing(*sequence) == "a");
	REQUIRE(playerUntil(*sequence, [](const PlayerState &state) { return state.rate == 1000; }));

	sequence.reset();
	obs_wait_for_destroy_queue();
}

TEST_CASE_METHOD(ObsFixture, "a sequence goes back to live when cut short, and not before", "[obs][sequence]")
{
	registerReplaySource({});
	const UiTasks ui;
	ReplayLibrary library;
	std::optional<SequencePlayer> sequence(std::in_place, library);
	REQUIRE(sequence->player());

	const Captured captured(library, {"a", "b"}, 120);
	REQUIRE(sequence->putOnAir(captured.id()));
	CHECK(playing(*sequence) == "a");
	// One replay at a time.
	CHECK_FALSE(sequence->putOnAir(captured.id()));

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
	ReplayLibrary library;
	std::optional<SequencePlayer> sequence(std::in_place, library);
	REQUIRE(sequence->player());

	const Captured captured(library, {"a", "b"}, 2);
	sequence->setEntries({{"a", false}, {"b", false}});
	CHECK_FALSE(sequence->putOnAir(captured.id()));
	CHECK(sequence->state().phase() == AirPhase::Live);

	REQUIRE(sequence->setShown("a", true));
	CHECK_FALSE(sequence->putOnAir(0));

	// Found on disk, with a manifest that cannot be read.
	tapeloop::FoundReplay found;
	found.manifest = "missing.tlr";
	found.broadcast = "Broadcast";
	found.intact = true;
	found.id = tapeloop::newReplayId();
	found.sources = {{"a", "Camera a"}};
	const uint64_t unreadable = library.addFound(found);
	REQUIRE(unreadable != 0);
	CHECK_FALSE(sequence->putOnAir(unreadable));

	// Damaged, even if what it names can be read.
	const TempDirectory folder;
	found.manifest =
		tapeloop::ReplayWriter().write(captureOf(folder.path(), {sourceOf("a", makeGops(2))})).manifest;
	found.intact = false;
	const uint64_t damaged = library.addFound(found);
	REQUIRE(damaged != 0);
	CHECK_FALSE(sequence->putOnAir(damaged));
	CHECK(sequence->state().phase() == AirPhase::Live);

	sequence.reset();
	obs_wait_for_destroy_queue();
}

TEST_CASE_METHOD(ObsFixture, "a closed sequence goes back to live and forgets its list until opened", "[obs][sequence]")
{
	registerReplaySource({});
	const UiTasks ui;
	ReplayLibrary library;
	std::optional<SequencePlayer> sequence(std::in_place, library);
	REQUIRE(sequence->player());

	const Captured captured(library, {"a", "b"}, 120);
	sequence->setEntries({{"b", true}, {"a", false}});
	REQUIRE(sequence->putOnAir(captured.id()));
	CHECK(playing(*sequence) == "b");

	sequence->close();
	CHECK(sequence->closed());
	CHECK(sequence->state().phase() == AirPhase::Live);
	CHECK(sequence->state().sequence().entries().empty());
	REQUIRE(playerUntil(*sequence, [](const PlayerState &state) { return !state.loaded; }));
	CHECK_FALSE(sequence->putOnAir(captured.id()));
	CHECK(sequence->state().phase() == AirPhase::Live);

	sequence->open();
	CHECK_FALSE(sequence->closed());
	REQUIRE(sequence->putOnAir(captured.id()));
	CHECK(keysOf(*sequence) == std::vector<std::string>{"a", "b"});
	CHECK(playing(*sequence) == "a");

	sequence.reset();
	obs_wait_for_destroy_queue();
}

TEST_CASE_METHOD(ObsFixture, "a replay put on air while written plays its next sources from its files",
		 "[obs][sequence]")
{
	// Declared before the source, whose decoding thread writes it until it is destroyed.
	std::atomic<int64_t> newest{-1};
	registerReplaySource(
		[&newest](bool) -> std::unique_ptr<PictureDecoder> { return std::make_unique<NewestDecoder>(newest); });
	const UiTasks ui;
	const TempDirectory folder;
	ReplayLibrary library;
	std::optional<SequencePlayer> sequence(std::in_place, library);
	REQUIRE(sequence->player());

	Gops first = makeGops(120);
	Gops second = makeGops(120, 1000);
	std::optional<tapeloop::ReplayCapture> capture(
		captureOf(folder.path(), {sourceOf("a", first), sourceOf("b", second)}));
	const uint64_t id = library.addCaptured(*capture, "Broadcast");
	REQUIRE(sequence->putOnAir(id));
	CHECK(playing(*sequence) == "a");

	tapeloop::WrittenReplay written = tapeloop::ReplayWriter().write(*capture);
	REQUIRE(library.stored(id, written.manifest, std::move(written.index)));
	// The buffers let go of the GOPs.
	capture.reset();
	first.clear();
	second.clear();
	sequence->skip();
	CHECK(playing(*sequence) == "b");
	CHECK(waitFor([&] { return newest.load() >= 1000; }, 10s));

	sequence.reset();
	obs_wait_for_destroy_queue();
}
