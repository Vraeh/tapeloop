// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include <util/base.h>

#include <cstdarg>
#include <cstring>

namespace tapeloop::test {

// Counts the log lines whose format holds `text`, and passes every line on.
struct LogCounter {
	const char *text;
	int lines = 0;
	log_handler_t previous = nullptr;
	void *previousParam = nullptr;

	explicit LogCounter(const char *counted) : text(counted)
	{
		base_get_log_handler(&previous, &previousParam);
		base_set_log_handler(count, this);
	}
	~LogCounter() { base_set_log_handler(previous, previousParam); }
	LogCounter(const LogCounter &) = delete;
	LogCounter &operator=(const LogCounter &) = delete;

	static void count(int level, const char *format, va_list args, void *param)
	{
		auto *counter = static_cast<LogCounter *>(param);
		if (std::strstr(format, counter->text)) {
			++counter->lines;
		}
		counter->previous(level, format, args, counter->previousParam);
	}
};

} // namespace tapeloop::test
