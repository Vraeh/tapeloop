/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com> */

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What the tests build obs_log with in place of malloc and free. */
void *tapeloopTestMalloc(size_t size);
void tapeloopTestFree(void *pointer);

#ifdef __cplusplus
}
#endif
