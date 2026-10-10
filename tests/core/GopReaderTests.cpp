// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/GopReader.hpp"

#include "core/FileIo.hpp"
#include "core/ReplayWriter.hpp"

#include "StoredReplays.hpp"
#include "TempDirectory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

using tapeloop::File;
using tapeloop::Gop;
using tapeloop::GopReader;
using tapeloop::ReplayIndex;
using tapeloop::ReplayWriter;
using tapeloop::StoredSource;
using tapeloop::WrittenReplay;
using tapeloop::test::captureOf;
using tapeloop::test::Gops;
using tapeloop::test::makeGops;
using tapeloop::test::sameGop;
using tapeloop::test::sourceOf;
using tapeloop::test::TempDirectory;

namespace fs = std::filesystem;

namespace {

struct Stored {
	WrittenReplay written;
	const StoredSource &source() const { return written.index.sources.front(); }
	const fs::path &manifest() const { return written.manifest; }
};

Stored store(const fs::path &folder, const Gops &gops)
{
	return {ReplayWriter().write(captureOf(folder, {sourceOf("a", gops)}))};
}

fs::path segmentOf(const Stored &stored, size_t gop)
{
	return tapeloop::segmentPath(stored.manifest(), stored.source(), stored.source().gops.at(gop).segment);
}

} // namespace

TEST_CASE("a GOP comes from the buffer while the buffer holds it")
{
	TempDirectory dir;
	const Gops gops = makeGops(3);
	const Stored stored = store(dir.path(), gops);
	fs::remove(segmentOf(stored, 0));
	GopReader reader;
	CHECK(reader.read(stored.manifest(), stored.source(), 1, gops[1]) == gops[1]);
	CHECK(reader.stats().live == 1);
	CHECK(reader.cachedBytes() == 0);
}

TEST_CASE("a GOP the buffer let go of is read from its segment")
{
	TempDirectory dir;
	const Gops gops = makeGops(3);
	const Stored stored = store(dir.path(), gops);
	std::weak_ptr<const Gop> expired = makeGops(1).front();
	REQUIRE(expired.expired());
	GopReader reader;
	for (size_t i = 0; i < gops.size(); ++i) {
		const std::shared_ptr<const Gop> read = reader.read(stored.manifest(), stored.source(), i, expired);
		REQUIRE(read);
		CHECK(read != gops[i]);
		CHECK(sameGop(*read, *gops[i]));
	}
	CHECK(reader.stats().read == 3);
	CHECK(reader.stats().live == 0);
}

TEST_CASE("a GOP read again comes from the cache")
{
	TempDirectory dir;
	const Gops gops = makeGops(2);
	const Stored stored = store(dir.path(), gops);
	GopReader reader;
	const std::shared_ptr<const Gop> first = reader.read(stored.manifest(), stored.source(), 1);
	REQUIRE(first);
	CHECK(reader.cachedBytes() == first->byteSize());
	fs::remove(segmentOf(stored, 1));
	CHECK(reader.read(stored.manifest(), stored.source(), 1) == first);
	CHECK(reader.stats().cached == 1);
	CHECK(reader.stats().read == 1);
}

TEST_CASE("the cache stays within its size and keeps the GOPs read last")
{
	TempDirectory dir;
	const Gops gops = makeGops(4);
	const Stored stored = store(dir.path(), gops);
	GopReader reader(2 * gops[0]->byteSize());
	for (size_t i = 0; i < 3; ++i) {
		REQUIRE(reader.read(stored.manifest(), stored.source(), i));
	}
	CHECK(reader.cachedBytes() == 2 * gops[0]->byteSize());
	// Reading GOP 1 again makes GOP 2 the oldest, so GOP 3 pushes GOP 2 out.
	REQUIRE(reader.read(stored.manifest(), stored.source(), 1));
	REQUIRE(reader.read(stored.manifest(), stored.source(), 3));
	fs::remove(segmentOf(stored, 0));
	CHECK(reader.read(stored.manifest(), stored.source(), 1));
	CHECK(reader.read(stored.manifest(), stored.source(), 3));
	CHECK_FALSE(reader.read(stored.manifest(), stored.source(), 2));
	CHECK_FALSE(reader.read(stored.manifest(), stored.source(), 0));
	CHECK(reader.stats().failed == 2);
}

TEST_CASE("a cache too small for one GOP still keeps the last one read")
{
	TempDirectory dir;
	const Gops gops = makeGops(2);
	const Stored stored = store(dir.path(), gops);
	GopReader reader(0);
	REQUIRE(reader.read(stored.manifest(), stored.source(), 0));
	const std::shared_ptr<const Gop> last = reader.read(stored.manifest(), stored.source(), 1);
	REQUIRE(last);
	CHECK(reader.cachedBytes() == last->byteSize());
	CHECK(reader.read(stored.manifest(), stored.source(), 1) == last);
}

