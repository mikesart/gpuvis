// SPDX-License-Identifier: LGPL-2.1
/*
 * Copyright (C) 2009, 2010 Red Hat Inc, Steven Rostedt <srostedt@redhat.com>
 *
 */
#define _LARGEFILE64_SOURCE
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <glob.h>
#include <sys/uio.h>

#include "tracefs.h"
#include "trace-cmd.h"
#include "trace-cmd-local.h"
#include "trace-write-local.h"
#include "list.h"
#include "trace-msg.h"

/* We can't depend on the host size for size_t, all must be 64 bit */
typedef unsigned long long	tsize_t;
typedef long long		stsize_t;

struct tracecmd_option {
	unsigned short	id;
	int		size;
	void		*data;
	tsize_t		offset;
	struct list_head list;
};

struct tracecmd_buffer {
	int				cpus;
	void				*name;
	tsize_t				offset;
	struct tracecmd_option		*option;
	struct list_head		list;
};

enum {
	OUTPUT_FL_SEND_META	= (1 << 0),
};

struct tracecmd_output {
	int			fd;
	int			page_size;
	int			cpus;
	struct tep_handle	*pevent;
	char			*tracing_dir;
	char			*kallsyms;
	int			nr_options;
	bool			quiet;
	unsigned long		file_state;
	unsigned long		file_version;

	/* size of meta-data strings, not yet stored in the file */
	unsigned long		strings_p;
	/* current virtual offset of meta-data string */
	unsigned long		strings_offs;

	unsigned long long	options_start;
	unsigned long long	options_next;
	bool			big_endian;
	bool			do_compress;
	struct tracecmd_compression *compress;

	struct list_head	options;
	struct list_head	buffers;
	struct tracecmd_msg_handle *msg_handle;
	char			*trace_clock;

	/* meta-data strings, not yet stored in the file */
	char			*strings;
};

struct list_event {
	struct list_event		*next;
	char				*name;
	char				*file;
};

struct list_event_system {
	struct list_event_system	*next;
	struct list_event		*events;
	char				*name;
};

#define HAS_SECTIONS(H) ((H)->file_version >= FILE_VERSION_SECTIONS)

static int write_options(struct tracecmd_output *handle);
static int save_string_section(struct tracecmd_output *handle, bool compress);

__hidden long long
do_write_check(struct tracecmd_output *handle, const void *data, long long size)
{
	if (handle->do_compress)
		return tracecmd_compress_buffer_write(handle->compress, data, size);

	if (handle->msg_handle)
		return tracecmd_msg_data_send(handle->msg_handle, data, size);

	return __do_write_check(handle->fd, data, size);
}

static inline off64_t do_lseek(struct tracecmd_output *handle, off_t offset, int whence)
{
	if (handle->do_compress)
		return tracecmd_compress_lseek(handle->compress, offset, whence);

	if (handle->msg_handle)
		return msg_lseek(handle->msg_handle, offset, whence);

	return lseek64(handle->fd, offset, whence);
}

static inline int do_preed(struct tracecmd_output *handle, void *dst, int len, off_t offset)
{
	if (handle->do_compress)
		return tracecmd_compress_pread(handle->compress, dst, len, offset);

	return pread(handle->fd, dst, len, offset);
}

static short convert_endian_2(struct tracecmd_output *handle, short val)
{
	if (!handle->pevent)
		return val;

	return tep_read_number(handle->pevent, &val, 2);
}

static int convert_endian_4(struct tracecmd_output *handle, int val)
{
	if (!handle->pevent)
		return val;

	return tep_read_number(handle->pevent, &val, 4);
}

static unsigned long long convert_endian_8(struct tracecmd_output *handle,
					   unsigned long long val)
{
	if (!handle->pevent)
		return val;

	return tep_read_number(handle->pevent, &val, 8);
}

__hidden void out_compression_reset(struct tracecmd_output *handle, bool compress)
{
	if (!compress || !handle->compress)
		return;

	tracecmd_compress_reset(handle->compress);
	handle->do_compress = false;
}

__hidden int out_uncompress_block(struct tracecmd_output *handle)
{
	int ret = 0;

	if (!handle->compress)
		return 0;

	ret = tracecmd_uncompress_block(handle->compress);
	if (!ret)
		handle->do_compress = true;

	return ret;
}

__hidden int out_compression_start(struct tracecmd_output *handle, bool compress)
{
	if (!compress || !handle->compress)
		return 0;

	tracecmd_compress_reset(handle->compress);
	handle->do_compress = true;

	return 0;
}

__hidden int out_compression_end(struct tracecmd_output *handle, bool compress)
{
	if (!compress || !handle->compress)
		return 0;

	handle->do_compress = false;
	return tracecmd_compress_block(handle->compress);
}

static long add_string(struct tracecmd_output *handle, const char *string)
{
	int size = strlen(string) + 1;
	int pos = handle->strings_p;
	char *strings;

	strings = realloc(handle->strings, pos + size);
	if (!strings)
		return -1;
	handle->strings = strings;
	memcpy(handle->strings + pos, string, size);
	handle->strings_p += size;

	return handle->strings_offs + pos;
}

/**
 * tracecmd_set_quiet - Set if to print output to the screen
 * @quiet: If non zero, print no output to the screen
 *
 */
void tracecmd_set_quiet(struct tracecmd_output *handle, bool set_quiet)
{
	if (handle)
		handle->quiet = set_quiet;
}

void tracecmd_set_out_clock(struct tracecmd_output *handle, const char *clock)
{
	if (handle && clock) {
		free(handle->trace_clock);
		handle->trace_clock = strdup(clock);
	}
}

/**
 * tracecmd_get_quiet - Get if to print output to the screen
 * Returns non zero, if no output to the screen should be printed
 *
 */
bool tracecmd_get_quiet(struct tracecmd_output *handle)
{
	if (handle)
		return handle->quiet;
	return false;
}

void tracecmd_output_free(struct tracecmd_output *handle)
{
	struct tracecmd_option *option;
	struct tracecmd_buffer *buffer;

	if (!handle)
		return;

	if (handle->tracing_dir)
		free(handle->tracing_dir);

	if (handle->pevent)
		tep_unref(handle->pevent);

	while (!list_empty(&handle->buffers)) {
		buffer = container_of(handle->buffers.next,
				      struct tracecmd_buffer, list);
		list_del(&buffer->list);
		free(buffer->name);
		free(buffer);
	}
	while (!list_empty(&handle->options)) {
		option = container_of(handle->options.next,
				      struct tracecmd_option, list);
		list_del(&option->list);
		free(option->data);
		free(option);
	}

	free(handle->strings);
	free(handle->trace_clock);
	tracecmd_compress_destroy(handle->compress);
	free(handle);
}

void tracecmd_output_close(struct tracecmd_output *handle)
{
	if (!handle)
		return;

	if (HAS_SECTIONS(handle)) {
		/* write any unsaved options at the end of trace files with sections */
		write_options(handle);

		/* write strings section */
		save_string_section(handle, true);
	}

	if (handle->fd >= 0) {
		close(handle->fd);
		handle->fd = -1;
	}

	tracecmd_output_free(handle);
}
static unsigned long get_size_fd(int fd)
{
	unsigned long long size = 0;
	char buf[BUFSIZ];
	int r;

	do {
		r = read(fd, buf, BUFSIZ);
		if (r > 0)
			size += r;
	} while (r > 0);

	lseek(fd, 0, SEEK_SET);

	return size;
}

static unsigned long get_size(const char *file)
{
	unsigned long long size = 0;
	int fd;

	fd = open(file, O_RDONLY);
	if (fd < 0) {
		tracecmd_warning("Can't read '%s'", file);
		return 0; /* Caller will fail with zero */
	}
	size = get_size_fd(fd);
	close(fd);

	return size;
}

static tsize_t copy_file_fd(struct tracecmd_output *handle, int fd, unsigned long long max)
{
	tsize_t rsize = BUFSIZ;
	tsize_t size = 0;
	char buf[BUFSIZ];
	stsize_t r;

	do {
		if (max && rsize > max)
			rsize = max;

		r = read(fd, buf, rsize);
		if (r > 0) {
			size += r;
			if (do_write_check(handle, buf, r))
				return 0;
			if (max) {
				max -= r;
				if (!max)
					break;
			}
		}
	} while (r > 0);

	return size;
}

static tsize_t copy_file(struct tracecmd_output *handle,
				    const char *file)
{
	tsize_t size = 0;
	int fd;

	fd = open(file, O_RDONLY);
	if (fd < 0) {
		tracecmd_warning("Can't read '%s'", file);
		return 0;
	}
	size = copy_file_fd(handle, fd, 0);
	close(fd);

	return size;
}

#define PAGES_IN_CHUNK 10
__hidden unsigned long long out_copy_fd_compress(struct tracecmd_output *handle,
						 int fd, unsigned long long max,
						 unsigned long long *write_size,
						 int page)
{
	unsigned long long rsize = 0;
	unsigned long long wsize = 0;
	unsigned long long size;
	int ret;

	if (handle->compress) {
		rsize = max;
		ret = tracecmd_compress_copy_from(handle->compress, fd,
						  PAGES_IN_CHUNK * page,
						  &rsize, &wsize);
		if (ret < 0)
			return 0;

		size = rsize;
		if (write_size)
			*write_size = wsize;
	} else {
		size = copy_file_fd(handle, fd, max);
		if (write_size)
			*write_size = size;
	}

	return size;
}

static tsize_t copy_file_compress(struct tracecmd_output *handle,
				  const char *file, unsigned long long *write_size)
{
	int ret;
	int fd;

	fd = open(file, O_RDONLY);
	if (fd < 0) {
		tracecmd_warning("Can't read '%s'", file);
		return 0;
	}

	ret = out_copy_fd_compress(handle, fd, 0, write_size, getpagesize());
	if (!ret)
		tracecmd_warning("Can't compress '%s'", file);

	close(fd);
	return ret;
}

/*
 * Finds the path to the debugfs/tracing
 * Allocates the string and stores it.
 */
static const char *find_tracing_dir(struct tracecmd_output *handle)
{
	if (!handle->tracing_dir) {
		const char *dir = tracefs_tracing_dir();

		if (dir)
			handle->tracing_dir = strdup(dir);
	}
	return handle->tracing_dir;
}

