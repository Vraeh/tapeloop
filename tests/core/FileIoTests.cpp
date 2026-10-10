// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/FileIo.hpp"

#include "TempDirectory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

using tapeloop::File;
using tapeloop::test::TempDirectory;

namespace {

std::vector<uint8_t> pattern(size_t size, uint8_t seed)
{
	std::vector<uint8_t> bytes(size);
	for (size_t i = 0; i < size; ++i) {
		bytes[i] = static_cast<uint8_t>(seed + i * 7);
	}
	return bytes;
}

std::vector<uint8_t> readAll(File &file)
{
	std::vector<uint8_t> bytes(static_cast<size_t>(file.size()));
	REQUIRE(file.readAt(0, bytes) == bytes.size());
	return bytes;
}

} // namespace

TEST_CASE("a new file is written and read back at any offset")
{
	TempDirectory dir;
	const auto path = dir.path() / "segment";
	const std::vector<uint8_t> head = pattern(100, 1);
	const std::vector<uint8_t> tail = pattern(300, 2);
	File file(path, File::Mode::CreateNew);
	REQUIRE(file.isOpen());
	file.writeAt(0, head);
	file.writeAt(8192, tail);
	file.flush();
	REQUIRE(file.size() == 8192 + tail.size());

	std::vector<uint8_t> read(tail.size());
	REQUIRE(file.readAt(8192, read) == tail.size());
	CHECK(read == tail);
	read.resize(head.size());
	REQUIRE(file.readAt(0, read) == head.size());
	CHECK(read == head);
	// Nothing was written between them.
	std::vector<uint8_t> gap(16, 0xFF);
	REQUIRE(file.readAt(4096, gap) == gap.size());
	CHECK(gap == std::vector<uint8_t>(16, 0));
}

TEST_CASE("reading at the end of a file returns what is there")
{
	TempDirectory dir;
	File file(dir.path() / "short", File::Mode::CreateNew);
	file.writeAt(0, pattern(50, 3));
	std::vector<uint8_t> read(100);
	CHECK(file.readAt(40, read) == 10);
	CHECK(file.readAt(50, read) == 0);
	CHECK(file.readAt(5000, read) == 0);
}

TEST_CASE("a new file is never one that exists")
{
	TempDirectory dir;
	const auto path = dir.path() / "taken";
	{
		File first(path, File::Mode::CreateNew);
		first.writeAt(0, pattern(10, 4));
	}
	CHECK_THROWS_AS(File(path, File::Mode::CreateNew), std::system_error);
	File again(path, File::Mode::ReadOnly);
	CHECK(readAll(again) == pattern(10, 4));
}

TEST_CASE("opening a file that does not exist fails")
{
	TempDirectory dir;
	CHECK_THROWS_AS(File(dir.path() / "missing", File::Mode::ReadOnly), std::system_error);
	CHECK_THROWS_AS(File(dir.path() / "missing", File::Mode::ReadWrite), std::system_error);
	CHECK_THROWS_AS(File(dir.path() / "no-such-dir" / "file", File::Mode::CreateNew), std::system_error);
	CHECK_FALSE(std::filesystem::exists(dir.path() / "missing"));
}

TEST_CASE("a directory cannot be read as a file")
{
	TempDirectory dir;
	// Windows refuses to open it, POSIX systems refuse to read it.
	const auto readDirectory = [&] {
		File file(dir.path(), File::Mode::ReadOnly);
		std::vector<uint8_t> bytes(16);
		file.readAt(0, bytes);
	};
	CHECK_THROWS_AS(readDirectory(), std::system_error);
}

TEST_CASE("a file opened read-only refuses writes")
{
	TempDirectory dir;
	const auto path = dir.path() / "manifest";
	File(path, File::Mode::CreateNew).writeAt(0, pattern(20, 5));
	File file(path, File::Mode::ReadOnly);
	CHECK_THROWS_AS(file.writeAt(0, pattern(4, 6)), std::system_error);
	CHECK(readAll(file) == pattern(20, 5));
}

