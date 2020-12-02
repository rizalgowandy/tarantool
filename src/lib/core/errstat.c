/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright 2010-2020, Tarantool AUTHORS, please see AUTHORS file.
 */

#include <string.h>
#include <time.h>
#include <sys/utsname.h>

#include "trivia/util.h"
#include "backtrace.h"
#include "errstat.h"
#include "cfg.h"
#include "say.h"

#define pr_fmt(fmt)		"errstat: " fmt
#define pr_info(fmt, ...)	say_info(pr_fmt(fmt), ##__VA_ARGS__)
#define pr_err(fmt, ...)	say_error(pr_fmt(fmt), ##__VA_ARGS__)
#define pr_crit(fmt, ...)	fprintf(stderr, pr_fmt(fmt), ##__VA_ARGS__)
#define pr_panic(fmt, ...)	panic(pr_fmt(fmt), ##__VA_ARGS__)

static struct errstat glob_errstat;
static bool cfg_send_crash = false;

/*
 * We don't need it to be optimized but rather a compact form.
 */
static unsigned char *
base64_encode(unsigned char *dst, unsigned char *src, size_t src_len)
{
	static int m[] = {0, 2, 1};
	static unsigned char t[] = {
		'A','B','C','D','E','F','G','H',
		'I','J','K','L','M','N','O','P',
		'Q','R','S','T','U','V','W','X',
		'Y','Z','a','b','c','d','e','f',
		'g','h','i','j','k','l','m','n',
		'o','p','q','r','s','t','u','v',
		'w','x','y','z','0','1','2','3',
		'4','5','6','7','8','9','+','/'
	};
	size_t i, j;

	for (i = 0, j = 0; i < src_len;) {
		uint32_t a = i < src_len ? src[i++] : 0;
		uint32_t b = i < src_len ? src[i++] : 0;
		uint32_t c = i < src_len ? src[i++] : 0;

		uint32_t d = (a << 0x10) + (b << 0x08) + c;

		dst[j++] = t[(d >> 3 * 6) & 0x3f];
		dst[j++] = t[(d >> 2 * 6) & 0x3f];
		dst[j++] = t[(d >> 1 * 6) & 0x3f];
		dst[j++] = t[(d >> 0 * 6) & 0x3f];
	}

	size_t dst_len = ERRSTAT_BASE64_LEN(src_len);
	j = m[src_len % 3];
	for (i = 0; i < j; i++)
		dst[dst_len - 1 - i] = '=';

	return dst;
}

static size_t
strlcpy(char *dst, const char *src, size_t size)
{
	size_t ret = strlen(src);
	if (size) {
		size_t len = (ret >= size) ? size - 1 : ret;
		memcpy(dst, src, len);
		dst[len] = '\0';
	}
	return ret;
}

#define strlcpy_a(dst, src) strlcpy(dst, src, sizeof(dst))

struct errstat *
errstat_get(void)
{
	return &glob_errstat;
}

static inline
uint64_t timespec_to_ns(struct timespec *ts)
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
	buf[len-1] = '\0';
	return buf;
}

void
errstat_init(const char *tarantool_bin)
{
	struct errstat_build *binfo = &errstat_get()->build_info;
	struct errstat_uname *uinfo = &errstat_get()->uname_info;
	struct errstat_crash *cinfo = &errstat_get()->crash_info;

	binfo->major = PACKAGE_VERSION_MAJOR;
	binfo->minor = PACKAGE_VERSION_MINOR;
	binfo->patch = PACKAGE_VERSION_PATCH;

	strlcpy_a(binfo->version, PACKAGE_VERSION);
	strlcpy_a(binfo->cmake_type, BUILD_INFO);

	static_assert(ERRSTAT_UNAME_BUF_LEN > sizeof(struct errstat_uname),
		      "uname_buf is too small");

	char uname_buf[ERRSTAT_UNAME_BUF_LEN];
	struct utsname *uname_ptr = (void *)uname_buf;
	if (uname(uname_ptr) == 0) {
		strlcpy_a(uinfo->sysname, uname_ptr->sysname);
		strlcpy_a(uinfo->nodename, uname_ptr->nodename);
		strlcpy_a(uinfo->release, uname_ptr->release);
		strlcpy_a(uinfo->version, uname_ptr->version);
		strlcpy_a(uinfo->machine, uname_ptr->machine);
	} else
		pr_err("can't fetch uname");

	strlcpy_a(cinfo->tarantool_bin, tarantool_bin);
	if (strlen(cinfo->tarantool_bin) < strlen(tarantool_bin))
		pr_panic("can't save binary path");

	static_assert(sizeof(cinfo->exec_argv_1) == 4,
		      "exec_argv_1 is too small");
	strlcpy_a(cinfo->exec_argv_1, "-e");
}