static char *get_tracing_file(struct tracecmd_output *handle, const char *name)
{
	const char *tracing;
	char *file;
	int ret;

	tracing = find_tracing_dir(handle);
	if (!tracing)
		return NULL;

	ret = asprintf(&file, "%s/%s", tracing, name);
	if (ret < 0)
		return NULL;

	return file;
}

static void put_tracing_file(char *file)
{
	free(file);
}

int tracecmd_ftrace_enable(int set)
{
	struct stat buf;
	char *path = "/proc/sys/kernel/ftrace_enabled";
	int fd;
	char *val = set ? "1" : "0";
	int ret = 0;

	/* if ftace_enable does not exist, simply ignore it */
	fd = stat(path, &buf);
	if (fd < 0)
		return ENODEV;

	fd = open(path, O_WRONLY);
	if (fd < 0) {
		tracecmd_warning("Can't %s ftrace", set ? "enable" : "disable");
		return EIO;
	}

	if (write(fd, val, 1) < 0)
		ret = -1;
	close(fd);

	return ret;
}

__hidden unsigned long long
out_write_section_header(struct tracecmd_output *handle, unsigned short header_id,
			 char *description, int flags, bool option)
{
	tsize_t endian8;
	tsize_t offset;
	long long size;
	short endian2;
	int endian4;
	int desc;

	if (header_id >= TRACECMD_OPTION_MAX)
		return -1;
	if (!HAS_SECTIONS(handle))
		return 0;
	if (!handle->compress)
		flags &= ~TRACECMD_SEC_FL_COMPRESS;
	offset = do_lseek(handle, 0, SEEK_CUR);
	if (option) {
		endian8 = convert_endian_8(handle, offset);
		if (!tracecmd_add_option(handle, header_id, 8, &endian8))
			return -1;
	}
	/* Section ID */
	endian2 = convert_endian_2(handle, header_id);
	if (do_write_check(handle, &endian2, 2))
		return (off64_t)-1;

	/* Section flags */
	endian2 = convert_endian_2(handle, flags);
	if (do_write_check(handle, &endian2, 2))
		return (off64_t)-1;

	/* Section description */
	if (description)
		desc = add_string(handle, description);
	else
		desc = -1;
	endian4 = convert_endian_4(handle, desc);
	if (do_write_check(handle, &endian4, 4))
		return (off64_t)-1;

	offset = do_lseek(handle, 0, SEEK_CUR);
	size = 0;
	/* Reserve for section size */
	if (do_write_check(handle, &size, 8))
		return (off64_t)-1;
	return offset;
}

__hidden int out_update_section_header(struct tracecmd_output *handle, tsize_t offset)
{
	tsize_t current;
	tsize_t endian8;
	tsize_t size;

	if (!HAS_SECTIONS(handle) || offset == 0)
		return 0;

	current = do_lseek(handle, 0, SEEK_CUR);
	/* The real size is the difference between the saved offset and
	 * the current offset - 8 bytes, the reserved space for the section size.
	 */
	size = current - offset;
	if (size < 8)
		return -1;
	size -= 8;
	if (do_lseek(handle, offset, SEEK_SET) == (off64_t)-1)
		return -1;

	endian8 = convert_endian_8(handle, size);
	if (do_write_check(handle, &endian8, 8))
		return -1;
	if (do_lseek(handle, current, SEEK_SET) == (off64_t)-1)
		return -1;
	return 0;
}

static int save_string_section(struct tracecmd_output *handle, bool compress)
{
	enum tracecmd_section_flags flags = 0;
	tsize_t offset;

	if (!handle->strings || !handle->strings_p)
		return 0;

	if (!check_out_state(handle, TRACECMD_OPTION_STRINGS)) {
		tracecmd_warning("Cannot write strings, unexpected state 0x%X",
				 handle->file_state);
		return -1;
	}

	if (compress)
		flags |= TRACECMD_SEC_FL_COMPRESS;
	offset = out_write_section_header(handle, TRACECMD_OPTION_STRINGS, "strings", flags, false);
	if (offset == (off64_t)-1)
		return -1;

	out_compression_start(handle, compress);

	if (do_write_check(handle, handle->strings, handle->strings_p))
		goto error;

	if (out_compression_end(handle, compress))
		goto error;

	if (out_update_section_header(handle, offset))
		return -1;

	handle->strings_offs += handle->strings_p;
	free(handle->strings);
	handle->strings = NULL;
	handle->strings_p = 0;
	handle->file_state = TRACECMD_OPTION_STRINGS;
	return 0;

error:
	out_compression_reset(handle, compress);
	return -1;
}

static int read_header_files(struct tracecmd_output *handle, bool compress)
{
	enum tracecmd_section_flags flags = 0;
	tsize_t size, check_size, endian8;
	struct stat st;
	tsize_t offset;
	char *path;
	int fd = -1;
	int ret;

	if (!check_out_state(handle, TRACECMD_FILE_HEADERS)) {
		tracecmd_warning("Cannot read header files, unexpected state 0x%X",
				 handle->file_state);
		return -1;
	}

	path = get_tracing_file(handle, "events/header_page");
	if (!path)
		return -1;

	if (compress)
		flags |= TRACECMD_SEC_FL_COMPRESS;
	offset = out_write_section_header(handle, TRACECMD_OPTION_HEADER_INFO,
					  "headers", flags, true);
	if (offset == (off64_t)-1)
		return -1;

	out_compression_start(handle, compress);
	ret = stat(path, &st);
	if (ret < 0) {
		/* old style did not show this info, just add zero */
		put_tracing_file(path);
		if (do_write_check(handle, "header_page", 12))
			goto out_close;
		size = 0;
		if (do_write_check(handle, &size, 8))
			goto out_close;
		if (do_write_check(handle, "header_event", 13))
			goto out_close;
		if (do_write_check(handle, &size, 8))
			goto out_close;
		if (out_compression_end(handle, compress))
			goto out_close;
		if (out_update_section_header(handle, offset))
			goto out_close;
		return 0;
	}

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		tracecmd_warning("can't read '%s'", path);
		goto out_close;
	}

	/* unfortunately, you can not stat debugfs files for size */
	size = get_size_fd(fd);

	if (do_write_check(handle, "header_page", 12))
		goto out_close;
	endian8 = convert_endian_8(handle, size);
	if (do_write_check(handle, &endian8, 8))
		goto out_close;
	check_size = copy_file_fd(handle, fd, 0);
	close(fd);
	if (size != check_size) {
		tracecmd_warning("wrong size for '%s' size=%lld read=%lld", path, size, check_size);
		errno = EINVAL;
		goto out_close;
	}
	put_tracing_file(path);

	path = get_tracing_file(handle, "events/header_event");
	if (!path)
		goto out_close;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		tracecmd_warning("can't read '%s'", path);
		goto out_close;
	}

	size = get_size_fd(fd);

	if (do_write_check(handle, "header_event", 13))
		goto out_close;
	endian8 = convert_endian_8(handle, size);
	if (do_write_check(handle, &endian8, 8))
		goto out_close;
	check_size = copy_file_fd(handle, fd, 0);
	close(fd);
	if (size != check_size) {
		tracecmd_warning("wrong size for '%s'", path);
		goto out_close;
	}
	put_tracing_file(path);
	if (out_compression_end(handle, compress))
		goto out_close;

	if (out_update_section_header(handle, offset))
		goto out_close;
	handle->file_state = TRACECMD_FILE_HEADERS;

	return 0;

 out_close:
	out_compression_reset(handle, compress);
	if (fd >= 0)
		close(fd);
	return -1;
}

static int copy_event_system(struct tracecmd_output *handle,
			     struct list_event_system *slist)
{
	struct list_event *elist;
	unsigned long long size, check_size, endian8;
	struct stat st;
	char *format;
	int endian4;
	int count = 0;
	int ret;

	for (elist = slist->events; elist; elist = elist->next)
		count++;

	endian4 = convert_endian_4(handle, count);
	if (do_write_check(handle, &endian4, 4))
		return -1;

	for (elist = slist->events; elist; elist = elist->next) {
		format = elist->file;
		ret = stat(format, &st);

		if (ret >= 0) {
			/* unfortunately, you can not stat debugfs files for size */
			size = get_size(format);
			endian8 = convert_endian_8(handle, size);
			if (do_write_check(handle, &endian8, 8))
				return -1;
			check_size = copy_file(handle, format);
			if (size != check_size) {
				tracecmd_warning("error in size of file '%s'", format);
				return -1;
			}
		}
	}

	return 0;
}

static void add_list_event_system(struct list_event_system **systems,
				  const char *system,
				  const char *event,
				  const char *path)
{
	struct list_event_system *slist;
	struct list_event *elist;

	for (slist = *systems; slist; slist = slist->next)
		if (strcmp(slist->name, system) == 0)
			break;

	if (!slist) {
		slist = malloc(sizeof(*slist));
		if (!slist)
			goto err_mem;
		slist->name = strdup(system);
		if (!slist->name) {
			free(slist);
			goto err_mem;
		}
		slist->next = *systems;
		slist->events = NULL;
		*systems = slist;
	}

	for (elist = slist->events; elist; elist = elist->next)
		if (strcmp(elist->name, event) == 0)
			break;

	if (!elist) {
		elist = malloc(sizeof(*elist));
		if (!elist)
			goto err_mem;
		elist->name = strdup(event);
		elist->file = strdup(path);
		if (!elist->name || !elist->file) {
			free(elist->name);
			free(elist->file);
			free(elist);
			goto err_mem;
		}
		elist->next = slist->events;
		slist->events = elist;
	}
	return;
 err_mem:
	 tracecmd_warning("Insufficient memory");
}

static void free_list_events(struct list_event_system *list)
{
	struct list_event_system *slist;
	struct list_event *elist;

	while (list) {
		slist = list;
		list = list->next;
		while (slist->events) {
			elist = slist->events;
			slist->events = elist->next;
			free(elist->name);
			free(elist->file);
			free(elist);
		}
		free(slist->name);
		free(slist);
	}
}

