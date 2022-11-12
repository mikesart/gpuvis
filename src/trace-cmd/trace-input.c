// SPDX-License-Identifier: LGPL-2.1
/*
 * Copyright (C) 2009, 2010 Red Hat Inc, Steven Rostedt <srostedt@redhat.com>
 *
 */
#define _GNU_SOURCE
#define _LARGEFILE64_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <regex.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include <linux/time64.h>

#include "trace-write-local.h"
#include "trace-cmd-local.h"
#include "trace-local.h"
#include "kbuffer.h"
#include "list.h"

#define _STRINGIFY(x) #x
#define STRINGIFY(x) _STRINGIFY(x)

#define MISSING_EVENTS (1 << 31)
#define MISSING_STORED (1 << 30)

#define COMMIT_MASK ((1 << 27) - 1)

/* force uncompressing in memory */
#define INMEMORY_DECOMPRESS

/* for debugging read instead of mmap */
static int force_read = 0;

struct page_map {
	struct list_head	list;
	off64_t			offset;
	off64_t			size;
	void			*map;
	int			ref_count;
};

struct page {
	struct list_head	list;
	off64_t			offset;
	struct tracecmd_input	*handle;
	struct page_map		*page_map;
	void			*map;
	int			ref_count;
	int			cpu;
	long long		lost_events;
#if DEBUG_RECORD
	struct tep_record	*records;
#endif
};

struct zchunk_cache {
	struct list_head		list;
	struct tracecmd_compress_chunk *chunk;
	void				*map;
	int				ref;
};

struct cpu_zdata {
	/* uncompressed cpu data */
	int			fd;
	char			file[26]; /* strlen(COMPR_TEMP_FILE) */
	unsigned int		count;
	unsigned int		last_chunk;
	struct list_head	cache;
	struct tracecmd_compress_chunk	*chunks;
};

#define COMPR_TEMP_FILE "/tmp/trace_cpu_dataXXXXXX"
struct cpu_data {
	/* the first two never change */
	unsigned long long	file_offset;
	unsigned long long	file_size;
	unsigned long long	offset;
	unsigned long long	size;
	unsigned long long	timestamp;
	unsigned long long	first_ts;
	struct list_head	page_maps;
	struct page_map		*page_map;
	struct page		**pages;
	struct tep_record	*next;
	struct page		*page;
	struct kbuffer		*kbuf;
	int			nr_pages;
	int			page_cnt;
	int			cpu;
	int			pipe_fd;
	struct cpu_zdata	compress;
};

struct cpu_file_data {
	int			cpu;
	unsigned long long	offset;
	unsigned long long	size;
};

struct input_buffer_instance {
	char			*name;
	size_t			offset;
	char			*clock;
	bool			latency;
	int			page_size;
	int			cpus;
	struct cpu_file_data	*cpu_data;
};

struct ts_offset_sample {
	long long	time;
	long long	offset;
	long long	scaling;
	long long	fraction;
};

struct guest_trace_info {
	struct guest_trace_info	*next;
	char			*name;
	unsigned long long	trace_id;
	int			vcpu_count;
	int			*cpu_pid;
};

struct timesync_offsets {
	int	ts_samples_count;
	struct ts_offset_sample	*ts_samples;
};

struct host_trace_info {
	unsigned long long	peer_trace_id;
	unsigned int		flags;
	bool			sync_enable;
	int			ts_samples_count;
	struct ts_offset_sample	*ts_samples;
	int			cpu_count;
	struct timesync_offsets	*ts_offsets;
};

struct tsc2nsec {
	int	mult;
	int	shift;
	unsigned long long offset;
};

struct file_section {
	unsigned long long		section_offset;
	unsigned long long		data_offset;
	int				id;
	int				flags;
	struct file_section		*next;
};

struct tracecmd_input {
	struct tep_handle	*pevent;
	struct tep_plugin_list	*plugin_list;
	struct tracecmd_input	*parent;
	struct tracecmd_filter	*filter;
	unsigned long		file_state;
	unsigned long long	trace_id;
	unsigned long long	next_offset;
	unsigned long		flags;
	int			fd;
	int			long_size;
	int			page_size;
	int			page_map_size;
	int			max_cpu;
	int			cpus;
	int			start_cpu;
	int			ref;
	int			nr_buffers;	/* buffer instances */
	bool			use_trace_clock;
	bool			read_page;
	bool			use_pipe;
	bool			read_zpage; /* uncompress pages in memory, do not use tmp files */
	bool			cpu_compressed;
	int			file_version;
	unsigned int		cpustats_size;
	struct cpu_zdata	latz;
	struct cpu_data 	*cpu_data;
	long long		ts_offset;
	struct tsc2nsec		tsc_calc;

	unsigned int		strings_size;	/* size of the metadata strings */
	char			*strings;	/* metadata strings */

	bool			read_compress;
	struct tracecmd_compression *compress;

	struct host_trace_info	host;
	double			ts2secs;
	char *			cpustats;
	char *			uname;
	char *			version;
	char *			trace_clock;
	struct input_buffer_instance	top_buffer;
	struct input_buffer_instance	*buffers;
	int			parsing_failures;
	struct guest_trace_info	*guest;

	struct tracecmd_ftrace	finfo;

	struct hook_list	*hooks;
	struct pid_addr_maps	*pid_maps;
	/* file information */
	struct file_section	*sections;
	bool			options_init;
	unsigned long long	options_start;
	unsigned long long	options_last_offset;
	size_t			total_file_size;

	/* For custom profilers. */
	tracecmd_show_data_func	show_data_func;

	void			*private;
};

__thread struct tracecmd_input *tracecmd_curr_thread_handle;

#define CHECK_READ_STATE(H, S) ((H)->file_version < FILE_VERSION_SECTIONS && (H)->file_state >= (S))
#define HAS_SECTIONS(H) ((H)->flags & TRACECMD_FL_SECTIONED)
#define HAS_COMPRESSION(H) ((H)->flags & TRACECMD_FL_COMPRESSION)

static int read_options_type(struct tracecmd_input *handle);

void tracecmd_set_flag(struct tracecmd_input *handle, int flag)
{
	handle->flags |= flag;
}

void tracecmd_clear_flag(struct tracecmd_input *handle, int flag)
{
	handle->flags &= ~flag;
}

unsigned long tracecmd_get_flags(struct tracecmd_input *handle)
{
	return handle->flags;
}

enum tracecmd_file_states tracecmd_get_file_state(struct tracecmd_input *handle)
{
	return handle->file_state;
}

void tracecmd_set_private(struct tracecmd_input *handle, void *data)
{
	handle->private = data;
}

void *tracecmd_get_private(struct tracecmd_input *handle)
{
	return handle->private;
}

#if DEBUG_RECORD
static void remove_record(struct page *page, struct tep_record *record)
{
	if (record->prev)
		record->prev->next = record->next;
	else
		page->records = record->next;
	if (record->next)
		record->next->prev = record->prev;
}
static void add_record(struct page *page, struct tep_record *record)
{
	if (page->records)
		page->records->prev = record;
	record->next = page->records;
	record->prev = NULL;
	page->records = record;
}
static const char *show_records(struct page **pages, int nr_pages)
{
	static char buf[BUFSIZ + 1];
	struct tep_record *record;
	struct page *page;
	int len;
	int i;

	memset(buf, 0, sizeof(buf));
	len = 0;
	for (i = 0; i < nr_pages; i++) {
		page = pages[i];
		if (!page)
			continue;
		for (record = page->records; record; record = record->next) {
			int n;
			n = snprintf(buf+len, BUFSIZ - len, " 0x%lx", record->alloc_addr);
			len += n;
			if (len >= BUFSIZ)
				break;
		}
	}
	return buf;
}
#else
static inline void remove_record(struct page *page, struct tep_record *record) {}
static inline void add_record(struct page *page, struct tep_record *record) {}
static const char *show_records(struct page **pages, int nr_pages)
{
	return "";
}
#endif

static int init_cpu(struct tracecmd_input *handle, int cpu);

static ssize_t do_read_fd(int fd, void *data, size_t size)
{
	ssize_t tot = 0;
	ssize_t r;

	do {
		r = read(fd, data + tot, size - tot);
		tot += r;

		if (!r)
			break;
		if (r < 0)
			return r;
	} while (tot != size);

	return tot;
}

static inline int do_lseek(struct tracecmd_input *handle, int offset, int whence)
{
	if (handle->read_compress)
		return tracecmd_compress_lseek(handle->compress, offset, whence);
	else
		return lseek(handle->fd, offset, whence);
}

static inline ssize_t do_read(struct tracecmd_input *handle, void *data, size_t size)
{
	if (handle->read_compress)
		return tracecmd_compress_buffer_read(handle->compress, data, size);
	else
		return do_read_fd(handle->fd, data, size);
}

static ssize_t
do_read_check(struct tracecmd_input *handle, void *data, size_t size)
{
	ssize_t ret;

	ret = do_read(handle, data, size);
	if (ret < 0)
		return ret;
	if (ret != size)
		return -1;

	return 0;
}

static char *read_string(struct tracecmd_input *handle)
{
	char buf[BUFSIZ];
	char *str = NULL;
	size_t size = 0;
	ssize_t i;
	ssize_t r;

	for (;;) {
		r = do_read(handle, buf, BUFSIZ);
		if (r <= 0)
			goto fail;

		for (i = 0; i < r; i++) {
			if (!buf[i])
				break;
		}
		if (i < r)
			break;

		if (str) {
			size += BUFSIZ;
			str = realloc(str, size);
			if (!str)
				return NULL;
			memcpy(str + (size - BUFSIZ), buf, BUFSIZ);
		} else {
			size = BUFSIZ;
			str = malloc(size);
			if (!str)
				return NULL;
			memcpy(str, buf, size);
		}
	}

	/* move the file descriptor to the end of the string */
	r = do_lseek(handle, -(r - (i+1)), SEEK_CUR);
	if (r < 0)
		goto fail;

	if (str) {
		size += i + 1;
		str = realloc(str, size);
		if (!str)
			return NULL;
		memcpy(str + (size - i), buf, i);
		str[size] = 0;
	} else {
		size = i + 1;
		str = malloc(size);
		if (!str)
			return NULL;
		memcpy(str, buf, i);
		str[i] = 0;
	}

	return str;

 fail:
	if (str)
		free(str);
	return NULL;
}

static int read2(struct tracecmd_input *handle, unsigned short *size)
{
	struct tep_handle *pevent = handle->pevent;
	unsigned short data;

	if (do_read_check(handle, &data, 2))
		return -1;

	*size = tep_read_number(pevent, &data, 2);
	return 0;
}

static int read4(struct tracecmd_input *handle, unsigned int *size)
{
	struct tep_handle *pevent = handle->pevent;
	unsigned int data;

	if (do_read_check(handle, &data, 4))
		return -1;

	*size = tep_read_number(pevent, &data, 4);
	return 0;
}

static int read8(struct tracecmd_input *handle, unsigned long long *size)
{
	struct tep_handle *pevent = handle->pevent;
	unsigned long long data;

	if (do_read_check(handle, &data, 8))
		return -1;

	*size = tep_read_number(pevent, &data, 8);
	return 0;
}

__hidden void in_uncompress_reset(struct tracecmd_input *handle)
{
	if (handle->compress) {
		handle->read_compress = false;
		tracecmd_compress_reset(handle->compress);
	}
}

__hidden int in_uncompress_block(struct tracecmd_input *handle)
{
	int ret = 0;

	if (handle->compress) {
		ret = tracecmd_uncompress_block(handle->compress);
		if (!ret)
			handle->read_compress = true;
	}
	return ret;
}

static struct file_section *section_get(struct tracecmd_input *handle, int id)
{
	struct file_section *sec;

	for (sec = handle->sections; sec; sec = sec->next) {
		if (sec->id == id)
			return sec;
	}

	return NULL;
}

static struct file_section *section_open(struct tracecmd_input *handle, int id)
{
	struct file_section *sec = section_get(handle, id);

	if (!sec)
		return NULL;

	if (lseek64(handle->fd, sec->data_offset, SEEK_SET) == (off64_t)-1)
		return NULL;

	if ((sec->flags & TRACECMD_SEC_FL_COMPRESS) && in_uncompress_block(handle))
		return NULL;

	return sec;
}

static void section_close(struct tracecmd_input *handle, struct file_section *sec)
{
	if (sec->flags & TRACECMD_SEC_FL_COMPRESS)
		in_uncompress_reset(handle);
}

static int section_add_or_update(struct tracecmd_input *handle, int id, int flags,
				 unsigned long long section_offset,
				 unsigned long long data_offset)
{
	struct file_section *sec = section_get(handle, id);

	if (!sec) {
		sec = calloc(1, sizeof(struct file_section));
		if (!sec)
			return -1;
		sec->next = handle->sections;
		handle->sections = sec;
		sec->id = id;
	}

	if (section_offset)
		sec->section_offset = section_offset;
	if (data_offset)
		sec->data_offset = data_offset;
	if (flags >= 0)
		sec->flags = flags;

	return 0;
}

static int read_header_files(struct tracecmd_input *handle)
{
	struct tep_handle *pevent = handle->pevent;
	unsigned long long size;
	char *header;
	char buf[BUFSIZ];

	if (CHECK_READ_STATE(handle, TRACECMD_FILE_HEADERS))
		return 0;

	if (!HAS_SECTIONS(handle))
		section_add_or_update(handle, TRACECMD_OPTION_HEADER_INFO, 0, 0,
				      lseek64(handle->fd, 0, SEEK_CUR));

	if (do_read_check(handle, buf, 12))
		return -1;

	if (memcmp(buf, "header_page", 12) != 0)
		return -1;

	if (read8(handle, &size) < 0)
		return -1;

	header = malloc(size);
	if (!header)
		return -1;

	if (do_read_check(handle, header, size))
		goto failed_read;

	tep_parse_header_page(pevent, header, size, handle->long_size);
	free(header);

	/*
	 * The size field in the page is of type long,
	 * use that instead, since it represents the kernel.
	 */
	handle->long_size = tep_get_header_page_size(pevent);

	if (do_read_check(handle, buf, 13))
		return -1;

	if (memcmp(buf, "header_event", 13) != 0)
		return -1;

	if (read8(handle, &size) < 0)
		return -1;

	header = malloc(size);
	if (!header)
		return -1;

	if (do_read_check(handle, header, size))
		goto failed_read;

	free(header);

	handle->file_state = TRACECMD_FILE_HEADERS;

	return 0;

 failed_read:
	free(header);
	return -1;
}

static int regex_event_buf(const char *file, int size, regex_t *epreg)
{
	char *buf;
	char *line;
	int ret;

	buf = malloc(size + 1);
	if (!buf) {
		tracecmd_warning("Insufficient memory");
		return 0;
	}

	strncpy(buf, file, size);
	buf[size] = 0;

	/* get the name from the first line */
	line = strtok(buf, "\n");
	if (!line) {
		tracecmd_warning("No newline found in '%s'", buf);
		return 0;
	}
	/* skip name if it is there */
	if (strncmp(line, "name: ", 6) == 0)
		line += 6;

	ret = regexec(epreg, line, 0, NULL, 0) == 0;

	free(buf);

	return ret;
}

static int read_ftrace_file(struct tracecmd_input *handle,
			    unsigned long long size,
			    int print, regex_t *epreg)
{
	struct tep_handle *pevent = handle->pevent;
	char *buf;

	buf = malloc(size);
	if (!buf)
		return -1;
	if (do_read_check(handle, buf, size)) {
		free(buf);
		return -1;
	}

	if (epreg) {
		if (print || regex_event_buf(buf, size, epreg))
			printf("%.*s\n", (int)size, buf);
	} else {
		if (tep_parse_event(pevent, buf, size, "ftrace"))
			handle->parsing_failures++;
	}
	free(buf);

	return 0;
}

static int read_event_file(struct tracecmd_input *handle,
			   char *system, unsigned long long size,
			   int print, int *sys_printed,
			   regex_t *epreg)
{
	struct tep_handle *pevent = handle->pevent;
	char *buf;

	buf = malloc(size);
	if (!buf)
		return -1;

	if (do_read_check(handle, buf, size)) {
		free(buf);
		return -1;
	}

	if (epreg) {
		if (print || regex_event_buf(buf, size, epreg)) {
			if (!*sys_printed) {
				printf("\nsystem: %s\n", system);
				*sys_printed = 1;
			}
			printf("%.*s\n", (int)size, buf);
		}
	} else {
		if (tep_parse_event(pevent, buf, size, system))
			handle->parsing_failures++;
	}
	free(buf);

	return 0;
}

static int make_preg_files(const char *regex, regex_t *system,
			   regex_t *event, int *unique)
{
	char *buf;
	char *sstr;
	char *estr;
	int ret;

	/* unique is set if a colon is found */
	*unique = 0;

	/* split "system:event" into "system" and "event" */

	buf = strdup(regex);
	if (!buf)
		return -ENOMEM;

	sstr = strtok(buf, ":");
	estr = strtok(NULL, ":");

	/* If no colon is found, set event == system */
	if (!estr)
		estr = sstr;
	else
		*unique = 1;

	ret = regcomp(system, sstr, REG_ICASE|REG_NOSUB);
	if (ret) {
		tracecmd_warning("Bad regular expression '%s'", sstr);
		goto out;
	}

	ret = regcomp(event, estr, REG_ICASE|REG_NOSUB);
	if (ret) {
		tracecmd_warning("Bad regular expression '%s'", estr);
		goto out;
	}

 out:
	free(buf);
	return ret;
}

static int read_ftrace_files(struct tracecmd_input *handle, const char *regex)
{
	unsigned long long size;
	regex_t spreg;
	regex_t epreg;
	regex_t *sreg = NULL;
	regex_t *ereg = NULL;
	unsigned int count, i;
	int print_all = 0;
	int unique;
	int ret;

	if (CHECK_READ_STATE(handle, TRACECMD_FILE_FTRACE_EVENTS))
		return 0;

	if (!HAS_SECTIONS(handle))
		section_add_or_update(handle, TRACECMD_OPTION_FTRACE_EVENTS, 0, 0,
				      lseek64(handle->fd, 0, SEEK_CUR));

	if (regex) {
		sreg = &spreg;
		ereg = &epreg;
		ret = make_preg_files(regex, sreg, ereg, &unique);
		if (ret)
			return -1;

		if (regexec(sreg, "ftrace", 0, NULL, 0) == 0) {
			/*
			 * If the system matches a regex that did
			 * not contain a colon, then print all events.
			 */
			if (!unique)
				print_all = 1;
		} else if (unique) {
			/*
			 * The user specified a unique event that did
			 * not match the ftrace system. Don't print any
			 * events here.
			 */
			regfree(sreg);
			regfree(ereg);
			sreg = NULL;
			ereg = NULL;
		}
	}

	ret = read4(handle, &count);
	if (ret < 0)
		goto out;

	for (i = 0; i < count; i++) {
		ret = read8(handle, &size);
		if (ret < 0)
			goto out;
		ret = read_ftrace_file(handle, size, print_all, ereg);
		if (ret < 0)
			goto out;
	}

	handle->file_state = TRACECMD_FILE_FTRACE_EVENTS;
	ret = 0;
out:
	if (sreg) {
		regfree(sreg);
		regfree(ereg);
	}

	return ret;
}

static int read_event_files(struct tracecmd_input *handle, const char *regex)
{
	unsigned long long size;
	char *system = NULL;
	regex_t spreg;
	regex_t epreg;
	regex_t *sreg = NULL;
	regex_t *ereg = NULL;
	regex_t *reg;
	unsigned int systems;
	unsigned int count;
	unsigned int i, x;
	int print_all;
	int sys_printed;
	int unique;
	int ret;

	if (CHECK_READ_STATE(handle, TRACECMD_FILE_ALL_EVENTS))
		return 0;

	if (!HAS_SECTIONS(handle))
		section_add_or_update(handle, TRACECMD_OPTION_EVENT_FORMATS, 0, 0,
				      lseek64(handle->fd, 0, SEEK_CUR));

	if (regex) {
		sreg = &spreg;
		ereg = &epreg;
		ret = make_preg_files(regex, sreg, ereg, &unique);
		if (ret)
			return -1;
	}

	ret = read4(handle, &systems);
	if (ret < 0)
		goto out;

	for (i = 0; i < systems; i++) {
		system = read_string(handle);
		if (!system) {
			ret = -1;
			goto out;
		}

		sys_printed = 0;
		print_all = 0;
		reg = ereg;

		if (sreg) {
			if (regexec(sreg, system, 0, NULL, 0) == 0) {
				/*
				 * If the user passed in a regex that
				 * did not contain a colon, then we can
				 * print all the events of this system.
				 */
				if (!unique)
					print_all = 1;
			} else if (unique) {
				/*
				 * The user passed in a unique event that
				 * specified a specific system and event.
				 * Since this system doesn't match this
				 * event, then we don't print any events
				 * for this system.
				 */
				reg = NULL;
			}
		}

		ret = read4(handle, &count);
		if (ret < 0)
			goto out;

		for (x=0; x < count; x++) {
			ret = read8(handle, &size);
			if (ret < 0)
				goto out;

			ret = read_event_file(handle, system, size,
					      print_all, &sys_printed,
					      reg);
			if (ret < 0)
				goto out;
		}
		free(system);
	}
	system = NULL;

	handle->file_state = TRACECMD_FILE_ALL_EVENTS;
	ret = 0;
 out:
	if (sreg) {
		regfree(sreg);
		regfree(ereg);
	}

	free(system);
	return ret;
}

