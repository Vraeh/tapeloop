/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com> */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

/* util/base.h defines and then undefines its own PRINTFATTR, so it cannot be reused here. */
#if defined(__GNUC__) || defined(__clang__)
#define TAPELOOP_PRINTF_ATTR(fmt, args) __attribute__((format(printf, fmt, args)))
#define TAPELOOP_FORMAT_STRING
#elif defined(_MSC_VER)
#include <sal.h>
#define TAPELOOP_PRINTF_ATTR(fmt, args)
#define TAPELOOP_FORMAT_STRING _Printf_format_string_
#else
#define TAPELOOP_PRINTF_ATTR(fmt, args)
#define TAPELOOP_FORMAT_STRING
#endif

extern const char *PLUGIN_NAME;
extern const char *PLUGIN_VERSION;

void obs_log(int log_level, TAPELOOP_FORMAT_STRING const char *format, ...) TAPELOOP_PRINTF_ATTR(2, 3);
extern void blogva(int log_level, const char *format, va_list args);

#ifdef __cplusplus
}
#endif
