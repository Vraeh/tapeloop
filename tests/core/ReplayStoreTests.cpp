// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/ReplayStore.hpp"

#include "StoredReplays.hpp"
#include "StoreStall.hpp"
#include "SyntheticEncoder.hpp"
#include "TempDirectory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using tapeloop::Clip;
using tapeloop::Gop;
using tapeloop::KeyedBuffer;
using tapeloop::Nanoseconds;
using tapeloop::ReplayStore;
using tapeloop::SourceBuffer;
using tapeloop::StoreResult;
using tapeloop::test::captureOf;
using tapeloop::test::fileBytes;
using tapeloop::test::Gops;
using tapeloop::test::makeGops;
using tapeloop::test::readManifest;
using tapeloop::test::requireReadsBack;
using tapeloop::test::slice;
using tapeloop::test::sourceOf;
using tapeloop::test::StoreStall;
using tapeloop::test::SyntheticEncoder;
using tapeloop::test::TempDirectory;

namespace fs = std::filesystem;

namespace {

size_t bytesOf(const Gops &gops)
{
	size_t bytes = 0;
	for (const std::shared_ptr<const Gop> &gop : gops) {
		bytes += gop->byteSize();
	}
	return bytes;
}

// Asks for a capture that holds the store's thread once it is written, and returns when
// it does: what is asked for next waits.
void stallStore(ReplayStore &store, const StoreStall &stall, const fs::path &folder)
{
	{
		Gops gops = makeGops(2);
		gops.back() = stall.hold(gops.back());
		store.write(captureOf(folder, {sourceOf("a", gops)}, "stalled"));
	}
	stall.waitReached();
}

} // namespace

TEST_CASE("a capture is written in the background and reported once")
{
	TempDirectory dir;
	const Gops gops = makeGops(4);
	ReplayStore store;
	const uint64_t ticket = store.write(captureOf(dir.path() / "Liga", {sourceOf("a", gops)}, "one"));
	CHECK(ticket == 1);
	store.waitUntilIdle();
	CHECK(store.pending() == 0);

	const std::vector<StoreResult> results = store.poll();
	REQUIRE(results.size() == 1);
	const StoreResult &result = results.front();
	CHECK(result.ticket == ticket);
	CHECK(result.kind == StoreResult::Kind::Capture);
	CHECK(result.error.empty());
	REQUIRE(result.written);
	CHECK(result.written->manifest == dir.path() / "Liga" / "one.tplp");
	CHECK(result.written->gopsAppended == 4);
	const std::optional<tapeloop::ReplayIndex> index = readManifest(result.written->manifest);
	REQUIRE(index);
	requireReadsBack(result.written->manifest, index->sources[0], gops);
	CHECK(store.poll().empty());
}

TEST_CASE("jobs run in the order they were asked for")
{
	TempDirectory dir;
	const Gops gops = makeGops(6);
	ReplayStore store;
	const uint64_t first = store.write(captureOf(dir.path() / "Liga", {sourceOf("a", slice(gops, 0, 4))}, "one"));
	const uint64_t second = store.write(captureOf(dir.path() / "Liga", {sourceOf("a", slice(gops, 2, 6))}, "two"));
	const uint64_t tags = store.writeTags(dir.path() / "Liga" / "one.tplp", {"Goal"});
	const uint64_t scan = store.scan(dir.path());
	store.waitUntilIdle();

	const std::vector<StoreResult> results = store.poll();
	REQUIRE(results.size() == 4);
	CHECK(results[0].ticket == first);
	CHECK(results[1].ticket == second);
	REQUIRE(results[1].written);
	CHECK(results[1].written->gopsShared == 2);
	CHECK(results[2].ticket == tags);
	CHECK(results[2].kind == StoreResult::Kind::Tags);
	CHECK(results[2].error.empty());
	CHECK(results[2].manifest == dir.path() / "Liga" / "one.tplp");
	CHECK(results[3].ticket == scan);
	REQUIRE(results[3].scan);
	REQUIRE(results[3].scan->replays.size() == 2);
	CHECK(results[3].scan->replays[0].tags == std::vector<std::string>{"Goal"});
	CHECK(results[3].scan->replays[1].intact);
}

