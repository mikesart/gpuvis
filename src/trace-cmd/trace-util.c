// SPDX-License-Identifier: LGPL-2.1
/*
 * Copyright (C) 2009, 2010 Red Hat Inc, Steven Rostedt <srostedt@redhat.com>
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <ctype.h>
#include <errno.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <limits.h>
#include <libgen.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <time.h>
#include <event-parse.h>
#include <event-utils.h>

#include "trace-cmd-private.h"
#include "trace-cmd-local.h"

#define LOCAL_PLUGIN_DIR ".trace-cmd/plugins"
#define PROC_STACK_FILE "/proc/sys/kernel/stack_tracer_enabled"

static bool debug;
static int log_level = TEP_LOG_CRITICAL;
static FILE *logfp;

const static struct {
	const char *clock_str;
	enum tracecmd_clocks clock_id;
} trace_clocks[] = {
	{"local", TRACECMD_CLOCK_LOCAL},
	{"global", TRACECMD_CLOCK_GLOBAL},
	{"counter", TRACECMD_CLOCK_COUNTER},
	{"uptime", TRACECMD_CLOCK_UPTIME},
	{"perf", TRACECMD_CLOCK_PERF},
	{"mono", TRACECMD_CLOCK_MONO},
	{"mono_raw", TRACECMD_CLOCK_MONO_RAW},
	{"boot", TRACECMD_CLOCK_BOOT},
	{"x86-tsc", TRACECMD_CLOCK_X86_TSC},
	{NULL, -1}
};

/**
 * tracecmd_clock_str2id - Convert ftrace clock name to clock ID
 * @clock: Ftrace clock name
 * Returns ID of the ftrace clock
 */
enum tracecmd_clocks tracecmd_clock_str2id(const char *clock)
{
	int i;

	if (!clock)
		return TRACECMD_CLOCK_UNKNOWN;

	for (i = 0; trace_clocks[i].clock_str; i++) {
		if (!strncmp(clock, trace_clocks[i].clock_str,
		    strlen(trace_clocks[i].clock_str)))
			return trace_clocks[i].clock_id;
	}
	return TRACECMD_CLOCK_UNKNOWN;
}

/**
 * tracecmd_clock_id2str - Convert clock ID to ftare clock name
 * @clock: Clock ID
 * Returns name of a ftrace clock
 */
const char *tracecmd_clock_id2str(enum tracecmd_clocks clock)
{
	int i;

	for (i = 0; trace_clocks[i].clock_str; i++) {
		if (trace_clocks[i].clock_id == clock)
			return trace_clocks[i].clock_str;
	}
	return NULL;
}

/**
 * tracecmd_set_debug - Set debug mode of the tracecmd library
 * @set_debug: The new "debug" mode. If true, the tracecmd library is
 * in "debug" mode
 */
void tracecmd_set_debug(bool set_debug)
{
	debug = set_debug;

	if (set_debug)
		tracecmd_set_loglevel(TEP_LOG_DEBUG);
	else
		tracecmd_set_loglevel(TEP_LOG_CRITICAL);
}

/**
 * tracecmd_get_debug - Get debug mode of tracecmd library
 * Returns true, if the tracecmd library is in debug mode.
 *
 */
bool tracecmd_get_debug(void)
{
	return debug;
}

void tracecmd_parse_cmdlines(struct tep_handle *pevent,
			     char *file, int size __maybe_unused)
{
	char *comm;
	char *line;
	char *next = NULL;
	int pid;

	line = strtok_r(file, "\n", &next);
	while (line) {
		sscanf(line, "%d %m[^\n]s", &pid, &comm);
		tep_register_comm(pevent, comm, pid);
		free(comm);
		line = strtok_r(NULL, "\n", &next);
	}
}