static void glob_events(struct tracecmd_output *handle,
			struct list_event_system **systems,
			const char *str)
{
	glob_t globbuf;
	char *events_path;
	char *system;
	char *event;
	char *path;
	char *file;
	char *ptr;
	int do_ftrace = 0;
	int events_len;
	int ret;
	int i;

	if (strncmp(str, "ftrace/", 7) == 0)
		do_ftrace = 1;

	events_path = get_tracing_file(handle, "events");
	events_len = strlen(events_path);

	path = malloc(events_len + strlen(str) +
		      strlen("/format") + 2);
	if (!path)
		return;
	path[0] = '\0';
	strcat(path, events_path);
	strcat(path, "/");
	strcat(path, str);
	strcat(path, "/format");
	put_tracing_file(events_path);

	globbuf.gl_offs = 0;
	ret = glob(path, 0, NULL, &globbuf);
	free(path);
	if (ret < 0)
		return;

	for (i = 0; i < globbuf.gl_pathc; i++) {
		file = globbuf.gl_pathv[i];
		system = strdup(file + events_len + 1);
		system = strtok_r(system, "/", &ptr);
		if (!ptr) {
			/* ?? should we warn? */
			free(system);
			continue;
		}

		if (!do_ftrace && strcmp(system, "ftrace") == 0) {
			free(system);
			continue;
		}

		event = strtok_r(NULL, "/", &ptr);
		if (!ptr) {
			/* ?? should we warn? */
			free(system);
			continue;
		}

		add_list_event_system(systems, system, event, file);
		free(system);
	}
	globfree(&globbuf);
}

static void
create_event_list_item(struct tracecmd_output *handle,
		       struct list_event_system **systems,
		       struct tracecmd_event_list *list)
{
	char *ptr;
	char *str;

	str = strdup(list->glob);
	if (!str)
		goto err_mem;

	/* system and event names are separated by a ':' */
	ptr = strchr(str, ':');
	if (ptr)
		*ptr = '/';
	else
		/* system and event may also be separated by a '/' */
		ptr = strchr(str, '/');

	if (ptr) {
		glob_events(handle, systems, str);
		free(str);
		return;
	}

	ptr = str;
	str = malloc(strlen(ptr) + 3);
	if (!str)
		goto err_mem;
	str[0] = '\0';
	strcat(str, ptr);
	strcat(str, "/*");
	glob_events(handle, systems, str);

	str[0] = '\0';
	strcat(str, "*/");
	strcat(str, ptr);
	glob_events(handle, systems, str);

	free(ptr);
	free(str);
	return;
 err_mem:
	 tracecmd_warning("Insufficient memory");
}

static int read_ftrace_files(struct tracecmd_output *handle, bool compress)
{
	enum tracecmd_section_flags flags = 0;
	struct list_event_system *systems = NULL;
	struct tracecmd_event_list list = { .glob = "ftrace/*" };
	tsize_t offset;
	int ret;

	if (!check_out_state(handle, TRACECMD_FILE_FTRACE_EVENTS)) {
		tracecmd_warning("Cannot read ftrace files, unexpected state 0x%X",
				 handle->file_state);
		return -1;
	}

	if (compress)
		flags |= TRACECMD_SEC_FL_COMPRESS;
	offset = out_write_section_header(handle, TRACECMD_OPTION_FTRACE_EVENTS,
					  "ftrace events", flags, true);
	if (offset == (off64_t)-1)
		return -1;

	create_event_list_item(handle, &systems, &list);
	out_compression_start(handle, compress);

	ret = copy_event_system(handle, systems);
	if (!ret)
		ret = out_compression_end(handle, compress);
	else
		out_compression_reset(handle, compress);

	free_list_events(systems);
	if (ret)
		return ret;
	if (out_update_section_header(handle, offset))
		return -1;

	handle->file_state = TRACECMD_FILE_FTRACE_EVENTS;

	return ret;
}

static struct list_event_system *
create_event_list(struct tracecmd_output *handle,
		  struct tracecmd_event_list *event_list)
{
	struct list_event_system *systems = NULL;
	struct tracecmd_event_list *list;

	for (list = event_list; list; list = list->next)
		create_event_list_item(handle, &systems, list);

	return systems;
}

static int read_event_files(struct tracecmd_output *handle,
			    struct tracecmd_event_list *event_list, bool compress)
{
	enum tracecmd_section_flags flags = 0;
	struct list_event_system *systems;
	struct list_event_system *slist;
	struct tracecmd_event_list *list;
	struct tracecmd_event_list all_events = { .glob = "*/*" };
	int count = 0;
	tsize_t offset;
	int endian4;
	int ret;

	if (!check_out_state(handle, TRACECMD_FILE_ALL_EVENTS)) {
		tracecmd_warning("Cannot read event files, unexpected state 0x%X",
				 handle->file_state);
		return -1;
	}

	if (compress)
		flags |= TRACECMD_SEC_FL_COMPRESS;
	offset = out_write_section_header(handle, TRACECMD_OPTION_EVENT_FORMATS,
					  "events format", flags, true);
	if (offset == (off64_t)-1)
		return -1;
	/*
	 * If any of the list is the special keyword "all" then
	 * just do all files.
	 */
	for (list = event_list; list; list = list->next) {
		if (strcmp(list->glob, "all") == 0)
			break;
	}
	/* all events are listed, use a global glob */
	if (!event_list || list)
		event_list = &all_events;

	systems = create_event_list(handle, event_list);

	for (slist = systems; slist; slist = slist->next)
		count++;
	out_compression_start(handle, compress);
	ret = -1;
	endian4 = convert_endian_4(handle, count);
	if (do_write_check(handle, &endian4, 4))
		goto out_free;

	ret = 0;
	for (slist = systems; !ret && slist; slist = slist->next) {
		if (do_write_check(handle, slist->name,
				   strlen(slist->name) + 1)) {
			ret = -1;
			continue;
		}
		ret = copy_event_system(handle, slist);
	}
	if (ret)
		goto out_free;

	ret = out_compression_end(handle, compress);
	if (ret)
		goto out_free;
	ret = out_update_section_header(handle, offset);

 out_free:
	if (!ret)
		handle->file_state = TRACECMD_FILE_ALL_EVENTS;
	else
		out_compression_reset(handle, compress);

	free_list_events(systems);

	return ret;
}

#define KPTR_UNINITIALIZED 'X'

static void set_proc_kptr_restrict(int reset)
{
	char *path = "/proc/sys/kernel/kptr_restrict";
	static char saved = KPTR_UNINITIALIZED;
	int fd, ret = -1;
	struct stat st;
	char buf;

	if ((reset && saved == KPTR_UNINITIALIZED) ||
	    (stat(path, &st) < 0))
		return;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		goto err;

	if (reset) {
		buf = saved;
	} else {
		if (read(fd, &buf, 1) < 0)
			goto err;
		saved = buf;
		buf = '0';
	}
	close(fd);

	fd = open(path, O_WRONLY);
	if (fd < 0)
		goto err;
	if (write(fd, &buf, 1) > 0)
		ret = 0;
err:
	if (fd > 0)
		close(fd);
	if (ret)
		tracecmd_warning("can't set kptr_restrict");
}

static int read_proc_kallsyms(struct tracecmd_output *handle, bool compress)
{
	enum tracecmd_section_flags flags = 0;
	unsigned int size, check_size, endian4;
	const char *path = "/proc/kallsyms";
	tsize_t offset;
	struct stat st;
	int ret;

	if (!check_out_state(handle, TRACECMD_FILE_KALLSYMS)) {
		tracecmd_warning("Cannot read kallsyms, unexpected state 0x%X",
				 handle->file_state);
		return -1;
	}

	if (handle->kallsyms)
		path = handle->kallsyms;

	if (compress)
		flags |= TRACECMD_SEC_FL_COMPRESS;
	offset = out_write_section_header(handle, TRACECMD_OPTION_KALLSYMS,
					  "kallsyms", flags, true);
	if (offset == (off64_t)-1)
		return -1;

	out_compression_start(handle, compress);
	ret = stat(path, &st);
	if (ret < 0) {
		/* not found */
		size = 0;
		endian4 = convert_endian_4(handle, size);
		ret = do_write_check(handle, &endian4, 4);
		goto out;
	}
	size = get_size(path);
	endian4 = convert_endian_4(handle, size);
	ret = do_write_check(handle, &endian4, 4);
	if (ret)
		goto out;

	set_proc_kptr_restrict(0);
	check_size = copy_file(handle, path);
	if (size != check_size) {
		errno = EINVAL;
		tracecmd_warning("error in size of file '%s'", path);
		set_proc_kptr_restrict(1);
		ret = -1;
		goto out;
	}
	set_proc_kptr_restrict(1);

	ret = out_compression_end(handle, compress);
	if (ret)
		goto out;

	ret = out_update_section_header(handle, offset);
out:
	if (!ret)
		handle->file_state = TRACECMD_FILE_KALLSYMS;
	else
		out_compression_reset(handle, compress);
	return ret;
}

static int read_ftrace_printk(struct tracecmd_output *handle, bool compress)
{
	enum tracecmd_section_flags flags = 0;
	unsigned int size, check_size, endian4;
	tsize_t offset;
	struct stat st;
	char *path;
	int ret;

	if (!check_out_state(handle, TRACECMD_FILE_PRINTK)) {
		tracecmd_warning("Cannot read printk, unexpected state 0x%X",
				 handle->file_state);
		return -1;
	}

	path = get_tracing_file(handle, "printk_formats");
	if (!path)
		return -1;

	if (compress)
		flags |= TRACECMD_SEC_FL_COMPRESS;
	offset = out_write_section_header(handle, TRACECMD_OPTION_PRINTK, "printk", flags, true);
	if (offset == (off64_t)-1)
		return -1;

	out_compression_start(handle, compress);
	ret = stat(path, &st);
	if (ret < 0) {
		/* not found */
		size = 0;
		endian4 = convert_endian_4(handle, size);
		if (do_write_check(handle, &endian4, 4))
			goto fail;
		goto out;
	}
	size = get_size(path);
	endian4 = convert_endian_4(handle, size);
	if (do_write_check(handle, &endian4, 4))
		goto fail;
	check_size = copy_file(handle, path);
	if (size != check_size) {
		errno = EINVAL;
		tracecmd_warning("error in size of file '%s'", path);
		goto fail;
	}

 out:
	put_tracing_file(path);
	if (out_compression_end(handle, compress))
		return -1;

	if (out_update_section_header(handle, offset))
		return -1;
	handle->file_state = TRACECMD_FILE_PRINTK;
	return 0;
 fail:
	put_tracing_file(path);
	out_compression_reset(handle, compress);
	return -1;
}

