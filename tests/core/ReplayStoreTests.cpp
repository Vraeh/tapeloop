// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/ReplayStore.hpp"

#include "StoredReplays.hpp"
#include "TempDirectory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using tapeloop::Clip;
using tapeloop::Gop;
using tapeloop::ReplayStore;
using tapeloop::StoreResult;
using tapeloop::test::captureOf;
using tapeloop::test::fileBytes;
using tapeloop::test::Gops;
using tapeloop::test::makeGops;
using tapeloop::test::readManifest;
using tapeloop::test::requireReadsBack;
using tapeloop::test::slice;
using tapeloop::test::sourceOf;
using tapeloop::test::TempDirectory;

namespace fs = std::filesystem;

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