void tracecmd_parse_proc_kallsyms(struct tep_handle *pevent,
			 char *file, unsigned int size __maybe_unused)
{
	unsigned long long addr;
	char *func;
	char *line;
	char *next = NULL;
	char *mod;
	char ch;

	line = strtok_r(file, "\n", &next);
	while (line) {
		int func_start, func_end = 0;
		int mod_start, mod_end = 0;
		int n;

		mod = NULL;
		errno = 0;
		n = sscanf(line, "%16llx %c %n%*s%n%*1[\t][%n%*s%n",
			   &addr, &ch, &func_start, &func_end, &mod_start, &mod_end);
		if (errno) {
			perror("sscanf");
			return;
		}

		if (n != 2 || !func_end)
			return;

		func = line + func_start;
		/*
		 * Hacks for
		 *  - arm arch that adds a lot of bogus '$a' functions
		 *  - x86-64 that reports per-cpu variable offsets as absolute
		 */
		if (func[0] != '$' && ch != 'A' && ch != 'a') {
			line[func_end] = 0;
			if (mod_end) {
				mod = line + mod_start;
				/* truncate the extra ']' */
				line[mod_end - 1] = 0;
			}
			tep_register_function(pevent, func, addr, mod);
		}

		line = strtok_r(NULL, "\n", &next);
	}
}

void tracecmd_parse_ftrace_printk(struct tep_handle *pevent,
			 char *file, unsigned int size __maybe_unused)
{
	unsigned long long addr;
	char *printk;
	char *line;
	char *next = NULL;
	char *addr_str;
	char *fmt;

	line = strtok_r(file, "\n", &next);
	while (line) {
		addr_str = strtok_r(line, ":", &fmt);
		if (!addr_str) {
			tracecmd_warning("printk format with empty entry");
			break;
		}
		addr = strtoull(addr_str, NULL, 16);
		/* fmt still has a space, skip it */
		printk = strdup(fmt+1);
		line = strtok_r(NULL, "\n", &next);
		tep_register_print_string(pevent, printk, addr);
		free(printk);
	}
}

/**
 * tracecmd_add_id - add an int to the event id list
 * @list: list to add the id to
 * @id: id to add
 * @len: current length of list of ids.
 *
 * The typical usage is:
 *
 *    events = tracecmd_add_id(events, id, len++);
 *
 * Returns the new allocated list with the id included.
 * the list will contain a '-1' at the end.
 *
 * The returned list should be freed with free().
 */
int *tracecmd_add_id(int *list, int id, int len)
{
	if (!list)
		list = malloc(sizeof(*list) * 2);
	else
		list = realloc(list, sizeof(*list) * (len + 2));
	if (!list)
		return NULL;

	list[len++] = id;
	list[len] = -1;

	return list;
}

struct add_plugin_data {
	int ret;
	int index;
	char **files;
};

static void add_plugin_file(struct tep_handle *pevent, const char *path,
			   const char *name, void *data)
{
	struct add_plugin_data *pdata = data;
	char **ptr;
	int size;
	int i;

	if (pdata->ret)
		return;

	size = pdata->index + 2;
	ptr = realloc(pdata->files, sizeof(char *) * size);
	if (!ptr)
		goto out_free;

	ptr[pdata->index] = strdup(name);
	if (!ptr[pdata->index])
		goto out_free;

	pdata->files = ptr;
	pdata->index++;
	pdata->files[pdata->index] = NULL;
	return;

 out_free:
	for (i = 0; i < pdata->index; i++)
		free(pdata->files[i]);
	free(pdata->files);
	pdata->files = NULL;
	pdata->ret = errno;
}

/**
 * trace_util_find_plugin_files - find list of possible plugin files
 * @suffix: The suffix of the plugin files to find
 *
 * Searches the plugin directory for files that end in @suffix, and
 * will return an allocated array of file names, or NULL if none is
 * found.
 *
 * Must check against TRACECMD_ISERR(ret) as if an error happens
 * the errno will be returned with the TRACECMD_ERR_MSK to denote
 * such an error occurred.
 *
 * Use trace_util_free_plugin_files() to free the result.
 */
__hidden char **trace_util_find_plugin_files(const char *suffix)
{
	struct add_plugin_data pdata;

	memset(&pdata, 0, sizeof(pdata));

	tep_load_plugins_hook(NULL, suffix, add_plugin_file, &pdata);

	if (pdata.ret)
		return TRACECMD_ERROR(pdata.ret);

	return pdata.files;
}

