/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright 2010-2020, Tarantool AUTHORS, please see AUTHORS file.
 */

#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/utsname.h>

#include "third_party/base64.h"
#include "small/static.h"
#include "trivia/util.h"
#include "backtrace.h"
#include "crash.h"
#include "cfg.h"
#include "say.h"

#define pr_fmt(fmt)		"crash: " fmt
#define pr_debug(fmt, ...)	say_debug(pr_fmt(fmt), ##__VA_ARGS__)
#define pr_info(fmt, ...)	say_info(pr_fmt(fmt), ##__VA_ARGS__)
#define pr_err(fmt, ...)	say_error(pr_fmt(fmt), ##__VA_ARGS__)
#define pr_syserr(fmt, ...)	say_syserror(pr_fmt(fmt), ##__VA_ARGS__)
#define pr_crit(fmt, ...)	fprintf(stderr, pr_fmt(fmt) "\n", ##__VA_ARGS__)
#define pr_panic(fmt, ...)	panic(pr_fmt(fmt), ##__VA_ARGS__)

/* Use strlcpy with destination as an array */
#define strlcpy_a(dst, src) strlcpy(dst, src, sizeof(dst))

#ifdef TARGET_OS_LINUX
#ifndef __x86_64__
# error "Non x86-64 architectures are not supported"
#endif
struct crash_greg {
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

static struct crash_info {
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
	struct crash_greg greg;
#endif
	/**
	 * Timestamp in nanoseconds (realtime).
	 */
	uint64_t timestamp_rt;
	/**
	 * Faulting address.
	 */
	void *siaddr;
	/**
	 * Crash signal number.
	 */
	int signo;
	/**
	 * Crash signal code.
	 */
	int sicode;
#ifdef ENABLE_BACKTRACE
	/*
	 * 4K of memory should be enough to keep the backtrace.
	 * In worst case it gonna be simply trimmed.
	 */
	char backtrace_buf[4096];
#endif
} crash_info;

static char tarantool_path[PATH_MAX];
static char feedback_host[2048];
static bool send_crashinfo = true;

static inline uint64_t
timespec_to_ns(struct timespec *ts)
{
	return (uint64_t)ts->tv_sec * 1000000000 + (uint64_t)ts->tv_nsec;
}

static char *
ns_to_localtime(uint64_t timestamp, char *buf, ssize_t len)
{
	time_t sec = timestamp / 1000000000;
	char *start = buf;
	struct tm tm;

	/*
	 * Use similar format as say_x logger. Except plain
	 * seconds should be enough.
	 */
	localtime_r(&sec, &tm);
	ssize_t total = strftime(start, len, "%F %T %Z", &tm);
	start += total;
	if (total < len)
		return buf;
	buf[len - 1] = '\0';
	return buf;
}

void
crash_init(const char *tarantool_bin)
{
	strlcpy_a(tarantool_path, tarantool_bin);
	if (strlen(tarantool_path) < strlen(tarantool_bin))
		pr_panic("executable path is trimmed");
}

void
crash_cfg(void)
{
	const char *host = cfg_gets("feedback_host");
	bool is_enabled = cfg_getb("feedback_enabled");
	bool no_crashinfo = cfg_getb("feedback_no_crashinfo");

	if (host == NULL || !is_enabled || no_crashinfo) {
		if (send_crashinfo) {
			pr_info("disable sedning crashinfo feedback");
			send_crashinfo = false;
			feedback_host[0] = '\0';
		}
		return;
	}

	if (strcmp(feedback_host, host) != 0) {
		strlcpy_a(feedback_host, host);
		if (strlen(feedback_host) < strlen(host))
			pr_panic("feedback_host is too long");
	}

	if (!send_crashinfo) {
		pr_info("enable sedning crashinfo feedback");
		send_crashinfo = true;
	}
}

/**
 * The routine is called inside crash signal handler so
 * be careful to not cause additional signals inside.
 */
static struct crash_info *
crash_collect(int signo, siginfo_t *siginfo, void *ucontext)
{
	struct crash_info *cinfo = &crash_info;
	struct timespec ts;

	if (clock_gettime(CLOCK_REALTIME, &ts) == 0)
		cinfo->timestamp_rt = timespec_to_ns(&ts);
	else
		cinfo->timestamp_rt = 0;

	cinfo->signo = signo;
	cinfo->sicode = siginfo->si_code;
	cinfo->siaddr = siginfo->si_addr;
	cinfo->context_addr = ucontext;
	cinfo->siginfo_addr = siginfo;

#ifdef ENABLE_BACKTRACE
	char *start = cinfo->backtrace_buf;
	backtrace(start, sizeof(cinfo->backtrace_buf));
#endif

#ifdef TARGET_OS_LINUX
	/*
	 * uc_mcontext on libc level looks somehow strange,
	 * they define an array of uint64_t where each register
	 * defined by REG_x macro.
	 *
	 * In turn the kernel is quite explicit about the context.
	 * Moreover it is a part of user ABI, thus won't be changed.
	 *
	 * Lets use memcpy here to make a copy in a fast way.
	 */
	ucontext_t *uc = ucontext;
	memcpy(&cinfo->greg, &uc->uc_mcontext, sizeof(cinfo->greg));
#endif

	return cinfo;
}

/**
 * Report crash information to the feedback daemon
 * (ie send it to feedback daemon).
 */
static void
crash_report_feedback_daemon(struct crash_info *cinfo)
{
	/*
	 * Update to a new number if format get changed.
	 */
	static const int crashinfo_version = 1;

	char *p = static_alloc(SMALL_STATIC_SIZE);
	char *e = &p[SMALL_STATIC_SIZE - 1];
	char *head = p;
	char *tail = &p[SMALL_STATIC_SIZE - 8];

	/*
	 * Note that while we encode the report we
	 * intensively use a tail of the allocated
	 * buffer as a temporary store.
	 */

#define snprintf_safe(fmt, ...)					\
	do {							\
		p += snprintf(p, e - p, fmt, ##__VA_ARGS__);	\
		if (p >= e)					\
			goto out;				\
	} while (0)

	/*
	 * Lets reuse tail of the buffer as a temp space.
	 */
	struct utsname *uname_ptr = (void *)&tail[-sizeof(struct utsname)];
	if (p >= (char *)(void *)uname_ptr)
		goto out;

	if (uname(uname_ptr) != 0) {
		pr_syserr("uname call failed, ignore");
		memset(uname_ptr, 0, sizeof(struct utsname));
	}

	snprintf_safe("{");
	snprintf_safe("\"uname\":{");
	snprintf_safe("\"sysname\":\"%s\",", uname_ptr->sysname);
	/*
	 * nodename might a sensitive information, skip.
	 */
	snprintf_safe("\"release\":\"%s\",", uname_ptr->release);
	snprintf_safe("\"version\":\"%s\",", uname_ptr->version);
	snprintf_safe("\"machine\":\"%s\"", uname_ptr->machine);
	snprintf_safe("},");

	if (p >= (char *)(void *)uname_ptr)
		goto out;

	snprintf_safe("\"build\":{");
	snprintf_safe("\"version\":\"%s\",", PACKAGE_VERSION);
	snprintf_safe("\"cmake_type\":\"%s\"", BUILD_INFO);
	snprintf_safe("},");

	snprintf_safe("\"signal\":{");
	snprintf_safe("\"signo\":%d,", cinfo->signo);
	snprintf_safe("\"si_code\":%d,", cinfo->sicode);
	if (cinfo->signo == SIGSEGV) {
		if (cinfo->sicode == SEGV_MAPERR) {
			snprintf_safe("\"si_code_str\":\"%s\",",
				      "SEGV_MAPERR");
		} else if (cinfo->sicode == SEGV_ACCERR) {
			snprintf_safe("\"si_code_str\":\"%s\",",
				      "SEGV_ACCERR");
		}
		snprintf_safe("\"si_addr\":\"0x%llx\",",
			      (long long)cinfo->siaddr);
	}

#ifdef ENABLE_BACKTRACE
	/*
	 * The backtrace itself is encoded into base64 form
	 * since it might have arbitrary symbols not suitable
	 * for json encoding (newlines and etc).
	 */
	size_t bt_len = strlen(cinfo->backtrace_buf);
	size_t bt_elen = base64_bufsize(bt_len, BASE64_NOWRAP);
	char *bt_base64 = &tail[-bt_elen-8];
	if (p >= bt_base64)
		goto out;
	base64_encode(cinfo->backtrace_buf, bt_len,
		      bt_base64, bt_elen, BASE64_NOWRAP);
	bt_base64[bt_elen] = '\0';
	snprintf_safe("\"backtrace\":\"%s\",", bt_base64);
#endif

	char *timestamp_rt_str = &tail[-128];
	if (p >= timestamp_rt_str)
		goto out;
	ns_to_localtime(cinfo->timestamp_rt, timestamp_rt_str, 128);
	snprintf_safe("\"timestamp\":\"%s\"", timestamp_rt_str);
	snprintf_safe("}");
	snprintf_safe("}");

	size_t report_len = p - head;
	size_t report_elen = base64_bufsize(report_len, BASE64_NOWRAP);

	char *report_base64 = &tail[-report_elen-8];
	if (p >= report_base64)
		goto out;
	base64_encode(head, report_len, report_base64,
		      report_elen, BASE64_NOWRAP);
	report_base64[report_elen] = '\0';

	/*
	 * Encoded report now sits at report_base64 position,
	 * at the tail of 'small' static buffer. Lets prepare
	 * the script to run (make sure the strings do not
	 * overlap).
	 */
	p = head;
	snprintf_safe("require(\'http.client\').post(\'%s\',"
		      "'{\"crashdump\":{\"version\":\"%d\","
		      "\"data\":", feedback_host,
		      crashinfo_version);
	if (report_base64 - p <= (long)(report_elen - 2))
		goto out;
	snprintf_safe("\"%s\"", report_base64);
	snprintf_safe("}}',{timeout=1}); os.exit(1);");

	pr_debug("crashinfo script: %s", head);

	char *exec_argv[4] = {
		[0] = tarantool_path,
		[1] = "-e",
		[2] = head,
		[3] = NULL,
	};

	/*
	 * Can't use fork here because libev has own
	 * at_fork helpers with mutex where we might
	 * stuck (see popen code).
	 */
	pid_t pid = vfork();
	if (pid == 0) {
		/*
		 * The script must exit at the end but there
		 * is no simple way to make sure from inside
		 * of a signal crash handler. So just hope it
		 * is running fine.
		 */
		execve(exec_argv[0], exec_argv, NULL);
		pr_crit("exec(%s,[%s,%s,%s,NULL]) failed",
			exec_argv[0], exec_argv[0],
			exec_argv[1], exec_argv[2]);
		_exit(1);
	} else if (pid < 0) {
		pr_crit("unable to vfork (errno %d)", errno);
	}

	return;
out:
	pr_crit("unable to prepare a crash report");
}

/**
 * Report crash information to the stderr
 * (usually a current console).
 */
static void
crash_report_stderr(struct crash_info *cinfo)
{
	if (cinfo->signo == SIGSEGV) {
		fprintf(stderr, "Segmentation fault\n");
		const char *signal_code_repr = NULL;

		switch (cinfo->sicode) {
		case SEGV_MAPERR:
			signal_code_repr = "SEGV_MAPERR";
			break;
		case SEGV_ACCERR:
			signal_code_repr = "SEGV_ACCERR";
			break;
		}

		if (signal_code_repr != NULL)
			fprintf(stderr, "  code: %s\n", signal_code_repr);
		else
			fprintf(stderr, "  code: %d\n", cinfo->sicode);
		/*
		 * fprintf is used instead of fdprintf, because
		 * fdprintf does not understand %p
		 */
		fprintf(stderr, "  addr: %p\n", cinfo->siaddr);
	} else {
		fprintf(stderr, "Got a fatal signal %d\n", cinfo->signo);
	}

	fprintf(stderr, "  context: %p\n", cinfo->context_addr);
	fprintf(stderr, "  siginfo: %p\n", cinfo->siginfo_addr);

#ifdef TARGET_OS_LINUX
# define fprintf_reg(__n, __v)				\
	fprintf(stderr, "  %-9s0x%-17llx%lld\n",	\
		__n, (long long)__v, (long long)__v)
	fprintf_reg("rax", cinfo->greg.ax);
	fprintf_reg("rbx", cinfo->greg.bx);
	fprintf_reg("rcx", cinfo->greg.cx);
	fprintf_reg("rdx", cinfo->greg.dx);
	fprintf_reg("rsi", cinfo->greg.si);
	fprintf_reg("rdi", cinfo->greg.di);
	fprintf_reg("rsp", cinfo->greg.sp);
	fprintf_reg("rbp", cinfo->greg.bp);
	fprintf_reg("r8", cinfo->greg.r8);
	fprintf_reg("r9", cinfo->greg.r9);
	fprintf_reg("r10", cinfo->greg.r10);
	fprintf_reg("r11", cinfo->greg.r11);
	fprintf_reg("r12", cinfo->greg.r12);
	fprintf_reg("r13", cinfo->greg.r13);
	fprintf_reg("r14", cinfo->greg.r14);
	fprintf_reg("r15", cinfo->greg.r15);
	fprintf_reg("rip", cinfo->greg.ip);
	fprintf_reg("eflags", cinfo->greg.flags);
	fprintf_reg("cs", cinfo->greg.cs);
	fprintf_reg("gs", cinfo->greg.gs);
	fprintf_reg("fs", cinfo->greg.fs);
	fprintf_reg("cr2", cinfo->greg.cr2);
	fprintf_reg("err", cinfo->greg.err);
	fprintf_reg("oldmask", cinfo->greg.oldmask);
	fprintf_reg("trapno", cinfo->greg.trapno);
# undef fprintf_reg
#endif /* TARGET_OS_LINUX */

	fprintf(stderr, "Current time: %u\n", (unsigned)time(0));
	fprintf(stderr, "Please file a bug at "
		"http://github.com/tarantool/tarantool/issues\n");

#ifdef ENABLE_BACKTRACE
	fprintf(stderr, "Attempting backtrace... Note: since the server has "
		"already crashed, \nthis may fail as well\n");
	fprintf(stderr, "%s", cinfo->backtrace_buf);
#endif
}

/**
 * Handle fatal (crashing) signal.
 *
 * Try to log as much as possible before dumping a core.
 *
 * Core files are not always allowed and it takes an effort to
 * extract useful information from them.
 *
 * *Recursive invocation*
 *
 * Unless SIGSEGV is sent by kill(), Linux resets the signal
 * a default value before invoking the handler.
 *
 * Despite that, as an extra precaution to avoid infinite
 * recursion, we count invocations of the handler, and
 * quietly _exit() when called for a second time.
 */
static void
crash_signal_cb(int signo, siginfo_t *siginfo, void *context)
{
	static volatile sig_atomic_t in_cb = 0;
	struct crash_info *cinfo;

	if (in_cb == 0) {
		in_cb = 1;
		cinfo = crash_collect(signo, siginfo, context);
		crash_report_stderr(cinfo);
		if (send_crashinfo)
			crash_report_feedback_daemon(cinfo);
	} else {
		/* Got a signal while running the handler. */
		fprintf(stderr, "Fatal %d while backtracing", signo);
	}

	/* Try to dump a core */
	struct sigaction sa = {
		.sa_handler = SIG_DFL,
	};
	sigemptyset(&sa.sa_mask);
	sigaction(SIGABRT, &sa, NULL);
	abort();
}

/**
 * Fatal signals we generate crash on.
 */
static const int crash_signals[] = { SIGSEGV, SIGFPE };

void
crash_signal_reset(void)
{
	struct sigaction sa = {
		.sa_handler = SIG_DFL,
	};
	sigemptyset(&sa.sa_mask);

	for (size_t i = 0; i < lengthof(crash_signals); i++) {
		if (sigaction(crash_signals[i], &sa, NULL) == 0)
			continue;
		pr_syserr("reset sigaction %d", crash_signals[i]);
	}
}

void
crash_signal_init(void)
{
	/*
	 * SA_RESETHAND resets handler action to the default
	 * one when entering handler.
	 *
	 * SA_NODEFER allows receiving the same signal
	 * during handler.
	 */
	struct sigaction sa = {
		.sa_flags = SA_RESETHAND | SA_NODEFER | SA_SIGINFO,
		.sa_sigaction = crash_signal_cb,
	};
	sigemptyset(&sa.sa_mask);

	for (size_t i = 0; i < lengthof(crash_signals); i++) {
		if (sigaction(crash_signals[i], &sa, NULL) == 0)
			continue;
		pr_panic("sigaction %d (%s)", crash_signals[i],
			 strerror(errno));
	}
}