TEST_CASE("an existing file is written in place")
{
	TempDirectory dir;
	const auto path = dir.path() / "manifest";
	File(path, File::Mode::CreateNew).writeAt(0, pattern(4096 * 3, 7));
	{
		File file(path, File::Mode::ReadWrite);
		file.writeAt(4096, pattern(4096, 8));
		file.flush();
	}
	File file(path, File::Mode::ReadOnly);
	std::vector<uint8_t> expected = pattern(4096 * 3, 7);
	const std::vector<uint8_t> slot = pattern(4096, 8);
	std::copy(slot.begin(), slot.end(), expected.begin() + 4096);
	CHECK(readAll(file) == expected);
}

TEST_CASE("a reader sees what a writer appends while both are open")
{
	TempDirectory dir;
	const auto path = dir.path() / "segment";
	File writer(path, File::Mode::CreateNew);
	writer.writeAt(0, pattern(4096, 9));
	File reader(path, File::Mode::ReadOnly);
	CHECK(readAll(reader) == pattern(4096, 9));
	writer.writeAt(4096, pattern(4096, 10));
	std::vector<uint8_t> appended(4096);
	REQUIRE(reader.readAt(4096, appended) == appended.size());
	CHECK(appended == pattern(4096, 10));
}

TEST_CASE("a file moves its handle and closes once")
{
	TempDirectory dir;
	File first(dir.path() / "a", File::Mode::CreateNew);
	File moved(std::move(first));
	CHECK_FALSE(first.isOpen());
	CHECK(moved.isOpen());
	moved.writeAt(0, pattern(8, 11));

	File other(dir.path() / "b", File::Mode::CreateNew);
	other = std::move(moved);
	CHECK_FALSE(moved.isOpen());
	REQUIRE(other.isOpen());
	CHECK(readAll(other) == pattern(8, 11));
	other.close();
	other.close();
	CHECK_FALSE(other.isOpen());
	CHECK_FALSE(File().isOpen());
}

TEST_CASE("a rename replaces the file it lands on")
{
	TempDirectory dir;
	const auto part = dir.path() / "replay.tplp.part";
	const auto published = dir.path() / "replay.tplp";
	File(part, File::Mode::CreateNew).writeAt(0, pattern(30, 12));
	File(published, File::Mode::CreateNew).writeAt(0, pattern(60, 13));
	tapeloop::renameFile(part, published);
	CHECK_FALSE(std::filesystem::exists(part));
	File file(published, File::Mode::ReadOnly);
	CHECK(readAll(file) == pattern(30, 12));
	CHECK_THROWS_AS(tapeloop::renameFile(part, published), std::system_error);
}

TEST_CASE("a directory can be made durable")
{
	TempDirectory dir;
	std::filesystem::create_directory(dir.path() / "data");
	File(dir.path() / "data" / "segment", File::Mode::CreateNew).flush();
	CHECK_NOTHROW(tapeloop::syncDirectory(dir.path() / "data"));
#ifndef _WIN32
	CHECK_THROWS_AS(tapeloop::syncDirectory(dir.path() / "missing"), std::system_error);
#endif
}

TEST_CASE("paths keep their UTF-8 spelling")
{
	TempDirectory dir;
	const std::string name = "F\xC3\xBAtbol 2026-10-09 21-00";
	const std::filesystem::path path = dir.path() / tapeloop::pathFromUtf8(name);
	CHECK(tapeloop::utf8FromPath(path.filename()) == name);
	std::filesystem::create_directory(path);
	std::vector<std::string> found;
	for (const auto &entry : std::filesystem::directory_iterator(dir.path())) {
		found.push_back(tapeloop::utf8FromPath(entry.path().filename()));
	}
	CHECK(found == std::vector<std::string>{name});
	CHECK(tapeloop::utf8FromPath(tapeloop::pathFromUtf8("")).empty());
}

TEST_CASE("a writer thread can lower its I/O priority")
{
	bool lowered = false;
	std::thread([&] { lowered = tapeloop::lowerThreadIoPriority(); }).join();
#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
	CHECK(lowered);
#else
	CHECK_FALSE(lowered);
#endif
}