/**
 * trace_util_free_plugin_files - free the result of trace_util_find_plugin_files()
 * @files: The result from trace_util_find_plugin_files()
 *
 * Frees the contents that were allocated by trace_util_find_plugin_files().
 */
void __hidden trace_util_free_plugin_files(char **files)
{
	int i;

	if (!files || TRACECMD_ISERR(files))
		return;

	for (i = 0; files[i]; i++) {
		free(files[i]);
	}
	free(files);
}

static char *get_source_plugins_dir(void)
{
	char *p, path[PATH_MAX+1];
	int ret;

	ret = readlink("/proc/self/exe", path, PATH_MAX);
	if (ret > PATH_MAX || ret < 0)
		return NULL;

	path[ret] = 0;
	dirname(path);
	p = strrchr(path, '/');
	if (!p)
		return NULL;
	/* Check if we are in the the source tree */
	if (strcmp(p, "/tracecmd") != 0)
		return NULL;

	strcpy(p, "/lib/traceevent/plugins");
	return strdup(path);
}

__hidden struct tep_plugin_list *
trace_load_plugins(struct tep_handle *tep, int flags)
{
	struct tep_plugin_list *list;
	char *path;

	if (flags & TRACECMD_FL_LOAD_NO_PLUGINS)
		tep_set_flag(tep, TEP_DISABLE_PLUGINS);
	if (flags & TRACECMD_FL_LOAD_NO_SYSTEM_PLUGINS)
		tep_set_flag(tep, TEP_DISABLE_SYS_PLUGINS);

	path = get_source_plugins_dir();
	if (path)
		tep_add_plugin_path(tep, path, TEP_PLUGIN_LAST);
	free(path);

	list = tep_load_plugins(tep);

	return list;
}

/**
 * tracecmd_set_loglevel - set log level of the library
 * @level: desired level of the library messages
 */
void tracecmd_set_loglevel(enum tep_loglevel level)
{
	log_level = level;
	tracefs_set_loglevel(level);
	tep_set_loglevel(level);
}

void __weak tracecmd_warning(const char *fmt, ...)
{
	va_list ap;

	if (log_level < TEP_LOG_WARNING)
		return;

	va_start(ap, fmt);
	tep_vprint("libtracecmd", TEP_LOG_WARNING, true, fmt, ap);
	va_end(ap);
}

void __weak tracecmd_info(const char *fmt, ...)
{
	va_list ap;

	if (log_level < TEP_LOG_INFO)
		return;

	va_start(ap, fmt);
	tep_vprint("libtracecmd", TEP_LOG_INFO, false, fmt, ap);
	va_end(ap);
}

void __weak tracecmd_critical(const char *fmt, ...)
{
	int ret;
	va_list ap;

	if (log_level < TEP_LOG_CRITICAL)
		return;

	va_start(ap, fmt);
	ret = tep_vprint("libtracecmd", TEP_LOG_CRITICAL, true, fmt, ap);
	va_end(ap);

	if (debug) {
		if (!ret)
			ret = -1;
		exit(ret);
	}
}

#define LOG_BUF_SIZE 1024
static void __plog(const char *prefix, const char *fmt, va_list ap, FILE *fp)
{
	static int newline = 1;
	char buf[LOG_BUF_SIZE];
	int r;

	r = vsnprintf(buf, LOG_BUF_SIZE, fmt, ap);

	if (r > LOG_BUF_SIZE)
		r = LOG_BUF_SIZE;

	if (logfp) {
		if (newline)
			fprintf(logfp, "[%d]%s%.*s", getpid(), prefix, r, buf);
		else
			fprintf(logfp, "[%d]%s%.*s", getpid(), prefix, r, buf);
		newline = buf[r - 1] == '\n';
		fflush(logfp);
		return;
	}

	fprintf(fp, "%.*s", r, buf);
}

void tracecmd_plog(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	__plog("", fmt, ap, stdout);
	va_end(ap);
	/* Make sure it gets to the screen, in case we crash afterward */
	fflush(stdout);
}

