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

#define ERRSTAT_REPORT_VERSION 1

/**
 * Build type information for statistics.
 */
struct errstat_build {
	/**
	 * Package major version - 1 for 1.6.7.
	 */
	int major;
	/**
	 * Package minor version - 6 for 1.6.7
	 */
	int minor;
	/**
	 * Package patch version - 7 for 1.6.7
	 */
	int patch;
	/**
	 * A string with major-minor-patch-commit-id identifier of the
	 * release, e.g. 2.7.0-62-g0b7726571.
	 */
	char version[64];
	/**
	 * Build type (Debug and etc).
	 */
	char cmake_type[64];
};

#ifdef TARGET_OS_LINUX
#ifndef __x86_64__
# error "Non x86-64 architectures are not supported"
#endif
struct errstat_greg {
	uint64_t	r8;
	uint64_t	r9;
	uint64_t	r10;
	uint64_t	r11;
	uint64_t	r12;
	uint64_t	r13;
	uint64_t	r14;
	uint64_t	r15;
	uint64_t	di;
	uint64_t	si;
	uint64_t	bp;
	uint64_t	bx;
	uint64_t	dx;
	uint64_t	ax;
	uint64_t	cx;
	uint64_t	sp;
	uint64_t	ip;
	uint64_t	flags;
	uint16_t	cs;
	uint16_t	gs;
	uint16_t	fs;
	uint16_t	ss;
	uint64_t	err;
	uint64_t	trapno;
	uint64_t	oldmask;
	uint64_t	cr2;
	uint64_t	fpstate;
	uint64_t	reserved1[8];
};
#endif /* TARGET_OS_LINUX */

#define ERRSTAT_BASE64_LEN(len)			(4 * (((len) + 2) / 3))

/*
 * 4K of memory should be enough to keep the backtrace.
 * In worst case it gonna be simply trimmed. Since we're
 * reporting it encoded the pain text shrinks to 3070 bytes.
 */
#define ERRSTAT_BACKTRACE_MAX			(4096)

/*
 * The report should include the bactrace
 * and all additional information we're
 * going to send.
 */
#define ERRSTAT_REPORT_PAYLOAD_MAX		(2048)
#ifdef ENABLE_BACKTRACE
# define ERRSTAT_REPORT_MAX			\
	(ERRSTAT_BACKTRACE_MAX +		\
	 ERRSTAT_REPORT_PAYLOAD_MAX)
# else
# define ERRSTAT_REPORT_MAX			\
	(ERRSTAT_REPORT_PAYLOAD_MAX)
#endif

/*
 * We encode report into base64 because
 * it is passed inside Lua script.
 */
#define ERRSTAT_REPORT_ENCODED_MAX		\
	ERRSTAT_BASE64_LEN(ERRSTAT_REPORT_MAX)


/*
 * The script to execute should contain encoded
 * report.
 */
#define ERRSTAT_REPORT_SCRIPT_PAYLOAD_MAX	(512)
#define ERRSTAT_REPORT_SCRIPT_MAX		\
	(ERRSTAT_REPORT_SCRIPT_PAYLOAD_MAX +	\
	 ERRSTAT_REPORT_ENCODED_MAX)

struct errstat_crash {
	/**
	 * Exec arguments pointers.
	 */
	char *exec_argv[4];
	/**
	 * Predefined argument "-e".
	 */
	char exec_argv_1[4];
	/**
	 * Crash report in plain json format.
	 */
	char report[ERRSTAT_REPORT_MAX];
	/**
	 * Crash report in base64 form.
	 */
	char report_encoded[ERRSTAT_REPORT_ENCODED_MAX];
	/**
	 * Tarantool executable to send report stript.
	 */
	char tarantool_bin[PATH_MAX];
	/**
	 * The script to evaluate by tarantool
	 * to send the report.
	 */
	char report_script[ERRSTAT_REPORT_SCRIPT_MAX];
#ifdef ENABLE_BACKTRACE
	/**
	 * Backtrace buffer.
	 */
	char backtrace_buf[ERRSTAT_BACKTRACE_MAX];
#endif
	/**
	 * Crash signal.
	 */
	int signo;
	/**
	 * Signal information.
	 */
	siginfo_t siginfo;
	/**
	 * These two are mostly useless as being
	 * plain addresses but keep for backward
	 * compatibility.
	 */
	void *context_addr;
	void *siginfo_addr;
#ifdef TARGET_OS_LINUX
	/**
	 * Registers contents.
	 */
	struct errstat_greg greg;
#endif
	/**
	 * Timestamp in nanoseconds (realtime).
	 */
	uint64_t timestamp_rt;
	/**
	 * Timestamp string representation to
	 * use on demand.
	 */
	char timestamp_rt_str[32];
	/**
	 * Crash collector host.
	 */
	char feedback_host[1024];
};

#define ERRSTAT_UNAME_BUF_LEN	1024
#define ERRSTAT_UNAME_FIELD_LEN	128
/**
 * Information about node.
 *
 * On linux there is new_utsname structure which
 * encodes each field to __NEW_UTS_LEN + 1 => 64 + 1 = 65.
 * So lets just reserve more data in advance.
 */
struct errstat_uname {
	char sysname[ERRSTAT_UNAME_FIELD_LEN];
	char nodename[ERRSTAT_UNAME_FIELD_LEN];
	char release[ERRSTAT_UNAME_FIELD_LEN];
	char version[ERRSTAT_UNAME_FIELD_LEN];
	char machine[ERRSTAT_UNAME_FIELD_LEN];
};

struct errstat {
	struct errstat_build build_info;
	struct errstat_uname uname_info;
	struct errstat_crash crash_info;
};

/**
 * Return a pointer to the info keeper.
 */
extern struct errstat *
errstat_get(void);

/**
 * Initialize error statistics.
 */
extern void
errstat_init(const char *tarantool_bin);

/**
 * Configure errstat.
 */
extern void
box_errstat_cfg(void);

/**
 * Reset everything except build information.
 */
extern void
errstat_reset(void);

/**
 * Collect a crash.
 */
extern void
errstat_collect_crash(int signo, siginfo_t *siginfo, void *context);

/**
 * Send a crash report.
 */
extern void
errstat_exec_send_crash(void);

#if defined(__cplusplus)
}
#endif /* defined(__cplusplus) */