TEST_CASE("a GOP whose segment is damaged or gone is not read")
{
	TempDirectory dir;
	const Gops gops = makeGops(4);
	const Stored stored = store(dir.path(), gops);
	const tapeloop::StoredGop &third = stored.source().gops[2];
	const fs::path segment = segmentOf(stored, 0);

	SECTION("cut short")
	{
		fs::resize_file(segment, third.offset + third.size / 2);
		GopReader reader;
		CHECK(reader.read(stored.manifest(), stored.source(), 1));
		CHECK_FALSE(reader.read(stored.manifest(), stored.source(), 2));
		CHECK_FALSE(reader.read(stored.manifest(), stored.source(), 3));
	}
	SECTION("a byte of its data changed")
	{
		File(segment, File::Mode::ReadWrite).writeAt(third.offset + third.size - 1, std::vector<uint8_t>{0x5A});
		GopReader reader;
		CHECK_FALSE(reader.read(stored.manifest(), stored.source(), 2));
		CHECK(reader.read(stored.manifest(), stored.source(), 3));
	}
	SECTION("gone")
	{
		fs::remove(segment);
		GopReader reader;
		CHECK_FALSE(reader.read(stored.manifest(), stored.source(), 0));
	}
	SECTION("not the chunk the manifest meant")
	{
		StoredSource changed = stored.source();
		changed.gops[2].offset = changed.gops[3].offset;
		GopReader reader;
		CHECK_FALSE(reader.read(stored.manifest(), changed, 2));
		changed = stored.source();
		changed.gops[2].packetCount += 1;
		CHECK_FALSE(reader.read(stored.manifest(), changed, 2));
	}
	SECTION("larger than the segment holds")
	{
		StoredSource changed = stored.source();
		changed.gops[1].size = uint64_t{1} << 40;
		changed.gops[2].offset = uint64_t{1} << 40;
		GopReader reader;
		CHECK_FALSE(reader.read(stored.manifest(), changed, 1));
		CHECK_FALSE(reader.read(stored.manifest(), changed, 2));
		CHECK(reader.read(stored.manifest(), changed, 3));
	}
	SECTION("a size its chunk's header does not give")
	{
		StoredSource changed = stored.source();
		changed.gops[1].size += tapeloop::kReplayAlignment;
		GopReader reader;
		CHECK_FALSE(reader.read(stored.manifest(), changed, 1));
		CHECK(reader.read(stored.manifest(), changed, 2));
	}
	SECTION("past the end of the replay")
	{
		GopReader reader;
		CHECK_FALSE(reader.read(stored.manifest(), stored.source(), 4));
		StoredSource changed = stored.source();
		changed.gops[0].run = 5;
		CHECK_FALSE(reader.read(stored.manifest(), changed, 0));
		CHECK(reader.stats().failed == 2);
	}
}

TEST_CASE("a self-contained replay is read from its own file, its index without its GOPs")
{
	TempDirectory dir;
	const Gops gops = makeGops(3);
	const Stored stored = store(dir.path(), gops);
	ReplayIndex index = stored.written.index;
	std::vector<uint8_t> bytes(tapeloop::kManifestIndexOffset, 0);
	for (size_t i = 0; i < gops.size(); ++i) {
		const tapeloop::ChunkPlace place = tapeloop::appendGopChunk(bytes, 0, *gops[i]);
		tapeloop::StoredGop &gop = index.sources.front().gops[i];
		gop = {tapeloop::kOwnFile, gop.packetCount, place.offset, place.size, place.headerCrc, gop.run};
	}
	const std::vector<uint8_t> head = tapeloop::encodeManifestHead(index, {});
	std::copy(head.begin(), head.end(), bytes.begin());
	const std::vector<uint8_t> tail = tapeloop::encodeManifestTail(index, bytes.size());
	bytes.insert(bytes.end(), tail.begin(), tail.end());
	const fs::path path = dir.path() / "exported.tplp";
	File(path, File::Mode::CreateNew).writeAt(0, bytes);

	const std::optional<ReplayIndex> read = tapeloop::readReplayIndex(path);
	REQUIRE(read);
	REQUIRE(tapeloop::selfContained(*read));
	CHECK(tapeloop::segmentPath(path, read->sources.front(), tapeloop::kOwnFile) == path);
	// The segments it was copied from are not needed.
	fs::remove_all(dir.path() / "data");
	GopReader reader;
	for (size_t i = 0; i < gops.size(); ++i) {
		const std::shared_ptr<const Gop> gop = reader.read(path, read->sources.front(), i);
		REQUIRE(gop);
		CHECK(sameGop(*gop, *gops[i]));
	}
}