void tracecmd_plog_error(const char *fmt, ...)
{
	va_list ap;
	char *str = "";

	va_start(ap, fmt);
	__plog("Error: ", fmt, ap, stderr);
	va_end(ap);
	if (errno)
		str = strerror(errno);
	if (logfp)
		fprintf(logfp, "\n%s\n", str);
	else
		fprintf(stderr, "\n%s\n", str);
}

/**
 * tracecmd_set_logfile - Set file for logging
 * @logfile: Name of the log file
 *
 * Returns 0 on successful completion or -1 in case of error
 */
int tracecmd_set_logfile(char *logfile)
{
	if (logfp)
		fclose(logfp);
	logfp = fopen(logfile, "w");
	if (!logfp)
		return -1;
	return 0;
}

/**
 * tracecmd_stack_tracer_status - Check stack trace status
 * @status: Returned stack trace status:
 *             0 - not configured, disabled
 *             non 0 - enabled
 *
 * Returns -1 in case of an error, 0 if file does not exist
 * (stack tracer not configured in kernel) or 1 on successful completion.
 */
int tracecmd_stack_tracer_status(int *status)
{
	struct stat stat_buf;
	char buf[64];
	long num;
	int fd;
	int n;

	if (stat(PROC_STACK_FILE, &stat_buf) < 0) {
		/* stack tracer not configured on running kernel */
		*status = 0; /* not configured means disabled */
		return 0;
	}

	fd = open(PROC_STACK_FILE, O_RDONLY);

	if (fd < 0)
		return -1;

	n = read(fd, buf, sizeof(buf));
	close(fd);

	if (n <= 0)
		return -1;

	if (n >= sizeof(buf))
		return -1;

	buf[n] = 0;

	errno = 0;
	num = strtol(buf, NULL, 10);

	/* Check for various possible errors */
	if (num > INT_MAX || num < INT_MIN || (!num && errno))
		return -1;

	*status = num;
	return 1; /* full success */
}

/**
 * tracecmd_count_cpus - Get the number of CPUs in the system
 *
 * Returns the number of CPUs in the system, or 0 in case of an error
 */
int tracecmd_count_cpus(void)
{
	static int once;
	char buf[1024];
	int cpus = 0;
	char *pbuf;
	size_t *pn;
	FILE *fp;
	size_t n;
	int r;

	cpus = sysconf(_SC_NPROCESSORS_CONF);
	if (cpus > 0)
		return cpus;

	if (!once) {
		once++;
		tracecmd_warning("sysconf could not determine number of CPUS");
	}

	/* Do the hack to figure out # of CPUS */
	n = 1024;
	pn = &n;
	pbuf = buf;

	fp = fopen("/proc/cpuinfo", "r");
	if (!fp) {
		tracecmd_critical("Can not read cpuinfo");
		return 0;
	}

	while ((r = getline(&pbuf, pn, fp)) >= 0) {
		char *p;

		if (strncmp(buf, "processor", 9) != 0)
			continue;
		for (p = buf+9; isspace(*p); p++)
			;
		if (*p == ':')
			cpus++;
	}
	fclose(fp);

	return cpus;
}

#define FNV_64_PRIME 0x100000001b3ULL
/*
 * tracecmd_generate_traceid - Generate a unique ID, used to identify
 *			       the current tracing session
 *
 * Returns unique ID
 */
unsigned long long tracecmd_generate_traceid(void)
{
	unsigned long long hash = 0;
	unsigned char *ustr;
	struct sysinfo sinfo;
	struct timespec ts;
	char *str = NULL;

	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	sysinfo(&sinfo);
	asprintf(&str, "%ld %ld %ld %ld %ld %ld %ld %ld %d",
		 ts.tv_sec, ts.tv_nsec,
		 sinfo.loads[0], sinfo.loads[1], sinfo.loads[2],
		 sinfo.freeram, sinfo.sharedram, sinfo.freeswap,
		 sinfo.procs);
	if (!str)
		return 0;
	ustr = (unsigned char *)str;
	hash = 0;
	while (*ustr) {
		hash ^= (unsigned long long)*ustr++;
		hash *= FNV_64_PRIME;
	}

	free(str);
	return hash;
}

bool tracecmd_is_version_supported(unsigned int version)
{
	if (version <= FILE_VERSION)
		return true;
	return false;
}
