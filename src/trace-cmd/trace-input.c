// SPDX-License-Identifier: LGPL-2.1
/*
 * Copyright (C) 2009, 2010 Red Hat Inc, Steven Rostedt <srostedt@redhat.com>
 *
 */

/* gpuvis change! */
#ifndef _LARGEFILE64_SOURCE
#define _LARGEFILE64_SOURCE
#endif

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
};

struct input_buffer_instance {
	char			*name;
	size_t			offset;
};

struct ts_offset_sample {
	long long	time;
	long long	offset;
	long long	scaling;
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

struct tracecmd_input {
	struct tep_handle	*pevent;
	unsigned long		file_state;
	struct tep_plugin_list	*plugin_list;
	struct tracecmd_input	*parent;
	unsigned long		flags;
	unsigned long long	trace_id;
	int			fd;
	int			long_size;
	int			page_size;
	int			page_map_size;
	int			cpus;
	int			ref;
	int			nr_buffers;	/* buffer instances */
	bool			use_trace_clock;
	bool			read_page;
	bool			use_pipe;
	int			file_version;
	struct cpu_data 	*cpu_data;
	long long		ts_offset;
	struct tsc2nsec		tsc_calc;

	struct host_trace_info	host;
	double			ts2secs;
	char *			cpustats;
	char *			uname;
	char *			version;
	char *			trace_clock;
	struct input_buffer_instance	*buffers;
	int			parsing_failures;
	struct guest_trace_info	*guest;

	struct tracecmd_ftrace	finfo;

	struct hook_list	*hooks;
	struct pid_addr_maps	*pid_maps;
	/* file information */
	size_t			header_files_start;
	size_t			ftrace_files_start;
	size_t			event_files_start;
	size_t			options_start;
	size_t			total_file_size;