void
box_errstat_cfg(void)
{
	struct errstat_crash *cinfo = &errstat_get()->crash_info;
	const char *feedback_host = cfg_gets("feedback_host");
	int feedback_enabled = cfg_getb("feedback_enabled");
	int charsh_enabled = cfg_getb("feedback_crash");

	if (feedback_enabled == 1 && charsh_enabled == 1&&
	    feedback_host != NULL) {
		strlcpy_a(cinfo->feedback_host, feedback_host);
		if (strlen(cinfo->feedback_host) < strlen(feedback_host))
			pr_panic("feedback_host is too long");
		pr_info("enable crash report");
		cfg_send_crash = true;
	} else {
		cfg_send_crash = false;
		pr_info("disable crash report");
		cinfo->feedback_host[0] = '\0';
	}
}

void
errstat_reset(void)
{
	struct errstat_crash *cinfo = &errstat_get()->crash_info;

#ifdef ENABLE_BACKTRACE
	cinfo->backtrace_buf[0] = '\0';
#endif
	memset(&cinfo->siginfo, 0, sizeof(cinfo->siginfo));
	cinfo->timestamp_rt = 0;
}

#ifdef TARGET_OS_LINUX
static void
collect_gregs(struct errstat_crash *cinfo, ucontext_t *uc)
{
	static_assert(sizeof(cinfo->greg) == sizeof(uc->uc_mcontext),
		      "GP regs are not matching signal frame");

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
	memcpy(&cinfo->greg, &uc->uc_mcontext, sizeof(cinfo->greg));
}
#endif

/**
 * The routine is called inside crash signal handler so
 * be carefull to not cause additional signals inside.
 */
void
errstat_collect_crash(int signo, siginfo_t *siginfo, void *ucontext)
{
	struct errstat_crash *cinfo = &errstat_get()->crash_info;

	struct timespec ts;
	if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
		cinfo->timestamp_rt = timespec_to_ns(&ts);
		ns_to_localtime(cinfo->timestamp_rt,
				cinfo->timestamp_rt_str,
				sizeof(cinfo->timestamp_rt_str));
	} else {
		cinfo->timestamp_rt = 0;
		memset(cinfo->timestamp_rt_str, 0,
		       sizeof(cinfo->timestamp_rt_str));
	}

	cinfo->signo = signo;
	cinfo->siginfo = *siginfo;

	cinfo->context_addr = ucontext;
	cinfo->siginfo_addr = siginfo;

#ifdef ENABLE_BACKTRACE
	char *start = cinfo->backtrace_buf;
	char *end = start + sizeof(cinfo->backtrace_buf) - 1;
	backtrace(start, end);
#endif

#ifdef TARGET_OS_LINUX
	collect_gregs(cinfo, ucontext);
#endif
}

/**
 * Prepare report in json format and put it into a buffer.
 */