static int read_proc_kallsyms(struct tracecmd_input *handle)
{
	struct tep_handle *tep = handle->pevent;
	unsigned int size;
	char *buf;

	if (CHECK_READ_STATE(handle, TRACECMD_FILE_KALLSYMS))
		return 0;
	if (!HAS_SECTIONS(handle))
		section_add_or_update(handle, TRACECMD_OPTION_KALLSYMS, 0, 0,
				      lseek64(handle->fd, 0, SEEK_CUR));

	if (read4(handle, &size) < 0)
		return -1;
	if (!size) {
		handle->file_state = TRACECMD_FILE_KALLSYMS;
		return 0; /* OK? */
	}

	buf = malloc(size+1);
	if (!buf)
		return -1;
	if (do_read_check(handle, buf, size)){
		free(buf);
		return -1;
	}
	buf[size] = 0;

	tep_parse_kallsyms(tep, buf);

	free(buf);

	handle->file_state = TRACECMD_FILE_KALLSYMS;

	return 0;
}

static int read_ftrace_printk(struct tracecmd_input *handle)
{
	unsigned int size;
	char *buf;

	if (CHECK_READ_STATE(handle, TRACECMD_FILE_PRINTK))
		return 0;

	if (!HAS_SECTIONS(handle))
		section_add_or_update(handle, TRACECMD_OPTION_PRINTK, 0, 0,
				      lseek64(handle->fd, 0, SEEK_CUR));

	if (read4(handle, &size) < 0)
		return -1;
	if (!size) {
		handle->file_state = TRACECMD_FILE_PRINTK;
		return 0; /* OK? */
	}

	buf = malloc(size + 1);
	if (!buf)
		return -1;
	if (do_read_check(handle, buf, size)) {
		free(buf);
		return -1;
	}

	buf[size] = 0;

	tep_parse_printk_formats(handle->pevent, buf);

	free(buf);

	handle->file_state = TRACECMD_FILE_PRINTK;

	return 0;
}

static int read_and_parse_cmdlines(struct tracecmd_input *handle);

/**
 * tracecmd_get_parsing_failures - get the count of parsing failures
 * @handle: input handle for the trace.dat file
 *
 * This returns the count of failures while parsing the event files
 */
int tracecmd_get_parsing_failures(struct tracecmd_input *handle)
{
	if (handle)
		return handle->parsing_failures;
	return 0;
}

static int read_cpus(struct tracecmd_input *handle)
{
	unsigned int cpus;

	if (CHECK_READ_STATE(handle, TRACECMD_FILE_CPU_COUNT))
		return 0;

	if (read4(handle, &cpus) < 0)
		return -1;

	handle->cpus = cpus;
	handle->max_cpu = cpus;
	tep_set_cpus(handle->pevent, handle->cpus);
	handle->file_state = TRACECMD_FILE_CPU_COUNT;

	return 0;
}

static int read_headers_v6(struct tracecmd_input *handle, enum tracecmd_file_states state,
			   const char *regex)
{
	int ret;

	/* Set to read all if state is zero */
	if (!state)
		state = TRACECMD_FILE_OPTIONS;

	if (state <= handle->file_state)
		return 0;

	handle->parsing_failures = 0;

	ret = read_header_files(handle);
	if (ret < 0)
		return -1;

	if (state <= handle->file_state)
		return 0;

	ret = read_ftrace_files(handle, NULL);
	if (ret < 0)
		return -1;

	if (state <= handle->file_state)
		return 0;

	ret = read_event_files(handle, regex);
	if (ret < 0)
		return -1;

	if (state <= handle->file_state)
		return 0;

	ret = read_proc_kallsyms(handle);
	if (ret < 0)
		return -1;

	if (state <= handle->file_state)
		return 0;

	ret = read_ftrace_printk(handle);
	if (ret < 0)
		return -1;

	if (state <= handle->file_state)
		return 0;

	if (read_and_parse_cmdlines(handle) < 0)
		return -1;

	if (state <= handle->file_state)
		return 0;

	if (read_cpus(handle) < 0)
		return -1;

	if (state <= handle->file_state)
		return 0;

	if (read_options_type(handle) < 0)
		return -1;

	return 0;
}

static int handle_options(struct tracecmd_input *handle);

static const char *get_metadata_string(struct tracecmd_input *handle, int offset)
{
	if (!handle || !handle->strings || offset < 0 || handle->strings_size >= offset)
		return NULL;

	return handle->strings + offset;
}

static int read_section_header(struct tracecmd_input *handle, unsigned short *id,
			       unsigned short *flags, unsigned long long *size, const char **description)
{
	unsigned short fl;
	unsigned short sec_id;
	unsigned long long sz;
	int desc;

	if (read2(handle, &sec_id))
		return -1;
	if (read2(handle, &fl))
		return -1;
	if (read4(handle, (unsigned int *)&desc))
		return -1;
	if (read8(handle, &sz))
		return -1;

	if (id)
		*id = sec_id;
	if (flags)
		*flags = fl;
	if (size)
		*size = sz;
	if (description)
		*description = get_metadata_string(handle, desc);

	return 0;
}

static int handle_section(struct tracecmd_input *handle, struct file_section *section,
			  const char *regex)
{
	unsigned short id, flags;
	unsigned long long size;
	int ret;

	if (lseek64(handle->fd, section->section_offset, SEEK_SET) == (off_t)-1)
		return -1;
	if (read_section_header(handle, &id, &flags, &size, NULL))
		return -1;
	section->flags = flags;
	if (id != section->id)
		return -1;

	section->data_offset = lseek64(handle->fd, 0, SEEK_CUR);
	if ((section->flags & TRACECMD_SEC_FL_COMPRESS) && in_uncompress_block(handle))
		return -1;

	switch (section->id) {
	case TRACECMD_OPTION_HEADER_INFO:
		ret = read_header_files(handle);
		break;
	case TRACECMD_OPTION_FTRACE_EVENTS:
		ret = read_ftrace_files(handle, NULL);
		break;
	case TRACECMD_OPTION_EVENT_FORMATS:
		ret = read_event_files(handle, regex);
		break;
	case TRACECMD_OPTION_KALLSYMS:
		ret = read_proc_kallsyms(handle);
		break;
	case TRACECMD_OPTION_PRINTK:
		ret = read_ftrace_printk(handle);
		break;
	case TRACECMD_OPTION_CMDLINES:
		ret = read_and_parse_cmdlines(handle);
		break;
	default:
		ret = 0;
		break;
	}

	if (section->flags & TRACECMD_SEC_FL_COMPRESS)
		in_uncompress_reset(handle);

	return ret;
}

static int read_headers(struct tracecmd_input *handle, const char *regex)
{
	struct file_section *section;

	if (handle->options_init)
		return 0;

	if (!handle->options_start)
		return -1;

	if (lseek64(handle->fd, handle->options_start, SEEK_SET) == (off64_t)-1) {
		tracecmd_warning("Filed to goto options offset %lld", handle->options_start);
		return -1;
	}

	if (handle_options(handle))
		return -1;

	section = handle->sections;
	while (section) {
		if (handle_section(handle, section, NULL))
			return -1;
		section = section->next;
	}

	handle->options_init = true;
	return 0;
}

/**
 * tracecmd_read_headers - read the header information from trace.dat
 * @handle: input handle for the trace.dat file
 * @state: The state to read up to or zero to read up to options.
 *
 * This reads the trace.dat file for various information. Like the
 * format of the ring buffer, event formats, ftrace formats, kallsyms
 * and printk. This may be called multiple times with different @state
 * values, to read partial data at a time. It will always continue
 * where it left off.
 */
int tracecmd_read_headers(struct tracecmd_input *handle,
			  enum tracecmd_file_states state)
{
	if (!HAS_SECTIONS(handle))
		return read_headers_v6(handle, state, NULL);
	return read_headers(handle, NULL);
}

static unsigned long long calc_page_offset(struct tracecmd_input *handle,
					   unsigned long long offset)
{
	return offset & ~(handle->page_size - 1);
}

static int read_page(struct tracecmd_input *handle, off64_t offset,
		     int cpu, void *map)
{
	off64_t save_seek;
	off64_t ret;

	if (handle->use_pipe) {
		ret = read(handle->cpu_data[cpu].pipe_fd, map, handle->page_size);
		/* Set EAGAIN if the pipe is empty */
		if (ret < 0) {
			errno = EAGAIN;
			return -1;

		} else if (ret == 0) {
			/* Set EINVAL when the pipe has closed */
			errno = EINVAL;
			return -1;
		}
		return 0;
	}

	/* other parts of the code may expect the pointer to not move */
	save_seek = lseek64(handle->fd, 0, SEEK_CUR);

	ret = lseek64(handle->fd, offset, SEEK_SET);
	if (ret < 0)
		return -1;
	ret = read(handle->fd, map, handle->page_size);
	if (ret < 0)
		return -1;

	/* reset the file pointer back */
	lseek64(handle->fd, save_seek, SEEK_SET);

	return 0;
}

/* page_map_size must be a power of two */
static unsigned long long normalize_size(unsigned long long size)
{
	/* From Hacker's Delight: or bits after first set bit to all 1s */
	size |= (size >> 1);
	size |= (size >> 2);
	size |= (size >> 4);
	size |= (size >> 8);
	size |= (size >> 16);
	size |= (size >> 32);

	/* Clear all bits except first one for previous power of two */
	return size - (size >> 1);
}

static void free_page_map(struct page_map *page_map)
{
	page_map->ref_count--;
	if (page_map->ref_count)
		return;

	munmap(page_map->map, page_map->size);
	list_del(&page_map->list);
	free(page_map);
}

#define CHUNK_CHECK_OFFSET(C, O)	((O) >= (C)->offset && (O) < ((C)->offset + (C)->size))

static int chunk_cmp(const void *A, const void *B)
{
	const struct tracecmd_compress_chunk *a = A;
	const struct tracecmd_compress_chunk *b = B;

	if (CHUNK_CHECK_OFFSET(b, a->offset))
		return 0;

	if (a->offset < b->offset)
		return -1;

	return 1;
}

static struct tracecmd_compress_chunk *get_zchunk(struct cpu_data *cpu, off64_t offset)
{
	struct cpu_zdata *cpuz = &cpu->compress;
	struct tracecmd_compress_chunk *chunk;
	struct tracecmd_compress_chunk key;

	if (!cpuz->chunks)
		return NULL;

	if (offset > (cpuz->chunks[cpuz->count - 1].offset + cpuz->chunks[cpuz->count - 1].size))
		return NULL;

	/* check if the requested offset is in the last requested chunk or in the next chunk */
	if (CHUNK_CHECK_OFFSET(cpuz->chunks + cpuz->last_chunk, offset))
		return cpuz->chunks + cpuz->last_chunk;

	cpuz->last_chunk++;
	if (cpuz->last_chunk < cpuz->count &&
	    CHUNK_CHECK_OFFSET(cpuz->chunks + cpuz->last_chunk, offset))
		return cpuz->chunks + cpuz->last_chunk;

	key.offset = offset;
	chunk = bsearch(&key, cpuz->chunks, cpuz->count, sizeof(*chunk), chunk_cmp);

	if (!chunk) /* should never happen */
		return NULL;

	cpuz->last_chunk = chunk - cpuz->chunks;
	return chunk;
}

static void free_zpage(struct cpu_data *cpu_data, void *map)
{
	struct zchunk_cache *cache;

	list_for_each_entry(cache, &cpu_data->compress.cache, list) {
		if (map <= cache->map && map > (cache->map + cache->chunk->size))
			goto found;
	}
	return;

found:
	cache->ref--;
	if (cache->ref)
		return;
	list_del(&cache->list);
	free(cache->map);
	free(cache);
}

static void *read_zpage(struct tracecmd_input *handle, int cpu, off64_t offset)
{
	struct cpu_data *cpu_data = &handle->cpu_data[cpu];
	struct tracecmd_compress_chunk *chunk;
	struct zchunk_cache *cache;
	void *map = NULL;
	int pindex;
	int size;

	offset -= cpu_data->file_offset;

	/* Look in the cache of already loaded chunks */
	list_for_each_entry(cache, &cpu_data->compress.cache, list) {
		if (CHUNK_CHECK_OFFSET(cache->chunk, offset)) {
			cache->ref++;
			goto out;
		}
	}

	chunk =  get_zchunk(cpu_data, offset);
	if (!chunk)
		return NULL;

	size = handle->page_size > chunk->size ? handle->page_size : chunk->size;
	map = malloc(size);
	if (!map)
		return NULL;

	if (tracecmd_uncompress_chunk(handle->compress, chunk, map) < 0)
		goto error;

	cache = calloc(1, sizeof(struct zchunk_cache));
	if (!cache)
		goto error;

	cache->ref = 1;
	cache->chunk = chunk;
	cache->map = map;
	list_add(&cache->list, &cpu_data->compress.cache);

	/* a chunk can hold multiple pages, get the requested one */
out:
	pindex = (offset - cache->chunk->offset) / handle->page_size;
	return cache->map + (pindex * handle->page_size);
error:
	free(map);
	return NULL;
}

static void *allocate_page_map(struct tracecmd_input *handle,
			       struct page *page, int cpu, off64_t offset)
{
	struct cpu_data *cpu_data = &handle->cpu_data[cpu];
	struct page_map *page_map;
	off64_t map_size;
	off64_t map_offset;
	void *map;
	int ret;
	int fd;

	if (handle->cpu_compressed) {
		if (handle->read_zpage)
			return read_zpage(handle, cpu, offset);
		offset -= cpu_data->file_offset;
	}

	if (handle->read_page) {
		map = malloc(handle->page_size);
		if (!map)
			return NULL;
		ret = read_page(handle, offset, cpu, map);
		if (ret < 0) {
			free(map);
			return NULL;
		}
		return map;
	}

	map_size = handle->page_map_size;
	map_offset = offset & ~(map_size - 1);

	if (!handle->cpu_compressed && map_offset < cpu_data->file_offset) {
		map_size -= cpu_data->file_offset - map_offset;
		map_offset = cpu_data->file_offset;
	}

	page_map = cpu_data->page_map;

	if (page_map && page_map->offset == map_offset)
		goto out;

	list_for_each_entry(page_map, &cpu_data->page_maps, list) {
		if (page_map->offset == map_offset)
			goto out;
	}

	page_map = calloc(1, sizeof(*page_map));
	if (!page_map)
		return NULL;

	if (map_offset + map_size > cpu_data->file_offset + cpu_data->file_size)
		map_size -= map_offset + map_size -
			(cpu_data->file_offset + cpu_data->file_size);

	if (cpu_data->compress.fd >= 0)
		fd = cpu_data->compress.fd;
	else
		fd = handle->fd;
 again:
	page_map->size = map_size;
	page_map->offset = map_offset;

	page_map->map = mmap(NULL, map_size, PROT_READ, MAP_PRIVATE, fd, map_offset);

	if (page_map->map == MAP_FAILED) {
		/* Try a smaller map */
		map_size >>= 1;
		if (map_size < handle->page_size) {
			free(page_map);
			return NULL;
		}
		handle->page_map_size = map_size;
		map_offset = offset & ~(map_size - 1);
		/*
		 * Note, it is now possible to get duplicate memory
		 * maps. But that's fine, the previous maps with
		 * larger sizes will eventually be unmapped.
		 */
		goto again;
	}

	list_add(&page_map->list, &cpu_data->page_maps);
 out:
	if (cpu_data->page_map != page_map) {
		struct page_map *old_map = cpu_data->page_map;
		cpu_data->page_map = page_map;
		page_map->ref_count++;
		if (old_map)
			free_page_map(old_map);
	}
	page->page_map = page_map;
	page_map->ref_count++;
	return page_map->map + offset - page_map->offset;
}

static struct page *allocate_page(struct tracecmd_input *handle,
				  int cpu, off64_t offset)
{
	struct cpu_data *cpu_data = &handle->cpu_data[cpu];
	struct page **pages;
	struct page *page;
	int index;

	index = (offset - cpu_data->file_offset) / handle->page_size;
	if (index >= cpu_data->nr_pages) {
		pages = realloc(cpu_data->pages, (index + 1) * sizeof(*cpu_data->pages));
		if (!pages)
			return NULL;
		memset(pages + cpu_data->nr_pages, 0,
		       (index + 1 - cpu_data->nr_pages) * sizeof(*cpu_data->pages));
		cpu_data->pages = pages;
		cpu_data->nr_pages = index + 1;
	}
	if (cpu_data->pages[index]) {
		cpu_data->pages[index]->ref_count++;
		return cpu_data->pages[index];
	}

	page = malloc(sizeof(*page));
	if (!page)
		return NULL;

	memset(page, 0, sizeof(*page));
	page->offset = offset;
	page->handle = handle;
	page->cpu = cpu;

	page->map = allocate_page_map(handle, page, cpu, offset);

	if (!page->map) {
		free(page);
		return NULL;
	}

	cpu_data->pages[index] = page;
	cpu_data->page_cnt++;
	page->ref_count = 1;

	return page;
}

static void __free_page(struct tracecmd_input *handle, struct page *page)
{
	struct cpu_data *cpu_data = &handle->cpu_data[page->cpu];
	struct page **pages;
	int index;

	if (!page->ref_count) {
		tracecmd_critical("Page ref count is zero!");
		return;
	}

	page->ref_count--;
	if (page->ref_count)
		return;

	if (handle->read_page)
		free(page->map);
	else if (handle->read_zpage)
		free_zpage(cpu_data, page->map);
	else
		free_page_map(page->page_map);

	index = (page->offset - cpu_data->file_offset) / handle->page_size;
	cpu_data->pages[index] = NULL;
	cpu_data->page_cnt--;

	free(page);

	if (handle->use_pipe) {
		for (index = cpu_data->nr_pages - 1; index > 0; index--)
			if (cpu_data->pages[index])
				break;
		if (index < (cpu_data->nr_pages - 1)) {
			pages = realloc(cpu_data->pages, (index + 1) * sizeof(*cpu_data->pages));
			if (!pages)
				return;
			cpu_data->pages = pages;
			cpu_data->nr_pages = index + 1;
		}
	}
}

static void free_page(struct tracecmd_input *handle, int cpu)
{
	if (!handle->cpu_data || cpu >= handle->cpus ||
	    !handle->cpu_data[cpu].page)
		return;

	__free_page(handle, handle->cpu_data[cpu].page);

	handle->cpu_data[cpu].page = NULL;
}

static void __free_record(struct tep_record *record)
{
	if (record->priv) {
		struct page *page = record->priv;
		remove_record(page, record);
		__free_page(page->handle, page);
	}

	free(record);
}

void tracecmd_free_record(struct tep_record *record)
{
	if (!record)
		return;

	if (!record->ref_count) {
		tracecmd_critical("record ref count is zero!");
		return;
	}

	record->ref_count--;

	if (record->ref_count)
		return;

	if (record->locked) {
		tracecmd_critical("freeing record when it is locked!");
		return;
	}

	record->data = NULL;

	__free_record(record);
}

void tracecmd_record_ref(struct tep_record *record)
{
	record->ref_count++;
#if DEBUG_RECORD
	/* Update locating of last reference */
	record->alloc_addr = (unsigned long)__builtin_return_address(0);
#endif
}

static void free_next(struct tracecmd_input *handle, int cpu)
{
	struct tep_record *record;

	if (!handle->cpu_data || cpu >= handle->cpus)
		return;

	record = handle->cpu_data[cpu].next;
	if (!record)
		return;

	handle->cpu_data[cpu].next = NULL;

	record->locked = 0;
	tracecmd_free_record(record);
}

/* This functions was taken from the Linux kernel */
static unsigned long long mul_u64_u32_shr(unsigned long long a,
					  unsigned long long mul, unsigned int shift)
{
	unsigned int ah, al;
	unsigned long long ret;

	al = a;
	ah = a >> 32;

	ret = (al * mul) >> shift;
	if (ah)
		ret += (ah * mul) << (32 - shift);

	return ret;
}

static inline unsigned long long
timestamp_correction_calc(unsigned long long ts, unsigned int flags,
			  struct ts_offset_sample *min,
			  struct ts_offset_sample *max)
{
	long long tscor;

	if (flags & TRACECMD_TSYNC_FLAG_INTERPOLATE) {
		long long delta = max->time - min->time;
		long long offset = ((long long)ts - min->time) *
				   (max->offset - min->offset);

		tscor = min->offset + (offset + delta / 2) / delta;
	} else {
		tscor = min->offset;
	}

	ts = (ts * min->scaling) >> min->fraction;
	if (tscor < 0)
		return ts - llabs(tscor);

	return ts + tscor;
}

static unsigned long long timestamp_host_sync(unsigned long long ts, int cpu,
					      struct tracecmd_input *handle)
{
	struct timesync_offsets *tsync;
	int min, mid, max;

	if (cpu >= handle->host.cpu_count)
		return ts;
	tsync = &handle->host.ts_offsets[cpu];

	/* We have one sample, nothing to calc here */
	if (tsync->ts_samples_count == 1)
		return ts + tsync->ts_samples[0].offset;

	/* We have two samples, nothing to search here */
	if (tsync->ts_samples_count == 2)
		return timestamp_correction_calc(ts, handle->host.flags,
						 &tsync->ts_samples[0],
						 &tsync->ts_samples[1]);