	/* For custom profilers. */
	tracecmd_show_data_func	show_data_func;
};

__thread struct tracecmd_input *tracecmd_curr_thread_handle;

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

static ssize_t do_read(struct tracecmd_input *handle, void *data, size_t size)
{
	ssize_t tot = 0;
	ssize_t r;

	do {
		r = read(handle->fd, data + tot, size - tot);
		tot += r;

		if (!r)
			break;
		if (r < 0)
			return r;
	} while (tot != size);

	return tot;
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
		if (r < 0)
			goto fail;
		if (!r)
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
	r = lseek(handle->fd, -(r - (i+1)), SEEK_CUR);
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

static int read_header_files(struct tracecmd_input *handle)
{
	struct tep_handle *pevent = handle->pevent;
	unsigned long long size;
	char *header;
	char buf[BUFSIZ];

	if (handle->file_state >= TRACECMD_FILE_HEADERS)
		return 0;

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

	handle->ftrace_files_start =
		lseek64(handle->fd, 0, SEEK_CUR);

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

	if (handle->file_state >= TRACECMD_FILE_FTRACE_EVENTS)
		return 0;

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

	if (read4(handle, &count) < 0)
		return -1;

	for (i = 0; i < count; i++) {
		if (read8(handle, &size) < 0)
			return -1;
		ret = read_ftrace_file(handle, size, print_all, ereg);
		if (ret < 0)
			return -1;
	}

	handle->event_files_start =
		lseek64(handle->fd, 0, SEEK_CUR);

	if (sreg) {
		regfree(sreg);
		regfree(ereg);
	}

	handle->file_state = TRACECMD_FILE_FTRACE_EVENTS;

	return 0;
}

static int read_event_files(struct tracecmd_input *handle, const char *regex)
{
	unsigned long long size;
	char *system;
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

	if (handle->file_state >= TRACECMD_FILE_ALL_EVENTS)
		return 0;

	if (regex) {
		sreg = &spreg;
		ereg = &epreg;
		ret = make_preg_files(regex, sreg, ereg, &unique);
		if (ret)
			return -1;
	}

	if (read4(handle, &systems) < 0)
		return -1;

	for (i = 0; i < systems; i++) {
		system = read_string(handle);
		if (!system)
			return -1;

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

		if (read4(handle, &count) < 0)
			goto failed;

		for (x=0; x < count; x++) {
			if (read8(handle, &size) < 0)
				goto failed;

			ret = read_event_file(handle, system, size,
					      print_all, &sys_printed,
					      reg);
			if (ret < 0)
				goto failed;
		}
		free(system);
	}

	if (sreg) {
		regfree(sreg);
		regfree(ereg);
	}

	handle->file_state = TRACECMD_FILE_ALL_EVENTS;

	return 0;

 failed:
	if (sreg) {
		regfree(sreg);
		regfree(ereg);
	}

	free(system);
	return -1;
}

static int read_proc_kallsyms(struct tracecmd_input *handle)
{
	struct tep_handle *pevent = handle->pevent;
	unsigned int size;
	char *buf;

	if (handle->file_state >= TRACECMD_FILE_KALLSYMS)
		return 0;

	if (read4(handle, &size) < 0)
		return -1;
	if (!size)
		return 0; /* OK? */

	buf = malloc(size+1);
	if (!buf)
		return -1;
	if (do_read_check(handle, buf, size)){
		free(buf);
		return -1;
	}
	buf[size] = 0;

	tep_parse_kallsyms(pevent, buf);

	free(buf);

	handle->file_state = TRACECMD_FILE_KALLSYMS;

	return 0;
}

static int read_ftrace_printk(struct tracecmd_input *handle)
{
	unsigned int size;
	char *buf;

	if (handle->file_state >= TRACECMD_FILE_PRINTK)
		return 0;

	if (read4(handle, &size) < 0)
		return -1;
	if (!size)
		return 0; /* OK? */

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

	if (handle->file_state >= TRACECMD_FILE_CPU_COUNT)
		return 0;

	if (read4(handle, &cpus) < 0)
		return -1;

	handle->cpus = cpus;
	tep_set_cpus(handle->pevent, handle->cpus);
	handle->file_state = TRACECMD_FILE_CPU_COUNT;

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

	tep_set_long_size(handle->pevent, handle->long_size);

	if (state <= handle->file_state)
		return 0;

	ret = read_ftrace_files(handle, NULL);
	if (ret < 0)
		return -1;

	if (state <= handle->file_state)
		return 0;

	ret = read_event_files(handle, NULL);
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

static void *allocate_page_map(struct tracecmd_input *handle,
			       struct page *page, int cpu, off64_t offset)
{
	struct cpu_data *cpu_data = &handle->cpu_data[cpu];
	struct page_map *page_map;
	off64_t map_size;
	off64_t map_offset;
	void *map;
	int ret;

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

	if (map_offset < cpu_data->file_offset) {
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

 again:
	page_map->size = map_size;
	page_map->offset = map_offset;

	page_map->map = mmap(NULL, map_size, PROT_READ, MAP_PRIVATE,
			 handle->fd, map_offset);

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
		tracecmd_critical("Page ref count is zero!\n");
		return;
	}

	page->ref_count--;
	if (page->ref_count)
		return;

	if (handle->read_page)
		free(page->map);
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
	long long scaling;
	long long tscor;

	if (flags & TRACECMD_TSYNC_FLAG_INTERPOLATE) {
		long long delta = max->time - min->time;
		long long offset = ((long long)ts - min->time) *
				   (max->offset - min->offset);

		scaling = (min->scaling + max->scaling) / 2;
		tscor = min->offset + (offset + delta / 2) / delta;

	} else {
		scaling = min->scaling;
		tscor = min->offset;
	}

	ts *= scaling;
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
	int ret;

	ret = get_page(handle, cpu, handle->cpu_data[cpu].file_offset);
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
	record->cpu = cpu;
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

static int init_cpu(struct tracecmd_input *handle, int cpu)
{
	struct cpu_data *cpu_data = &handle->cpu_data[cpu];
	int i;

	cpu_data->offset = cpu_data->file_offset;
	cpu_data->size = cpu_data->file_size;
	cpu_data->timestamp = 0;

	list_head_init(&cpu_data->page_maps);

	if (!cpu_data->size) {
		printf("CPU %d is empty\n", cpu);
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
		int i;							\
		for (i = 0; i < ts_offsets->ts_samples_count; i++)	\
			safe_read(ts_offsets->ts_samples[i].type, 8);	\
	} while (0)

static int tsync_offset_load(struct tep_handle	*tep,
			     struct timesync_offsets *ts_offsets, char *buf, int size)
{
	int start_size = size;
	int i, j;

	safe_read_loop(time);
	safe_read_loop(offset);
	safe_read_loop(scaling);
	qsort(ts_offsets->ts_samples, ts_offsets->ts_samples_count,
	      sizeof(struct ts_offset_sample), tsync_offset_cmp);
	/* Filter possible samples with equal time */
	for (i = 0, j = 0; i < ts_offsets->ts_samples_count; i++) {
		if (i == 0 || ts_offsets->ts_samples[i].time != ts_offsets->ts_samples[i-1].time)
			ts_offsets->ts_samples[j++] = ts_offsets->ts_samples[i];
	}
	ts_offsets->ts_samples_count = j;

	return start_size - size;
}

static int tsync_cpu_offsets_load(struct tracecmd_input *handle, char *buf, int size)
{
	struct tep_handle *tep = handle->pevent;
	int ret;
	int i;

	safe_read(handle->host.cpu_count, 4);
	handle->host.ts_offsets = calloc(handle->host.cpu_count,
					 sizeof(struct timesync_offsets));
	if (!handle->host.ts_offsets)
		return -ENOMEM;
	for (i = 0; i < handle->host.cpu_count; i++) {
		safe_read(handle->host.ts_offsets[i].ts_samples_count, 4);
		handle->host.ts_offsets[i].ts_samples = calloc(handle->host.ts_offsets[i].ts_samples_count,
							       sizeof(struct ts_offset_sample));
		if (!handle->host.ts_offsets[i].ts_samples)
			return -ENOMEM;
		ret = tsync_offset_load(tep, &handle->host.ts_offsets[i], buf, size);
		if (ret <= 0)
			return -EFAULT;
		size -= ret;
		buf += ret;
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

/* gpuvis change! */
static void tracecmd_parse_tgids(struct tep_handle *tep,
                                 char *file, int size __maybe_unused)
{
	char *next = NULL;
	int pid, tgid;
	char *endptr;
	char *line;

	line = strtok_r(file, "\n", &next);
	while (line) {
		pid = strtol(line, &endptr, 10);
		if (endptr && *endptr == ' ') {
			tgid = strtol(endptr + 1, NULL, 10);
			tep_register_tgid(tep, tgid, pid);
		}
		line = strtok_r(NULL, "\n", &next);
	}
}

static int handle_options(struct tracecmd_input *handle)
{
	long long offset;
	unsigned short option;
	unsigned int size;
	char *cpustats = NULL;
	unsigned int cpustats_size = 0;
	struct input_buffer_instance *buffer;
	struct hook_list *hook;
	char *buf;
	int cpus;
	int ret;

	/* By default, use usecs, unless told otherwise */
	handle->flags |= TRACECMD_FL_IN_USECS;
	handle->options_start = lseek64(handle->fd, 0, SEEK_CUR);

	for (;;) {
		if (do_read_check(handle, &option, 2))
			return -1;

		if (option == TRACECMD_OPTION_DONE)
			break;

		/* next 4 bytes is the size of the option */
		if (do_read_check(handle, &size, 4))
			return -1;
		size = tep_read_number(handle->pevent, &size, 4);
		buf = malloc(size);
		if (!buf)
			return -ENOMEM;
		if (do_read_check(handle, buf, size))
			return -1;

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
			 */
			if (size < 16 || (handle->flags & TRACECMD_FL_RAW_TS))
				break;
			handle->host.peer_trace_id = tep_read_number(handle->pevent,
								     buf, 8);
			handle->host.flags = tep_read_number(handle->pevent,
							     buf + 8, 4);
			ret = tsync_cpu_offsets_load(handle, buf + 12, size - 12);
			if (ret < 0)
				return ret;
			tracecmd_enable_tsync(handle, true);
			break;
		case TRACECMD_OPTION_CPUSTAT:
			buf[size-1] = '\n';
			cpustats = realloc(cpustats, cpustats_size + size + 1);
			if (!cpustats)
				return -ENOMEM;
			memcpy(cpustats + cpustats_size, buf, size);
			cpustats_size += size;
			cpustats[cpustats_size] = 0;
			break;
		case TRACECMD_OPTION_BUFFER:
			/* A buffer instance is saved at the end of the file */
			handle->nr_buffers++;
			handle->buffers = realloc(handle->buffers,
						  sizeof(*handle->buffers) * handle->nr_buffers);
			if (!handle->buffers)
				return -ENOMEM;
			buffer = &handle->buffers[handle->nr_buffers - 1];
			buffer->name = strdup(buf + 8);
			if (!buffer->name) {
				free(handle->buffers);
				handle->buffers = NULL;
				return -ENOMEM;
			}
			offset = *(unsigned long long *)buf;
			buffer->offset = tep_read_number(handle->pevent, &offset, 8);
			break;
		case TRACECMD_OPTION_TRACECLOCK:
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
			/* gpuvis change! */
			if (size == sizeof(int))
			{
				cpus = *(int *)buf;
				handle->cpus = tep_read_number(handle->pevent, &cpus, 4);
			}
			else if (size > sizeof( unsigned long long ))
			{
				tracecmd_parse_tgids(handle->pevent, buf, size);
			}
			break;
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
			break;

		/* gpuvis change! */
		case TRACECMD_OPTION_SAVED_TGIDS:
			tracecmd_parse_tgids(handle->pevent, buf, size);
			break;
		default:
			tracecmd_warning("unknown option %d", option);
			break;
		}

		free(buf);

	}

	handle->cpustats = cpustats;

	return 0;
}

static int read_options_type(struct tracecmd_input *handle)
{
	char buf[10];

	if (handle->file_state >= TRACECMD_FILE_CPU_LATENCY)
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

static int read_cpu_data(struct tracecmd_input *handle)
{
	struct tep_handle *pevent = handle->pevent;
	enum kbuffer_long_size long_size;
	enum kbuffer_endian endian;
	unsigned long long size;
	unsigned long long max_size = 0;
	unsigned long long pages;
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
		unsigned long long offset;

		handle->cpu_data[cpu].cpu = cpu;

		handle->cpu_data[cpu].kbuf = kbuffer_alloc(long_size, endian);
		if (!handle->cpu_data[cpu].kbuf)
			goto out_free;
		if (tep_is_old_format(pevent))
			kbuffer_set_old_format(handle->cpu_data[cpu].kbuf);

		read8(handle, &offset);
		read8(handle, &size);

		handle->cpu_data[cpu].file_offset = offset;
		handle->cpu_data[cpu].file_size = size;
		if (size > max_size)
			max_size = size;

		if (size && (offset + size > handle->total_file_size)) {
			/* this happens if the file got truncated */
			printf("File possibly truncated. "
				"Need at least %llu, but file size is %zu.\n",
				offset + size, handle->total_file_size);
			errno = EINVAL;
			goto out_free;
		}
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

	return 0;

 out_free:
	for ( ; cpu >= 0; cpu--) {
		free_page(handle, cpu);
		kbuffer_free(handle->cpu_data[cpu].kbuf);
		handle->cpu_data[cpu].kbuf = NULL;
	}
	return -1;
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

	if (handle->file_state >= TRACECMD_FILE_CMD_LINES)
		return 0;

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

	/* Clear usecs if not one of the specified clocks */
	if (strcmp(clock, "local") && strcmp(clock, "global") &&
	    strcmp(clock, "uptime") && strcmp(clock, "perf") &&
	    strncmp(clock, "mono", 4) && strcmp(clock, TSCNSEC_CLOCK))
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

/**
 * tracecmd_init_data - prepare reading the data from trace.dat
 * @handle: input handle for the trace.dat file
 *
 * This prepares reading the data from trace.dat. This is called
 * after tracecmd_read_headers() and before tracecmd_read_data().
 */
int tracecmd_init_data(struct tracecmd_input *handle)
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
	int ret;

	if (!regex)
		regex = ".*";

	if (!handle->ftrace_files_start) {
		lseek64(handle->fd, handle->header_files_start, SEEK_SET);
		read_header_files(handle);
	}
	ret = read_ftrace_files(handle, regex);
	if (ret < 0)
		return;

	read_event_files(handle, regex);
	return;
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
	char *version;
	char buf[BUFSIZ];
	unsigned long ver;

	handle = malloc(sizeof(*handle));
	if (!handle)
		return NULL;
	memset(handle, 0, sizeof(*handle));

	handle->fd = fd;
	handle->ref = 1;

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
	tracecmd_info("version = %s\n", version);
	ver = strtol(version, NULL, 10);
	if (!ver && errno)
		goto failed_read;
	if (!tracecmd_is_version_supported(ver)) {
		tracecmd_warning("Unsupported file version %lu", ver);
		goto failed_read;
	}
	handle->file_version = ver;
	free(version);

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

	read4(handle, &page_size);
	handle->page_size = page_size;

	handle->header_files_start =
		lseek64(handle->fd, 0, SEEK_CUR);

	handle->total_file_size =
		lseek64(handle->fd, 0, SEEK_END);

	handle->header_files_start =
		lseek64(handle->fd, handle->header_files_start, SEEK_SET);

	handle->file_state = TRACECMD_FILE_INIT;

	return handle;

 failed_read:
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

/**
 * tracecmd_close - close and free the trace.dat handle
 * @handle: input handle for the trace.dat file
 *
 * Close the file descriptor of the handle and frees
 * the resources allocated by the handle.
 */
void tracecmd_close(struct tracecmd_input *handle)
{
	int cpu;

	if (!handle)
		return;

	if (handle->ref <= 0) {
		tracecmd_warning("tracecmd: bad ref count on handle\n");
		return;
	}

	if (--handle->ref)
		return;

	for (cpu = 0; cpu < handle->cpus; cpu++) {
		/* The tracecmd_peek_data may have cached a record */
		free_next(handle, cpu);
		free_page(handle, cpu);
		if (handle->cpu_data && handle->cpu_data[cpu].kbuf) {
			kbuffer_free(handle->cpu_data[cpu].kbuf);
			if (handle->cpu_data[cpu].page_map)
				free_page_map(handle->cpu_data[cpu].page_map);

			if (handle->cpu_data[cpu].page_cnt)
				tracecmd_warning("%d pages still allocated on cpu %d%s",
						 handle->cpu_data[cpu].page_cnt, cpu,
						 show_records(handle->cpu_data[cpu].pages,
							      handle->cpu_data[cpu].nr_pages));
			free(handle->cpu_data[cpu].pages);
		}
	}

	free(handle->cpustats);
	free(handle->cpu_data);
	free(handle->uname);
	free(handle->trace_clock);
	close(handle->fd);

	tracecmd_free_hooks(handle->hooks);
	handle->hooks = NULL;

	trace_pid_map_free(handle->pid_maps);
	handle->pid_maps = NULL;

	trace_tsync_offset_free(&handle->host);
	trace_guests_free(handle);

	if (handle->flags & TRACECMD_FL_BUFFER_INSTANCE)
		tracecmd_close(handle->parent);
	else {
		/* Only main handle frees plugins and pevent */
		tep_unload_plugins(handle->plugin_list, handle->pevent);
		tep_free(handle->pevent);
	}
	free(handle);
}

static int read_copy_size8(struct tracecmd_input *handle, int fd, unsigned long long *size)
{
	/* read size */
	if (do_read_check(handle, size, 8))
		return -1;

	if (__do_write_check(fd, size, 8))
		return -1;

	*size = tep_read_number(handle->pevent, size, 8);
	return 0;
}

static int read_copy_size4(struct tracecmd_input *handle, int fd, unsigned int *size)
{
	/* read size */
	if (do_read_check(handle, size, 4))
		return -1;

	if (__do_write_check(fd, size, 4))
		return -1;

	*size = tep_read_number(handle->pevent, size, 4);
	return 0;
}

static int read_copy_data(struct tracecmd_input *handle,
			  unsigned long long size, int fd)
{
	char *buf;

	buf = malloc(size);
	if (!buf)
		return -1;
	if (do_read_check(handle, buf, size))
		goto failed_read;

	if (__do_write_check(fd, buf, size))
		goto failed_read;
	
	free(buf);

	return 0;

 failed_read:
	free(buf);
	return -1;
}

static int copy_header_files(struct tracecmd_input *handle, int fd)
{
	unsigned long long size;

	if (handle->file_state != TRACECMD_FILE_HEADERS - 1)
		return -1;

	/* "header_page"  */
	if (read_copy_data(handle, 12, fd) < 0)
		return -1;

	if (read_copy_size8(handle, fd, &size) < 0)
		return -1;

	if (read_copy_data(handle, size, fd) < 0)
		return -1;

	/* "header_event"  */
	if (read_copy_data(handle, 13, fd) < 0)
		return -1;

	if (read_copy_size8(handle, fd, &size) < 0)
		return -1;

	if (read_copy_data(handle, size, fd) < 0)
		return -1;

	handle->file_state = TRACECMD_FILE_HEADERS;

	return 0;
}

static int copy_ftrace_files(struct tracecmd_input *handle, int fd)
{
	unsigned long long size;
	unsigned int count;
	unsigned int i;

	if (handle->file_state != TRACECMD_FILE_FTRACE_EVENTS - 1)
		return -1;

	if (read_copy_size4(handle, fd, &count) < 0)
		return -1;

	for (i = 0; i < count; i++) {

		if (read_copy_size8(handle, fd, &size) < 0)
			return -1;

		if (read_copy_data(handle, size, fd) < 0)
			return -1;
	}

	handle->file_state = TRACECMD_FILE_FTRACE_EVENTS;

	return 0;
}

static int copy_event_files(struct tracecmd_input *handle, int fd)
{
	unsigned long long size;
	char *system;
	unsigned int systems;
	unsigned int count;
	unsigned int i,x;

	if (handle->file_state != TRACECMD_FILE_ALL_EVENTS - 1)
		return -1;

	if (read_copy_size4(handle, fd, &systems) < 0)
		return -1;

	for (i = 0; i < systems; i++) {
		system = read_string(handle);
		if (!system)
			return -1;
		if (__do_write_check(fd, system, strlen(system) + 1)) {
			free(system);
			return -1;
		}
		free(system);

		if (read_copy_size4(handle, fd, &count) < 0)
			return -1;

		for (x=0; x < count; x++) {
			if (read_copy_size8(handle, fd, &size) < 0)
				return -1;

			if (read_copy_data(handle, size, fd) < 0)
				return -1;
		}
	}

	handle->file_state = TRACECMD_FILE_ALL_EVENTS;

	return 0;
}

static int copy_proc_kallsyms(struct tracecmd_input *handle, int fd)
{
	unsigned int size;

	if (handle->file_state != TRACECMD_FILE_KALLSYMS - 1)
		return -1;

	if (read_copy_size4(handle, fd, &size) < 0)
		return -1;
	if (!size)
		return 0; /* OK? */

	if (read_copy_data(handle, size, fd) < 0)
		return -1;

	handle->file_state = TRACECMD_FILE_KALLSYMS;

	return 0;
}

static int copy_ftrace_printk(struct tracecmd_input *handle, int fd)
{
	unsigned int size;

	if (handle->file_state != TRACECMD_FILE_PRINTK - 1)
		return -1;

	if (read_copy_size4(handle, fd, &size) < 0)
		return -1;
	if (!size)
		return 0; /* OK? */

	if (read_copy_data(handle, size, fd) < 0)
		return -1;

	handle->file_state = TRACECMD_FILE_PRINTK;

	return 0;
}

static int copy_command_lines(struct tracecmd_input *handle, int fd)
{
	unsigned long long size;

	if (handle->file_state != TRACECMD_FILE_CMD_LINES - 1)
		return -1;

	if (read_copy_size8(handle, fd, &size) < 0)
		return -1;
	if (!size)
		return 0; /* OK? */

	if (read_copy_data(handle, size, fd) < 0)
		return -1;

	handle->file_state = TRACECMD_FILE_CMD_LINES;

	return 0;
}

/**
 * tracecmd_copy_headers - Copy headers from a tracecmd_input handle to a file descriptor
 * @handle: input handle for the trace.dat file to copy from.
 * @fd: The file descriptor to copy to.
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
int tracecmd_copy_headers(struct tracecmd_input *handle, int fd,
			  enum tracecmd_file_states start_state,
			  enum tracecmd_file_states end_state)
{
	int ret;

	if (!start_state)
		start_state = TRACECMD_FILE_HEADERS;
	if (!end_state)
		end_state = TRACECMD_FILE_CMD_LINES;

	if (start_state > end_state)
		return -1;

	if (end_state < TRACECMD_FILE_HEADERS)
		return 0;

	if (handle->file_state >= start_state) {
		/* Set the handle to just before the start state */
		lseek64(handle->fd, handle->header_files_start, SEEK_SET);
		/* Now that the file handle has moved, change its state */
		handle->file_state = TRACECMD_FILE_INIT;
	}

	/* Try to bring the input up to the start state - 1 */
	ret = tracecmd_read_headers(handle, start_state - 1);
	if (ret < 0)
		goto out;

	switch (start_state) {
	case TRACECMD_FILE_HEADERS:
		ret = copy_header_files(handle, fd);
		if (ret < 0)
			goto out;

		/* fallthrough */
	case TRACECMD_FILE_FTRACE_EVENTS:
		/* handle's state is now updating with the copies */
		if (end_state <= handle->file_state)
			return 0;

		ret = copy_ftrace_files(handle, fd);
		if (ret < 0)
			goto out;

		/* fallthrough */
	case TRACECMD_FILE_ALL_EVENTS:
		if (end_state <= handle->file_state)
			return 0;

		ret = copy_event_files(handle, fd);
		if (ret < 0)
			goto out;

		/* fallthrough */
	case TRACECMD_FILE_KALLSYMS:
		if (end_state <= handle->file_state)
			return 0;

		ret = copy_proc_kallsyms(handle, fd);
		if (ret < 0)
			goto out;

		/* fallthrough */
	case TRACECMD_FILE_PRINTK:
		if (end_state <= handle->file_state)
			return 0;

		ret = copy_ftrace_printk(handle, fd);
		if (ret < 0)
			goto out;

		/* fallthrough */
	case TRACECMD_FILE_CMD_LINES:
		if (end_state <= handle->file_state)
			return 0;

		ret = copy_command_lines(handle, fd);
	default:
		break;
	}

 out:
	return ret < 0 ? -1 : 0;
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
	new_handle->cpu_data = NULL;
	new_handle->nr_buffers = 0;
	new_handle->buffers = NULL;
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

	/* Save where we currently are */
	offset = lseek64(handle->fd, 0, SEEK_CUR);

	ret = lseek64(handle->fd, buffer->offset, SEEK_SET);
	if (ret < 0) {
		tracecmd_warning("could not seek to buffer %s offset %ld\n",
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
		tracecmd_warning("failed to read sub buffer %s\n", buffer->name);
		goto error;
	}

	ret = lseek64(handle->fd, offset, SEEK_SET);
	if (ret < 0) {
		tracecmd_warning("could not seek to back to offset %ld\n", offset);
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
	return handle->cpus;
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
 * tracecmd_get_cpustats - return the saved cpustats
 * @handle: input handle for the trace.dat file
 *
 * Provides a method to extract the cpustats saved in @handle.
 *
 * Returns a string of the cpustats that was saved in the trace.dat file.
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