TEST_CASE("a failed job says why and the store goes on")
{
	TempDirectory dir;
	ReplayStore store;
	tapeloop::ReplayCapture empty = captureOf(dir.path(), {sourceOf("a", makeGops(1))});
	empty.sources.front().clip = Clip();
	store.write(std::move(empty));
	store.writeTags(dir.path() / "missing.tplp", {"Goal"});
	store.write(captureOf(dir.path(), {sourceOf("a", makeGops(2))}));
	store.waitUntilIdle();

	const std::vector<StoreResult> results = store.poll();
	REQUIRE(results.size() == 3);
	CHECK(results[0].error == "a replay needs a source with frames");
	CHECK_FALSE(results[0].written);
	CHECK_FALSE(results[1].error.empty());
	CHECK(results[1].manifest == dir.path() / "missing.tplp");
	CHECK(results[2].error.empty());
	CHECK(results[2].written);
}

TEST_CASE("a capture lets go of its GOPs once written")
{
	TempDirectory dir;
	std::vector<std::weak_ptr<const Gop>> held;
	ReplayStore store;
	{
		const Gops gops = makeGops(3);
		for (const std::shared_ptr<const Gop> &gop : gops) {
			held.push_back(gop);
		}
		store.write(captureOf(dir.path(), {sourceOf("a", gops)}));
	}
	store.waitUntilIdle();
	for (const std::weak_ptr<const Gop> &gop : held) {
		CHECK(gop.expired());
	}
	CHECK(store.poll().size() == 1);
}

TEST_CASE("a capture's files are set before it lets go of its GOPs")
{
	TempDirectory dir;
	const auto files = std::make_shared<tapeloop::ReplayFiles>();
	// 1 once the store has let go of the capture's last GOP, 2 when its files were set by
	// then.
	std::atomic<int> freed{0};
	const Gops gops = makeGops(3);
	ReplayStore store;
	{
		Gops captured = gops;
		captured.back() = std::shared_ptr<const Gop>(gops.back().get(), [&files, &freed](const Gop *) {
			freed = files->get() ? 2 : 1;
		});
		store.write(captureOf(dir.path(), {sourceOf("a", captured)}), files);
	}
	store.waitUntilIdle();
	CHECK(freed == 2);

	const std::vector<StoreResult> results = store.poll();
	REQUIRE(results.size() == 1);
	REQUIRE(results[0].written);
	const std::shared_ptr<const tapeloop::ReplayFiles::Files> set = files->get();
	REQUIRE(set);
	CHECK(set->manifest == results[0].written->manifest);
	REQUIRE(set->index);
	REQUIRE(set->index->sources.size() == 1);
	requireReadsBack(set->manifest, set->index->sources[0], gops);
}

TEST_CASE("the oldest captures waiting to be written are dropped first")
{
	TempDirectory dir;
	ReplayStore store;
	StoreStall stall;
	stallStore(store, stall, dir.path());

	std::vector<std::weak_ptr<const Gop>> held;
	size_t newest = 0;
	const auto capture = [&](int64_t firstFrame, const std::string &stem) {
		const Gops gops = makeGops(3, firstFrame);
		held.push_back(gops.front());
		newest = bytesOf(gops);
		return store.write(captureOf(dir.path(), {sourceOf("a", gops)}, stem));
	};
	const uint64_t one = capture(100, "one");
	const uint64_t tags = store.writeTags(dir.path() / "stalled.tplp", {"Goal"});
	const uint64_t two = capture(200, "two");
	const uint64_t three = capture(300, "three");
	CHECK(store.pending() == 5);
	CHECK(store.heldBytes({}) == 3 * newest);

	// Room for one capture: the two oldest go, and the tag edit between them stays.
	CHECK(store.dropWaiting(newest, {}) == 2);
	CHECK(store.pending() == 3);
	CHECK(store.heldBytes({}) == newest);
	CHECK(held[0].expired());
	CHECK(held[1].expired());
	CHECK_FALSE(held[2].expired());
	const std::vector<StoreResult> dropped = store.poll();
	REQUIRE(dropped.size() == 2);
	CHECK(dropped[0].ticket == one);
	CHECK(dropped[1].ticket == two);
	for (const StoreResult &result : dropped) {
		CHECK(result.kind == StoreResult::Kind::Capture);
		CHECK(result.error == ReplayStore::kDropped);
		CHECK_FALSE(result.written);
	}

	// The capture being written and the newest one stay, however little room is left.
	CHECK(store.dropWaiting(0, {}) == 0);
	CHECK(store.poll().empty());

	stall.release();
	store.waitUntilIdle();
	CHECK(store.pending() == 0);
	CHECK(held[2].expired());
	const std::vector<StoreResult> results = store.poll();
	REQUIRE(results.size() == 3);
	CHECK(results[0].ticket == 1);
	CHECK(results[0].written);
	CHECK(results[1].ticket == tags);
	CHECK(results[1].error.empty());
	CHECK(results[2].ticket == three);
	CHECK(results[2].written);
	CHECK(fs::exists(dir.path() / "stalled.tplp"));
	CHECK_FALSE(fs::exists(dir.path() / "one.tplp"));
	CHECK_FALSE(fs::exists(dir.path() / "two.tplp"));
	CHECK(fs::exists(dir.path() / "three.tplp"));
}