	/* We have more than two samples */
	if (ts <= tsync->ts_samples[0].time)
		return timestamp_correction_calc(ts, handle->host.flags,
						 &tsync->ts_samples[0],
						 &tsync->ts_samples[1]);
	else if (ts >= tsync->ts_samples[tsync->ts_samples_count-1].time)
		return timestamp_correction_calc(ts, handle->host.flags,
						 &tsync->ts_samples[tsync->ts_samples_count-2],
						 &tsync->ts_samples[tsync->ts_samples_count-1]);
	min = 0;
	max = tsync->ts_samples_count-1;
	mid = (min + max)/2;
	while (min <= max) {
		if (ts < tsync->ts_samples[mid].time)
			max = mid - 1;
		else if (ts > tsync->ts_samples[mid].time)
			min = mid + 1;
		else
			break;
		mid = (min + max)/2;
	}

	return timestamp_correction_calc(ts, handle->host.flags,
					 &tsync->ts_samples[mid],
					 &tsync->ts_samples[mid+1]);
}

static unsigned long long timestamp_calc(unsigned long long ts, int cpu,
					 struct tracecmd_input *handle)
{
	/* do not modify raw timestamps */
	if (handle->flags & TRACECMD_FL_RAW_TS)
		return ts;

	/* Guest trace file, sync with host timestamps */
	if (handle->host.sync_enable)
		ts = timestamp_host_sync(ts, cpu, handle);

	if (handle->ts2secs) {
		/* user specified clock frequency */
		ts *= handle->ts2secs;
	} else if (handle->tsc_calc.mult) {
		/* auto calculated TSC clock frequency */
		ts = mul_u64_u32_shr(ts, handle->tsc_calc.mult, handle->tsc_calc.shift);
	}

	/* User specified time offset with --ts-offset or --date options */
	ts += handle->ts_offset;

	return ts;
}

/*
 * Page is mapped, now read in the page header info.
 */
static int update_page_info(struct tracecmd_input *handle, int cpu)
{
	struct tep_handle *pevent = handle->pevent;
	void *ptr = handle->cpu_data[cpu].page->map;
	struct kbuffer *kbuf = handle->cpu_data[cpu].kbuf;

	/* FIXME: handle header page */
	if (tep_get_header_timestamp_size(pevent) != 8) {
		tracecmd_warning("expected a long long type for timestamp");
		return -1;
	}

	kbuffer_load_subbuffer(kbuf, ptr);
	if (kbuffer_subbuffer_size(kbuf) > handle->page_size) {
		tracecmd_warning("bad page read, with size of %d", kbuffer_subbuffer_size(kbuf));
		return -1;
	}
	handle->cpu_data[cpu].timestamp = timestamp_calc(kbuffer_timestamp(kbuf),
							 cpu, handle);

	return 0;
}

/*
 * get_page maps a page for a given cpu.
 *
 * Returns 1 if the page was already mapped,
 *         0 if it mapped successfully
 *        -1 on error
 */
static int get_page(struct tracecmd_input *handle, int cpu,
		    off64_t offset)
{
	/* Don't map if the page is already where we want */
	if (handle->cpu_data[cpu].offset == offset &&
	    handle->cpu_data[cpu].page)
		return 1;

	/* Do not map no data for CPU */
	if (!handle->cpu_data[cpu].size)
		return -1;

	if (offset & (handle->page_size - 1)) {
		errno = -EINVAL;
		tracecmd_critical("bad page offset %llx", offset);
		return -1;
	}

	if (offset < handle->cpu_data[cpu].file_offset ||
	    offset > handle->cpu_data[cpu].file_offset +
	    handle->cpu_data[cpu].file_size) {
		errno = -EINVAL;
		tracecmd_critical("bad page offset %llx", offset);
		return -1;
	}

	handle->cpu_data[cpu].offset = offset;
	handle->cpu_data[cpu].size = (handle->cpu_data[cpu].file_offset +
				      handle->cpu_data[cpu].file_size) -
					offset;

	free_page(handle, cpu);

	handle->cpu_data[cpu].page = allocate_page(handle, cpu, offset);
	if (!handle->cpu_data[cpu].page)
		return -1;

	if (update_page_info(handle, cpu))
		return -1;

	return 0;
}

static int get_next_page(struct tracecmd_input *handle, int cpu)
{
	off64_t offset;

	if (!handle->cpu_data[cpu].page && !handle->use_pipe)
		return 0;

	free_page(handle, cpu);

	if (handle->cpu_data[cpu].size <= handle->page_size) {
		handle->cpu_data[cpu].offset = 0;
		return 0;
	}

	offset = handle->cpu_data[cpu].offset + handle->page_size;

	return get_page(handle, cpu, offset);
}

static struct tep_record *
peek_event(struct tracecmd_input *handle, unsigned long long offset,
	   int cpu)
{
	struct tep_record *record = NULL;

	/*
	 * Since the timestamp is calculated from the beginning
	 * of the page and through each event, we reset the
	 * page to the beginning. This is just used by
	 * tracecmd_read_at.
	 */
	update_page_info(handle, cpu);

	do {
		free_next(handle, cpu);
		record = tracecmd_peek_data(handle, cpu);
		if (record && (record->offset + record->record_size) > offset)
			break;
        } while (record);

	return record;
}

static struct tep_record *
read_event(struct tracecmd_input *handle, unsigned long long offset,
	   int cpu)
{
	struct tep_record *record;

	record = peek_event(handle, offset, cpu);
	if (record)
		record = tracecmd_read_data(handle, cpu);
	return record;
}

static struct tep_record *
find_and_peek_event(struct tracecmd_input *handle, unsigned long long offset,
		    int *pcpu)
{
	unsigned long long page_offset;
	int cpu;

	/* find the cpu that this offset exists in */
	for (cpu = 0; cpu < handle->cpus; cpu++) {
		if (offset >= handle->cpu_data[cpu].file_offset &&
		    offset < handle->cpu_data[cpu].file_offset +
		    handle->cpu_data[cpu].file_size)
			break;
	}

	/* Not found? */
	if (cpu == handle->cpus)
		return NULL;

	/* Move this cpu index to point to this offest */
	page_offset = calc_page_offset(handle, offset);

	if (get_page(handle, cpu, page_offset) < 0)
		return NULL;

	if (pcpu)
		*pcpu = cpu;

	return peek_event(handle, offset, cpu);
}


static struct tep_record *
find_and_read_event(struct tracecmd_input *handle, unsigned long long offset,
		    int *pcpu)
{
	struct tep_record *record;
	int cpu;

	record = find_and_peek_event(handle, offset, &cpu);
	if (record) {
		record = tracecmd_read_data(handle, cpu);
		if (pcpu)
			*pcpu = cpu;
	}
	return record;
}

/**
 * tracecmd_read_at - read a record from a specific offset
 * @handle: input handle for the trace.dat file
 * @offset: the offset into the file to find the record
 * @pcpu: pointer to a variable to store the CPU id the record was found in
 *
 * This function is useful when looking for a previous record.
 * You can store the offset of the record "record->offset" and use that
 * offset to retreive the record again without needing to store any
 * other information about the record.
 *
 * The record returned must be freed.
 */
struct tep_record *
tracecmd_read_at(struct tracecmd_input *handle, unsigned long long offset,
		 int *pcpu)
{
	unsigned long long page_offset;
	int cpu;

	page_offset = calc_page_offset(handle, offset);

	/* check to see if we have this page already */
	for (cpu = 0; cpu < handle->cpus; cpu++) {
		if (handle->cpu_data[cpu].offset == page_offset &&
		    handle->cpu_data[cpu].file_size)
			break;
	}

	if (cpu < handle->cpus && handle->cpu_data[cpu].page) {
		if (pcpu)
			*pcpu = cpu;
		return read_event(handle, offset, cpu);
	} else
		return find_and_read_event(handle, offset, pcpu);
}

/**
 * tracecmd_refresh_record - remaps the records data
 * @handle: input handle for the trace.dat file
 * @record: the record to be refreshed
 *
 * A record data points to a mmap section of memory.
 * by reading new records the mmap section may be unmapped.
 * This will refresh the record's data mapping.
 *
 * ===== OBSOLETED BY PAGE REFERENCES =====
 *
 * Returns 1 if page is still mapped (does not modify CPU iterator)
 *         0 on successful mapping (was not mapped before,
 *                      This will update CPU iterator to point to
 *                      the next record)
 *        -1 on error.
 */
int tracecmd_refresh_record(struct tracecmd_input *handle,
			    struct tep_record *record)
{
	unsigned long long page_offset;
	int cpu = record->cpu;
	struct cpu_data *cpu_data = &handle->cpu_data[cpu];
	int index;
	int ret;

	page_offset = calc_page_offset(handle, record->offset);
	index = record->offset & (handle->page_size - 1);

	ret = get_page(handle, record->cpu, page_offset);
	if (ret < 0)
		return -1;

	/* If the page is still mapped, there's nothing to do */
	if (ret)
		return 1;

	record->data = kbuffer_read_at_offset(cpu_data->kbuf, index, &record->ts);
	cpu_data->timestamp = record->ts;

	return 0;
}

/**
 * tracecmd_read_cpu_first - get the first record in a CPU
 * @handle: input handle for the trace.dat file
 * @cpu: the CPU to search
 *
 * This returns the first (by time) record entry in a given CPU.
 *
 * The record returned must be freed.
 */
struct tep_record *
tracecmd_read_cpu_first(struct tracecmd_input *handle, int cpu)
{
	unsigned long long page_offset;
	int ret;

	if (cpu >= handle->cpus)
		return NULL;

	page_offset = calc_page_offset(handle, handle->cpu_data[cpu].file_offset);

	ret = get_page(handle, cpu, page_offset);
	if (ret < 0)
		return NULL;

	/* If the page was already mapped, we need to reset it */
	if (ret)
		update_page_info(handle, cpu);
		
	free_next(handle, cpu);

	return tracecmd_read_data(handle, cpu);
}

/**
 * tracecmd_read_cpu_last - get the last record in a CPU
 * @handle: input handle for the trace.dat file
 * @cpu: the CPU to search
 *
 * This returns the last (by time) record entry in a given CPU.
 *
 * The record returned must be freed.
 */
struct tep_record *
tracecmd_read_cpu_last(struct tracecmd_input *handle, int cpu)
{
	struct tep_record *record = NULL;
	off64_t offset, page_offset;

	offset = handle->cpu_data[cpu].file_offset +
		handle->cpu_data[cpu].file_size;

	if (offset & (handle->page_size - 1))
		offset &= ~(handle->page_size - 1);
	else
		offset -= handle->page_size;

	page_offset = offset;

 again:
	if (get_page(handle, cpu, page_offset) < 0)
		return NULL;

	offset = page_offset;

	do {
		tracecmd_free_record(record);
		record = tracecmd_read_data(handle, cpu);
		if (record)
			offset = record->offset;
	} while (record);

	record = tracecmd_read_at(handle, offset, NULL);

	/*
	 * It is possible that a page has just a timestamp
	 * or just padding on it.
	 */
	if (!record) {
		if (page_offset == handle->cpu_data[cpu].file_offset)
			return NULL;
		page_offset -= handle->page_size;
		goto again;
	}

	return record;
}

/**
 * tracecmd_set_cpu_to_timestamp - set the CPU iterator to a given time
 * @handle: input handle for the trace.dat file
 * @cpu: the CPU pointer to set
 * @ts: the timestamp to set the CPU at.
 *
 * This sets the CPU iterator used by tracecmd_read_data and
 * tracecmd_peek_data to a location in the CPU storage near
 * a given timestamp. It will try to set the iterator to a time before
 * the time stamp and not actually at a given time.
 *
 * To use this to find a record in a time field, call this function
 * first, than iterate with tracecmd_read_data to find the records
 * you need.
 */
int
tracecmd_set_cpu_to_timestamp(struct tracecmd_input *handle, int cpu,
			      unsigned long long ts)
{
	struct cpu_data *cpu_data = &handle->cpu_data[cpu];
	off64_t start, end, next;

	if (cpu < 0 || cpu >= handle->cpus) {
		errno = -EINVAL;
		return -1;
	}

	if (!cpu_data->size)
		return -1;

	if (!cpu_data->page) {
		if (init_cpu(handle, cpu))
		    return -1;
	}

	if (cpu_data->timestamp == ts) {
		/*
		 * If a record is cached, then that record is most
		 * likely the matching timestamp. Otherwise we need
		 * to start from the beginning of the index;
		 */
		if (!cpu_data->next ||
		    cpu_data->next->ts != ts)
			update_page_info(handle, cpu);
		return 0;
	}

	/* Set to the first record on current page */
	update_page_info(handle, cpu);

	if (cpu_data->timestamp < ts) {
		start = cpu_data->offset;
		end = cpu_data->file_offset + cpu_data->file_size;
		if (end & (handle->page_size - 1))
			end &= ~(handle->page_size - 1);
		else
			end -= handle->page_size;
		next = end;
	} else {
		end = cpu_data->offset;
		start = cpu_data->file_offset;
		next = start;
	}

	while (start < end) {
		if (get_page(handle, cpu, next) < 0)
			return -1;

		if (cpu_data->timestamp == ts)
			break;

		if (cpu_data->timestamp < ts)
			start = next;
		else
			end = next;

		next = start + (end - start) / 2;
		next = calc_page_offset(handle, next);

		/* Prevent an infinite loop if start and end are a page off */
		if (next == start)
			start = next += handle->page_size;
	}

	/*
	 * We need to end up on a page before the time stamp.
	 * We go back even if the timestamp is the same. This is because
	 * we want the event with the timestamp, not the page. The page
	 * can start with the timestamp we are looking for, but the event
	 * may be on the previous page.
	 */
	if (cpu_data->timestamp >= ts &&
	    cpu_data->offset > cpu_data->file_offset)
		get_page(handle, cpu, cpu_data->offset - handle->page_size);

	return 0;
}

/**
 * tracecmd_set_all_cpus_to_timestamp - set all CPUs iterator to a given time
 * @handle: input handle for the trace.dat file
 * @cpu: the CPU pointer to set
 * @ts: the timestamp to set the CPU at.
 *
 * This sets the CPU iterator used by tracecmd_read_data and
 * tracecmd_peek_data to a location in the CPU storage near
 * a given timestamp. It will try to set the iterator to a time before
 * the time stamp and not actually at a given time.
 *
 * To use this to find a record in a time field, call this function
 * first, than iterate with tracecmd_read_next_data to find the records
 * you need.
 */
void
tracecmd_set_all_cpus_to_timestamp(struct tracecmd_input *handle,
				   unsigned long long time)
{
	int cpu;

	for (cpu = 0; cpu < handle->cpus; cpu++)
		tracecmd_set_cpu_to_timestamp(handle, cpu, time);
}

/**
 * tracecmd_set_cursor - set the offset for the next tracecmd_read_data
 * @handle: input handle for the trace.dat file
 * @cpu: the CPU pointer to set
 * @offset: the offset to place the cursor
 *
 * Set the pointer to the next read or peek. This is useful when
 * needing to read sequentially and then look at another record
 * out of sequence without breaking the iteration. This is done with:
 *
 *  record = tracecmd_peek_data()
 *  offset = record->offset;
 *  record = tracecmd_read_at();
 *   - do what ever with record -
 *  tracecmd_set_cursor(handle, cpu, offset);
 *
 *  Now the next tracecmd_peek_data or tracecmd_read_data will return
 *  the original record.
 */
int tracecmd_set_cursor(struct tracecmd_input *handle,
			int cpu, unsigned long long offset)
{
	struct cpu_data *cpu_data = &handle->cpu_data[cpu];
	unsigned long long page_offset;

	if (cpu < 0 || cpu >= handle->cpus)
		return -1;

	if (offset < cpu_data->file_offset ||
	    offset > cpu_data->file_offset + cpu_data->file_size)
		return -1; 	/* cpu does not have this offset. */

	/* Move this cpu index to point to this offest */
	page_offset = calc_page_offset(handle, offset);

	if (get_page(handle, cpu, page_offset) < 0)
		return -1;

	peek_event(handle, offset, cpu);

	return 0;
}

/**
 * tracecmd_get_cursor - get the offset for the next tracecmd_read_data
 * @handle: input handle for the trace.dat file
 * @cpu: the CPU pointer to get the cursor from
 *
 * Returns the offset of the next record that would be read.
 */
unsigned long long
tracecmd_get_cursor(struct tracecmd_input *handle, int cpu)
{
	struct cpu_data *cpu_data = &handle->cpu_data[cpu];
	struct kbuffer *kbuf = cpu_data->kbuf;

	if (cpu < 0 || cpu >= handle->cpus)
		return 0;

	/*
	 * Use the next pointer if it exists and matches the
	 * current timestamp.
	 */
	if (cpu_data->next &&
	    cpu_data->next->ts == cpu_data->timestamp)
		return cpu_data->next->offset;

	/*
	 * Either the next point does not exist, or it does
	 * not match the timestamp. The next read will use the
	 * current page.
	 *
	 * If the offset is at the end, then return that.
	 */
	if (cpu_data->offset >= cpu_data->file_offset +
	    cpu_data->file_size)
		return cpu_data->offset;

	return cpu_data->offset + kbuffer_curr_offset(kbuf);
}

/**
 * tracecmd_translate_data - create a record from raw data
 * @handle: input handle for the trace.dat file
 * @ptr: raw data to read
 * @size: the size of the data
 *
 * This function tries to create a record from some given
 * raw data. The data does not need to be from the trace.dat file.
 * It can be stored from another location.
 *
 * Note, since the timestamp is calculated from within the trace
 * buffer, the timestamp for the record will be zero, since it
 * can't calculate it.
 *
 * The record returned must be freed.
 */
struct tep_record *
tracecmd_translate_data(struct tracecmd_input *handle,
			void *ptr, int size)
{
	struct tep_handle *pevent = handle->pevent;
	struct tep_record *record;
	unsigned int length;
	int swap = 1;

	/* minimum record read is 8, (warn?) (TODO: make 8 into macro) */
	if (size < 8)
		return NULL;

	record = malloc(sizeof(*record));
	if (!record)
		return NULL;
	memset(record, 0, sizeof(*record));

	record->ref_count = 1;
	if (tep_is_local_bigendian(pevent) == tep_is_file_bigendian(pevent))
		swap = 0;
	record->data = kbuffer_translate_data(swap, ptr, &length);
	record->size = length;
	if (record->data)
		record->record_size = record->size + (record->data - ptr);

	return record;
}


/**
 * tracecmd_peek_data - return the record at the current location.
 * @handle: input handle for the trace.dat file
 * @cpu: the CPU to pull from
 *
 * This returns the record at the current location of the CPU
 * iterator. It does not increment the CPU iterator.
 */
struct tep_record *
tracecmd_peek_data(struct tracecmd_input *handle, int cpu)
{
	struct tep_record *record;
	unsigned long long ts;
	struct kbuffer *kbuf;
	struct page *page;
	int index;
	void *data;

	if (cpu >= handle->cpus)
		return NULL;

	page = handle->cpu_data[cpu].page;
	kbuf = handle->cpu_data[cpu].kbuf;

	/* Hack to work around function graph read ahead */
	tracecmd_curr_thread_handle = handle;

	if (handle->cpu_data[cpu].next) {

		record = handle->cpu_data[cpu].next;
		if (!record->data) {
			tracecmd_critical("Something freed the record");
			return NULL;
		}

		if (handle->cpu_data[cpu].timestamp == record->ts)
			return record;

		/*
		 * The timestamp changed, which means the cached
		 * record is no longer valid. Reread a new record.
		 */
		free_next(handle, cpu);
	}

read_again:
	if (!page) {
		if (handle->use_pipe) {
			get_next_page(handle, cpu);
			page = handle->cpu_data[cpu].page;
		}
		if (!page)
			return NULL;
	}

	data = kbuffer_read_event(kbuf, &ts);
	if (!data) {
		if (get_next_page(handle, cpu))
			return NULL;
		page = handle->cpu_data[cpu].page;
		goto read_again;
	}

	handle->cpu_data[cpu].timestamp = timestamp_calc(ts, cpu, handle);

	index = kbuffer_curr_offset(kbuf);

	record = malloc(sizeof(*record));
	if (!record)
		return NULL;
	memset(record, 0, sizeof(*record));

	record->ts = handle->cpu_data[cpu].timestamp;
	record->size = kbuffer_event_size(kbuf);
	record->cpu = handle->cpu_data[cpu].cpu;
	record->data = data;
	record->offset = handle->cpu_data[cpu].offset + index;
	record->missed_events = kbuffer_missed_events(kbuf);
	record->ref_count = 1;
	record->locked = 1;

	handle->cpu_data[cpu].next = record;

	record->record_size = kbuffer_curr_size(kbuf);
	record->priv = page;
	add_record(page, record);
	page->ref_count++;

	kbuffer_next_event(kbuf, NULL);

	return record;
}

/**
 * tracecmd_read_data - read the next record and increment
 * @handle: input handle for the trace.dat file
 * @cpu: the CPU to pull from
 *
 * This returns the record at the current location of the CPU
 * iterator and increments the CPU iterator.
 *
 * The record returned must be freed.
 */
struct tep_record *
tracecmd_read_data(struct tracecmd_input *handle, int cpu)
{
	struct tep_record *record;

	if (cpu >= handle->cpus)
		return NULL;

	record = tracecmd_peek_data(handle, cpu);
	handle->cpu_data[cpu].next = NULL;
	if (record) {
		record->locked = 0;
#if DEBUG_RECORD
		record->alloc_addr = (unsigned long)__builtin_return_address(0);
#endif
	}
	return record;
}