static void
prepare_report_script(void)
{
	struct errstat_crash *cinfo = &errstat_get()->crash_info;
	struct errstat_build *binfo = &errstat_get()->build_info;
	struct errstat_uname *uinfo = &errstat_get()->uname_info;

	char *p, *e;

#ifdef ENABLE_BACKTRACE
	/*
	 * We can't use arbitrary data which can be misinterpreted
	 * by Lua script when we pass it to a script.
	 *
	 * WARNING: We use report_encoded as a temp buffer.
	 */
	size_t bt_len = strlen(cinfo->backtrace_buf);
	size_t bt_elen = ERRSTAT_BASE64_LEN(bt_len);
	if (bt_elen >= sizeof(cinfo->report_encoded))
		pr_panic("backtrace space is too small");

	base64_encode((unsigned char *)cinfo->report_encoded,
		      (unsigned char *)cinfo->backtrace_buf, bt_len);
	cinfo->report_encoded[bt_elen] = '\0';
	memcpy(cinfo->backtrace_buf, cinfo->report_encoded, bt_elen + 1);
#endif

#define snprintf_safe(fmt, ...)					\
	do {							\
		p += snprintf(p, e - p, fmt, ##__VA_ARGS__);	\
		if (p >= e)					\
			goto out;				\
	} while (0)

	e = cinfo->report + sizeof(cinfo->report) - 1;
	p = cinfo->report;

	snprintf_safe("{");
	snprintf_safe("\"uname\":{");
	snprintf_safe("\"sysname\":\"%s\",", uinfo->sysname);
#if 0
	/*
	 * nodename might a sensitive information so don't
	 * send it by default.
	 */
	snprintf_safe("\"nodename\":\"%s\",", uinfo->nodename);
#endif
	snprintf_safe("\"release\":\"%s\",", uinfo->release);
	snprintf_safe("\"version\":\"%s\",", uinfo->version);
	snprintf_safe("\"machine\":\"%s\"", uinfo->machine);
	snprintf_safe("},");

	snprintf_safe("\"build\":{");
	snprintf_safe("\"major\":%d,", binfo->major);
	snprintf_safe("\"minor\":%d,", binfo->minor);
	snprintf_safe("\"patch\":%d,", binfo->patch);
	snprintf_safe("\"version\":\"%s\",", binfo->version);
	snprintf_safe("\"cmake_type\":\"%s\"", binfo->cmake_type);
	snprintf_safe("},");

	snprintf_safe("\"signal\":{");
	snprintf_safe("\"signo\":%d,", cinfo->signo);
	snprintf_safe("\"si_code\":%d,", cinfo->siginfo.si_code);
	if (cinfo->signo == SIGSEGV) {
		if (cinfo->siginfo.si_code == SEGV_MAPERR) {
			snprintf_safe("\"si_code_str\":\"%s\",",
				      "SEGV_MAPERR");
		} else if (cinfo->siginfo.si_code == SEGV_ACCERR) {
			snprintf_safe("\"si_code_str\":\"%s\",",
				      "SEGV_ACCERR");
		}
		snprintf_safe("\"si_addr\":\"0x%llx\",",
			      (long long)cinfo->siginfo.si_addr);
	}
#ifdef ENABLE_BACKTRACE
	snprintf_safe("\"backtrace\":\"%s\",", cinfo->backtrace_buf);
#endif
	snprintf_safe("\"timestamp\":\"0x%llx\",",
		      (long long)cinfo->timestamp_rt);
	snprintf_safe("\"timestamp_str\":\"%s\"",
		      cinfo->timestamp_rt_str);
	snprintf_safe("}");
	snprintf_safe("}\'");

	size_t report_len = strlen(cinfo->report);
	size_t report_elen = ERRSTAT_BASE64_LEN(report_len);
	if (report_elen >= sizeof(cinfo->report_encoded))
		pr_panic("report encoded space is too small");

	base64_encode((unsigned char *)cinfo->report_encoded,
		      (unsigned char *)cinfo->report,
		      report_len);
	cinfo->report_encoded[report_elen] = '\0';

	e = cinfo->report_script + sizeof(cinfo->report_script) - 1;
	p = cinfo->report_script;

	strcpy(cinfo->feedback_host, "127.0.0.1:1500");
	snprintf_safe("require(\'http.client\').post(\'%s\',"
		      "'{\"crashdump\":{\"version\":\"%d\","
		      "\"data\":\"%s\"}}',{timeout=1}); os.exit(1);",
		      cinfo->feedback_host,
		      ERRSTAT_REPORT_VERSION,
		      cinfo->report_encoded);

#undef snprintf_safe
	return;

out:
	pr_crit("unable to prepare a crash report");
	struct sigaction sa = {
		.sa_handler = SIG_DFL,
	};
	sigemptyset(&sa.sa_mask);
	sigaction(SIGABRT, &sa, NULL);

	abort();
}

void
errstat_exec_send_crash(void)
{
	if (!cfg_send_crash)
		return;

	prepare_report_script();

	struct errstat_crash *cinfo = &errstat_get()->crash_info;
	cinfo->exec_argv[0] = cinfo->tarantool_bin;
	cinfo->exec_argv[1] = cinfo->exec_argv_1;
	cinfo->exec_argv[2] = cinfo->report_script;
	cinfo->exec_argv[3] = NULL;

	/*
	 * The script must exit at the end but there
	 * is no simple way to make sure from inside
	 * of a signal crash handler. So just hope it
	 * is running fine.
	 */
	execve(cinfo->tarantool_bin, cinfo->exec_argv, NULL);
	pr_panic("errstat: exec(%s,[%s,%s,%s,NULL]) failed",
		 cinfo->tarantool_bin,
		 cinfo->exec_argv[0],
		 cinfo->exec_argv[1],
		 cinfo->exec_argv[2]);
}