TEST_CASE("captures waiting to be written count what their buffers no longer hold")
{
	TempDirectory dir;
	SyntheticEncoder encoder({});
	tapeloop::SourceBufferConfig config;
	config.window = 1h;
	config.maxBytes = std::numeric_limits<size_t>::max();
	config.frameDuration = encoder.frameDuration();
	SourceBuffer buffer(config);
	for (int i = 0; i < 75; ++i) {
		buffer.push(encoder.next());
	}
	ReplayStore store;
	StoreStall stall;
	stallStore(store, stall, dir.path());

	// Two sealed GOPs and a copy of the one being built, shared by both captures.
	const Clip clip = buffer.clip(Nanoseconds::min(), Nanoseconds::max());
	REQUIRE(clip.gops().size() == 3);
	const Gops gops(clip.gops().begin(), clip.gops().end());
	store.write(captureOf(dir.path(), {{"a", "Camera a", clip}}, "one"));
	store.write(captureOf(dir.path(), {{"a", "Camera a", clip}}, "two"));

	const std::vector<KeyedBuffer> buffers{{"a", &buffer}};
	const std::vector<KeyedBuffer> others{{"b", &buffer}};
	CHECK(store.heldBytes(buffers) == gops.back()->byteSize());
	CHECK(store.heldBytes(others) == bytesOf(gops));
	CHECK(store.heldBytes({}) == bytesOf(gops));
	buffer.clear();
	CHECK(store.heldBytes(buffers) == bytesOf(gops));
}

TEST_CASE("closing the store finishes the captures still asked for")
{
	TempDirectory dir;
	const Gops gops = makeGops(8);
	{
		ReplayStore store;
		for (size_t i = 0; i < 8; ++i) {
			store.write(captureOf(dir.path(), {sourceOf("a", slice(gops, 0, i + 1))}, std::to_string(i)));
		}
		store.writeTags(dir.path() / "7.tplp", {"Last"});
		store.scan(dir.path());
	}
	for (size_t i = 0; i < 8; ++i) {
		const fs::path manifest = dir.path() / (std::to_string(i) + ".tplp");
		const std::optional<tapeloop::ReplayIndex> index = readManifest(manifest);
		REQUIRE(index);
		requireReadsBack(manifest, index->sources[0], slice(gops, 0, i + 1));
	}
	CHECK(tapeloop::decodeTags(fileBytes(dir.path() / "7.tplp")) == std::vector<std::string>{"Last"});
}

TEST_CASE("the store takes jobs and gives results from other threads")
{
	TempDirectory dir;
	const Gops gops = makeGops(4);
	ReplayStore store;
	std::atomic<bool> done{false};
	std::thread capturing([&] {
		for (int i = 0; i < 20; ++i) {
			store.write(captureOf(dir.path(), {sourceOf("a", gops)}, "replay"));
		}
		done = true;
	});
	size_t results = 0;
	while (!done || store.pending() != 0) {
		for (const StoreResult &result : store.poll()) {
			CHECK(result.error.empty());
			++results;
		}
		std::this_thread::yield();
	}
	capturing.join();
	store.waitUntilIdle();
	results += store.poll().size();
	CHECK(results == 20);
	CHECK(fs::exists(dir.path() / "replay (20).tplp"));
}

TEST_CASE("the store's thread writes at a low I/O priority")
{
	ReplayStore store;
	TempDirectory dir;
	// Once a job has run, the thread has tried.
	store.scan(dir.path());
	store.waitUntilIdle();
#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
	CHECK(store.lowPriority() == true);
#else
	CHECK(store.lowPriority() == false);
#endif
}