/**
 * tracecmd_read_next_data - read the next record
 * @handle: input handle to the trace.dat file
 * @rec_cpu: return pointer to the CPU that the record belongs to
 *
 * This returns the next record by time. This is different than
 * tracecmd_read_data in that it looks at all CPUs. It does a peek
 * at each CPU and the record with the earliest time stame is
 * returned. If @rec_cpu is not NULL it gets the CPU id the record was
 * on. The CPU cursor of the returned record is moved to the
 * next record.
 *
 * Multiple reads of this function will return a serialized list
 * of all records for all CPUs in order of time stamp.
 *
 * The record returned must be freed.
 */
struct tep_record *
tracecmd_read_next_data(struct tracecmd_input *handle, int *rec_cpu)
{
	struct tep_record *record;
	int next_cpu;

	record = tracecmd_peek_next_data(handle, &next_cpu);
	if (!record)
		return NULL;

	if (rec_cpu)
		*rec_cpu = next_cpu;

	return tracecmd_read_data(handle, next_cpu);
}

/**
 * tracecmd_iterate_events - iterate events over a given handle
 * @handle: The handle to iterate over
 * @cpus: The CPU set to filter on (NULL for all CPUs)
 * @cpu_size: The size of @cpus (ignored if @cpus is NULL)
 * @callback: The callback function for each event
 * @callback_data: The data to pass to the @callback.
 *
 * Will loop over all events in @handle (filtered by the given @cpus),
 * and will call @callback for each event in order of the event's records
 * timestamp.
 *
 * Returns the -1 on error, or the value of the callbacks.
 */
int tracecmd_iterate_events(struct tracecmd_input *handle,
			    cpu_set_t *cpus, int cpu_size,
			    int (*callback)(struct tracecmd_input *handle,
					    struct tep_record *,
					    int, void *),
			    void *callback_data)
{
	struct tep_record **records;
	struct tep_record *record;
	unsigned long long last_timestamp = 0;
	int next_cpu;
	int cpu;
	int ret = 0;

	records = calloc(handle->max_cpu, sizeof(*records));
	if (!records)
		return -1;

	for (cpu = 0; cpu < handle->max_cpu; cpu++) {
		if (cpus && !CPU_ISSET_S(cpu, cpu_size, cpus))
			continue;

		records[cpu] = tracecmd_peek_data(handle, cpu);
	}

	do {
		next_cpu = -1;
		for (cpu = 0; cpu < handle->max_cpu; cpu++) {
			record = records[cpu];
			if (!record)
				continue;

			if (next_cpu < 0 || record->ts < last_timestamp) {
				next_cpu = cpu;
				last_timestamp = record->ts;
			}
		}
		if (next_cpu >= 0) {
			/* Need to call read_data to increment to the next record */
			record = tracecmd_read_data(handle, next_cpu);
			records[next_cpu] = tracecmd_peek_data(handle, next_cpu);

			if (!handle->filter ||
			    tracecmd_filter_match(handle->filter, record) == TRACECMD_FILTER_MATCH)
				ret = callback(handle, record, next_cpu, callback_data);

			tracecmd_free_record(record);
		}

	} while (next_cpu >= 0 && ret >= 0);

	for (cpu = 0; cpu < handle->max_cpu; cpu++)
		tracecmd_free_record(records[cpu]);

	free(records);

	return ret;
}

struct record_handle {
	struct tep_record		*record;
	struct tracecmd_input		*handle;
};

/**
 * tracecmd_iterate_events_multi - iterate events over multiple handles
 * @handles: An array of handles to iterate over
 * @nr_handles: The number of handles in the @handles array.
 * @callback: The callback function for each event
 * @callback_data: The data to pass to the @callback.
 *
 * Will loop over all CPUs for each handle in @handles and call the
 * @callback in the order of the timestamp for each event's record
 * for each handle.
 *
 * Returns the -1 on error, or the value of the callbacks.
 */
int tracecmd_iterate_events_multi(struct tracecmd_input **handles,
				  int nr_handles,
				  int (*callback)(struct tracecmd_input *handle,
						  struct tep_record *,
						  int, void *),
				  void *callback_data)
{
	struct tracecmd_input *handle;
	struct record_handle *records;
	struct tep_record *record;
	unsigned long long last_timestamp = 0;
	int next_cpu;
	int cpus = 0;
	int all_cpus = 0;
	int cpu;
	int i;
	int ret = 0;

	for (i = 0; i < nr_handles; i++) {
		handle = handles[i];
		cpus += handle->max_cpu;
	}

	records = calloc(cpus, sizeof(*records));
	if (!records)
		return -1;

	for (i = 0; i < nr_handles; i++) {
		handle = handles[i];
		handle->start_cpu = all_cpus;
		for (cpu = 0; cpu < handle->max_cpu; cpu++) {
			records[all_cpus + cpu].record = tracecmd_peek_data(handle, cpu);
			records[all_cpus + cpu].handle = handle;
		}
		all_cpus += cpu;
	}

	do {
		next_cpu = -1;
		for (cpu = 0; cpu < all_cpus; cpu++) {
			record = records[cpu].record;
			if (!record)
				continue;

			if (next_cpu < 0 || record->ts < last_timestamp) {
				next_cpu = cpu;
				last_timestamp = record->ts;
			}
		}
		if (next_cpu >= 0) {
			record = records[next_cpu].record;
			handle = records[next_cpu].handle;
			cpu = next_cpu - handle->start_cpu;
			/* Need to call read_data to increment to the next record */
			record = tracecmd_read_data(handle, cpu);
			records[next_cpu].record = tracecmd_peek_data(handle, cpu);

			if (!handle->filter ||
			    tracecmd_filter_match(handle->filter, record) == TRACECMD_FILTER_MATCH)
				ret = callback(handle, record, next_cpu, callback_data);
			tracecmd_free_record(record);
		}

	} while (next_cpu >= 0 && ret >= 0);

	/*
	 * The records array contains only records that were taken via
	 * tracecmd_peek_data(), and do not need to be freed.
	 */
	free(records);

	return ret;
}

/**
 * tracecmd_peek_next_data - return the next record
 * @handle: input handle to the trace.dat file
 * @rec_cpu: return pointer to the CPU that the record belongs to
 *
 * This returns the next record by time. This is different than
 * tracecmd_peek_data in that it looks at all CPUs. It does a peek
 * at each CPU and the record with the earliest time stame is
 * returned. If @rec_cpu is not NULL it gets the CPU id the record was
 * on. It does not increment the CPU iterator.
 */
struct tep_record *
tracecmd_peek_next_data(struct tracecmd_input *handle, int *rec_cpu)
{
	unsigned long long ts;
	struct tep_record *record, *next_record = NULL;
	int next_cpu;
	int cpu;

	if (rec_cpu)
		*rec_cpu = -1;

	next_cpu = -1;
	ts = 0;

	for (cpu = 0; cpu < handle->cpus; cpu++) {
		record = tracecmd_peek_data(handle, cpu);
		if (record && (!next_record || record->ts < ts)) {
			ts = record->ts;
			next_cpu = cpu;
			next_record = record;
		}
	}

	if (next_record) {
		if (rec_cpu)
			*rec_cpu = next_cpu;
		return next_record;
	}

	return NULL;
}

/**
 * tracecmd_read_prev - read the record before the given record
 * @handle: input handle to the trace.dat file
 * @record: the record to use to find the previous record.
 *
 * This returns the record before the @record on its CPU. If
 * @record is the first record, NULL is returned. The cursor is set
 * as if the previous record was read by tracecmd_read_data().
 *
 * @record can not be NULL, otherwise NULL is returned; the
 * record ownership goes to this function.
 *
 * Note, this is not that fast of an algorithm, since it needs
 * to build the timestamp for the record.
 *
 * The record returned must be freed with tracecmd_free_record().
 */
struct tep_record *
tracecmd_read_prev(struct tracecmd_input *handle, struct tep_record *record)
{
	unsigned long long offset, page_offset;;
	struct cpu_data *cpu_data;
	int index;
	int cpu;

	if (!record)
		return NULL;

	cpu = record->cpu;
	offset = record->offset;
	cpu_data = &handle->cpu_data[cpu];

	page_offset = calc_page_offset(handle, offset);
	index = offset - page_offset;

	/* Note, the record passed in could have been a peek */
	free_next(handle, cpu);

	/* Reset the cursor */
	/* Should not happen */
	if (get_page(handle, cpu, page_offset) < 0)
		return NULL;

	update_page_info(handle, cpu);

	/* Find the record before this record */
	index = 0;
	for (;;) {
		record = tracecmd_read_data(handle, cpu);
		/* Should not happen! */
		if (!record)
			return NULL;
		if (record->offset == offset)
			break;
		index = record->offset - page_offset;
		tracecmd_free_record(record);
	}
	tracecmd_free_record(record);

	if (index)
		/* we found our record */
		return tracecmd_read_at(handle, page_offset + index, NULL);

	/* reset the index to start at the beginning of the page */
	update_page_info(handle, cpu);

	/* The previous record is on the previous page */
	for (;;) {
		/* check if this is the first page */
		if (page_offset == cpu_data->file_offset)
			return NULL;
		page_offset -= handle->page_size;

		/* Updating page to a new page will reset index to 0 */
		get_page(handle, cpu, page_offset);

		record = NULL;
		index = 0;
		do {
			if (record) {
				index = record->offset - page_offset;
				tracecmd_free_record(record);
			}
			record = tracecmd_read_data(handle, cpu);
			/* Should not happen */
			if (!record)
				return NULL;
		} while (record->offset != offset);
		tracecmd_free_record(record);

		if (index)
			/* we found our record */
			return tracecmd_read_at(handle, page_offset + index, NULL);
	}

	/* Not reached */
}

static int init_cpu_zfile(struct tracecmd_input *handle, int cpu)
{
	struct cpu_data *cpu_data;
	unsigned long long size;
	off64_t offset;

	cpu_data = &handle->cpu_data[cpu];
	offset = lseek64(handle->fd, 0, SEEK_CUR);
	if (lseek64(handle->fd, cpu_data->file_offset, SEEK_SET) == (off_t)-1)
		return -1;

	strcpy(cpu_data->compress.file, COMPR_TEMP_FILE);
	cpu_data->compress.fd = mkstemp(cpu_data->compress.file);
	if (cpu_data->compress.fd < 0)
		return -1;

	if (tracecmd_uncompress_copy_to(handle->compress, cpu_data->compress.fd, NULL, &size))
		return -1;

	if (lseek64(handle->fd, offset, SEEK_SET) == (off_t)-1)
		return -1;

	cpu_data->file_offset = handle->next_offset;
	handle->next_offset = (handle->next_offset + size + handle->page_size - 1) &
		~(handle->page_size - 1);
	cpu_data->offset = cpu_data->file_offset;

	cpu_data->file_size = size;
	cpu_data->size = size;
	return 0;
}

static int init_cpu_zpage(struct tracecmd_input *handle, int cpu)
{
	struct cpu_data *cpu_data = &handle->cpu_data[cpu];
	int count;
	int i;

	if (lseek64(handle->fd, cpu_data->file_offset, SEEK_SET) == (off_t)-1)
		return -1;

	count = tracecmd_load_chunks_info(handle->compress, &cpu_data->compress.chunks);
	if (count < 0)
		return -1;

	cpu_data->compress.count = count;
	cpu_data->compress.last_chunk = 0;

	cpu_data->file_offset = handle->next_offset;
	cpu_data->file_size = 0;

	for (i = 0; i < count; i++)
		cpu_data->file_size += cpu_data->compress.chunks[i].size;

	cpu_data->offset = cpu_data->file_offset;
	cpu_data->size = cpu_data->file_size;
	handle->next_offset = (handle->next_offset + cpu_data->file_size + handle->page_size - 1) &
		~(handle->page_size - 1);
	return 0;
}

static int init_cpu(struct tracecmd_input *handle, int cpu)
{
	struct cpu_data *cpu_data = &handle->cpu_data[cpu];
	int ret;
	int i;

	if (handle->cpu_compressed && cpu_data->file_size > 0) {
		if (handle->read_zpage)
			ret = init_cpu_zpage(handle, cpu);
		else
			ret = init_cpu_zfile(handle, cpu);
		if (ret)
			return ret;
	} else {
		cpu_data->offset = cpu_data->file_offset;
		cpu_data->size = cpu_data->file_size;
	}
	cpu_data->timestamp = 0;

	list_head_init(&cpu_data->page_maps);
	list_head_init(&cpu_data->compress.cache);

	if (!cpu_data->size) {
		tracecmd_info("CPU %d is empty", cpu);
		return 0;
	}

	cpu_data->nr_pages = (cpu_data->size + handle->page_size - 1) / handle->page_size;
	if (!cpu_data->nr_pages)
		cpu_data->nr_pages = 1;
	cpu_data->pages = calloc(cpu_data->nr_pages, sizeof(*cpu_data->pages));
	if (!cpu_data->pages)
		return -1;

	if (handle->use_pipe) {
		/* Just make a page, it will be nuked later */
		cpu_data->page = malloc(sizeof(*cpu_data->page));
		if (!cpu_data->page)
			goto fail;

		memset(cpu_data->page, 0, sizeof(*cpu_data->page));
		cpu_data->pages[0] = cpu_data->page;
		cpu_data->page_cnt = 1;
		cpu_data->page->ref_count = 1;
		return 0;
	}

	cpu_data->page = allocate_page(handle, cpu, cpu_data->offset);
	if (!cpu_data->page && !handle->read_page) {
		perror("mmap");
		fprintf(stderr, "Can not mmap file, will read instead\n");

		if (cpu) {
			/*
			 * If the other CPUs had size and was able to mmap
			 * then bail.
			 */
			for (i = 0; i < cpu; i++) {
				if (handle->cpu_data[i].size)
					goto fail;
			}
		}

		/* try again without mmapping, just read it directly */
		handle->read_page = true;
		cpu_data->page = allocate_page(handle, cpu, cpu_data->offset);
		if (!cpu_data->page)
			/* Still no luck, bail! */
			goto fail;
	}

	if (update_page_info(handle, cpu))
		goto fail;
	cpu_data->first_ts = cpu_data->timestamp;

	return 0;
 fail:
	free(cpu_data->pages);
	cpu_data->pages = NULL;
	free(cpu_data->page);
	cpu_data->page = NULL;
	return -1;
}

void tracecmd_set_ts_offset(struct tracecmd_input *handle,
			    long long offset)
{
	handle->ts_offset = offset;
}

/**
 * tracecmd_add_ts_offset - Add value to the offset which will be applied to the timestamps of all
 *			    events from given trace file
 * @handle: input handle to the trace.dat file
 * @offset: value, that will be added to the offset
 */
void tracecmd_add_ts_offset(struct tracecmd_input *handle,
			    long long offset)
{
	handle->ts_offset += offset;
}

void tracecmd_set_ts2secs(struct tracecmd_input *handle,
			 unsigned long long hz)
{
	double ts2secs;

	ts2secs = (double)NSEC_PER_SEC / (double)hz;
	handle->ts2secs = ts2secs;
	handle->use_trace_clock = false;
}

static int tsync_offset_cmp(const void *a, const void *b)
{
	struct ts_offset_sample *ts_a = (struct ts_offset_sample *)a;
	struct ts_offset_sample *ts_b = (struct ts_offset_sample *)b;

	if (ts_a->time > ts_b->time)
		return 1;
	if (ts_a->time < ts_b->time)
		return -1;
	return 0;
}

#define safe_read(R, C)					\
	do {						\
		if ((C) > size)				\
			return -EFAULT;			\
		(R) = tep_read_number(tep, buf, (C));	\
		buf += (C);				\
		size -= (C);				\
	} while (0)

#define safe_read_loop(type)						\
	do {								\
		int ii;							\
		for (ii = 0; ii < ts_offsets->ts_samples_count; ii++)	\
			safe_read(ts_offsets->ts_samples[ii].type, 8);	\
	} while (0)

static int tsync_cpu_offsets_load(struct tracecmd_input *handle, char *buf, int size)
{
	struct tep_handle *tep = handle->pevent;
	struct timesync_offsets *ts_offsets;
	int i, j, k;

	safe_read(handle->host.cpu_count, 4);
	handle->host.ts_offsets = calloc(handle->host.cpu_count,
					 sizeof(struct timesync_offsets));
	if (!handle->host.ts_offsets)
		return -ENOMEM;
	for (i = 0; i < handle->host.cpu_count; i++) {
		ts_offsets = &handle->host.ts_offsets[i];
		safe_read(ts_offsets->ts_samples_count, 4);
		ts_offsets->ts_samples = calloc(ts_offsets->ts_samples_count,
						sizeof(struct ts_offset_sample));
		if (!ts_offsets->ts_samples)
			return -ENOMEM;
		safe_read_loop(time);
		safe_read_loop(offset);
		safe_read_loop(scaling);
	}

	if (size > 0) {
		for (i = 0; i < handle->host.cpu_count; i++) {
			ts_offsets = &handle->host.ts_offsets[i];
			safe_read_loop(fraction);
		}
	}

	for (i = 0; i < handle->host.cpu_count; i++) {
		ts_offsets = &handle->host.ts_offsets[i];
		qsort(ts_offsets->ts_samples, ts_offsets->ts_samples_count,
		      sizeof(struct ts_offset_sample), tsync_offset_cmp);
		/* Filter possible samples with equal time */
		for (k = 0, j = 0; k < ts_offsets->ts_samples_count; k++) {
			if (k == 0 || ts_offsets->ts_samples[k].time != ts_offsets->ts_samples[k-1].time)
				ts_offsets->ts_samples[j++] = ts_offsets->ts_samples[k];
		}
		ts_offsets->ts_samples_count = j;
	}

	return 0;
}

static void trace_tsync_offset_free(struct host_trace_info *host)
{
	int i;

	if (host->ts_offsets) {
		for (i = 0; i < host->cpu_count; i++)
			free(host->ts_offsets[i].ts_samples);
		free(host->ts_offsets);
		host->ts_offsets = NULL;
	}
}

static int trace_pid_map_cmp(const void *a, const void *b)
{
	struct tracecmd_proc_addr_map *m_a = (struct tracecmd_proc_addr_map *)a;
	struct tracecmd_proc_addr_map *m_b = (struct tracecmd_proc_addr_map *)b;

	if (m_a->start > m_b->start)
	if (m_a->start < m_b->start)
		return -1;
	return 0;
}

static void procmap_free(struct pid_addr_maps *maps)
{
	int i;

	if (!maps)
		return;
	if (maps->lib_maps) {
		for (i = 0; i < maps->nr_lib_maps; i++)
			free(maps->lib_maps[i].lib_name);
		free(maps->lib_maps);
	}
	free(maps->proc_name);
	free(maps);
}

static void trace_guests_free(struct tracecmd_input *handle)
{
	struct guest_trace_info *guest;

	while (handle->guest) {
		guest = handle->guest;
		handle->guest = handle->guest->next;
		free(guest->name);
		free(guest->cpu_pid);
		free(guest);
	}
}

static int trace_guest_load(struct tracecmd_input *handle, char *buf, int size)
{
	struct guest_trace_info *guest = NULL;
	int cpu;
	int i;

	guest = calloc(1, sizeof(struct guest_trace_info));
	if (!guest)
		goto error;

	/*
	 * Guest name, null terminated string
	 * long long (8 bytes) trace-id
	 * int (4 bytes) number of guest CPUs
	 * array of size number of guest CPUs:
	 *	int (4 bytes) Guest CPU id
	 *	int (4 bytes) Host PID, running the guest CPU
	 */

	guest->name = strndup(buf, size);
	if (!guest->name)
		goto error;
	buf += strlen(guest->name) + 1;
	size -= strlen(guest->name) + 1;

	if (size < sizeof(long long))
		goto error;
	guest->trace_id = tep_read_number(handle->pevent, buf, sizeof(long long));
	buf += sizeof(long long);
	size -= sizeof(long long);

	if (size < sizeof(int))
		goto error;
	guest->vcpu_count = tep_read_number(handle->pevent, buf, sizeof(int));
	buf += sizeof(int);
	size -= sizeof(int);

	guest->cpu_pid = calloc(guest->vcpu_count, sizeof(int));
	if (!guest->cpu_pid)
		goto error;

	for (i = 0; i < guest->vcpu_count; i++) {
		if (size < 2 * sizeof(int))
			goto error;
		cpu = tep_read_number(handle->pevent, buf, sizeof(int));
		buf += sizeof(int);
		if (cpu >= guest->vcpu_count)
			goto error;
		guest->cpu_pid[cpu] = tep_read_number(handle->pevent,
						      buf, sizeof(int));
		buf += sizeof(int);
		size -= 2 * sizeof(int);
	}

	guest->next = handle->guest;
	handle->guest = guest;
	return 0;

error:
	if (guest) {
		free(guest->cpu_pid);
		free(guest->name);
		free(guest);
	}
	return -1;
}