static int save_tracing_file_data(struct tracecmd_output *handle,
				  const char *filename)
{
	unsigned long long endian8;
	char *file = NULL;
	struct stat st;
	off64_t check_size;
	off64_t size;
	int ret = -1;

	file = get_tracing_file(handle, filename);
	if (!file)
		return -1;

	ret = stat(file, &st);
	if (ret >= 0) {
		size = get_size(file);
		endian8 = convert_endian_8(handle, size);
		if (do_write_check(handle, &endian8, 8))
			goto out_free;
		check_size = copy_file(handle, file);
		if (size != check_size) {
			errno = EINVAL;
			tracecmd_warning("error in size of file '%s'", file);
			goto out_free;
		}
	} else {
		size = 0;
		endian8 = convert_endian_8(handle, size);
		if (do_write_check(handle, &endian8, 8))
			goto out_free;
	}
	ret = 0;

out_free:
	put_tracing_file(file);
	return ret;
}

static int write_compression_header(struct tracecmd_output *handle)
{
	const char *name = NULL;
	const char *ver = NULL;
	int ret;

	ret = tracecmd_compress_proto_get_name(handle->compress, &name, &ver);
	if (ret < 0 || !name || !ver) {
		name = "none";
		ver = "";
	}

	if (do_write_check(handle, name, strlen(name) + 1))
		return -1;

	if (do_write_check(handle, ver, strlen(ver) + 1))
		return -1;

	return 0;
}

static int get_trace_page_size(struct tracecmd_output *handle, const char *name)
{
	struct tracefs_instance *instance;
	struct tep_handle *tep = NULL;
	int psize, size;
	char *buff = NULL;

	/* In case of an error, return user space page size */
	psize = getpagesize();

	instance = tracefs_instance_alloc(find_tracing_dir(handle), name);
	if (!instance)
		goto out;

	buff = tracefs_instance_file_read(instance, "events/header_page", &size);
	if (!buff)
		goto out;

	tep = tep_alloc();
	if (!tep)
		goto out;

	if (tep_parse_header_page(tep, buff, size, sizeof(long long)))
		goto out;

	psize = tep_get_sub_buffer_size(tep);

out:
	tracefs_instance_free(instance);
	tep_free(tep);
	free(buff);

	return psize;
}

/**
 * tracecmd_output_create_fd - allocate new output handle to a trace file
 * @fd: File descriptor for the handle to write to.
 *
 * Allocate a tracecmd_output descriptor and perform minimal initialization.
 * @fd will be set as the file descriptor for the handle. Nothing is
 * written in the file yet, and if @fd is -1, then all writes will be ignored.
 *
 * Returns a pointer to a newly allocated file descriptor for the use of creating
 * a tracecmd data file. In case of an error, NULL is returned. The returned
 * handle must be freed with tracecmd_output_close() or tracecmd_output_free()
 */
struct tracecmd_output *tracecmd_output_create_fd(int fd)
{
	struct tracecmd_output *handle;

	handle = calloc(1, sizeof(*handle));
	if (!handle)
		return NULL;

	handle->fd = fd;

	handle->file_version = FILE_VERSION_DEFAULT;

	handle->page_size = get_trace_page_size(handle, NULL);
	handle->big_endian = tracecmd_host_bigendian();

	list_head_init(&handle->options);
	list_head_init(&handle->buffers);

	handle->file_state = TRACECMD_FILE_ALLOCATED;

	return handle;
}

/**
 * tracecmd_output_set_msg - associated an output file handle with network message handle
 * @handle: output handle to a trace file.
 * @msg_handle: network handle, allocated by tracecmd_msg_handle_alloc()
 *
 * Associate an output file handle (@handle) to a network stream (@msg_handle).
 * All subsequent calls to @handle will send data over the network using @msg_handle
 * instead of writing to a file.
 *
 * This must be called after the handle file version is set and before calling
 * tracecmd_output_write_headers().
 *
 * Returns 0 on success, or -1 if the output file handle is not allocated or not
 * in the expected state.
 */
int tracecmd_output_set_msg(struct tracecmd_output *handle, struct tracecmd_msg_handle *msg_handle)
{
	if (!handle || handle->file_state != TRACECMD_FILE_ALLOCATED)
		return -1;

	handle->msg_handle = msg_handle;
	/* Force messages to be cached in a temp file before sending through the socket */
	if (handle->msg_handle && HAS_SECTIONS(handle))
		tracecmd_msg_handle_cache(handle->msg_handle);

	return 0;
}

/**
 * tracecmd_output_set_trace_dir - Set a custom tracing dir, instead of system default
 * @handle: output handle to a trace file.
 * @tracing_dir: full path to a directory with tracing files
 *
 * Associate the output file handle (@handle) with a custom tracing directory
 * (@tracing_dir), to be used when creating the trace file instead of using the
 * system default tracig directory.
 *
 * Must be called before tracecmd_output_write_headers().
 *
 * Returns 0 on success, or -1 if the output file handle is not allocated or not
 * in the expected state.
 */
int tracecmd_output_set_trace_dir(struct tracecmd_output *handle, const char *tracing_dir)
{
	if (!handle || handle->file_state != TRACECMD_FILE_ALLOCATED)
		return -1;

	free(handle->tracing_dir);
	if (tracing_dir) {
		handle->tracing_dir = strdup(tracing_dir);
		if (!handle->tracing_dir)
			return -1;
	} else
		handle->tracing_dir = NULL;

	return 0;
}

/**
 * tracecmd_output_set_kallsyms - Set a custom kernel symbols file
 * @handle: output handle to a trace file.
 * @tracing_dir: full path to a file with kernel symbols
 *
 * Have the output file handle (@handle) use a custom kernel symbols file instead
 * of the default /proc/kallsyms.
 *
 * Must be called before tracecmd_output_write_headers().
 *
 * Returns 0 on success, or -1 if the output file handle is not allocated or
 * not in the expected state.
 */
int tracecmd_output_set_kallsyms(struct tracecmd_output *handle, const char *kallsyms)
{
	if (!handle || handle->file_state != TRACECMD_FILE_ALLOCATED)
		return -1;

	free(handle->kallsyms);
	if (kallsyms) {
		handle->kallsyms = strdup(kallsyms);
		if (!handle->kallsyms)
			return -1;
	} else
		handle->kallsyms = NULL;

	return 0;
}

/**
 * tracecmd_output_set_from_input - Inherit parameters from an existing trace file
 * @handle: output handle to a trace file.
 * @ihandle: input handle to an existing trace file.
 *
 * Have the output file handle (@handle) inherit the properties of a given
 * input file handle (@ihandle).
 *
 * The parameters that are copied are:
 *  - tep handle
 *  - page size
 *  - file endian
 *  - file version
 *  - file compression protocol
 *
 * Must be called before tracecmd_output_write_headers().
 *
 * Returns 0 on success, or -1 if the output file handle is not allocated or
 * not in expected state.
 */
int tracecmd_output_set_from_input(struct tracecmd_output *handle, struct tracecmd_input *ihandle)
{
	const char *cname = NULL;
	const char *cver = NULL;

	if (!handle || !ihandle || handle->file_state != TRACECMD_FILE_ALLOCATED)
		return -1;

	/* get endian, page size, file version and compression */
	/* Use the pevent of the ihandle for later writes */
	handle->pevent = tracecmd_get_tep(ihandle);
	tep_ref(handle->pevent);
	handle->page_size = tracecmd_page_size(ihandle);
	handle->file_version = tracecmd_get_in_file_version(ihandle);
	handle->big_endian = tep_is_file_bigendian(handle->pevent);

	if (!tracecmd_get_file_compress_proto(ihandle, &cname, &cver)) {
		handle->compress = tracecmd_compress_alloc(cname, cver, handle->fd,
							    handle->pevent, handle->msg_handle);
		if (!handle->compress)
			return -1;

		if (handle->file_version < FILE_VERSION_COMPRESSION)
			handle->file_version = FILE_VERSION_COMPRESSION;
	}

	return 0;
}

/**
 * tracecmd_output_set_version - Set file version of the output handle
 * @handle: output handle to a trace file.
 * @file_version: desired file version
 *
 * This API must be called before tracecmd_output_write_headers().
 *
 * Returns 0 on success, or -1 if the output file handle is not allocated or not in expected state.
 */
int tracecmd_output_set_version(struct tracecmd_output *handle, int file_version)
{
	if (!handle || handle->file_state != TRACECMD_FILE_ALLOCATED)
		return -1;
	if (file_version < FILE_VERSION_MIN || file_version > FILE_VERSION_MAX)
		return -1;
	handle->file_version = file_version;
	if (handle->file_version < FILE_VERSION_COMPRESSION)
		handle->compress = NULL;
	return 0;
}

/**
 * tracecmd_output_set_compression - Set file compression algorithm of the output handle
 * @handle: output handle to a trace file.
 * @compression: name of the desired compression algorithm. Can be one of:
 *		 - "none" - do not use compression
 *		 - "all" - use the best available compression algorithm
 *		 - or specific name of the desired compression algorithm
 *
 * This API must be called before tracecmd_output_write_headers().
 *
 * Returns 0 on success, or -1 in case of an error:
 *   - the output file handle is not allocated or not in expected state.
 *   - the specified compression algorithm is not available
 */
int tracecmd_output_set_compression(struct tracecmd_output *handle, const char *compression)
{
	if (!handle || handle->file_state != TRACECMD_FILE_ALLOCATED)
		return -1;

	handle->compress = NULL;
	if (compression && strcmp(compression, "none")) {
		if (!strcmp(compression, "any")) {
			handle->compress = tracecmd_compress_alloc(NULL, NULL, handle->fd,
								    handle->pevent,
								    handle->msg_handle);
			if (!handle->compress)
				tracecmd_warning("No compression algorithms are supported");
		} else {
			handle->compress = tracecmd_compress_alloc(compression, NULL, handle->fd,
								    handle->pevent,
								    handle->msg_handle);
			if (!handle->compress) {
				tracecmd_warning("Compression algorithm %s is not supported",
						  compression);
				return -1;
			}
		}
	}
	if (handle->compress && handle->file_version < FILE_VERSION_COMPRESSION) {
		handle->file_version = FILE_VERSION_COMPRESSION;
		if (handle->msg_handle)
			tracecmd_msg_handle_cache(handle->msg_handle);
	}

	return 0;
}

/**
 * output_write_init - Write the initial data into the trace file
 * @handle: output handle to a trace file.
 *
 * Must be called after tracecmd_output_set_*() functions and before writing
 * anything else.
 *
 * The initial information to be written into the file:
 *  - initial file magic bytes
 *  - file version
 *  - data endian
 *  - long size
 *  - page size
 *  - compression header
 *
 * Returns 0 on success, or -1 if the output file handle is not allocated or
 * not in the expected state.
 */
