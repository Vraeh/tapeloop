// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "AllocationSeam.h"

#include <plugin-support.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

// LOG_WARNING and LOG_INFO of util/base.h, which the tests do not have.
constexpr int kLogWarning = 200;
constexpr int kLogInfo = 300;

struct LoggedLine {
	int level;
	std::string text;
};

std::vector<LoggedLine> loggedLines;
bool failNextAllocation = false;
int liveAllocations = 0;

std::string prefixed(const std::string &text)
{
	return std::string("[") + PLUGIN_NAME + "] " + text;
}

} // namespace

// Stands in for libobs, which the plugin's obs_log hands every line to.
extern "C" void blogva(int log_level, const char *format, va_list args)
{
	va_list measured;
	va_copy(measured, args);
	const int length = std::vsnprintf(nullptr, 0, format, measured);
	va_end(measured);
	// A throw from here would cross obs_log, which is C, so a failure is only recorded.
	if (length < 0) {
		loggedLines.push_back({log_level, "unformattable line"});
		return;
	}
	std::string text(static_cast<size_t>(length), '\0');
	std::vsnprintf(text.data(), text.size() + 1, format, args);
	loggedLines.push_back({log_level, text});
}

extern "C" void *tapeloopTestMalloc(size_t size)
{
	if (failNextAllocation) {
		failNextAllocation = false;
		return nullptr;
	}
	void *pointer = std::malloc(size);
	if (pointer) {
		++liveAllocations;
	}
	return pointer;
}

extern "C" void tapeloopTestFree(void *pointer)
{
	if (pointer) {
		--liveAllocations;
	}
	std::free(pointer);
}

TEST_CASE("obs_log prefixes a line with the plugin's name")
{
	loggedLines.clear();
	obs_log(kLogInfo, "version %s n=%d", "1.2", 7);
	REQUIRE(loggedLines.size() == 1);
	CHECK(loggedLines[0].level == kLogInfo);
	CHECK(loggedLines[0].text == prefixed("version 1.2 n=7"));
	CHECK(liveAllocations == 0);
}

TEST_CASE("obs_log logs a line unprefixed when the prefixed format cannot be allocated")
{
	loggedLines.clear();
	failNextAllocation = true;
	obs_log(kLogWarning, "version %s n=%d", "1.2", 7);
	CHECK_FALSE(failNextAllocation);
	REQUIRE(loggedLines.size() == 1);
	CHECK(loggedLines[0].level == kLogWarning);
	CHECK(loggedLines[0].text == "version 1.2 n=7");
	CHECK(liveAllocations == 0);

	// The failure is not remembered: the next line has its prefix again.
	obs_log(kLogInfo, "%d%%", 50);
	REQUIRE(loggedLines.size() == 2);
	CHECK(loggedLines[1].text == prefixed("50%"));
	CHECK(liveAllocations == 0);
}
