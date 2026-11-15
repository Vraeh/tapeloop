/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com> */

/* The headers come first so that the names below change only in obs_log itself. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "AllocationSeam.h"

#define malloc tapeloopTestMalloc
#define free tapeloopTestFree

/* Configured from src/plugin-support.c.in, as the plugin's own build does. */
#include "plugin-support.c"