static int output_write_init(struct tracecmd_output *handle)
{
	unsigned long long offset;
	char buf[BUFSIZ];
	int endian4;

	if (!handle || handle->file_state != TRACECMD_FILE_ALLOCATED)
		return -1;

	buf[0] = 23;
	buf[1] = 8;
	buf[2] = 68;
	memcpy(buf + 3, "tracing", 7);

	if (do_write_check(handle, buf, 10))
		return -1;

	sprintf(buf, "%lu", handle->file_version);
	if (do_write_check(handle, buf, strlen(buf) + 1))
		return -1;

	if (handle->big_endian)
		buf[0] = 1;
	else
		buf[0] = 0;
	if (do_write_check(handle, buf, 1))
		return -1;

	/* save size of long (this may not be what the kernel is) */
	buf[0] = sizeof(long);
	if (do_write_check(handle, buf, 1))
		return -1;

	endian4 = convert_endian_4(handle, handle->page_size);
	if (do_write_check(handle, &endian4, 4))
		return -1;

	if (handle->file_version >= FILE_VERSION_COMPRESSION) {
		if (write_compression_header(handle))
			return -1;
	}

	if (HAS_SECTIONS(handle)) {
		/* Write 0 as options offset and save its location */
		offset = 0;
		handle->options_start = do_lseek(handle, 0, SEEK_CUR);
		if (do_write_check(handle, &offset, 8))
			return -1;
	}

	handle->file_state = TRACECMD_FILE_INIT;
	return 0;
}

/**
 * tracecmd_output_write_headers - Write the trace file headers
 * @handle: output handle to a trace file.
 * @list: desired events that will be included in the trace file.
 *	  It can be NULL for all available events
 *
 * These headers are written in the file:
 *  - header files from the tracing directory
 *  - ftrace events from the tracing directory
 *  - event file from the tracing directory - all or only the one from @list
 *  - kernel symbols from the tracing directory
 *  - kernel printk strings from the tracing directory
 *
 * Returns 0 on success, or -1 in case of an error.
 */
int tracecmd_output_write_headers(struct tracecmd_output *handle,
				  struct tracecmd_event_list *list)
{
	bool compress = false;

	if (!handle || handle->file_state < TRACECMD_FILE_ALLOCATED)
		return -1;

	/* Write init data, if not written yet */
	if (handle->file_state < TRACECMD_FILE_INIT && output_write_init(handle))
		return -1;
	if (handle->compress)
		compress = true;
	if (read_header_files(handle, compress))
		return -1;
	if (read_ftrace_files(handle, compress))
		return -1;
	if (read_event_files(handle, list, compress))
		return -1;
	if (read_proc_kallsyms(handle, compress))
		return -1;
	if (read_ftrace_printk(handle, compress))
		return -1;
	return 0;
}

/**
 * tracecmd_add_option_v - add options to the file
 * @handle: the output file handle name
 * @id: the id of the option
 * @size: the size of the option data
 * @data: the data to write to the file
 * @vector: array of vectors, pointing to the data to write in the file
 * @count: number of items in the vector array
 *
 *
 * Returns handle to update option if needed.
 *  Just the content can be updated, with smaller or equal to
 *  content than the specified size.
 */
struct tracecmd_option *
tracecmd_add_option_v(struct tracecmd_output *handle,
		      unsigned short id, const struct iovec *vector, int count)

{
	struct tracecmd_option *option;
	char *data = NULL;
	int i, size = 0;

	/*
	 * We can only add options before tracing data were written.
	 * This may change in the future.
	 */
	if (!HAS_SECTIONS(handle) && handle->file_state > TRACECMD_FILE_OPTIONS)
		return NULL;

	for (i = 0; i < count; i++)
		size += vector[i].iov_len;
	/* Some IDs (like TRACECMD_OPTION_TRACECLOCK) pass vector with 0 / NULL data */
	if (size) {
		data = malloc(size);
		if (!data) {
			tracecmd_warning("Insufficient memory");
			return NULL;
		}
	}
	option = calloc(1, sizeof(*option));
	if (!option) {
		tracecmd_warning("Could not allocate space for option");
		free(data);
		return NULL;
	}

	handle->nr_options++;
	option->data = data;
	for (i = 0; i < count; i++) {
		if (vector[i].iov_base && vector[i].iov_len) {
			memcpy(data, vector[i].iov_base, vector[i].iov_len);
			data += vector[i].iov_len;
		}
	}

	option->size = size;
	option->id = id;

	list_add_tail(&option->list, &handle->options);

	return option;
}

/**
 * tracecmd_add_option - add options to the file
 * @handle: the output file handle name
 * @id: the id of the option
 * @size: the size of the option data
 * @data: the data to write to the file
 *
 * Returns handle to update option if needed
 *  Just the content can be updated, with smaller or equal to
 *  content than the specified size
 */
struct tracecmd_option *
tracecmd_add_option(struct tracecmd_output *handle,
		    unsigned short id, int size, const void *data)
{
	struct iovec vect;

	vect.iov_base = (void *) data;
	vect.iov_len = size;
	return tracecmd_add_option_v(handle, id, &vect, 1);
}

int tracecmd_write_cpus(struct tracecmd_output *handle, int cpus)
{
	int ret;

	if (!check_out_state(handle, TRACECMD_FILE_CPU_COUNT)) {
		tracecmd_warning("Cannot write CPU count into the file, unexpected state 0x%X",
				 handle->file_state);
		return -1;
	}

	if (!HAS_SECTIONS(handle)) {
		cpus = convert_endian_4(handle, cpus);
		ret = do_write_check(handle, &cpus, 4);
		if (ret < 0)
			return ret;
	} else {
		tracecmd_add_option(handle, TRACECMD_OPTION_CPUCOUNT, sizeof(int), &cpus);
	}

	handle->file_state = TRACECMD_FILE_CPU_COUNT;
	return 0;
}

static int write_options_v6(struct tracecmd_output *handle)
{
	struct tracecmd_option *options;
	unsigned short option;
	unsigned short endian2;
	unsigned int endian4;

	/* If already written, ignore */
	if (handle->file_state == TRACECMD_FILE_OPTIONS)
		return 0;
	if (!check_out_state(handle, TRACECMD_FILE_OPTIONS)) {
		tracecmd_warning("Cannot write options into the file, unexpected state 0x%X",
				 handle->file_state);
		return -1;
	}

	if (do_write_check(handle, "options  ", 10))
		return -1;
	handle->options_start = do_lseek(handle, 0, SEEK_CUR);
	list_for_each_entry(options, &handle->options, list) {
		endian2 = convert_endian_2(handle, options->id);
		if (do_write_check(handle, &endian2, 2))
			return -1;

		endian4 = convert_endian_4(handle, options->size);
		if (do_write_check(handle, &endian4, 4))
			return -1;

		/* Save the data location in case it needs to be updated */
		options->offset = do_lseek(handle, 0, SEEK_CUR);

		if (do_write_check(handle, options->data,
				   options->size))
			return -1;
	}

	option = TRACECMD_OPTION_DONE;

	if (do_write_check(handle, &option, 2))
		return -1;

	handle->file_state = TRACECMD_FILE_OPTIONS;
	return 0;
}

static int update_options_start(struct tracecmd_output *handle, off64_t offset)
{
	if (do_lseek(handle, handle->options_start, SEEK_SET) == (off64_t)-1)
		return -1;
	offset = convert_endian_8(handle, offset);
	if (do_write_check(handle, &offset, 8))
		return -1;
	return 0;
}

/**
 * tracecmd_pepare_options - perpare a previous options for the next
 * @handle: The handle to update the options for.
 * @offset: The offset to set the previous options to.
 * @whence: Where in the file to offset from.
 *
 * In a case of cached writes for network access, the options offset
 * cannot be written once it goes over the network. This is used
 * to update the next options to a known location.
 *
 * tracecmd_write_options() must be called when the offset is at the next
 * location, otherwise the data file will end up corrupted.
 *
 * Returns zero on success and -1 on error.
 */
int tracecmd_prepare_options(struct tracecmd_output *handle, off64_t offset, int whence)
{
	tsize_t curr;
	int ret;

	/* No options to start with? */
	if (!handle->options_start)
		return 0;

	curr = do_lseek(handle, 0, SEEK_CUR);

	switch (whence) {
	case SEEK_SET:
		/* just use offset */
		break;
	case SEEK_CUR:
		offset += curr;
		break;
	case SEEK_END:
		offset = do_lseek(handle, offset, SEEK_END);
		if (offset == (off64_t)-1)
			return -1;
		break;
	}
	ret = update_options_start(handle, offset);
	if (ret < 0)
		return -1;

	handle->options_next = offset;

	curr = do_lseek(handle, curr, SEEK_SET);

	return curr == -1 ? -1 : 0;
}

static tsize_t write_options_start(struct tracecmd_output *handle)
{
	tsize_t offset;
	int ret;

	offset = do_lseek(handle, 0, SEEK_CUR);

	if (handle->options_next) {
		/* options_start was already updated */
		if (handle->options_next != offset) {
			tracecmd_warning("Options offset (%lld) does not match expected (%lld)",
					 offset, handle->options_next);
			return -1;
		}
		handle->options_next = 0;
		/* Will be updated at the end */
		handle->options_start = 0;
	}

	/* Append to the previous options section, if any */
	if (handle->options_start) {
		ret = update_options_start(handle, offset);
		if (ret < 0)
			return -1;
		offset = do_lseek(handle, offset, SEEK_SET);
		if (offset == (off_t)-1)
			return -1;
	}

	return out_write_section_header(handle, TRACECMD_OPTION_DONE, "options", 0, false);
}

static tsize_t write_options_end(struct tracecmd_output *handle, tsize_t offset)
{
	unsigned long long endian8;
	unsigned short endian2;
	unsigned int endian4;

	endian2 = convert_endian_2(handle, TRACECMD_OPTION_DONE);
	if (do_write_check(handle, &endian2, 2))
		return -1;
	endian4 = convert_endian_4(handle, 8);
	if (do_write_check(handle, &endian4, 4))
		return -1;
	endian8 = 0;
	handle->options_start = do_lseek(handle, 0, SEEK_CUR);
	if (do_write_check(handle, &endian8, 8))
		return -1;
	if (out_update_section_header(handle, offset))
		return -1;

	return 0;
}