/* Needs to be a constant, and 4K should be good enough */
#define STR_PROCMAP_LINE_MAX	4096
static int trace_pid_map_load(struct tracecmd_input *handle, char *buf)
{
	struct pid_addr_maps *maps = NULL;
	char mapname[STR_PROCMAP_LINE_MAX+1];
	char *line;
	int res;
	int ret;
	int i;

	maps = calloc(1, sizeof(*maps));
	if (!maps)
		return -ENOMEM;

	ret  = -EINVAL;
	line = strchr(buf, '\n');
	if (!line)
		goto out_fail;

	*line = '\0';
	if (strlen(buf) > STR_PROCMAP_LINE_MAX)
		goto out_fail;

	res = sscanf(buf, "%x %x %"STRINGIFY(STR_PROCMAP_LINE_MAX)"s", &maps->pid, &maps->nr_lib_maps, mapname);
	if (res != 3)
		goto out_fail;

	ret  = -ENOMEM;
	maps->proc_name = strdup(mapname);
	if (!maps->proc_name)
		goto out_fail;

	maps->lib_maps = calloc(maps->nr_lib_maps, sizeof(struct tracecmd_proc_addr_map));
	if (!maps->lib_maps)
		goto out_fail;

	buf = line + 1;
	line = strchr(buf, '\n');
	for (i = 0; i < maps->nr_lib_maps; i++) {
		if (!line)
			break;
		*line = '\0';
		if (strlen(buf) > STR_PROCMAP_LINE_MAX)
			break;
		res = sscanf(buf, "%llx %llx %s", &maps->lib_maps[i].start,
			     &maps->lib_maps[i].end, mapname);
		if (res != 3)
			break;
		maps->lib_maps[i].lib_name = strdup(mapname);
		if (!maps->lib_maps[i].lib_name)
			goto out_fail;
		buf = line + 1;
		line = strchr(buf, '\n');
	}

	ret  = -EINVAL;
	if (i != maps->nr_lib_maps)
		goto out_fail;

	qsort(maps->lib_maps, maps->nr_lib_maps,
	      sizeof(*maps->lib_maps), trace_pid_map_cmp);

	maps->next = handle->pid_maps;
	handle->pid_maps = maps;

	return 0;

out_fail:
	procmap_free(maps);
	return ret;
}

static void trace_pid_map_free(struct pid_addr_maps *maps)
{
	struct pid_addr_maps *del;

	while (maps) {
		del = maps;
		maps = maps->next;
		procmap_free(del);
	}
}

static int trace_pid_map_search(const void *a, const void *b)
{
	struct tracecmd_proc_addr_map *key = (struct tracecmd_proc_addr_map *)a;
	struct tracecmd_proc_addr_map *map = (struct tracecmd_proc_addr_map *)b;

	if (key->start >= map->end)
		return 1;
	if (key->start < map->start)
		return -1;
	return 0;
}

/**
 * tracecmd_search_task_map - Search task memory address map
 * @handle: input handle to the trace.dat file
 * @pid: pid of the task
 * @addr: address from the task memory space.
 *
 * Map of the task memory can be saved in the trace.dat file, using the option
 * "--proc-map". If there is such information, this API can be used to look up
 * into this memory map to find what library is loaded at the given @addr.
 *
 * A pointer to struct tracecmd_proc_addr_map is returned, containing the name
 * of the library at given task @addr and the library start and end addresses.
 */
struct tracecmd_proc_addr_map *
tracecmd_search_task_map(struct tracecmd_input *handle,
			 int pid, unsigned long long addr)
{
	struct tracecmd_proc_addr_map *lib;
	struct tracecmd_proc_addr_map key;
	struct pid_addr_maps *maps;

	if (!handle || !handle->pid_maps)
		return NULL;

	maps = handle->pid_maps;
	while (maps) {
		if (maps->pid == pid)
			break;
		maps = maps->next;
	}
	if (!maps || !maps->nr_lib_maps || !maps->lib_maps)
		return NULL;
	key.start = addr;
	lib = bsearch(&key, maps->lib_maps, maps->nr_lib_maps,
		      sizeof(*maps->lib_maps), trace_pid_map_search);

	return lib;
}

__hidden unsigned int get_meta_strings_size(struct tracecmd_input *handle)
{
	return handle->strings_size;
}

__hidden unsigned long long get_last_option_offset(struct tracecmd_input *handle)
{
	return handle->options_last_offset;
}

static int handle_option_done(struct tracecmd_input *handle, char *buf, int size)
{
	unsigned long long offset;

	if (size < 8)
		return -1;

	offset = lseek64(handle->fd, 0, SEEK_CUR);
	if (offset >= size)
		handle->options_last_offset = offset - size;

	offset = tep_read_number(handle->pevent, buf, 8);
	if (!offset)
		return 0;

	if (lseek64(handle->fd, offset, SEEK_SET) == (off_t)-1)
		return -1;

	return handle_options(handle);
}

static inline int save_read_number(struct tep_handle *tep, char *data, int *data_size,
				   int *read_pos, int bytes, unsigned long long *num)
{
	if (bytes > *data_size)
		return -1;

	*num = tep_read_number(tep, (data + *read_pos), bytes);
	*read_pos += bytes;
	*data_size -= bytes;
	return 0;
}

static inline char *save_read_string(char *data, int *data_size, int *read_pos)
{
	char *str;

	if (*data_size < 1)
		return NULL;

	str = strdup(data + *read_pos);
	if (!str)
		return NULL;
	*data_size -= (strlen(str) + 1);
	if (*data_size < 0) {
		free(str);
		return NULL;
	}
	*read_pos += (strlen(str) + 1);

	return str;
}

static int handle_buffer_option(struct tracecmd_input *handle,
				unsigned short id, char *data, int size)
{
	struct input_buffer_instance *buff;
	struct cpu_file_data *cpu_data;
	unsigned long long tmp;
	long long max_cpu = -1;
	int rsize = 0;
	char *name;
	int i;

	if (save_read_number(handle->pevent, data, &size, &rsize, 8, &tmp))
		return -1;

	name = save_read_string(data, &size, &rsize);
	if (!name)
		return -1;

	if (*name == '\0') {
		/* top buffer */
		buff = &handle->top_buffer;
	} else {
		buff = realloc(handle->buffers, sizeof(*handle->buffers) * (handle->nr_buffers + 1));
		if (!buff) {
			free(name);
			return -1;
		}
		handle->buffers = buff;
		handle->nr_buffers++;

		buff = &handle->buffers[handle->nr_buffers - 1];
	}
	memset(buff, 0, sizeof(struct input_buffer_instance));
	buff->name = name;
	buff->offset = tmp;

	if (!HAS_SECTIONS(handle))
		return 0;

	/* file sections specific data */
	buff->clock = save_read_string(data, &size, &rsize);
	if (!buff->clock)
		return -1;

	if (*name == '\0' && !handle->trace_clock)
		handle->trace_clock = strdup(buff->clock);

	if (id == TRACECMD_OPTION_BUFFER) {
		if (save_read_number(handle->pevent, data, &size, &rsize, 4, &tmp))
			return -1;
		buff->page_size = tmp;

		if (save_read_number(handle->pevent, data, &size, &rsize, 4, &tmp))
			return -1;
		buff->cpus = tmp;
		if (!buff->cpus)
			return 0;
		cpu_data = calloc(buff->cpus, sizeof(*cpu_data));
		if (!cpu_data)
			return -1;
		for (i = 0; i < buff->cpus; i++) {
			if (save_read_number(handle->pevent, data, &size, &rsize, 4, &tmp))
				goto fail;
			if ((long long)tmp > max_cpu)
				max_cpu = tmp;
			cpu_data[i].cpu = tmp;
			if (save_read_number(handle->pevent, data,
					     &size, &rsize, 8, &cpu_data[i].offset))
				goto fail;
			if (save_read_number(handle->pevent, data,
					     &size, &rsize, 8, &cpu_data[i].size))
				goto fail;
		}
		if (buff->cpus == max_cpu + 1) {
			/* Check to make sure cpus match the index */
			for (i = 0; i < buff->cpus; i++) {
				if (cpu_data[i].cpu != i)
					goto copy_buffer;
			}
			buff->cpu_data = cpu_data;
		} else {
 copy_buffer:
			buff->cpu_data = calloc(max_cpu + 1, sizeof(*cpu_data));
			if (!buff->cpu_data)
				goto fail;
			for (i = 0; i < buff->cpus; i++) {
				if (buff->cpu_data[cpu_data[i].cpu].size) {
					tracecmd_warning("More than one buffer defined for CPU %d (buffer %d)\n",
							 cpu_data[i].cpu, i);
					goto fail;
				}
				buff->cpu_data[cpu_data[i].cpu] = cpu_data[i];
			}
			buff->cpus = max_cpu + 1;
			free(cpu_data);
		}
	} else {
		buff->latency = true;
	}
	return 0;
fail:
	free(cpu_data);
	return -1;
}

static int handle_options(struct tracecmd_input *handle)
{
	long long offset;
	unsigned short option;
	unsigned int size;
	unsigned short id, flags;
	char *cpustats = NULL;
	struct hook_list *hook;
	bool compress = false;
	char *buf;
	int cpus;
	int ret;

	if (!HAS_SECTIONS(handle)) {
		handle->options_start = lseek64(handle->fd, 0, SEEK_CUR);
	} else {
		if (read_section_header(handle, &id, &flags, NULL, NULL))
			return -1;
		if (id != TRACECMD_OPTION_DONE)
			return -1;
		if (flags & TRACECMD_SEC_FL_COMPRESS)
			compress = true;
	}

	if (compress && in_uncompress_block(handle))
		return -1;

	for (;;) {
		ret = read2(handle, &option);
		if (ret)
			goto out;

		if (!HAS_SECTIONS(handle) && option == TRACECMD_OPTION_DONE)
			break;

		/* next 4 bytes is the size of the option */
		ret = read4(handle, &size);
		if (ret)
			goto out;
		buf = malloc(size);
		if (!buf) {
			ret = -ENOMEM;
			goto out;
		}
		ret = do_read_check(handle, buf, size);
		if (ret)
			goto out;

		switch (option) {
		case TRACECMD_OPTION_DATE:
			/*
			 * A time has been mapped that is the
			 * difference between the timestamps and
			 * gtod. It is stored as ASCII with '0x'
			 * appended.
			 */
			if (handle->flags &
			    (TRACECMD_FL_IGNORE_DATE | TRACECMD_FL_RAW_TS))
				break;
			offset = strtoll(buf, NULL, 0);
			/* Convert from micro to nano */
			offset *= 1000;
			handle->ts_offset += offset;
			break;
		case TRACECMD_OPTION_OFFSET:
			/*
			 * Similar to date option, but just adds an
			 * offset to the timestamp.
			 */
			if (handle->flags & TRACECMD_FL_RAW_TS)
				break;
			offset = strtoll(buf, NULL, 0);
			handle->ts_offset += offset;
			break;
		case TRACECMD_OPTION_TIME_SHIFT:
			/*
			 * long long int (8 bytes) trace session ID
			 * int (4 bytes) protocol flags.
			 * int (4 bytes) CPU count.
			 * array of size [CPU count]:
			 * [
			 *  int (4 bytes) count of timestamp offsets.
			 *  long long array of size [count] of times,
			 *      when the offsets were calculated.
			 *  long long array of size [count] of timestamp offsets.
			 *  long long array of size [count] of timestamp scaling ratios.*
			 * ]
			 * array of size [CPU count]:
			 * [
			 *  long long array of size [count] of timestamp scaling fraction bits.*
			 * ]*
			 */
			if (size < 16 || (handle->flags & TRACECMD_FL_RAW_TS))
				break;
			handle->host.peer_trace_id = tep_read_number(handle->pevent,
								     buf, 8);
			handle->host.flags = tep_read_number(handle->pevent,
							     buf + 8, 4);
			ret = tsync_cpu_offsets_load(handle, buf + 12, size - 12);
			if (ret < 0)
				goto out;
			tracecmd_enable_tsync(handle, true);
			break;
		case TRACECMD_OPTION_CPUSTAT:
			buf[size-1] = '\n';
			cpustats = realloc(handle->cpustats,
					   handle->cpustats_size + size + 1);
			if (!cpustats) {
				ret = -ENOMEM;
				goto out;
			}
			memcpy(cpustats + handle->cpustats_size, buf, size);
			handle->cpustats_size += size;
			cpustats[handle->cpustats_size] = 0;
			handle->cpustats = cpustats;
			break;
		case TRACECMD_OPTION_BUFFER:
		case TRACECMD_OPTION_BUFFER_TEXT:
			ret = handle_buffer_option(handle, option, buf, size);
			if (ret < 0)
				goto out;
			break;
		case TRACECMD_OPTION_TRACECLOCK:
			tracecmd_parse_trace_clock(handle, buf, size);
			if (!handle->ts2secs)
				handle->use_trace_clock = true;
			break;
		case TRACECMD_OPTION_UNAME:
			handle->uname = strdup(buf);
			break;
		case TRACECMD_OPTION_VERSION:
			handle->version = strdup(buf);
			break;
		case TRACECMD_OPTION_HOOK:
			hook = tracecmd_create_event_hook(buf);
			hook->next = handle->hooks;
			handle->hooks = hook;
			break;
		case TRACECMD_OPTION_CPUCOUNT:
			if ( size > sizeof( unsigned long long ) ) {
				//$$$ tracecmd_parse_tgids(handle->pevent, buf, size);
			} else {
				cpus = *(int *)buf;
				handle->cpus = tep_read_number(handle->pevent, &cpus, 4);
				if (handle->cpus > handle->max_cpu)
					handle->max_cpu = handle->cpus;
				tep_set_cpus(handle->pevent, handle->cpus);
			}
			break;
		//$$$ mikesart case TRACECMD_OPTION_SAVED_TGIDS:
		//$$$ mikesart 	tracecmd_parse_tgids(handle->pevent, buf, size);
		//$$$ mikesart 	break;
		case TRACECMD_OPTION_PROCMAPS:
			if (buf[size-1] == '\0')
				trace_pid_map_load(handle, buf);
			break;
		case TRACECMD_OPTION_TRACEID:
			if (size < 8)
				break;
			handle->trace_id = tep_read_number(handle->pevent,
							   buf, 8);
			break;
		case TRACECMD_OPTION_GUEST:
			trace_guest_load(handle, buf, size);
			break;
		case TRACECMD_OPTION_TSC2NSEC:
			if (size < 16 || (handle->flags & TRACECMD_FL_RAW_TS))
				break;
			handle->tsc_calc.mult = tep_read_number(handle->pevent,
								buf, 4);
			handle->tsc_calc.shift = tep_read_number(handle->pevent,
								 buf + 4, 4);
			handle->tsc_calc.offset = tep_read_number(handle->pevent,
								  buf + 8, 8);
			if (!(handle->flags & TRACECMD_FL_RAW_TS))
				handle->flags |= TRACECMD_FL_IN_USECS;
			break;
		case TRACECMD_OPTION_HEADER_INFO:
		case TRACECMD_OPTION_FTRACE_EVENTS:
		case TRACECMD_OPTION_EVENT_FORMATS:
		case TRACECMD_OPTION_KALLSYMS:
		case TRACECMD_OPTION_PRINTK:
		case TRACECMD_OPTION_CMDLINES:
			if (size < 8)
				break;
			section_add_or_update(handle, option, -1,
					      tep_read_number(handle->pevent, buf, 8), 0);
			break;
		case TRACECMD_OPTION_DONE:
			if (compress)
				in_uncompress_reset(handle);
			ret = handle_option_done(handle, buf, size);
			free(buf);
			return ret;

		default:
			tracecmd_warning("unknown option %d", option);
			break;
		}

		free(buf);

	}

	ret = 0;

out:
	if (compress)
		in_uncompress_reset(handle);
	return ret;
}

static int read_options_type(struct tracecmd_input *handle)
{
	char buf[10];

	if (CHECK_READ_STATE(handle, TRACECMD_FILE_CPU_LATENCY))
		return 0;

	if (do_read_check(handle, buf, 10))
		return -1;

	/* check if this handles options */
	if (strncmp(buf, "options", 7) == 0) {
		if (handle_options(handle) < 0)
			return -1;
		handle->file_state = TRACECMD_FILE_OPTIONS;
		if (do_read_check(handle, buf, 10))
			return -1;
	}

	/*
	 * Check if this is a latency report or flyrecord.
	 */
	if (strncmp(buf, "latency", 7) == 0)
		handle->file_state = TRACECMD_FILE_CPU_LATENCY;
	else if (strncmp(buf, "flyrecord", 9) == 0)
		handle->file_state = TRACECMD_FILE_CPU_FLYRECORD;
	else
		return -1;

	return 0;
}

int tracecmd_latency_data_read(struct tracecmd_input *handle, char **buf, size_t *size)
{
	struct cpu_zdata *zdata = &handle->latz;
	void *data;
	int rsize;
	int fd = -1;
	int id;

	if (!handle || !buf || !size)
		return -1;
	if (handle->file_state != TRACECMD_FILE_CPU_LATENCY)
		return -1;

	if (!handle->cpu_compressed) {
		fd = handle->fd;
	} else if (!handle->read_zpage) {
		if (zdata->fd < 0)
			return -1;
		fd = zdata->fd;
	}

	/* Read data from a file */
	if (fd >= 0) {
		if (!(*buf)) {
			*size = BUFSIZ;
			*buf = malloc(*size);
			if (!(*buf))
				return -1;
		}
		return do_read_fd(fd, *buf, *size);
	}

	/* Uncompress data in memory */
	if (zdata->last_chunk >= zdata->count)
		return 0;

	id = zdata->last_chunk;
	if (!*buf || *size < zdata->chunks[id].size) {
		data = realloc(*buf, zdata->chunks[id].size);
		if (!data)
			return -1;
		*buf = data;
		*size = zdata->chunks[id].size;
	}

	if (tracecmd_uncompress_chunk(handle->compress, &zdata->chunks[id], *buf))
		return -1;

	rsize = zdata->chunks[id].size;
	zdata->last_chunk++;
	return rsize;
}

static int init_cpu_data(struct tracecmd_input *handle)
{
	enum kbuffer_long_size long_size;
	enum kbuffer_endian endian;
	unsigned long long max_size = 0;
	unsigned long long pages;
	int cpu;

	/* We expect this to be flyrecord */
	if (handle->file_state != TRACECMD_FILE_CPU_FLYRECORD)
		return -1;

	if (force_read)
		handle->read_page = true;

	if (handle->long_size == 8)
		long_size = KBUFFER_LSIZE_8;
	else
		long_size = KBUFFER_LSIZE_4;

	if (tep_is_file_bigendian(handle->pevent))
		endian = KBUFFER_ENDIAN_BIG;
	else
		endian = KBUFFER_ENDIAN_LITTLE;

	for (cpu = 0; cpu < handle->cpus; cpu++) {
		handle->cpu_data[cpu].compress.fd = -1;
		handle->cpu_data[cpu].kbuf = kbuffer_alloc(long_size, endian);
		if (!handle->cpu_data[cpu].kbuf)
			goto out_free;
		if (tep_is_old_format(handle->pevent))
			kbuffer_set_old_format(handle->cpu_data[cpu].kbuf);

		if (handle->cpu_data[cpu].file_size > max_size)
			max_size = handle->cpu_data[cpu].file_size;
	}

	/* Calculate about a meg of pages for buffering */
	pages = handle->page_size ? max_size / handle->page_size : 0;
	if (!pages)
		pages = 1;
	pages = normalize_size(pages);
	handle->page_map_size = handle->page_size * pages;
	if (handle->page_map_size < handle->page_size)
		handle->page_map_size = handle->page_size;


	for (cpu = 0; cpu < handle->cpus; cpu++) {
		if (init_cpu(handle, cpu))
			goto out_free;
	}

	return 0;

 out_free:
	for ( ; cpu >= 0; cpu--) {
		free_page(handle, cpu);
		kbuffer_free(handle->cpu_data[cpu].kbuf);
		handle->cpu_data[cpu].kbuf = NULL;
	}
	return -1;
}

int init_latency_data(struct tracecmd_input *handle)
{
	unsigned long long wsize;
	int ret;

	if (!handle->cpu_compressed)
		return 0;

	if (handle->read_zpage) {
		handle->latz.count = tracecmd_load_chunks_info(handle->compress, &handle->latz.chunks);
		if (handle->latz.count < 0)
			return -1;
	} else {
		strcpy(handle->latz.file, COMPR_TEMP_FILE);
		handle->latz.fd = mkstemp(handle->latz.file);
		if (handle->latz.fd < 0)
			return -1;

		ret = tracecmd_uncompress_copy_to(handle->compress, handle->latz.fd, NULL, &wsize);
		if (ret)
			return -1;

		lseek64(handle->latz.fd, 0, SEEK_SET);
	}

	return 0;
}

static int init_buffer_cpu_data(struct tracecmd_input *handle, struct input_buffer_instance *buffer)
{
	unsigned long long offset;
	unsigned long long size;
	unsigned short id, flags;
	int cpu;

	if (handle->cpu_data)
		return -1;

	if (lseek64(handle->fd, buffer->offset, SEEK_SET) == (off_t)-1)
		return -1;
	if (read_section_header(handle, &id, &flags, NULL, NULL))
		return -1;
	if (flags & TRACECMD_SEC_FL_COMPRESS)
		handle->cpu_compressed = true;
	if (buffer->latency) {
		handle->file_state = TRACECMD_FILE_CPU_LATENCY;
		return init_latency_data(handle) == 0 ? 1 : -1;
	}
	handle->file_state = TRACECMD_FILE_CPU_FLYRECORD;
	handle->cpus = buffer->cpus;
	if (handle->max_cpu < handle->cpus)
		handle->max_cpu = handle->cpus;

	handle->cpu_data = calloc(handle->cpus, sizeof(*handle->cpu_data));
	if (!handle->cpu_data)
		return -1;

	for (cpu = 0; cpu < handle->cpus; cpu++) {
		handle->cpu_data[cpu].cpu = buffer->cpu_data[cpu].cpu;
		offset = buffer->cpu_data[cpu].offset;
		size = buffer->cpu_data[cpu].size;
		handle->cpu_data[cpu].file_offset = offset;
		handle->cpu_data[cpu].file_size = size;
		handle->cpu_data[cpu].compress.fd = -1; /* gpuvis change! */
		if (size && (offset + size > handle->total_file_size)) {
			/* this happens if the file got truncated */
			printf("File possibly truncated. "
				"Need at least %llu, but file size is %zu.\n",
				offset + size, handle->total_file_size);
			errno = EINVAL;
			return -1;
		}
	}

	return init_cpu_data(handle);
}

