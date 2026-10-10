// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/ReplayNames.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <ctime>
#include <optional>
#include <string>

using tapeloop::LocalTime;
using tapeloop::safeFileName;

TEST_CASE("a safe file name keeps what every file system accepts")
{
	CHECK(safeFileName("Liga 2026") == "Liga 2026");
	CHECK(safeFileName("F\xC3\xBAtbol \xE2\x9A\xBD") == "F\xC3\xBAtbol \xE2\x9A\xBD");
	CHECK(safeFileName("2b6f3c1e-0d4a-4c47-9a51-7f1f4c2b9e10") == "2b6f3c1e-0d4a-4c47-9a51-7f1f4c2b9e10");
	CHECK(safeFileName("a.b") == "a.b");
}

TEST_CASE("a safe file name replaces what some file system refuses")
{
	CHECK(safeFileName("Final: A/B") == "Final_ A_B");
	CHECK(safeFileName("<>:\"/\\|?*") == "_________");
	CHECK(safeFileName(std::string("tab\there\nnull\0end", 17)) == "tab_here_null_end");
	CHECK(safeFileName("del\x7F") == "del_");
	CHECK(safeFileName(".hidden") == "_hidden");
}

TEST_CASE("a safe file name replaces bytes that are not UTF-8")
{
	// A lone continuation byte, a lead byte without its continuation, an overlong slash,
	// a surrogate, and a code point past U+10FFFF.
	CHECK(safeFileName("a\x80z") == "a_z");
	CHECK(safeFileName("a\xC3z") == "a_z");
	CHECK(safeFileName("a\xC0\xAFz") == "a__z");
	CHECK(safeFileName("a\xED\xA0\x80z") == "a___z");
	CHECK(safeFileName("a\xF4\x90\x80\x80z") == "a____z");
	CHECK(safeFileName("a\xE0\x80\xAFz") == "a___z");
	CHECK(safeFileName("a\xF0\x80\x80\x80z") == "a____z");
	CHECK(safeFileName("end\xE2\x9A") == "end__");
	CHECK(safeFileName("\xF0\x9F\x8E\xA5") == "\xF0\x9F\x8E\xA5");
}

TEST_CASE("a safe file name drops what Windows drops at the end")
{
	CHECK(safeFileName("Match. ") == "Match");
	CHECK(safeFileName("...") == "_");
	CHECK(safeFileName("") == "_");
	CHECK(safeFileName("   ") == "_");
	CHECK(safeFileName(" leading") == " leading");
}

TEST_CASE("a safe file name never names a Windows device")
{
	CHECK(safeFileName("CON") == "_CON");
	CHECK(safeFileName("nul.txt") == "_nul.txt");
	CHECK(safeFileName("Com7") == "_Com7");
	CHECK(safeFileName("lpt9.tplp") == "_lpt9.tplp");
	CHECK(safeFileName("COM0") == "COM0");
	CHECK(safeFileName("CONSOLE") == "CONSOLE");
	CHECK(safeFileName("LPT") == "LPT");
}

TEST_CASE("a safe file name is cut at a character boundary")
{
	const std::string ascii(200, 'a');
	CHECK(safeFileName(ascii) == std::string(120, 'a'));
	// 119 bytes, then a two-byte character that would end at byte 121.
	const std::string accented = std::string(119, 'a') + "\xC3\xBA" + "b";
	CHECK(safeFileName(accented) == std::string(119, 'a'));
	const std::string fits = std::string(118, 'a') + "\xC3\xBA" + "b";
	CHECK(safeFileName(fits) == std::string(118, 'a') + "\xC3\xBA");
	CHECK(safeFileName(std::string(119, 'a') + "  .x") == std::string(119, 'a'));
}

TEST_CASE("a broadcast folder is named after the collection and the minute")
{
	const LocalTime time{2026, 10, 9, 21, 5, 42};
	CHECK(tapeloop::broadcastFolderName("Liga", time) == "Liga 2026-10-09 21-05");
	CHECK(tapeloop::broadcastFolderName("Final: A/B", time) == "Final_ A_B 2026-10-09 21-05");
	CHECK(tapeloop::broadcastFolderName("", LocalTime{2027, 1, 2, 3, 4, 5}) == "_ 2027-01-02 03-04");
}