static int write_options(struct tracecmd_output *handle)
{
	struct tracecmd_option *options;
	unsigned short endian2;
	unsigned int endian4;
	bool new = false;
	tsize_t offset;

	/* Check if there are unsaved options */
	list_for_each_entry(options, &handle->options, list) {
		if (!options->offset) {
			new = true;
			break;
		}
	}
	/*
	 * Even if there are no new options, if options_next is set, it requires
	 * adding a new empty options section as the previous one already
	 * points to it.
	 */
	if (!new && !handle->options_next)
		return 0;

	offset = write_options_start(handle);
	if (offset == (off_t)-1)
		return -1;

	list_for_each_entry(options, &handle->options, list) {
		/* Option is already saved, skip it */
		if (options->offset)
			continue;
		endian2 = convert_endian_2(handle, options->id);
		if (do_write_check(handle, &endian2, 2))
			return -1;
		endian4 = convert_endian_4(handle, options->size);
		if (do_write_check(handle, &endian4, 4))
			return -1;
		/* Save the data location */
		options->offset = do_lseek(handle, 0, SEEK_CUR);
		if (do_write_check(handle, options->data, options->size))
			return -1;
	}

	return write_options_end(handle, offset);
}

/**
 * trace_get_options - Get the current options from the output file handle
 * @handle: The output file descriptor that has options.
 * @len: Returns the length of the buffer allocated and returned.
 *
 * Reads the options that have not been written to the file yet,
 * puts them into an allocated buffer and sets @len to the size
 * added. Used by trace-msg.c to send options over the network.
 *
 * Note, the options cannot be referenced again once this is called.
 *  New options can be added and referenced.
 *
 * Returns an allocated buffer (must be freed with free()) that contains
 *   the options to send, with @len set to the size of the content.
 *   NULL on error (and @len is undefined).
 */
__hidden void *trace_get_options(struct tracecmd_output *handle, size_t *len)
{
	struct tracecmd_msg_handle msg_handle;
	struct tracecmd_output out_handle;
	struct tracecmd_option *options;
	unsigned short endian2;
	unsigned int endian4;
	tsize_t offset;
	void *buf = NULL;

	/* Use the msg_cache as our output */
	memset(&msg_handle, 0, sizeof(msg_handle));
	msg_handle.cfd = -1;
	if (tracecmd_msg_handle_cache(&msg_handle) < 0)
		return NULL;

	out_handle = *handle;
	out_handle.fd = msg_handle.cfd;
	out_handle.msg_handle = &msg_handle;

	list_for_each_entry(options, &handle->options, list) {
		/* Option is already saved, skip it */
		if (options->offset)
			continue;
		endian2 = convert_endian_2(handle, options->id);
		if (do_write_check(&out_handle, &endian2, 2))
			goto out;
		endian4 = convert_endian_4(handle, options->size);
		if (do_write_check(&out_handle, &endian4, 4))
			goto out;
		/* The option can not be referenced again */
		options->offset = -1;
		if (do_write_check(&out_handle, options->data, options->size))
			goto out;
	}

	offset = do_lseek(&out_handle, 0, SEEK_CUR);
	buf = malloc(offset);
	if (!buf)
		goto out;

	if (do_lseek(&out_handle, 0, SEEK_SET) == (off64_t)-1)
		goto out;
	*len = read(msg_handle.cfd, buf, offset);
	if (*len != offset) {
		free(buf);
		buf = NULL;
	}

 out:
	close(msg_handle.cfd);
	return buf;
}

/**
 * trace_append_options - Append options to the file
 * @handle: The output file descriptor that has options.
 * @buf: The options to append.
 * @len: The length of @buf.
 *
 * Will add an options section header for the content of @buf to
 * be written as options into the @handle.
 * Used by trace-msg.c to retrieve options over the network.
 *
 * Returns 0 on success and -1 on error.
 */
__hidden int trace_append_options(struct tracecmd_output *handle, void *buf,
				  size_t len)
{
	tsize_t offset;

	offset = write_options_start(handle);
	if (offset == (off_t)-1)
		return -1;

	if (do_write_check(handle, buf, len))
		return -1;

	return write_options_end(handle, offset);
}

int tracecmd_write_meta_strings(struct tracecmd_output *handle)
{
	if (!HAS_SECTIONS(handle))
		return 0;

	return save_string_section(handle, true);
}

int tracecmd_write_options(struct tracecmd_output *handle)
{
	if (!HAS_SECTIONS(handle))
		return write_options_v6(handle);
	return write_options(handle);
}

static int append_options_v6(struct tracecmd_output *handle)
{
	struct tracecmd_option *options;
	unsigned short option;
	unsigned short endian2;
	unsigned int endian4;
	off_t offset;
	int r;

	/*
	 * We can append only if options are already written and tracing data
	 * is not yet written
	 */
	if (handle->file_state != TRACECMD_FILE_OPTIONS)
		return -1;

	if (do_lseek(handle, 0, SEEK_END) == (off_t)-1)
		return -1;
	offset = do_lseek(handle, -2, SEEK_CUR);
	if (offset == (off_t)-1)
		return -1;

	r = do_preed(handle, &option, 2, offset);
	if (r != 2 || option != TRACECMD_OPTION_DONE)
		return -1;

	list_for_each_entry(options, &handle->options, list) {
		endian2 = convert_endian_2(handle, options->id);
		if (do_write_check(handle, &endian2, 2))
			return -1;

		endian4 = convert_endian_4(handle, options->size);
		if (do_write_check(handle, &endian4, 4))
			return -1;

		/* Save the data location in case it needs to be updated */
		options->offset = do_lseek(handle, 0, SEEK_CUR);

		if (do_write_check(handle, options->data,
				   options->size))
			return -1;
	}

	option = TRACECMD_OPTION_DONE;

	if (do_write_check(handle, &option, 2))
		return -1;

	return 0;
}

int tracecmd_append_options(struct tracecmd_output *handle)
{
	if (!HAS_SECTIONS(handle))
		return append_options_v6(handle);
	return write_options(handle);
}

static struct tracecmd_option *
add_buffer_option_v6(struct tracecmd_output *handle, const char *name, int cpus)
{
	struct tracecmd_option *option;
	char *buf;
	int size = 8 + strlen(name) + 1;

	buf = calloc(1, size);
	if (!buf) {
		tracecmd_warning("Failed to malloc buffer");
		return NULL;
	}
	*(tsize_t *)buf = 0;
	strcpy(buf + 8, name);

	option = tracecmd_add_option(handle, TRACECMD_OPTION_BUFFER, size, buf);
	free(buf);

	/*
	 * In case a buffer instance has different number of CPUs as the
	 * local machine.
	 */
	if (cpus)
		tracecmd_add_option(handle, TRACECMD_OPTION_CPUCOUNT,
				    sizeof(int), &cpus);

	return option;
}

int tracecmd_add_buffer_info(struct tracecmd_output *handle, const char *name, int cpus)
{
	struct tracecmd_buffer *buf;

	buf = calloc(1, sizeof(struct tracecmd_buffer));
	if (!buf)
		return -1;
	buf->name = strdup(name);
	buf->cpus = cpus;
	if (!buf->name) {
		free(buf);
		return -1;
	}
	list_add_tail(&buf->list, &handle->buffers);
	return 0;
}

int tracecmd_write_buffer_info(struct tracecmd_output *handle)
{
	struct tracecmd_option *option;
	struct tracecmd_buffer *buf;

	if (HAS_SECTIONS(handle))
		return 0;

	list_for_each_entry(buf, &handle->buffers, list) {
		option = add_buffer_option_v6(handle, buf->name, buf->cpus);
		if (!option)
			return -1;
		buf->option = option;
	}

	return 0;
}

static tsize_t get_buffer_file_offset(struct tracecmd_output *handle, const char *name)
{
	struct tracecmd_buffer *buf;

	list_for_each_entry(buf, &handle->buffers, list) {
		if (!strcmp(name, buf->name)) {
			if (!buf->option)
				break;
			return buf->option->offset;
		}
	}
	return 0;
}

int tracecmd_write_cmdlines(struct tracecmd_output *handle)
{
	enum tracecmd_section_flags flags = 0;
	bool compress = false;
	tsize_t offset;
	int ret;

	if (!check_out_state(handle, TRACECMD_FILE_CMD_LINES)) {
		tracecmd_warning("Cannot write command lines into the file, unexpected state 0x%X",
				 handle->file_state);
		return -1;
	}

	if (handle->compress)
		compress = true;

	if (compress)
		flags |= TRACECMD_SEC_FL_COMPRESS;
	offset = out_write_section_header(handle, TRACECMD_OPTION_CMDLINES,
					  "command lines", flags, true);
	if (offset == (off64_t)-1)
		return -1;

	out_compression_start(handle, compress);

	ret = save_tracing_file_data(handle, "saved_cmdlines");
	if (ret < 0) {
		out_compression_reset(handle, compress);
		return ret;
	}

	if (out_compression_end(handle, compress))
		return -1;

	if (out_update_section_header(handle, offset))
		return -1;

	handle->file_state = TRACECMD_FILE_CMD_LINES;
	return 0;
}

static char *get_clock(struct tracecmd_output *handle)
{
	struct tracefs_instance *inst;

	if (handle->trace_clock)
		return handle->trace_clock;

	/*
	 * If no clock is set on this handle, get the trace clock of
	 * the top instance in the handle's tracing dir
	 */
	if (!handle->tracing_dir) {
		handle->trace_clock = tracefs_get_clock(NULL);
		return handle->trace_clock;
	}

	inst = tracefs_instance_alloc(handle->tracing_dir, NULL);
	if (!inst)
		return NULL;
	handle->trace_clock = tracefs_get_clock(inst);
	tracefs_instance_free(inst);
	return handle->trace_clock;
}