static int read_cpu_data(struct tracecmd_input *handle)
{
	unsigned long long size;
	int cpus;
	int cpu;

	/*
	 * Check if this is a latency report or not.
	 */
	if (handle->file_state == TRACECMD_FILE_CPU_LATENCY)
		return 1;

	/* We expect this to be flyrecord */
	if (handle->file_state != TRACECMD_FILE_CPU_FLYRECORD)
		return -1;

	cpus = handle->cpus;

	handle->cpu_data = malloc(sizeof(*handle->cpu_data) * handle->cpus);
	if (!handle->cpu_data)
		return -1;
	memset(handle->cpu_data, 0, sizeof(*handle->cpu_data) * handle->cpus);

	for (cpu = 0; cpu < handle->cpus; cpu++) {
		unsigned long long offset;

		handle->cpu_data[cpu].cpu = cpu;
		handle->cpu_data[cpu].compress.fd = -1; /* gpuvis change! */
		read8(handle, &offset);
		read8(handle, &size);
		handle->cpu_data[cpu].file_offset = offset;
		handle->cpu_data[cpu].file_size = size;
		if (size && (offset + size > handle->total_file_size)) {
			/* this happens if the file got truncated */
			printf("File possibly truncated. "
				"Need at least %llu, but file size is %zu.\n",
				offset + size, handle->total_file_size);
			errno = EINVAL;
			return -1;
		}
	}

	/*
	 * It is possible that an option changed the number of CPUs.
	 * If that happened, then there's "empty" cpu data saved for
	 * backward compatibility.
	 */
	if (cpus < handle->cpus) {
		unsigned long long ignore;
		int once = 0;

		read8(handle, &ignore); /* offset */
		read8(handle, &ignore); /* size */
		if (ignore != 0) {
			if (!once) {
				tracecmd_warning("ignored CPU data not zero size");
				once++;
			}
		}
	}

	return init_cpu_data(handle);
}

static int read_data_and_size(struct tracecmd_input *handle,
				     char **data, unsigned long long *size)
{
	if (read8(handle, size) < 0)
		return -1;
	*data = malloc(*size + 1);
	if (!*data)
		return -1;
	if (do_read_check(handle, *data, *size)) {
		free(*data);
		return -1;
	}

	return 0;
}

static int read_and_parse_cmdlines(struct tracecmd_input *handle)
{
	struct tep_handle *pevent = handle->pevent;
	unsigned long long size;
	char *cmdlines;

	if (CHECK_READ_STATE(handle, TRACECMD_FILE_CMD_LINES))
		return 0;

	if (!HAS_SECTIONS(handle))
		section_add_or_update(handle, TRACECMD_OPTION_CMDLINES, 0, 0,
				      lseek64(handle->fd, 0, SEEK_CUR));


	if (read_data_and_size(handle, &cmdlines, &size) < 0)
		return -1;
	cmdlines[size] = 0;
	tep_parse_saved_cmdlines(pevent, cmdlines);
	free(cmdlines);

	handle->file_state = TRACECMD_FILE_CMD_LINES;

	return 0;
}

static void extract_trace_clock(struct tracecmd_input *handle, char *line)
{
	char *clock = NULL;
	char *next = NULL;
	char *data;

	data = strtok_r(line, "[]", &next);
	sscanf(data, "%ms", &clock);
	/* TODO: report if it fails to allocate */
	handle->trace_clock = clock;

	if (!clock)
		return;

	/* Clear usecs if raw timestamps are requested */
	if (handle->flags & TRACECMD_FL_RAW_TS)
		handle->flags &= ~TRACECMD_FL_IN_USECS;

	/* tsc_calc is a conversion to nanoseconds */
	if (handle->tsc_calc.mult)
		return;

	/* Clear usecs if not one of the specified clocks */
	if (strcmp(clock, "local") && strcmp(clock, "global") &&
	    strcmp(clock, "uptime") && strcmp(clock, "perf") &&
	    strncmp(clock, "mono", 4) && strcmp(clock, TSCNSEC_CLOCK) &&
	    strcmp(clock, "tai"))
		handle->flags &= ~TRACECMD_FL_IN_USECS;
}

void tracecmd_parse_trace_clock(struct tracecmd_input *handle,
				char *file, int size __maybe_unused)
{
	char *line;
	char *next = NULL;

	line = strtok_r(file, " ", &next);
	while (line) {
		/* current trace_clock is shown as "[local]". */
		if (*line == '[')
			return extract_trace_clock(handle, line);
		line = strtok_r(NULL, " ", &next);
	}
}

static int read_and_parse_trace_clock(struct tracecmd_input *handle,
							struct tep_handle *pevent)
{
	unsigned long long size;
	char *trace_clock;

	if (read_data_and_size(handle, &trace_clock, &size) < 0)
		return -1;
	trace_clock[size] = 0;
	tracecmd_parse_trace_clock(handle, trace_clock, size);
	free(trace_clock);
	return 0;
}

static int init_data_v6(struct tracecmd_input *handle)
{
	struct tep_handle *pevent = handle->pevent;
	int ret;

	ret = read_cpu_data(handle);
	if (ret < 0)
		return ret;

	if (handle->use_trace_clock) {
		/*
		 * There was a bug in the original setting of
		 * the trace_clock file which let it get
		 * corrupted. If it fails to read, force local
		 * clock.
		 */
		if (read_and_parse_trace_clock(handle, pevent) < 0) {
			char clock[] = "[local]";
			tracecmd_warning("File has trace_clock bug, using local clock");
			tracecmd_parse_trace_clock(handle, clock, 8);
		}
	}
	return ret;
}

static int init_data(struct tracecmd_input *handle)
{
	return init_buffer_cpu_data(handle, &handle->top_buffer);
}

/**
 * tracecmd_init_data - prepare reading the data from trace.dat
 * @handle: input handle for the trace.dat file
 *
 * This prepares reading the data from trace.dat. This is called
 * after tracecmd_read_headers() and before tracecmd_read_data().
 */
int tracecmd_init_data(struct tracecmd_input *handle)
{
	int ret;

	if (!HAS_SECTIONS(handle))
		ret = init_data_v6(handle);
	else
		ret = init_data(handle);
	tracecmd_blk_hack(handle);

	return ret;
}

/**
 * tracecmd_make_pipe - Have the handle read a pipe instead of a file
 * @handle: input handle to read from a pipe
 * @cpu: the cpu that the pipe represents
 * @fd: the read end of the pipe
 * @cpus: the total number of cpus for this handle
 *
 * In order to stream data from the binary trace files and produce
 * output or analyze the data, a tracecmd_input descriptor needs to
 * be created, and then converted into a form that can act on a
 * pipe.
 *
 * Note, there are limitations to what this descriptor can do.
 * Most notibly, it can not read backwards. Once a page is read
 * it can not be read at a later time (except if a record is attached
 * to it and is holding the page ref).
 *
 * It is expected that the handle has already been created and
 * tracecmd_read_headers() has run on it.
 */
int tracecmd_make_pipe(struct tracecmd_input *handle, int cpu, int fd, int cpus)
{
	enum kbuffer_long_size long_size;
	enum kbuffer_endian endian;

	handle->read_page = true;
	handle->use_pipe = true;

	if (!handle->cpus) {
		handle->cpus = cpus;
		handle->cpu_data = malloc(sizeof(*handle->cpu_data) * handle->cpus);
		if (!handle->cpu_data)
			return -1;
	}

	if (cpu >= handle->cpus)
		return -1;


	if (handle->long_size == 8)
		long_size = KBUFFER_LSIZE_8;
	else
		long_size = KBUFFER_LSIZE_4;

	if (tep_is_file_bigendian(handle->pevent))
		endian = KBUFFER_ENDIAN_BIG;
	else
		endian = KBUFFER_ENDIAN_LITTLE;

	memset(&handle->cpu_data[cpu], 0, sizeof(handle->cpu_data[cpu]));
	handle->cpu_data[cpu].pipe_fd = fd;
	handle->cpu_data[cpu].cpu = cpu;

	handle->cpu_data[cpu].kbuf = kbuffer_alloc(long_size, endian);
	if (!handle->cpu_data[cpu].kbuf)
		return -1;
	if (tep_is_old_format(handle->pevent))
		kbuffer_set_old_format(handle->cpu_data[cpu].kbuf);

	handle->cpu_data[cpu].file_offset = 0;
	handle->cpu_data[cpu].file_size = -1;

	init_cpu(handle, cpu);

	return 0;
}

/**
 * tracecmd_print_events - print the events that are stored in trace.dat
 * @handle: input handle for the trace.dat file
 * @regex: regex of events to print (NULL is all events)
 *
 * This is a debugging routine to print out the events that
 * are stored in a given trace.dat file.
 */
void tracecmd_print_events(struct tracecmd_input *handle, const char *regex)
{
	if (!regex)
		regex = ".*";

	if (!HAS_SECTIONS(handle))
		read_headers_v6(handle, TRACECMD_FILE_ALL_EVENTS, regex);

	read_headers(handle, regex);
}

/* Show the cpu data stats */
static void show_cpu_stats(struct tracecmd_input *handle)
{
	struct cpu_data *cpu_data;
	int i;

	for (i = 0; i < handle->cpus; i++) {
		cpu_data = &handle->cpu_data[i];
		printf("CPU%d data recorded at offset=0x%llx\n",
		       i, cpu_data->file_offset);
		printf("    %lld bytes in size\n", cpu_data->file_size);
	}
}

/**
 * tracecmd_print_stats - prints the stats recorded in the options.
 * @handle: input handle for the trace.dat file
 *
 * Looks for the option TRACECMD_OPTION_CPUSTAT and prints out what's
 * stored there, if it is found. Otherwise it prints that none were found.
 */
void tracecmd_print_stats(struct tracecmd_input *handle)
{
	if (handle->cpustats)
		printf("%s\n", handle->cpustats);
	else
		printf(" No stats in this file\n");

	show_cpu_stats(handle);
}

/**
 * tracecmd_print_uname - prints the recorded uname if it was recorded
 * @handle: input handle for the trace.dat file
 *
 * Looks for the option TRACECMD_OPTION_UNAME and prints out what's
 * stored there, if it is found. Otherwise it prints that none were found.
 */
void tracecmd_print_uname(struct tracecmd_input *handle)
{
	if (handle->uname)
		printf("%s\n", handle->uname);
	else
		printf(" uname was not recorded in this file\n");
}

/**
 * tracecmd_print_uname - prints the recorded uname if it was recorded
 * @handle: input handle for the trace.dat file
 *
 * Looks for the option TRACECMD_OPTION_VERSION and prints out what's
 * stored there, if it is found. Otherwise it prints that none were found.
 */
void tracecmd_print_version(struct tracecmd_input *handle)
{
	if (handle->version)
		printf("%s\n", handle->version);
	else
		printf(" version was not recorded in this file\n");
}

/**
 * tracecmd_hooks - return the event hooks that were used in record
 * @handle: input handle for the trace.dat file
 *
 * If trace-cmd record used -H to save hooks, they are parsed and
 * presented as hooks here.
 *
 * Returns the hook list (do not free it, they are freed on close)
 */
struct hook_list *tracecmd_hooks(struct tracecmd_input *handle)
{
	return handle->hooks;
}

static int init_metadata_strings(struct tracecmd_input *handle, int size)
{
	char *tmp;

	tmp = realloc(handle->strings, handle->strings_size + size);
	if (!tmp)
		return -1;

	handle->strings = tmp;
	if (do_read_check(handle, handle->strings + handle->strings_size, size))
		return -1;

	handle->strings_size += size;

	return 0;
}

static int read_metadata_strings(struct tracecmd_input *handle)
{
	unsigned short flags;
	int found = 0;
	unsigned short id;
	unsigned int csize, rsize;
	unsigned long long size;
	off64_t offset;

	offset = lseek64(handle->fd, 0, SEEK_CUR);
	do {
		if (read_section_header(handle, &id, &flags, &size, NULL))
			break;
		if (id == TRACECMD_OPTION_STRINGS) {
			found++;
			if ((flags & TRACECMD_SEC_FL_COMPRESS)) {
				read4(handle, &csize);
				read4(handle, &rsize);
				do_lseek(handle, -8, SEEK_CUR);
				if (in_uncompress_block(handle))
					break;
			} else {
				rsize = size;
			}
			init_metadata_strings(handle, rsize);
			if (flags & TRACECMD_SEC_FL_COMPRESS)
				in_uncompress_reset(handle);
		} else {
			if (lseek64(handle->fd, size, SEEK_CUR) == (off_t)-1)
				break;
		}
	} while (1);

	if (lseek64(handle->fd, offset, SEEK_SET) == (off_t)-1)
		return -1;

	return found ? 0 : -1;
}

/**
 * tracecmd_alloc_fd - create a tracecmd_input handle from a file descriptor
 * @fd: the file descriptor for the trace.dat file
 * @flags: bitmask of enum tracecmd_open_flags
 *
 * Allocate a tracecmd_input handle from a file descriptor and open the
 * file. This tests if the file is of trace-cmd format and allocates
 * a parse event descriptor.
 *
 * The returned pointer is not ready to be read yet. A tracecmd_read_headers()
 * and tracecmd_init_data() still need to be called on the descriptor.
 *
 * Unless you know what you are doing with this, you want to use
 * tracecmd_open_fd() instead.
 */
struct tracecmd_input *tracecmd_alloc_fd(int fd, int flags)
{
	struct tracecmd_input *handle;
	char test[] = TRACECMD_MAGIC;
	unsigned int page_size;
	size_t offset;
	char *version = NULL;
	char *zver = NULL;
	char *zname = NULL;
	char buf[BUFSIZ];
	unsigned long ver;

	handle = malloc(sizeof(*handle));
	if (!handle)
		return NULL;
	memset(handle, 0, sizeof(*handle));

	handle->fd = fd;
	handle->ref = 1;
	handle->latz.fd = -1;
	/* By default, use usecs, unless told otherwise */
	handle->flags |= TRACECMD_FL_IN_USECS;

#ifdef INMEMORY_DECOMPRESS
	handle->read_zpage = 1;
#endif
	if (do_read_check(handle, buf, 3))
		goto failed_read;

	if (memcmp(buf, test, 3) != 0)
		goto failed_read;

	if (do_read_check(handle, buf, 7))
		goto failed_read;
	if (memcmp(buf, "tracing", 7) != 0)
		goto failed_read;

	version = read_string(handle);
	if (!version)
		goto failed_read;
	tracecmd_info("version = %s", version);
	ver = strtol(version, NULL, 10);
	if (!ver && errno)
		goto failed_read;
	if (!tracecmd_is_version_supported(ver)) {
		tracecmd_warning("Unsupported file version %lu", ver);
		goto failed_read;
	}
	handle->file_version = ver;
	free(version);
	version = NULL;

	if (handle->file_version >= FILE_VERSION_SECTIONS)
		handle->flags |= TRACECMD_FL_SECTIONED;
	if (handle->file_version >= FILE_VERSION_COMPRESSION)
		handle->flags |= TRACECMD_FL_COMPRESSION;

	if (do_read_check(handle, buf, 1))
		goto failed_read;

	handle->pevent = tep_alloc();
	if (!handle->pevent)
		goto failed_read;

	/* register default ftrace functions first */
	if (!(flags & TRACECMD_FL_LOAD_NO_PLUGINS) &&
	    !(flags & TRACECMD_FL_LOAD_NO_SYSTEM_PLUGINS))
		tracecmd_ftrace_overrides(handle, &handle->finfo);

	handle->plugin_list = trace_load_plugins(handle->pevent, flags);

	tep_set_file_bigendian(handle->pevent, buf[0]);
	tep_set_local_bigendian(handle->pevent, tracecmd_host_bigendian());

	do_read_check(handle, buf, 1);
	handle->long_size = buf[0];
	tep_set_long_size(handle->pevent, handle->long_size);

	read4(handle, &page_size);
	handle->page_size = page_size;
	handle->next_offset = page_size;

	offset = lseek64(handle->fd, 0, SEEK_CUR);
	handle->total_file_size = lseek64(handle->fd, 0, SEEK_END);
	lseek64(handle->fd, offset, SEEK_SET);

	if (HAS_COMPRESSION(handle)) {
		zname = read_string(handle);
		if (!zname)
			goto failed_read;

		zver = read_string(handle);
		if (!zver)
			goto failed_read;

		if (strcmp(zname, "none") == 0) {
			handle->read_zpage = false;
			handle->flags &= ~TRACECMD_FL_COMPRESSION;
		} else {
			handle->compress = tracecmd_compress_alloc(zname, zver,
								   handle->fd,
								   handle->pevent, NULL);
			if (!handle->compress) {
				tracecmd_warning("Unsupported file compression %s %s", zname, zver);
				goto failed_read;
			}
		}

		free(zname);
		free(zver);
	}

	if (HAS_SECTIONS(handle)) {
		if (read8(handle, &(handle->options_start))) {
			tracecmd_warning("Filed to read the offset of the first option section");
			goto failed_read;
		}
		read_metadata_strings(handle);
	}

	handle->file_state = TRACECMD_FILE_INIT;

	return handle;

 failed_read:
	free(version);
	free(zname);
	free(zver);
	free(handle);

	return NULL;
}

/**
 * tracecmd_alloc_fd - create a tracecmd_input handle from a file name
 * @file: the file name of the file that is of tracecmd data type.
 * @flags: bitmask of enum tracecmd_open_flags
 *
 * Allocate a tracecmd_input handle from a given file name and open the
 * file. This tests if the file is of trace-cmd format and allocates
 * a parse event descriptor.
 *
 * The returned pointer is not ready to be read yet. A tracecmd_read_headers()
 * and tracecmd_init_data() still need to be called on the descriptor.
 *
 * Unless you know what you are doing with this, you want to use
 * tracecmd_open() instead.
 */
struct tracecmd_input *tracecmd_alloc(const char *file, int flags)
{
	int fd;

	fd = open(file, O_RDONLY);
	if (fd < 0)
		return NULL;

	return tracecmd_alloc_fd(fd, flags);
}

/**
 * tracecmd_open_fd - create a tracecmd_handle from the trace.dat file descriptor
 * @fd: the file descriptor for the trace.dat file
 * @flags: bitmask of enum tracecmd_open_flags
 */
struct tracecmd_input *tracecmd_open_fd(int fd, int flags)
{
	struct tracecmd_input *handle;
	int ret;

	handle = tracecmd_alloc_fd(fd, flags);
	if (!handle)
		return NULL;

	if (tracecmd_read_headers(handle, 0) < 0)
		goto fail;

	if ((ret = tracecmd_init_data(handle)) < 0)
		goto fail;

	return handle;

fail:
	tracecmd_close(handle);
	return NULL;
}

/**
 * tracecmd_open - create a tracecmd_handle from a given file
 * @file: the file name of the file that is of tracecmd data type.
 * @flags: bitmask of enum tracecmd_open_flags
 */
struct tracecmd_input *tracecmd_open(const char *file, int flags)
{
	int fd;

	fd = open(file, O_RDONLY);
	if (fd < 0)
		return NULL;

	return tracecmd_open_fd(fd, flags);
}

/**
 * tracecmd_open_head - create a tracecmd_handle from a given file, read
 *			and parse only the trace headers from the file
 * @file: the file name of the file that is of tracecmd data type.
 * @flags: bitmask of enum tracecmd_open_flags
 */
struct tracecmd_input *tracecmd_open_head(const char *file, int flags)
{
	struct tracecmd_input *handle;
	int fd;

	fd = open(file, O_RDONLY);
	if (fd < 0)
		return NULL;

	handle = tracecmd_alloc_fd(fd, flags);
	if (!handle)
		return NULL;

	if (tracecmd_read_headers(handle, 0) < 0)
		goto fail;

	return handle;

fail:
	tracecmd_close(handle);
	return NULL;
}

/**
 * tracecmd_ref - add a reference to the handle
 * @handle: input handle for the trace.dat file
 *
 * Some applications may share a handle between parts of
 * the application. Let those parts add reference counters
 * to the handle, and the last one to close it will free it.
 */
void tracecmd_ref(struct tracecmd_input *handle)
{
	if (!handle)
		return;

	handle->ref++;
}

static inline void free_buffer(struct input_buffer_instance *buf)
{
	free(buf->name);
	free(buf->clock);
	free(buf->cpu_data);
}

/**
 * tracecmd_close - close and free the trace.dat handle
 * @handle: input handle for the trace.dat file
 *
 * Close the file descriptor of the handle and frees
 * the resources allocated by the handle.
 */
