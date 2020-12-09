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
 * Initialize crash subsystem.
 */
void
crash_init(const char *tarantool_bin);

/**
 * Initialize crash signal handlers.
 */
void
crash_signal_init(void);

/**
 * Configure crash engine from box.cfg.
 */
void
crash_cfg(void);

/**
 * Reset crash signal handlers.
 */
void
crash_signal_reset(void);

#if defined(__cplusplus)
}
#endif /* defined(__cplusplus) */