__hidden struct tracecmd_option *
out_add_buffer_option(struct tracecmd_output *handle, const char *name,
		      unsigned short id, unsigned long long data_offset,
		      int cpus, struct data_file_write *cpu_data, int page_size)
{
	struct tracecmd_option *option;
	int i, j = 0, k = 0;
	int *cpu_ids = NULL;
	struct iovec *vect;
	char *clock;

	if (!HAS_SECTIONS(handle))
		return NULL;

	clock = get_clock(handle);
	if (!clock) {
		tracecmd_warning("Could not find clock, set to 'local'");
		clock = "local";
	}

	/*
	 * Buffer flyrecord option:
	 *  - trace data offset in the file
	 *  - buffer name
	 *  - buffer clock
	 *  - page size
	 *  - CPU count
	 *  - for each CPU:
	 *    - CPU id
	 *    - CPU trace data offset in the file
	 *    - CPU trace data size
	 */

	/*
	 * Buffer latency option:
	 *  - trace data offset in the file
	 *  - buffer name
	 *  - buffer clock
	 */

	/*
	 * 5 : offset, name, clock, page size, count
	 * 3 : cpu offset, name, clock
	 */
	vect = calloc(5 + (cpus * 3), sizeof(struct iovec));
	if (!vect)
		return NULL;
	if (cpus) {
		cpu_ids = calloc(cpus, sizeof(int));
		if (!cpu_ids) {
			free(vect);
			return NULL;
		}
	}
	vect[j].iov_base = (void *) &data_offset;
	vect[j++].iov_len = 8;
	vect[j].iov_base = (void *) name;
	vect[j++].iov_len = strlen(name) + 1;
	vect[j].iov_base = (void *) clock;
	vect[j++].iov_len = strlen(clock) + 1;
	if (id == TRACECMD_OPTION_BUFFER) {
		vect[j].iov_base = &page_size;
		vect[j++].iov_len = 4;
		vect[j].iov_base = (void *) &k;
		vect[j++].iov_len = 4;
		for (i = 0; i < cpus; i++) {
			if (!cpu_data[i].file_size)
				continue;
			cpu_ids[i] = i;
			vect[j].iov_base = &cpu_ids[i];
			vect[j++].iov_len = 4;
			vect[j].iov_base = &cpu_data[i].data_offset;
			vect[j++].iov_len = 8;
			vect[j].iov_base = &cpu_data[i].write_size;
			vect[j++].iov_len = 8;
			k++;
		}
	}

	option = tracecmd_add_option_v(handle, id, vect, j);
	free(vect);
	free(cpu_ids);

	return option;
}

struct tracecmd_output *tracecmd_create_file_latency(const char *output_file, int cpus,
						     int file_version, const char *compression)
{
	enum tracecmd_section_flags flags = 0;
	struct tracecmd_output *handle;
	tsize_t offset;
	char *path;

	handle = tracecmd_output_create(output_file);
	if (!handle)
		return NULL;

	if (file_version && tracecmd_output_set_version(handle, file_version))
		goto out_free;

	if (compression) {
		if (tracecmd_output_set_compression(handle, compression))
			goto out_free;
	} else if (file_version >= FILE_VERSION_COMPRESSION) {
		tracecmd_output_set_compression(handle, "any");
	}

	if (tracecmd_output_write_headers(handle, NULL))
		goto out_free;
	/*
	 * Save the command lines;
	 */
	if (tracecmd_write_cmdlines(handle) < 0)
		goto out_free;

	if (tracecmd_write_cpus(handle, cpus) < 0)
		goto out_free;
	if (tracecmd_write_buffer_info(handle) < 0)
		goto out_free;
	if (tracecmd_write_options(handle) < 0)
		goto out_free;

	if (!check_out_state(handle, TRACECMD_FILE_CPU_LATENCY)) {
		tracecmd_warning("Cannot write latency data into the file, unexpected state 0x%X",
				 handle->file_state);
		goto out_free;
	}

	if (!HAS_SECTIONS(handle) && do_write_check(handle, "latency  ", 10))
		goto out_free;

	path = get_tracing_file(handle, "trace");
	if (!path)
		goto out_free;

	offset = do_lseek(handle, 0, SEEK_CUR);
	if (HAS_SECTIONS(handle) &&
	    !out_add_buffer_option(handle, "", TRACECMD_OPTION_BUFFER_TEXT,
				   offset, 0, NULL, getpagesize()))
		goto out_free;
	if (handle->compress)
		flags |= TRACECMD_SEC_FL_COMPRESS;

	offset = out_write_section_header(handle, TRACECMD_OPTION_BUFFER_TEXT,
					  "buffer latency", flags, false);

	copy_file_compress(handle, path, NULL);
	if (out_update_section_header(handle, offset))
		goto out_free;

	put_tracing_file(path);

	handle->file_state = TRACECMD_FILE_CPU_LATENCY;

	if (HAS_SECTIONS(handle))
		tracecmd_write_options(handle);

	return handle;

out_free:
	tracecmd_output_close(handle);
	return NULL;
}

static int save_clock(struct tracecmd_output *handle, char *clock)
{
	unsigned long long endian8;
	char *str = NULL;
	int ret;

	ret = asprintf(&str, "[%s]", clock);
	if (ret < 0)
		return -1;

	endian8 = convert_endian_8(handle, strlen(str));
	ret = do_write_check(handle, &endian8, 8);
	if (ret)
		goto out;
	ret = do_write_check(handle, str, strlen(str));

out:
	free(str);
	return ret;
}

static int update_buffer_cpu_offset_v6(struct tracecmd_output *handle,
				       const char *name, tsize_t offset)
{
	tsize_t b_offset;
	tsize_t current;

	if (!name)
		name = "";

	b_offset = get_buffer_file_offset(handle, name);
	if (!b_offset) {
		tracecmd_warning("Cannot find description for buffer %s", name);
		return -1;
	}

	current = do_lseek(handle, 0, SEEK_CUR);

	/* Go to the option data, where will write the offest */
	if (do_lseek(handle, b_offset, SEEK_SET) == (off64_t)-1) {
		tracecmd_warning("could not seek to %lld", b_offset);
		return -1;
	}

	if (do_write_check(handle, &offset, 8))
		return -1;

	/* Go back to end of file */
	if (do_lseek(handle, current, SEEK_SET) == (off64_t)-1) {
		tracecmd_warning("could not seek to %lld", offset);
		return -1;
	}
	return 0;
}

__hidden int out_write_emty_cpu_data(struct tracecmd_output *handle, int cpus)
{
	unsigned long long zero = 0;
	char *clock;
	int ret;
	int i;

	if (HAS_SECTIONS(handle))
		return 0;

	ret = handle->file_state == TRACECMD_FILE_CPU_FLYRECORD ? 0 :
				    check_file_state(handle->file_version,
						     handle->file_state,
						     TRACECMD_FILE_CPU_FLYRECORD);
	if (ret < 0) {
		tracecmd_warning("Cannot write trace data into the file, unexpected state 0x%X",
				 handle->file_state);
		return ret;
	}

	if (do_write_check(handle, "flyrecord", 10))
		return -1;

	for (i = 0; i < cpus; i++) {
		/* Write 0 for trace data offset and size */
		if (do_write_check(handle, &zero, 8))
			return -1;

		if (do_write_check(handle, &zero, 8))
			return -1;
	}
	clock = get_clock(handle);
	if (clock && save_clock(handle, clock))
		return -1;

	handle->file_state = TRACECMD_FILE_CPU_FLYRECORD;
	return 0;
}

__hidden int out_write_cpu_data(struct tracecmd_output *handle,
				int cpus, struct cpu_data_source *data, const char *buff_name)
{
	struct data_file_write *data_files = NULL;
	enum tracecmd_section_flags flags = 0;
	tsize_t data_offs, offset;
	unsigned long long endian8;
	unsigned long long read_size;
	int page_size;
	char *clock;
	char *str;
	int ret;
	int i;

	/* This can be called multiple times (when recording instances) */
	ret = handle->file_state == TRACECMD_FILE_CPU_FLYRECORD ? 0 :
				    check_file_state(handle->file_version,
						     handle->file_state,
						     TRACECMD_FILE_CPU_FLYRECORD);
	if (ret < 0) {
		tracecmd_warning("Cannot write trace data into the file, unexpected state 0x%X",
				 handle->file_state);
		goto out_free;
	}

	if (*buff_name == '\0')
		page_size = handle->page_size;
	else
		page_size = get_trace_page_size(handle, buff_name);

	data_offs = do_lseek(handle, 0, SEEK_CUR);
	if (!HAS_SECTIONS(handle) && do_write_check(handle, "flyrecord", 10))
		goto out_free;

	if (handle->compress)
		flags |= TRACECMD_SEC_FL_COMPRESS;
	if (asprintf(&str, "buffer flyrecord %s", buff_name) < 1)
		goto out_free;
	offset = out_write_section_header(handle, TRACECMD_OPTION_BUFFER, str, flags, false);
	free(str);
	if (offset == (off_t)-1)
		goto out_free;

	data_files = calloc(cpus, sizeof(*data_files));
	if (!data_files)
		goto out_free;

	for (i = 0; i < cpus; i++) {
		data_files[i].file_size = data[i].size;
		/*
		 * Place 0 for the data offset and size, and save the offsets to
		 * updated them with the correct data later.
		 */
		if (!HAS_SECTIONS(handle)) {
			endian8 = 0;
			data_files[i].file_data_offset = do_lseek(handle, 0, SEEK_CUR);
			if (do_write_check(handle, &endian8, 8))
				goto out_free;
			data_files[i].file_write_size = do_lseek(handle, 0, SEEK_CUR);
			if (do_write_check(handle, &endian8, 8))
				goto out_free;
		}
	}

	if (!HAS_SECTIONS(handle)) {
		update_buffer_cpu_offset_v6(handle, buff_name, data_offs);
		clock = get_clock(handle);
		if (clock && save_clock(handle, clock))
			goto out_free;
	}

	for (i = 0; i < cpus; i++) {
		data_files[i].data_offset = do_lseek(handle, 0, SEEK_CUR);
		/* Page align offset */
		data_files[i].data_offset += page_size - 1;
		data_files[i].data_offset &= ~(page_size - 1);

		ret = do_lseek(handle, data_files[i].data_offset, SEEK_SET);
		if (ret == (off64_t)-1)
			goto out_free;

		if (!tracecmd_get_quiet(handle))
			fprintf(stderr, "CPU%d data recorded at offset=0x%llx\n",
				i, (unsigned long long)data_files[i].data_offset);

		if (data[i].size) {
			if (lseek64(data[i].fd, data[i].offset, SEEK_SET) == (off64_t)-1)
				goto out_free;
			read_size = out_copy_fd_compress(handle, data[i].fd,
							 data[i].size, &data_files[i].write_size,
							 page_size);

			if (read_size != data_files[i].file_size) {
				errno = EINVAL;
				tracecmd_warning("did not match size of %lld to %lld",
						 read_size, data_files[i].file_size);
				goto out_free;
			}
		} else {
			data_files[i].write_size = 0;
		}

		if (!HAS_SECTIONS(handle)) {
			/* Write the real CPU data offset in the file */
			if (do_lseek(handle, data_files[i].file_data_offset, SEEK_SET) == (off64_t)-1)
				goto out_free;
			endian8 = convert_endian_8(handle, data_files[i].data_offset);
			if (do_write_check(handle, &endian8, 8))
				goto out_free;
			/* Write the real CPU data size in the file */
			if (do_lseek(handle, data_files[i].file_write_size, SEEK_SET) == (off64_t)-1)
				goto out_free;
			endian8 = convert_endian_8(handle, data_files[i].write_size);
			if (do_write_check(handle, &endian8, 8))
				goto out_free;
			offset = data_files[i].data_offset + data_files[i].write_size;
			if (do_lseek(handle, offset, SEEK_SET) == (off64_t)-1)
				goto out_free;
		}
		if (!tracecmd_get_quiet(handle)) {
			fprintf(stderr, "    %llu bytes in size",
				(unsigned long long)data_files[i].write_size);
			if (flags & TRACECMD_SEC_FL_COMPRESS)
				fprintf(stderr, " (%llu uncompressed)",
					(unsigned long long)data_files[i].file_size);
			fprintf(stderr, "\n");
		}
	}

	if (HAS_SECTIONS(handle) &&
	    !out_add_buffer_option(handle, buff_name,  TRACECMD_OPTION_BUFFER,
				   data_offs, cpus, data_files, page_size))
		goto out_free;

	free(data_files);
	if (do_lseek(handle, 0, SEEK_END) == (off64_t)-1)
		return -1;

	if (out_update_section_header(handle, offset))
		goto out_free;

	handle->file_state = TRACECMD_FILE_CPU_FLYRECORD;

	if (HAS_SECTIONS(handle))
		tracecmd_write_options(handle);

	return 0;

 out_free:
	do_lseek(handle, 0, SEEK_END);
	free(data_files);
	return -1;
}