void tracecmd_close(struct tracecmd_input *handle)
{
	struct zchunk_cache *cache;
	struct file_section *del_sec;
	struct cpu_data *cpu_data;
	struct page_map *page_map, *n;
	int cpu;
	int i;

	if (!handle)
		return;

	if (handle->ref <= 0) {
		tracecmd_warning("tracecmd: bad ref count on handle");
		return;
	}

	if (--handle->ref)
		return;

	for (cpu = 0; cpu < handle->cpus; cpu++) {
		/* The tracecmd_peek_data may have cached a record */
		free_next(handle, cpu);
		free_page(handle, cpu);
		if (handle->cpu_data) {
			cpu_data = &handle->cpu_data[cpu];
			if (cpu_data->kbuf) {
				kbuffer_free(cpu_data->kbuf);
				if (cpu_data->page_map)
					free_page_map(cpu_data->page_map);

				if (cpu_data->page_cnt)
					tracecmd_warning("%d pages still allocated on cpu %d%s",
							 cpu_data->page_cnt, cpu,
							 show_records(cpu_data->pages,
								      cpu_data->nr_pages));
				free(cpu_data->pages);
			}
			if (cpu_data->compress.count) {
				if (cpu_data->compress.fd >= 0) {
					close(cpu_data->compress.fd);
					unlink(cpu_data->compress.file);
				}
				while (!list_empty(&cpu_data->compress.cache)) {
					cache = container_of(cpu_data->compress.cache.next,
							struct zchunk_cache, list);
					list_del(&cache->list);
					free(cache->map);
					free(cache);
				}
				free(cpu_data->compress.chunks);
			}
			if (cpu_data->page_maps.next) {
				list_for_each_entry_safe(page_map, n, &cpu_data->page_maps, list) {
					list_del(&page_map->list);
					free(page_map);
				}
			}
		}
	}

	free(handle->cpustats);
	free(handle->cpu_data);
	free(handle->uname);
	free(handle->trace_clock);
	free(handle->strings);
	free(handle->version);
	close(handle->fd);
	free(handle->latz.chunks);
	if (handle->latz.fd >= 0) {
		close(handle->latz.fd);
		unlink(handle->latz.file);
	}
	while (handle->sections) {
		del_sec = handle->sections;
		handle->sections = handle->sections->next;
		free(del_sec);
	}

	free_buffer(&handle->top_buffer);
	for (i = 0; i < handle->nr_buffers; i++)
		free_buffer(&handle->buffers[i]);
	free(handle->buffers);

	tracecmd_free_hooks(handle->hooks);
	handle->hooks = NULL;

	trace_pid_map_free(handle->pid_maps);
	handle->pid_maps = NULL;

	trace_tsync_offset_free(&handle->host);
	trace_guests_free(handle);

	tracecmd_filter_free(handle->filter);

	if (handle->flags & TRACECMD_FL_BUFFER_INSTANCE)
		tracecmd_close(handle->parent);
	else {
		/* Only main handle frees plugins, pevent and compression context */
		tracecmd_compress_destroy(handle->compress);
		tep_unload_plugins(handle->plugin_list, handle->pevent);
		tep_free(handle->pevent);
	}
	free(handle);
}

static int read_copy_size8(struct tracecmd_input *in_handle,
			   struct tracecmd_output *out_handle, unsigned long long *size)
{
	/* read size */
	if (do_read_check(in_handle, size, 8))
		return -1;

	if (do_write_check(out_handle, size, 8))
		return -1;

	*size = tep_read_number(in_handle->pevent, size, 8);
	return 0;
}

static int read_copy_size4(struct tracecmd_input *in_handle, struct tracecmd_output *out_handle,
			   unsigned int *size)
{
	/* read size */
	if (do_read_check(in_handle, size, 4))
		return -1;

	if (do_write_check(out_handle, size, 4))
		return -1;

	*size = tep_read_number(in_handle->pevent, size, 4);
	return 0;
}

static int read_copy_data(struct tracecmd_input *in_handle,
			  unsigned long long size,
			  struct tracecmd_output *out_handle)
{
	char *buf;

	buf = malloc(size);
	if (!buf)
		return -1;
	if (do_read_check(in_handle, buf, size))
		goto failed_read;

	if (do_write_check(out_handle, buf, size))
		goto failed_read;
	
	free(buf);

	return 0;

 failed_read:
	free(buf);
	return -1;
}


static bool check_in_state(struct tracecmd_input *handle, int new_state)
{
	return check_file_state(handle->file_version, handle->file_state, new_state);
}

static int copy_header_files(struct tracecmd_input *in_handle,
			     struct tracecmd_output *out_handle)
{
	bool compress = out_check_compression(out_handle);
	struct file_section *sec;
	unsigned long long offset;
	unsigned long long size;

	if (!check_in_state(in_handle, TRACECMD_FILE_HEADERS) ||
	    !check_out_state(out_handle, TRACECMD_FILE_HEADERS))
		return -1;

	sec = section_open(in_handle, TRACECMD_OPTION_HEADER_INFO);
	if (!sec)
		return -1;

	offset = out_write_section_header(out_handle, TRACECMD_OPTION_HEADER_INFO,
					  "headers", TRACECMD_SEC_FL_COMPRESS, true);
	out_compression_start(out_handle, compress);

	/* "header_page"  */
	if (read_copy_data(in_handle, 12, out_handle) < 0)
		goto error;

	if (read_copy_size8(in_handle, out_handle, &size) < 0)
		goto error;

	if (read_copy_data(in_handle, size, out_handle) < 0)
		goto error;

	/* "header_event"  */
	if (read_copy_data(in_handle, 13, out_handle) < 0)
		goto error;

	if (read_copy_size8(in_handle, out_handle, &size) < 0)
		goto error;

	if (read_copy_data(in_handle, size, out_handle) < 0)
		goto error;

	in_handle->file_state = TRACECMD_FILE_HEADERS;
	if (out_compression_end(out_handle, compress))
		goto error;

	out_set_file_state(out_handle, in_handle->file_state);
	section_close(in_handle, sec);

	if (out_update_section_header(out_handle, offset))
		goto error;

	return 0;
error:
	out_compression_reset(out_handle, compress);
	section_close(in_handle, sec);
	return -1;
}

static int copy_ftrace_files(struct tracecmd_input *in_handle, struct tracecmd_output *out_handle)
{
	bool compress = out_check_compression(out_handle);
	struct file_section *sec;
	unsigned long long offset;
	unsigned long long size;
	unsigned int count;
	unsigned int i;

	if (!check_in_state(in_handle, TRACECMD_FILE_FTRACE_EVENTS) ||
	    !check_out_state(out_handle, TRACECMD_FILE_FTRACE_EVENTS))
		return -1;

	sec = section_open(in_handle, TRACECMD_OPTION_FTRACE_EVENTS);
	if (!sec)
		return -1;
	offset = out_write_section_header(out_handle, TRACECMD_OPTION_FTRACE_EVENTS,
					  "ftrace events", TRACECMD_SEC_FL_COMPRESS, true);

	out_compression_start(out_handle, compress);

	if (read_copy_size4(in_handle, out_handle, &count) < 0)
		goto error;

	for (i = 0; i < count; i++) {

		if (read_copy_size8(in_handle, out_handle, &size) < 0)
			goto error;

		if (read_copy_data(in_handle, size, out_handle) < 0)
			goto error;
	}

	in_handle->file_state = TRACECMD_FILE_FTRACE_EVENTS;
	if (out_compression_end(out_handle, compress))
		goto error;

	out_set_file_state(out_handle, in_handle->file_state);

	section_close(in_handle, sec);

	if (out_update_section_header(out_handle, offset))
		goto error;

	return 0;
error:
	out_compression_reset(out_handle, compress);
	section_close(in_handle, sec);
	return -1;
}

static int copy_event_files(struct tracecmd_input *in_handle, struct tracecmd_output *out_handle)
{
	bool compress = out_check_compression(out_handle);
	struct file_section *sec;
	unsigned long long offset;
	unsigned long long size;
	char *system;
	unsigned int systems;
	unsigned int count;
	unsigned int i,x;

	if (!check_in_state(in_handle, TRACECMD_FILE_ALL_EVENTS) ||
	    !check_out_state(out_handle, TRACECMD_FILE_ALL_EVENTS))
		return -1;

	sec = section_open(in_handle, TRACECMD_OPTION_EVENT_FORMATS);
	if (!sec)
		return -1;
	offset = out_write_section_header(out_handle, TRACECMD_OPTION_EVENT_FORMATS,
					  "events format", TRACECMD_SEC_FL_COMPRESS, true);

	out_compression_start(out_handle, compress);

	if (read_copy_size4(in_handle, out_handle, &systems) < 0)
		goto error;

	for (i = 0; i < systems; i++) {
		system = read_string(in_handle);
		if (!system)
			goto error;
		if (do_write_check(out_handle, system, strlen(system) + 1)) {
			free(system);
			goto error;
		}
		free(system);

		if (read_copy_size4(in_handle, out_handle, &count) < 0)
			goto error;

		for (x=0; x < count; x++) {
			if (read_copy_size8(in_handle, out_handle, &size) < 0)
				goto error;

			if (read_copy_data(in_handle, size, out_handle) < 0)
				goto error;
		}
	}

	in_handle->file_state = TRACECMD_FILE_ALL_EVENTS;
	if (out_compression_end(out_handle, compress))
		goto error;

	out_set_file_state(out_handle, in_handle->file_state);

	section_close(in_handle, sec);

	if (out_update_section_header(out_handle, offset))
		goto error;

	return 0;
error:
	out_compression_reset(out_handle, compress);
	section_close(in_handle, sec);
	return -1;
}

static int copy_proc_kallsyms(struct tracecmd_input *in_handle, struct tracecmd_output *out_handle)
{
	bool compress = out_check_compression(out_handle);
	struct file_section *sec;
	unsigned long long offset;
	unsigned int size;

	if (!check_in_state(in_handle, TRACECMD_FILE_KALLSYMS) ||
	    !check_out_state(out_handle, TRACECMD_FILE_KALLSYMS))
		return -1;

	sec = section_open(in_handle, TRACECMD_OPTION_KALLSYMS);
	if (!sec)
		return -1;
	offset = out_write_section_header(out_handle, TRACECMD_OPTION_KALLSYMS,
					  "kallsyms", TRACECMD_SEC_FL_COMPRESS, true);

	out_compression_start(out_handle, compress);
	if (read_copy_size4(in_handle, out_handle, &size) < 0)
		goto error;

	if (!size)
		goto out; /* OK? */

	if (read_copy_data(in_handle, size, out_handle) < 0)
		goto error;
out:
	in_handle->file_state = TRACECMD_FILE_KALLSYMS;
	if (out_compression_end(out_handle, compress))
		goto error;

	out_set_file_state(out_handle, in_handle->file_state);

	section_close(in_handle, sec);

	if (out_update_section_header(out_handle, offset))
		goto error;

	return 0;
error:
	out_compression_reset(out_handle, compress);
	section_close(in_handle, sec);
	return -1;
}

static int copy_ftrace_printk(struct tracecmd_input *in_handle, struct tracecmd_output *out_handle)
{
	bool compress = out_check_compression(out_handle);
	struct file_section *sec;
	unsigned long long offset;
	unsigned int size;

	if (!check_in_state(in_handle, TRACECMD_FILE_PRINTK) ||
	    !check_out_state(out_handle, TRACECMD_FILE_PRINTK))
		return -1;

	sec = section_open(in_handle, TRACECMD_OPTION_PRINTK);
	if (!sec)
		return -1;

	offset = out_write_section_header(out_handle, TRACECMD_OPTION_PRINTK,
					  "printk", TRACECMD_SEC_FL_COMPRESS, true);

	out_compression_start(out_handle, compress);

	if (read_copy_size4(in_handle, out_handle, &size) < 0)
		goto error;

	if (!size)
		goto out; /* OK? */

	if (read_copy_data(in_handle, size, out_handle) < 0)
		goto error;

out:
	in_handle->file_state = TRACECMD_FILE_PRINTK;
	if (out_compression_end(out_handle, compress))
		goto error;

	out_set_file_state(out_handle, in_handle->file_state);

	section_close(in_handle, sec);

	if (out_update_section_header(out_handle, offset))
		goto error;

	return 0;
error:
	out_compression_reset(out_handle, compress);
	section_close(in_handle, sec);
	return -1;
}

static int copy_command_lines(struct tracecmd_input *in_handle, struct tracecmd_output *out_handle)
{
	bool compress = out_check_compression(out_handle);
	struct file_section *sec;
	unsigned long long offset;
	unsigned long long size;

	if (!check_in_state(in_handle, TRACECMD_FILE_CMD_LINES) ||
	    !check_out_state(out_handle, TRACECMD_FILE_CMD_LINES))
		return -1;

	sec = section_open(in_handle, TRACECMD_OPTION_CMDLINES);
	if (!sec)
		return -1;
	offset = out_write_section_header(out_handle, TRACECMD_OPTION_CMDLINES,
					  "command lines", TRACECMD_SEC_FL_COMPRESS, true);

	out_compression_start(out_handle, compress);

	if (read_copy_size8(in_handle, out_handle, &size) < 0)
		goto error;

	if (!size)
		goto out; /* OK? */

	if (read_copy_data(in_handle, size, out_handle) < 0)
		goto error;

out:
	in_handle->file_state = TRACECMD_FILE_CMD_LINES;
	if (out_compression_end(out_handle, compress))
		goto error;

	out_set_file_state(out_handle, in_handle->file_state);

	section_close(in_handle, sec);

	if (out_update_section_header(out_handle, offset))
		goto error;

	return 0;
error:
	out_compression_reset(out_handle, compress);
	section_close(in_handle, sec);
	return -1;
}

static int copy_cpu_count(struct tracecmd_input *in_handle, struct tracecmd_output *out_handle)
{
	unsigned int cpus;

	if (!check_in_state(in_handle, TRACECMD_FILE_CPU_COUNT) ||
	    !check_out_state(out_handle, TRACECMD_FILE_CPU_COUNT))
		return -1;

	if (!HAS_SECTIONS(in_handle)) {
		if (read4(in_handle, &cpus))
			return -1;
	} else {
		cpus = in_handle->max_cpu;
	}

	if (tracecmd_get_out_file_version(out_handle) < FILE_VERSION_SECTIONS) {
		cpus = tep_read_number(in_handle->pevent, &cpus, 4);
		if (do_write_check(out_handle, &cpus, 4))
			return -1;
	} else {
		tracecmd_add_option(out_handle, TRACECMD_OPTION_CPUCOUNT, sizeof(int), &cpus);
	}

	in_handle->file_state = TRACECMD_FILE_CPU_COUNT;
	out_set_file_state(out_handle, in_handle->file_state);

	return 0;
}

/**
 * tracecmd_copy_headers - Copy headers from a tracecmd_input handle to a file descriptor
 * @in_handle: input handle for the trace.dat file to copy from.
 * @out_handle: output handle to the trace.dat file to copy to.
 * @start_state: The file state to start copying from (zero for the beginnig)
 * @end_state: The file state to stop at (zero for up to cmdlines)
 *
 * This is used to copy trace header data of a trace.dat file to a
 * file descriptor. Using @start_state and @end_state it may be used
 * multiple times against the input handle.
 *
 * NOTE: The input handle is also modified, and ends at the end
 *       state as well.
 */
int tracecmd_copy_headers(struct tracecmd_input *in_handle,
			  struct tracecmd_output *out_handle,
			  enum tracecmd_file_states start_state,
			  enum tracecmd_file_states end_state)
{
	struct file_section *sec = NULL;
	int ret;

	if (!start_state)
		start_state = TRACECMD_FILE_HEADERS;
	if (!end_state)
		end_state = TRACECMD_FILE_CMD_LINES;

	if (start_state > end_state)
		return -1;

	if (end_state < TRACECMD_FILE_HEADERS)
		return 0;

	if (in_handle->file_state >= start_state) {
		/* Set the handle to just before the start state */
		sec = section_open(in_handle, TRACECMD_OPTION_HEADER_INFO);
		if (!sec)
			return -1;
		/* Now that the file handle has moved, change its state */
		in_handle->file_state = TRACECMD_FILE_INIT;
	}

	/* Try to bring the input up to the start state - 1 */
	ret = tracecmd_read_headers(in_handle, start_state - 1);
	if (sec)
		section_close(in_handle, sec);
	if (ret < 0)
		goto out;

	switch (start_state) {
	case TRACECMD_FILE_HEADERS:
		ret = copy_header_files(in_handle, out_handle);
		if (ret < 0)
			goto out;

		/* fallthrough */
	case TRACECMD_FILE_FTRACE_EVENTS:
		/* handle's state is now updating with the copies */
		if (end_state <= in_handle->file_state)
			return 0;

		ret = copy_ftrace_files(in_handle, out_handle);
		if (ret < 0)
			goto out;

		/* fallthrough */
	case TRACECMD_FILE_ALL_EVENTS:
		if (end_state <= in_handle->file_state)
			return 0;

		ret = copy_event_files(in_handle, out_handle);
		if (ret < 0)
			goto out;

		/* fallthrough */
	case TRACECMD_FILE_KALLSYMS:
		if (end_state <= in_handle->file_state)
			return 0;

		ret = copy_proc_kallsyms(in_handle, out_handle);
		if (ret < 0)
			goto out;

		/* fallthrough */
	case TRACECMD_FILE_PRINTK:
		if (end_state <= in_handle->file_state)
			return 0;

		ret = copy_ftrace_printk(in_handle, out_handle);
		if (ret < 0)
			goto out;

		/* fallthrough */
	case TRACECMD_FILE_CMD_LINES:
		if (end_state <= in_handle->file_state)
			return 0;

		ret = copy_command_lines(in_handle, out_handle);
		if (ret < 0)
			goto out;

		/* fallthrough */
	case TRACECMD_FILE_CPU_COUNT:
		if (end_state <= in_handle->file_state)
			return 0;

		ret = copy_cpu_count(in_handle, out_handle);
		if (ret < 0)
			goto out;

		/* fallthrough */
	default:
		break;
	}

 out:
	return ret < 0 ? -1 : 0;
}

int tracecmd_copy_buffer_descr(struct tracecmd_input *in_handle,
			       struct tracecmd_output *out_handle)
{
	int i;

	if (tracecmd_get_out_file_version(out_handle) >= FILE_VERSION_SECTIONS)
		return 0;

	for (i = 0; i < in_handle->nr_buffers; i++)
		tracecmd_add_buffer_info(out_handle, in_handle->buffers[i].name, 0);

	return tracecmd_write_buffer_info(out_handle);
}

static int copy_options_recursive(struct tracecmd_input *in_handle,
				  struct tracecmd_output *out_handle)
{
	unsigned short id, flags = 0;
	unsigned short option, en2;
	unsigned long long next;
	unsigned int size, en4;
	bool skip;

	for (;;) {
		if (do_read_check(in_handle, &option, 2))
			return -1;

		en2 = tep_read_number(in_handle->pevent, &option, 2);

		if (en2 == TRACECMD_OPTION_DONE && !HAS_SECTIONS(in_handle))
			return 0;

		/* next 4 bytes is the size of the option */
		if (do_read_check(in_handle, &size, 4))
			return -1;

		en4 = tep_read_number(in_handle->pevent, &size, 4);
		if (en2 == TRACECMD_OPTION_DONE) {
			/* option done v7 */
			if (en4 < 8)
				return -1;

			if (read8(in_handle, &next))
				return -1;

			if (!next)
				break;

			if (do_lseek(in_handle, next, SEEK_SET) == (off64_t)-1)
				return -1;

			if (read_section_header(in_handle, &id, &flags, NULL, NULL))
				return -1;

			if (id != TRACECMD_OPTION_DONE)
				return -1;

			if (flags & TRACECMD_SEC_FL_COMPRESS && in_uncompress_block(in_handle))
				return -1;

			return copy_options_recursive(in_handle, out_handle);
		}
		/* Do not copy these, as they have file specific offsets */
		switch (en2) {
		case TRACECMD_OPTION_BUFFER:
		case TRACECMD_OPTION_BUFFER_TEXT:
		case TRACECMD_OPTION_HEADER_INFO:
		case TRACECMD_OPTION_FTRACE_EVENTS:
		case TRACECMD_OPTION_EVENT_FORMATS:
		case TRACECMD_OPTION_KALLSYMS:
		case TRACECMD_OPTION_PRINTK:
		case TRACECMD_OPTION_CMDLINES:
			skip = true;
			break;
		default:
			skip = false;
			break;
		}
		if (skip) {
			do_lseek(in_handle, en4, SEEK_CUR);
			continue;
		}
		if (do_write_check(out_handle, &option, 2))
			return -1;

		if (do_write_check(out_handle, &size, 4))
			return -1;

		if (read_copy_data(in_handle, en4, out_handle))
			return -1;
	}

	return 0;
}