TEST_CASE("a long collection name is shortened so that the date stays")
{
	const LocalTime time{2026, 10, 9, 21, 5, 42};
	const std::string name = tapeloop::broadcastFolderName(std::string(110, 'a'), time);
	CHECK(name == std::string(103, 'a') + " 2026-10-09 21-05");
	CHECK(name.size() == tapeloop::kMaxFileNameBytes);
	// Cut at a character boundary, and without the spaces it would end with.
	const std::string accented = std::string(102, 'a') + "\xC3\xBA" + "bbb";
	CHECK(tapeloop::broadcastFolderName(accented, time) == std::string(102, 'a') + " 2026-10-09 21-05");
	CHECK(tapeloop::broadcastFolderName(std::string(101, 'a') + "  x", time) ==
	      std::string(101, 'a') + " 2026-10-09 21-05");
}

TEST_CASE("a safe file name stays within the limit asked for")
{
	CHECK(safeFileName("abcdef", 3) == "abc");
	CHECK(safeFileName("CON", 3) == "_CO");
	CHECK(safeFileName("nul.\xC3\xBA", 6) == "_nul");
	CHECK(safeFileName("nul.\xC3\xBA", 7) == "_nul.\xC3\xBA");
	CHECK(safeFileName("", 1) == "_");
	CHECK(safeFileName("abc", 0) == "_");
}

TEST_CASE("a replay is named after the second it was captured")
{
	CHECK(tapeloop::replayFileStem(LocalTime{2026, 10, 9, 21, 5, 42}) == "2026-10-09 21-05-42");
	CHECK(tapeloop::replayFileStem(LocalTime{2026, 1, 2, 3, 4, 5}) == "2026-01-02 03-04-05");
}

TEST_CASE("segments are numbered per source")
{
	CHECK(tapeloop::segmentFileName("cam-1", 7) == "cam-1-000007.tpls");
	CHECK(tapeloop::segmentFileName("cam/1", 1234567) == "cam_1-1234567.tpls");
	CHECK(tapeloop::segmentSequenceOf("cam-1-000007.tpls", "cam-1") == 7u);
	CHECK(tapeloop::segmentSequenceOf("cam_1-1234567.tpls", "cam/1") == 1234567u);
	CHECK(tapeloop::segmentSequenceOf("cam-1-4294967295.tpls", "cam-1") == 4294967295u);
	CHECK_FALSE(tapeloop::segmentSequenceOf("cam-1-4294967296.tpls", "cam-1"));
	CHECK_FALSE(tapeloop::segmentSequenceOf("cam-1-.tpls", "cam-1"));
	CHECK_FALSE(tapeloop::segmentSequenceOf("cam-1-00x7.tpls", "cam-1"));
	CHECK_FALSE(tapeloop::segmentSequenceOf("cam-1-000007.tplp", "cam-1"));
	CHECK_FALSE(tapeloop::segmentSequenceOf("cam-2-000007.tpls", "cam-1"));
	// Another source whose key starts with this one's.
	CHECK_FALSE(tapeloop::segmentSequenceOf("cam-1-2-000007.tpls", "cam-1"));
}

TEST_CASE("a time reads as the local clock shows it")
{
	std::tm parts{};
	parts.tm_year = 2026 - 1900;
	parts.tm_mon = 9;
	parts.tm_mday = 9;
	parts.tm_hour = 21;
	parts.tm_min = 5;
	parts.tm_sec = 42;
	parts.tm_isdst = -1;
	const std::time_t seconds = std::mktime(&parts);
	REQUIRE(seconds != static_cast<std::time_t>(-1));
	const auto time = std::chrono::system_clock::from_time_t(seconds) + std::chrono::milliseconds(999);
	CHECK(tapeloop::localTimeOf(time) == LocalTime{2026, 10, 9, 21, 5, 42});
}