int tracecmd_write_cpu_data(struct tracecmd_output *handle,
			    int cpus, char * const *cpu_data_files, const char *buff_name)
{
	struct cpu_data_source *data;
	struct stat st;
	int size = 0;
	int ret;
	int i;

	if (!buff_name)
		buff_name = "";

	data = calloc(cpus, sizeof(struct cpu_data_source));
	if (!data)
		return -1;

	for (i = 0; i < cpus; i++) {
		ret = stat(cpu_data_files[i], &st);
		if (ret < 0) {
			tracecmd_warning("can not stat '%s'", cpu_data_files[i]);
			break;
		}
		data[i].fd = open(cpu_data_files[i], O_RDONLY);
		if (data[i].fd < 0) {
			tracecmd_warning("Can't read '%s'", data[i].fd);
			break;
		}

		data[i].size = st.st_size;
		data[i].offset = 0;
		size += st.st_size;
	}

	if (i < cpus)
		ret = -1;
	else
		ret = out_write_cpu_data(handle, cpus, data, buff_name);

	for (i--; i >= 0; i--)
		close(data[i].fd);

	free(data);
	return ret;
}

int tracecmd_append_cpu_data(struct tracecmd_output *handle,
			     int cpus, char * const *cpu_data_files)
{
	int ret;

	ret = tracecmd_write_cpus(handle, cpus);
	if (ret)
		return ret;
	ret = tracecmd_write_buffer_info(handle);
	if (ret)
		return ret;
	ret = tracecmd_write_options(handle);
	if (ret)
		return ret;

	return tracecmd_write_cpu_data(handle, cpus, cpu_data_files, NULL);
}

int tracecmd_append_buffer_cpu_data(struct tracecmd_output *handle,
				    const char *name, int cpus, char * const *cpu_data_files)
{
	return tracecmd_write_cpu_data(handle, cpus, cpu_data_files, name);
}

struct tracecmd_output *tracecmd_get_output_handle_fd(int fd)
{
	struct tracecmd_output *handle = NULL;
	struct tracecmd_input *ihandle;
	const char *cname = NULL;
	const char *cver = NULL;
	int fd2;

	/* Move the file descriptor to the beginning */
	if (lseek(fd, 0, SEEK_SET) == (off_t)-1)
		return NULL;

	/* dup fd to be used by the ihandle bellow */
	fd2 = dup(fd);
	if (fd2 < 0)
		return NULL;

	/* get a input handle from this */
	ihandle = tracecmd_alloc_fd(fd2, TRACECMD_FL_LOAD_NO_PLUGINS);
	if (!ihandle)
		return NULL;
	tracecmd_read_headers(ihandle, 0);

	/* move the file descriptor to the end */
	if (lseek(fd, 0, SEEK_END) == (off_t)-1)
		goto out_free;

	/* create a partial output handle */
	handle = calloc(1, sizeof(*handle));
	if (!handle)
		goto out_free;

	handle->fd = fd;

	/* get tep, state, endian and page size */
	handle->file_state = tracecmd_get_file_state(ihandle);
	/* Use the tep of the ihandle for later writes */
	handle->pevent = tracecmd_get_tep(ihandle);
	tep_ref(handle->pevent);
	handle->page_size = tracecmd_page_size(ihandle);
	handle->file_version = tracecmd_get_in_file_version(ihandle);
	handle->options_start = get_last_option_offset(ihandle);
	handle->strings_offs = get_meta_strings_size(ihandle);
	list_head_init(&handle->options);
	list_head_init(&handle->buffers);

	if (!tracecmd_get_file_compress_proto(ihandle, &cname, &cver)) {
		handle->compress = tracecmd_compress_alloc(cname, cver, handle->fd,
							   handle->pevent, handle->msg_handle);
		if (!handle->compress)
			goto out_free;
	}
	tracecmd_close(ihandle);

	return handle;

 out_free:
	tracecmd_close(ihandle);
	free(handle);
	return NULL;
}

/**
 * tracecmd_output_create - Create new output handle to a trace file with given name
 * @output_file: Name of the trace file that will be created.
 *
 * The @output_file parameter can be NULL. In this case the output handle is created
 * and initialized, but is not associated with a file.
 *
 * Returns pointer to created outpuy handle, or NULL in case of an error.
 */
struct tracecmd_output *tracecmd_output_create(const char *output_file)
{
	struct tracecmd_output *out;
	int fd = -1;

	if (output_file) {
		fd = open(output_file, O_RDWR | O_CREAT | O_TRUNC | O_LARGEFILE, 0644);
		if (fd < 0)
			return NULL;
	}
	out = tracecmd_output_create_fd(fd);
	if (!out && fd >= 0) {
		close(fd);
		unlink(output_file);
	}

	return out;
}

/**
 * tracecmd_copy - copy the headers of one trace.dat file for another
 * @ihandle: input handle of the trace.dat file to copy
 * @file: the trace.dat file to create
 * @state: what data will be copied from the source handle
 * @file_version: version of the output file
 * @compression: compression of the output file, can be one of:
 *		 NULL - inherit compression from the input file
 *		 "any" - compress the output file with the best available algorithm
 *		 "none" - do not compress the output file
 *		 algorithm_name - compress the output file with specified algorithm
 *
 * Reads the header information and creates a new trace data file
 * with the same characteristics (events and all) and returns
 * tracecmd_output handle to this new file.
 */
struct tracecmd_output *tracecmd_copy(struct tracecmd_input *ihandle, const char *file,
				      enum tracecmd_file_states state, int file_version,
				      const char *compression)
{
	enum tracecmd_file_states fstate;
	struct tracecmd_output *handle;

	handle = tracecmd_output_create(file);
	if (!handle)
		return NULL;

	if (tracecmd_output_set_from_input(handle, ihandle))
		goto out_free;

	if (file_version >= FILE_VERSION_MIN)
		tracecmd_output_set_version(handle, file_version);

	if (compression && tracecmd_output_set_compression(handle, compression))
		goto out_free;

	output_write_init(handle);
	fstate = state > TRACECMD_FILE_CPU_COUNT ? TRACECMD_FILE_CPU_COUNT : state;
	if (tracecmd_copy_headers(ihandle, handle, 0, fstate) < 0)
		goto out_free;

	if (tracecmd_copy_buffer_descr(ihandle, handle) < 0)
		goto out_free;

	if (state >= TRACECMD_FILE_OPTIONS &&
	    tracecmd_copy_options(ihandle, handle) < 0)
		goto out_free;

	if (state >= TRACECMD_FILE_CPU_LATENCY &&
	    tracecmd_copy_trace_data(ihandle, handle) < 0)
		goto out_free;

	if (HAS_SECTIONS(handle))
		tracecmd_write_options(handle);

	/* The file is all ready to have cpu data attached */
	return handle;

out_free:
	if (handle)
		tracecmd_output_close(handle);

	unlink(file);
	return NULL;
}

__hidden void out_set_file_state(struct tracecmd_output *handle, int new_state)
{
	handle->file_state = new_state;
}

__hidden bool check_out_state(struct tracecmd_output *handle, int new_state)
{
	return check_file_state(handle->file_version, handle->file_state, new_state);
}

__hidden bool out_check_compression(struct tracecmd_output *handle)
{
	return (handle->compress != NULL);
}

__hidden int out_save_options_offset(struct tracecmd_output *handle, unsigned long long start)
{
	unsigned long long new, en8;

	if (HAS_SECTIONS(handle)) {
		/* Append to the previous options section, if any */
		if (!handle->options_start)
			return -1;

		new = do_lseek(handle, 0, SEEK_CUR);
		if (do_lseek(handle, handle->options_start, SEEK_SET) == (off64_t)-1)
			return -1;

		en8 = convert_endian_8(handle, start);
		if (do_write_check(handle, &en8, 8))
			return -1;

		handle->options_start = new;
		if (do_lseek(handle, new, SEEK_SET) == (off64_t)-1)
			return -1;
	} else {
		handle->options_start = start;
	}

	return 0;
}

/**
 * tracecmd_get_out_file_version - return the trace.dat file version
 * @handle: output handle for the trace.dat file
 */
unsigned long tracecmd_get_out_file_version(struct tracecmd_output *handle)
{
	return handle->file_version;
}

unsigned long long tracecmd_get_out_file_offset(struct tracecmd_output *handle)
{
	return do_lseek(handle, 0, SEEK_CUR);
}