static int copy_options(struct tracecmd_input *in_handle, struct tracecmd_output *out_handle)
{
	unsigned long long offset, start;
	unsigned short id, en2, flags = 0;
	int tmp;

	if (HAS_SECTIONS(in_handle)) {
		if (read_section_header(in_handle, &id, &flags, NULL, NULL))
			return -1;

		if (id != TRACECMD_OPTION_DONE)
			return -1;

		if (flags & TRACECMD_SEC_FL_COMPRESS && in_uncompress_block(in_handle))
			return -1;
	}
	start = tracecmd_get_out_file_offset(out_handle);
	if (tracecmd_get_out_file_version(out_handle) < FILE_VERSION_SECTIONS) {
		if (do_write_check(out_handle, "options  ", 10))
			return -1;
	}

	offset = out_write_section_header(out_handle, TRACECMD_OPTION_DONE, "options", 0, false);

	if (copy_options_recursive(in_handle, out_handle))
		goto error;

	id = TRACECMD_OPTION_DONE;
	en2 = tep_read_number(in_handle->pevent, &id, 2);
	if (do_write_check(out_handle, &en2, 2))
		goto error;

	if (tracecmd_get_out_file_version(out_handle) < FILE_VERSION_SECTIONS) {
		out_save_options_offset(out_handle, start);
	} else {
		tmp = 8;
		if (do_write_check(out_handle, &tmp, 4))
			goto error;

		out_save_options_offset(out_handle, start);
		start = 0;
		if (do_write_check(out_handle, &start, 8))
			goto error;
	}
	out_update_section_header(out_handle, offset);
	if (flags & TRACECMD_SEC_FL_COMPRESS)
		in_uncompress_reset(in_handle);
	in_handle->file_state = TRACECMD_FILE_OPTIONS;
	out_set_file_state(out_handle, in_handle->file_state);
	/* Append local options */
	return tracecmd_append_options(out_handle);

error:
	if (flags & TRACECMD_SEC_FL_COMPRESS)
		in_uncompress_reset(in_handle);
	return 0;
}

int tracecmd_copy_options(struct tracecmd_input *in_handle,
			  struct tracecmd_output *out_handle)
{
	if (!check_in_state(in_handle, TRACECMD_FILE_OPTIONS) ||
	    !check_out_state(out_handle, TRACECMD_FILE_OPTIONS))
		return -1;

	if (!in_handle->options_start)
		return 0;

	if (lseek64(in_handle->fd, in_handle->options_start, SEEK_SET) == (off64_t)-1)
		return -1;

	if (copy_options(in_handle, out_handle) < 0)
		return -1;

	return 0;
}

static int copy_trace_latency(struct tracecmd_input *in_handle,
			      struct tracecmd_output *out_handle, const char *buf_name)
{
	int page_size = getpagesize();
	unsigned long long wsize;
	unsigned long long offset;
	int fd;

	if (tracecmd_get_out_file_version(out_handle) < FILE_VERSION_SECTIONS &&
	    do_write_check(out_handle, "latency  ", 10))
		return -1;

	offset = tracecmd_get_out_file_offset(out_handle);

	if (tracecmd_get_out_file_version(out_handle) >= FILE_VERSION_SECTIONS &&
	    !out_add_buffer_option(out_handle, buf_name, TRACECMD_OPTION_BUFFER_TEXT,
				   offset, 0, NULL, page_size))
		return -1;

	offset = out_write_section_header(out_handle, TRACECMD_OPTION_BUFFER_TEXT,
					  "buffer latency", TRACECMD_SEC_FL_COMPRESS, false);

	if (in_handle->latz.fd >= 0)
		fd = in_handle->latz.fd;
	else
		fd = in_handle->fd;

	if (!out_copy_fd_compress(out_handle, fd, 0, &wsize, page_size))
		return -1;

	if (out_update_section_header(out_handle, offset))
		return -1;

	out_set_file_state(out_handle, TRACECMD_FILE_CPU_LATENCY);
	return 0;
}

static int copy_trace_flyrecord_data(struct tracecmd_input *in_handle,
				     struct tracecmd_output *out_handle, const char *buff_name)
{
	struct cpu_data_source *data;
	int total_size = 0;
	int cpus;
	int ret;
	int i, j;

	if (tracecmd_get_out_file_version(out_handle) < FILE_VERSION_SECTIONS)
		cpus = in_handle->max_cpu;
	else
		cpus = in_handle->cpus;

	data = calloc(cpus, sizeof(struct cpu_data_source));
	if (!data)
		return -1;

	for (i = 0; i < in_handle->cpus; i++) {
		j = in_handle->cpu_data[i].cpu;
		data[j].size = in_handle->cpu_data[i].file_size;
		total_size += data[j].size;
		if (in_handle->cpu_data[i].compress.fd >= 0) {
			data[j].fd = in_handle->cpu_data[i].compress.fd;
			data[j].offset = 0;
		} else {
			data[j].fd = in_handle->fd;
			data[j].offset = in_handle->cpu_data[i].file_offset;
		}
	}
	if (total_size || tracecmd_get_out_file_version(out_handle) < FILE_VERSION_SECTIONS)
		ret = out_write_cpu_data(out_handle, cpus, data, buff_name);
	else
		ret = 0;
	free(data);

	return ret;
}

static int copy_flyrecord_buffer(struct tracecmd_input *in_handle,
				 struct tracecmd_output *out_handle, int index)
{
	struct tracecmd_input *instance;
	const char *name;
	int ret;

	name = tracecmd_buffer_instance_name(in_handle, index);
	if (!name)
		return -1;

	instance = tracecmd_buffer_instance_handle(in_handle, index);
	if (!instance)
		return -1;

	if (!tracecmd_get_quiet(out_handle) && *name)
		fprintf(stderr, "\nBuffer: %s\n\n", name);

	if (in_handle->buffers[index].latency)
		ret = copy_trace_latency(in_handle, out_handle, name);
	else
		ret = copy_trace_flyrecord_data(instance, out_handle, name);
	tracecmd_close(instance);

	return ret;
}

static int copy_trace_data_from_v6(struct tracecmd_input *in_handle,
				   struct tracecmd_output *out_handle)
{
	char buf[10];
	int ret;
	int i;

	if (do_read_check(in_handle, buf, 10))
		return -1;

	if (strncmp(buf, "latency", 7) == 0)
		in_handle->file_state = TRACECMD_FILE_CPU_LATENCY;
	else if (strncmp(buf, "flyrecord", 9) == 0)
		in_handle->file_state = TRACECMD_FILE_CPU_FLYRECORD;

	tracecmd_init_data(in_handle);
	tracecmd_set_out_clock(out_handle, in_handle->trace_clock);

	if (in_handle->file_state == TRACECMD_FILE_CPU_LATENCY)
		return copy_trace_latency(in_handle, out_handle, "");

	/* top instance */
	ret = copy_trace_flyrecord_data(in_handle, out_handle, "");
	if (ret)
		return ret;

	for (i = 0; i < in_handle->nr_buffers; i++)
		copy_flyrecord_buffer(in_handle, out_handle, i);

	return 0;
}

static int copy_trace_data_from_v7(struct tracecmd_input *in_handle,
				   struct tracecmd_output *out_handle)
{
	int ret;
	int i;

	/* Force using temporary files for trace data decompression */
	in_handle->read_zpage = false;
	ret = tracecmd_init_data(in_handle);
	if (ret < 0)
		return ret;
	tracecmd_set_out_clock(out_handle, in_handle->trace_clock);

	/* copy top buffer */
	if (in_handle->top_buffer.latency)
		ret = copy_trace_latency(in_handle, out_handle, in_handle->top_buffer.name);
	else if (in_handle->top_buffer.cpus)
		ret = copy_trace_flyrecord_data(in_handle, out_handle,
						in_handle->top_buffer.name);
	else if (tracecmd_get_out_file_version(out_handle) < FILE_VERSION_SECTIONS)
		ret = out_write_emty_cpu_data(out_handle, in_handle->max_cpu);
	if (ret)
		return ret;

	for (i = 0; i < in_handle->nr_buffers; i++)
		copy_flyrecord_buffer(in_handle, out_handle, i);

	return 0;
}

__hidden int tracecmd_copy_trace_data(struct tracecmd_input *in_handle,
				      struct tracecmd_output *out_handle)
{
	int ret;

	if (!check_in_state(in_handle, TRACECMD_FILE_CPU_FLYRECORD) ||
	    !check_out_state(out_handle, TRACECMD_FILE_CPU_FLYRECORD))
		return -1;

	if (in_handle->file_version < FILE_VERSION_SECTIONS)
		ret = copy_trace_data_from_v6(in_handle, out_handle);
	else
		ret = copy_trace_data_from_v7(in_handle, out_handle);

	return ret;
}

/**
 * tracecmd_record_at_buffer_start - return true if record is first on subbuffer
 * @handle: input handle for the trace.dat file
 * @record: The record to test if it is the first record on page
 *
 * Returns true if the record is the first record on the page.
 */
int tracecmd_record_at_buffer_start(struct tracecmd_input *handle,
				    struct tep_record *record)
{
	struct page *page = record->priv;
	struct kbuffer *kbuf = handle->cpu_data[record->cpu].kbuf;
	int offset;

	if (!page || !kbuf)
		return 0;

	offset = record->offset - page->offset;
	return offset == kbuffer_start_of_data(kbuf);
}

unsigned long long tracecmd_page_ts(struct tracecmd_input *handle,
				    struct tep_record *record)
{
	struct page *page = record->priv;
	struct kbuffer *kbuf = handle->cpu_data[record->cpu].kbuf;

	if (!page || !kbuf)
		return 0;

	return kbuffer_subbuf_timestamp(kbuf, page->map);
}

unsigned int tracecmd_record_ts_delta(struct tracecmd_input *handle,
				      struct tep_record *record)
{
	struct kbuffer *kbuf = handle->cpu_data[record->cpu].kbuf;
	struct page *page = record->priv;
	int offset;

	if (!page || !kbuf)
		return 0;

	offset = record->offset - page->offset;

	return kbuffer_ptr_delta(kbuf, page->map + offset);
}

struct kbuffer *tracecmd_record_kbuf(struct tracecmd_input *handle,
				     struct tep_record *record)
{
	return handle->cpu_data[record->cpu].kbuf;
}

void *tracecmd_record_page(struct tracecmd_input *handle,
			   struct tep_record *record)
{
	struct page *page = record->priv;

	return page ? page->map : NULL;
}

void *tracecmd_record_offset(struct tracecmd_input *handle,
			     struct tep_record *record)
{
	struct page *page = record->priv;
	int offset;

	if (!page)
		return NULL;

	offset = record->offset - page->offset;

	return page->map + offset;
}

int tracecmd_buffer_instances(struct tracecmd_input *handle)
{
	return handle->nr_buffers;
}

const char *tracecmd_buffer_instance_name(struct tracecmd_input *handle, int indx)
{
	if (indx >= handle->nr_buffers)
		return NULL;

	return handle->buffers[indx].name;
}

struct tracecmd_input *
tracecmd_buffer_instance_handle(struct tracecmd_input *handle, int indx)
{
	struct tracecmd_input *new_handle;
	struct input_buffer_instance *buffer = &handle->buffers[indx];
	size_t offset;
	ssize_t ret;

	if (indx >= handle->nr_buffers)
		return NULL;

	/*
	 * We make a copy of the current handle, but we substitute
	 * the cpu data with the cpu data for this buffer.
	 */
	new_handle = malloc(sizeof(*handle));
	if (!new_handle)
		return NULL;

	*new_handle = *handle;
	memset(&new_handle->top_buffer, 0, sizeof(new_handle->top_buffer));
	new_handle->cpu_data = NULL;
	new_handle->nr_buffers = 0;
	new_handle->buffers = NULL;
	new_handle->version = NULL;
	new_handle->sections = NULL;
	new_handle->strings = NULL;
	new_handle->guest = NULL;
	new_handle->ref = 1;
	if (handle->trace_clock) {
		new_handle->trace_clock = strdup(handle->trace_clock);
		if (!new_handle->trace_clock) {
			free(new_handle);
			return NULL;
		}
	}
	memset(&new_handle->host, 0, sizeof(new_handle->host));
	new_handle->parent = handle;
	new_handle->cpustats = NULL;
	new_handle->hooks = NULL;
	if (handle->uname)
		/* Ignore if fails to malloc, no biggy */
		new_handle->uname = strdup(handle->uname);
	tracecmd_ref(handle);

	new_handle->fd = dup(handle->fd);

	new_handle->flags |= TRACECMD_FL_BUFFER_INSTANCE;

	new_handle->pid_maps = NULL;
	if (!HAS_SECTIONS(handle)) {
		/* Save where we currently are */
		offset = lseek64(handle->fd, 0, SEEK_CUR);

		ret = lseek64(handle->fd, buffer->offset, SEEK_SET);
		if (ret == (off64_t)-1) {
			tracecmd_warning("could not seek to buffer %s offset %ld",
					  buffer->name, buffer->offset);
			goto error;
		}
		/*
		 * read_options_type() is called right after the CPU count so update
		 * file state accordingly.
		 */
		new_handle->file_state = TRACECMD_FILE_CPU_COUNT;
		ret = read_options_type(new_handle);
		if (!ret)
			ret = read_cpu_data(new_handle);

		if (ret < 0) {
			tracecmd_warning("failed to read sub buffer %s", buffer->name);
			goto error;
		}
		ret = lseek64(handle->fd, offset, SEEK_SET);
		if (ret < 0) {
			tracecmd_warning("could not seek to back to offset %ld", offset);
			goto error;
		}
	} else {
		new_handle->page_size = handle->buffers[indx].page_size;
		if (init_buffer_cpu_data(new_handle, buffer) < 0)
			goto error;
	}

	return new_handle;

error:
	tracecmd_close(new_handle);
	return NULL;
}

int tracecmd_is_buffer_instance(struct tracecmd_input *handle)
{
	return handle->flags & TRACECMD_FL_BUFFER_INSTANCE;
}

/**
 * tracecmd_long_size - return the size of "long" for the arch
 * @handle: input handle for the trace.dat file
 */
int tracecmd_long_size(struct tracecmd_input *handle)
{
	return handle->long_size;
}

/**
 * tracecmd_page_size - return the PAGE_SIZE for the arch
 * @handle: input handle for the trace.dat file
 */
int tracecmd_page_size(struct tracecmd_input *handle)
{
	return handle->page_size;
}

/**
 * tracecmd_page_size - return the number of CPUs recorded
 * @handle: input handle for the trace.dat file
 */
int tracecmd_cpus(struct tracecmd_input *handle)
{
	return handle->max_cpu;
}

/**
 * tracecmd_get_tep - return the tep handle
 * @handle: input handle for the trace.dat file
 */
struct tep_handle *tracecmd_get_tep(struct tracecmd_input *handle)
{
	return handle->pevent;
}

/**
 * tracecmd_get_in_file_version - return the trace.dat file version
 * @handle: input handle for the trace.dat file
 */
unsigned long tracecmd_get_in_file_version(struct tracecmd_input *handle)
{
	return handle->file_version;
}

/**
 * tracecmd_get_file_compress_proto - get name and version of compression algorithm
 * @handle: input handle for the trace.dat file
 * @name: return, name of the compression algorithm.
 * @version: return, version of the compression algorithm.
 *
 * Get the name and the version of the compression algorithm, used to
 * compress the file associated with @handle.
 * Returns 0 on success, or -1 in case of an error. If 0 is returned,
 * the name and version of the algorithm are stored in @name and @version.
 * The returned strings must *not* be freed.
 */
int tracecmd_get_file_compress_proto(struct tracecmd_input *handle,
				     const char **name, const char **version)
{
	return tracecmd_compress_proto_get_name(handle->compress, name, version);
}

/**
 * tracecmd_get_use_trace_clock - return use_trace_clock
 * @handle: input handle for the trace.dat file
 */
bool tracecmd_get_use_trace_clock(struct tracecmd_input *handle)
{
	return handle->use_trace_clock;
}

/**
 * tracecmd_get_options_offset - get offset of the options sections in the file
 * @handle: input handle for the trace.dat file
 */
size_t tracecmd_get_options_offset(struct tracecmd_input *handle)
{
	return handle->options_start;
}

/**
 * tracecmd_get_trace_clock - return the saved trace clock
 * @handle: input handle for the trace.dat file
 *
 * Returns a string of the clock that was saved in the trace.dat file.
 * The string should not be freed, as it points to the internal
 * structure data.
 */
const char *tracecmd_get_trace_clock(struct tracecmd_input *handle)
{
	return handle->trace_clock;
}

/**
 * tracecmd_get_cpustats - return the saved cpu stats
 * @handle: input handle for the trace.dat file
 *
 * Provides a method to extract the cpu stats saved in @handle.
 *
 * Returns a string of the cpu stats that was saved in the trace.dat file.
 * The string should not be freed, as it points to the internal
 * structure data.
 */
const char *tracecmd_get_cpustats(struct tracecmd_input *handle)
{
	return handle->cpustats;
}

/**
 * tracecmd_get_uname - return the saved name and kernel information
 * @handle: input handle for the trace.dat file
 *
 * Provides a method to extract the system information saved in @handle.
 *
 * Returns a string of the system information that was saved in the
 * trace.dat file.
 * The string should not be freed, as it points to the internal
 * structure data.
 */
const char *tracecmd_get_uname(struct tracecmd_input *handle)
{
	return handle->uname;
}

/**
 * tracecmd_get_version - return the saved version information
 * @handle: input handle for the trace.dat file
 *
 * Provides a method to extract the version string saved in @handle.
 *
 * Returns a string of the version that was saved in the trace.dat file.
 * The string should not be freed, as it points to the internal
 * structure data.
 */
const char *tracecmd_get_version(struct tracecmd_input *handle)
{
	return handle->version;
}

/**
 * tracecmd_get_cpu_file_size - return the saved cpu file size
 * @handle: input handle for the trace.dat file
 * @cpu: cpu index
 *
 * Provides a method to extract the cpu file size saved in @handle.
 *
 * Returns the cpu file size saved in trace.dat file or (off64_t)-1 for
 * invalid cpu index.
 */
off64_t tracecmd_get_cpu_file_size(struct tracecmd_input *handle, int cpu)
{
	if (cpu < 0 || cpu >= handle->cpus)
		return (off64_t)-1;
	return handle->cpu_data[cpu].file_size;
}

/**
 * tracecmd_get_show_data_func - return the show data func
 * @handle: input handle for the trace.dat file
 */
tracecmd_show_data_func
tracecmd_get_show_data_func(struct tracecmd_input *handle)
{
	return handle->show_data_func;
}

/**
 * tracecmd_set_show_data_func - set the show data func
 * @handle: input handle for the trace.dat file
 */
void tracecmd_set_show_data_func(struct tracecmd_input *handle,
				 tracecmd_show_data_func func)
{
	handle->show_data_func = func;
}

/**
 * tracecmd_get_traceid - get the trace id of the session
 * @handle: input handle for the trace.dat file
 *
 * Returns the trace id, written in the trace file
 */
unsigned long long tracecmd_get_traceid(struct tracecmd_input *handle)
{
	return handle->trace_id;
}

/**
 * tracecmd_get_first_ts - get the timestamp of the first recorded event
 * @handle: input handle for the trace.dat file
 *
 * Returns the timestamp of the first recorded event
 */
unsigned long long tracecmd_get_first_ts(struct tracecmd_input *handle)
{
	unsigned long long ts = 0;
	bool first = true;
	int i;

	for (i = 0; i < handle->cpus; i++) {
		/* Ignore empty buffers */
		if (!handle->cpu_data[i].size)
			continue;
		if (first || ts > handle->cpu_data[i].first_ts)
			ts = handle->cpu_data[i].first_ts;
		first = false;
	}

	return ts;
}

/**
 * tracecmd_get_guest_cpumap - get the mapping of guest VCPU to host process
 * @handle: input handle for the trace.dat file
 * @trace_id: ID of the guest tracing session
 * @name: return, name of the guest
 * @vcpu_count: return, number of VPUs
 * @cpu_pid: return, array with guest VCPU to host process mapping
 *
 * Returns @name of the guest, number of VPUs (@vcpu_count)
 * and array @cpu_pid with size @vcpu_count. Array index is VCPU id, array
 * content is PID of the host process, running this VCPU.
 *
 * This information is stored in host trace.dat file
 */
int tracecmd_get_guest_cpumap(struct tracecmd_input *handle,
			      unsigned long long trace_id,
			      const char **name,
			      int *vcpu_count, const int **cpu_pid)
{
	struct guest_trace_info	*guest = handle->guest;

	while (guest) {
		if (guest->trace_id == trace_id)
			break;
		guest = guest->next;
	}
	if (!guest)
		return -1;

	if (name)
		*name = guest->name;
	if (vcpu_count)
		*vcpu_count = guest->vcpu_count;
	if (cpu_pid)
		*cpu_pid = guest->cpu_pid;
	return 0;
}

/**
 * tracecmd_enable_tsync - enable / disable the timestamps correction
 * @handle: input handle for the trace.dat file
 * @enable: enable / disable the timestamps correction
 *
 * Enables or disables timestamps correction on file load, using the array of
 * recorded time offsets. If "enable" is true, but there are no time offsets,
 * function fails and -1 is returned.
 *
 * Returns -1 in case of an error, or 0 otherwise
 */
int tracecmd_enable_tsync(struct tracecmd_input *handle, bool enable)
{
	if (enable &&
	    (!handle->host.ts_offsets || !handle->host.cpu_count))
		return -1;

	handle->host.sync_enable = enable;

	return 0;
}

__hidden struct tracecmd_filter *tracecmd_filter_get(struct tracecmd_input *handle)
{
	return handle->filter;
}

__hidden void tracecmd_filter_set(struct tracecmd_input *handle,
				  struct tracecmd_filter *filter)
{
	/* This can be used to set filter to NULL though. */
	if (handle->filter && filter) {
		tracecmd_warning("Filter exists and setting a new one");
		return;
	}

	handle->filter = filter;
}
