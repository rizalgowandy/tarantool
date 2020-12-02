/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright 2010-2020, Tarantool AUTHORS, please see AUTHORS file.
 */
#pragma once

#include <stdint.h>
#include <signal.h>
#include <limits.h>

#include "trivia/config.h"

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

/**
 * Initialize crash signal handlers.
 */
void
crash_signal_init(void);

/**
 * Reset crash signal handlers.
 */
void
crash_signal_reset(void);

#if defined(__cplusplus)
}
#endif /* defined(__cplusplus) */
