// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include <util/base.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace tapeloop::test {

// Counts the log lines whose format holds `text`, keeps the last of them, and passes
// every line on.
struct LogCounter {
	const char *text;
	int lines = 0;
	log_handler_t previous = nullptr;
	void *previousParam = nullptr;
	std::mutex lastMutex;
	std::string last;

	explicit LogCounter(const char *counted) : text(counted)
	{
		base_get_log_handler(&previous, &previousParam);
		base_set_log_handler(count, this);
	}
	~LogCounter() { base_set_log_handler(previous, previousParam); }
	LogCounter(const LogCounter &) = delete;
	LogCounter &operator=(const LogCounter &) = delete;

	std::string lastLine()
	{
		const std::lock_guard lock(lastMutex);
		return last;
	}

	static void count(int level, const char *format, va_list args, void *param)
	{
		auto *counter = static_cast<LogCounter *>(param);
		if (std::strstr(format, counter->text)) {
			++counter->lines;
			char line[1024];
			va_list copy;
			va_copy(copy, args);
			std::vsnprintf(line, sizeof(line), format, copy);
			va_end(copy);
			const std::lock_guard lock(counter->lastMutex);
			counter->last = line;
		}
		counter->previous(level, format, args, counter->previousParam);
	}
};

} // namespace tapeloop::test
