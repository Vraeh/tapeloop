// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

// Built as it is, which must succeed, and once with each call that must not compile.
#include <plugin-support.h>

namespace {

// LOG_INFO of util/base.h, which the tests do not have.
constexpr int kLogInfo = 300;

} // namespace

void obsLogFormatCheck(const char *fromFile, int count)
{
#if defined(TAPELOOP_BAD_NONLITERAL_FORMAT)
	obs_log(kLogInfo, fromFile);
#elif defined(TAPELOOP_BAD_WRONG_TYPE)
	obs_log(kLogInfo, "count %s", count);
#elif defined(TAPELOOP_BAD_MISSING_ARGUMENT)
	obs_log(kLogInfo, "count %d of %s", count);
#else
	obs_log(kLogInfo, "%s", fromFile);
	obs_log(kLogInfo, "count %d of %s", count, fromFile);
#endif
}
