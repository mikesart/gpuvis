// SPDX-License-Identifier: LGPL-2.1
/*
 * Copyright (C) 2008, 2009, 2010 Red Hat Inc, Steven Rostedt <srostedt@redhat.com>
 *
 * Updates:
 * Copyright (C) 2019, VMware, Tzvetomir Stoyanov <tz.stoyanov@gmail.com>
 *
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <regex.h>
#include <limits.h>
#include <pthread.h>
#include <string.h>
#include "tracefs.h"
#include "tracefs-local.h"

enum {
	FLAG_INSTANCE_NEWLY_CREATED	= (1 << 0),
	FLAG_INSTANCE_DELETED		= (1 << 1),
};


struct tracefs_options_mask	toplevel_supported_opts;
struct tracefs_options_mask	toplevel_enabled_opts;

__hidden inline struct tracefs_options_mask *
supported_opts_mask(struct tracefs_instance *instance)
{
	return instance ? &instance->supported_opts : &toplevel_supported_opts;
}

__hidden inline struct tracefs_options_mask *
enabled_opts_mask(struct tracefs_instance *instance)
{
	return instance ? &instance->enabled_opts : &toplevel_enabled_opts;
}

/**
 * instance_alloc - allocate a new ftrace instance
 * @trace_dir - Full path to the tracing directory, where the instance is
 * @name: The name of the instance (instance will point to this)
 *
 * Returns a newly allocated instance, or NULL in case of an error.
 */
static struct tracefs_instance *instance_alloc(const char *trace_dir, const char *name)
{
	struct tracefs_instance *instance;

	instance = (struct tracefs_instance *)calloc(1, sizeof(*instance)); /* gpuvis change! */
	if (!instance)
		goto error;
	instance->trace_dir = strdup(trace_dir);
	if (!instance->trace_dir)
		goto error;
	if (name) {
		instance->name = strdup(name);
		if (!instance->name)
			goto error;
	}

	if (pthread_mutex_init(&instance->lock, NULL) < 0)
		goto error;

	instance->ftrace_filter_fd = -1;
	instance->ftrace_notrace_fd = -1;
	instance->ftrace_marker_fd = -1;
	instance->ftrace_marker_raw_fd = -1;

	return instance;

error:
	if (instance) {
		free(instance->name);
		free(instance->trace_dir);
		free(instance);
	}
	return NULL;
}


__hidden int trace_get_instance(struct tracefs_instance *instance)
{
	int ret;

	pthread_mutex_lock(&instance->lock);
	if (instance->flags & FLAG_INSTANCE_DELETED) {
		ret = -1;
	} else {
		instance->ref++;
		ret = 0;
	}
	pthread_mutex_unlock(&instance->lock);
	return ret;
}

__hidden void trace_put_instance(struct tracefs_instance *instance)
{
	pthread_mutex_lock(&instance->lock);
	if (--instance->ref < 0)
		instance->flags |= FLAG_INSTANCE_DELETED;
	pthread_mutex_unlock(&instance->lock);

	if (!(instance->flags & FLAG_INSTANCE_DELETED))
		return;

	if (instance->ftrace_filter_fd >= 0)
		close(instance->ftrace_filter_fd);

	if (instance->ftrace_notrace_fd >= 0)
		close(instance->ftrace_notrace_fd);

	if (instance->ftrace_marker_fd >= 0)
		close(instance->ftrace_marker_fd);

	if (instance->ftrace_marker_raw_fd >= 0)
		close(instance->ftrace_marker_raw_fd);

	free(instance->trace_dir);
	free(instance->name);
	pthread_mutex_destroy(&instance->lock);
	free(instance);
}

/**
 * tracefs_instance_free - Free an instance, previously allocated by
			   tracefs_instance_create()
 * @instance: Pointer to the instance to be freed
 *
 */
void tracefs_instance_free(struct tracefs_instance *instance)
{
	if (!instance)
		return;

	trace_put_instance(instance);
}

static mode_t get_trace_file_permissions(char *name)
{
	mode_t rmode = 0;
	struct stat st;
	char *path;
	int ret;

	path = tracefs_get_tracing_file(name);
	if (!path)
		return 0;
	ret = stat(path, &st);
	if (ret)
		goto out;
	rmode = st.st_mode & ACCESSPERMS;
out:
	tracefs_put_tracing_file(path);
	return rmode;
}

/**
 * tracefs_instance_is_new - Check if the instance is newly created by the library
 * @instance: Pointer to an ftrace instance
 *
 * Returns true, if the ftrace instance is newly created by the library or
 * false otherwise.
 */
bool tracefs_instance_is_new(struct tracefs_instance *instance)
{
	if (instance && (instance->flags & FLAG_INSTANCE_NEWLY_CREATED))
		return true;
	return false;
}

/**
 * tracefs_instance_create - Create a new ftrace instance
 * @name: Name of the instance to be created
 *
 * Allocates and initializes a new instance structure. If the instance does not
 * exist in the system, create it.
 * Returns a pointer to a newly allocated instance, or NULL in case of an error.
 * The returned instance must be freed by tracefs_instance_free().
 */
struct tracefs_instance *tracefs_instance_create(const char *name)
{
	struct tracefs_instance *inst = NULL;
	char *path = NULL;
	const char *tdir;
	struct stat st;
	mode_t mode;
	int ret;

	tdir = tracefs_tracing_dir();
	if (!tdir)
		return NULL;
	inst = instance_alloc(tdir, name);
	if (!inst)
		return NULL;

	path = tracefs_instance_get_dir(inst);
	ret = stat(path, &st);
	if (ret < 0) {
		/* Cannot create the top instance, if it does not exist! */
		if (!name)
			goto error;
		mode = get_trace_file_permissions("instances");
		if (mkdir(path, mode))
			goto error;
		inst->flags |= FLAG_INSTANCE_NEWLY_CREATED;
	}
	tracefs_put_tracing_file(path);
	return inst;

error:
	tracefs_instance_free(inst);
	return NULL;
}

/**
 * tracefs_instance_alloc - Allocate an instance structure for existing trace instance
 * @tracing_dir: full path to the system trace directory, where the new instance is
 *		 if NULL, the default top tracing directory is used.
 * @name: Name of the instance.
 *
 * Allocates and initializes a new instance structure. If the instance does not
 * exist, do not create it and exit with error.
 * Returns a pointer to a newly allocated instance, or NULL in case of an error
 * or the requested instance does not exists.
 * The returned instance must be freed by tracefs_instance_free().
 */
struct tracefs_instance *tracefs_instance_alloc(const char *tracing_dir,
						const char *name)
{
	struct tracefs_instance *inst = NULL;
	char file[PATH_MAX];
	const char *tdir;
	struct stat st;
	int ret;

	if (tracing_dir) {
		ret = stat(tracing_dir, &st);
		if (ret < 0 || !S_ISDIR(st.st_mode))
			return NULL;
		tdir = tracing_dir;

	} else
		tdir = tracefs_tracing_dir();
	if (!tdir)
		return NULL;

	if (name) {
		sprintf(file, "%s/instances/%s", tdir, name);
		ret = stat(file, &st);
		if (ret < 0 || !S_ISDIR(st.st_mode))
			return NULL;
	}
	inst = instance_alloc(tdir, name);

	return inst;
}

/**
 * tracefs_instance_destroy - Remove a ftrace instance
 * @instance: Pointer to the instance to be removed
 *
 * Returns -1 in case of an error, or 0 otherwise.
 */
int tracefs_instance_destroy(struct tracefs_instance *instance)
{
	char *path;
	int ret = -1;

	if (!instance || !instance->name) {
		tracefs_warning("Cannot remove top instance");
		return -1;
	}

	path = tracefs_instance_get_dir(instance);
	if (path)
		ret = rmdir(path);
	tracefs_put_tracing_file(path);
	if (ret) {
		pthread_mutex_lock(&instance->lock);
		instance->flags |= FLAG_INSTANCE_DELETED;
		pthread_mutex_unlock(&instance->lock);
	}

	return ret;
}

/**
 * tracefs_instance_get_file - return the path to an instance file.
 * @instance: ftrace instance, can be NULL for the top instance
 * @file: name of file to return
 *
 * Returns the path of the @file for the given @instance, or NULL in
 * case of an error.
 *
 * Must use tracefs_put_tracing_file() to free the returned string.
 */
char *
tracefs_instance_get_file(struct tracefs_instance *instance, const char *file)
{
	char *path = NULL;
	int ret;

	if (!instance)
		return tracefs_get_tracing_file(file);
	if (!instance->name)
		ret = asprintf(&path, "%s/%s", instance->trace_dir, file);
	else
		ret = asprintf(&path, "%s/instances/%s/%s",
			       instance->trace_dir, instance->name, file);
	if (ret < 0)
		return NULL;

	return path;
}

/**
 * tracefs_instance_get_dir - return the path to the instance directory.
 * @instance: ftrace instance, can be NULL for the top instance
 *
 * Returns the full path to the instance directory
 *
 * Must use tracefs_put_tracing_file() to free the returned string.
 */
char *tracefs_instance_get_dir(struct tracefs_instance *instance)
{
	char *path = NULL;
	int ret;

	if (!instance) /* Top instance of default system trace directory */
		return trace_find_tracing_dir(false);

	if (!instance->name)
		return strdup(instance->trace_dir);

	ret = asprintf(&path, "%s/instances/%s", instance->trace_dir, instance->name);
	if (ret < 0) {
		tracefs_warning("Failed to allocate path for instance %s",
				instance->name);
		return NULL;
	}

	return path;
}

/**
 * tracefs_instance_get_name - return the name of an instance
 * @instance: ftrace instance
 *
 * Returns the name of the given @instance.
 * The returned string must *not* be freed.
 */
const char *tracefs_instance_get_name(struct tracefs_instance *instance)
{
	if (instance)
		return instance->name;
	return NULL;
}

/**
 * tracefs_instance_get_trace_dir - return the top trace directory, where the instance is confuigred
 * @instance: ftrace instance
 *
 * Returns the top trace directory where the given @instance is configured.
 * The returned string must *not* be freed.
 */
const char *tracefs_instance_get_trace_dir(struct tracefs_instance *instance)
{
	if (instance)
		return instance->trace_dir;
	return NULL;
}

static int write_file(const char *file, const char *str, int flags)
{
	int ret = 0;
	int fd;

	fd = open(file, flags);
	if (fd < 0) {
		tracefs_warning("Failed to open '%s'", file);
		return -1;
	}

	if (str)
		ret = write(fd, str, strlen(str));

	close(fd);
	return ret;
}

static int instance_file_write(struct tracefs_instance *instance,
			       const char *file, const char *str, int flags)
{
	struct stat st;
	char *path;
	int ret;

	path = tracefs_instance_get_file(instance, file);
	if (!path)
		return -1;
	ret = stat(path, &st);
	if (ret == 0)
		ret = write_file(path, str, flags);
	tracefs_put_tracing_file(path);

	return ret;
}

/**
 * tracefs_instance_file_write - Write in trace file of specific instance.
 * @instance: ftrace instance, can be NULL for the top instance
 * @file: name of the file
 * @str: nul terminated string, that will be written in the file.
 *
 * Returns the number of written bytes, or -1 in case of an error
 */
int tracefs_instance_file_write(struct tracefs_instance *instance,
				 const char *file, const char *str)
{
	return instance_file_write(instance, file, str, O_WRONLY | O_TRUNC);
}

/**
 * tracefs_instance_file_append - Append to a trace file of specific instance.
 * @instance: ftrace instance, can be NULL for the top instance.
 * @file: name of the file.
 * @str: nul terminated string, that will be appended to the file.
 *
 * Returns the number of appended bytes, or -1 in case of an error.
 */
int tracefs_instance_file_append(struct tracefs_instance *instance,
				 const char *file, const char *str)
{
	return instance_file_write(instance, file, str, O_WRONLY);
}

/**
 * tracefs_instance_file_clear - Clear a trace file of specific instance.
 * Note, it only opens with O_TRUNC and closes the file. If the file has
 * content that does not get cleared in this way, this will not have any
 * effect. For example, set_ftrace_filter can have probes that are not
 * cleared by O_TRUNC:
 *
 * echo "schedule:stacktrace" > set_ftrace_filter
 *
 * This function will not clear the above "set_ftrace_filter" after that
 * command.
 * @instance: ftrace instance, can be NULL for the top instance.
 * @file: name of the file to clear.
 *
 * Returns 0 on success, or -1 in case of an error.
 */
int tracefs_instance_file_clear(struct tracefs_instance *instance,
				const char *file)
{
	return instance_file_write(instance, file, NULL, O_WRONLY | O_TRUNC);
}

/**
 * tracefs_instance_file_read - Read from a trace file of specific instance.
 * @instance: ftrace instance, can be NULL for the top instance
 * @file: name of the file
 * @psize: returns the number of bytes read
 *
 * Returns a pointer to a nul terminated string, read from the file, or NULL in
 * case of an error.
 * The return string must be freed by free()
 */
char *tracefs_instance_file_read(struct tracefs_instance *instance,
				 const char *file, int *psize)
{
	char *buf = NULL;
	int size = 0;
	char *path;

	path = tracefs_instance_get_file(instance, file);
	if (!path)
		return NULL;

	size = str_read_file(path, &buf, true);

	tracefs_put_tracing_file(path);
	if (buf && psize)
		*psize = size;

	return buf;
}

/**
 * tracefs_instance_file_read_number - Read long long integer from a trace file.
 * @instance: ftrace instance, can be NULL for the top instance
 * @file: name of the file
 * @res: The integer from the file.
 *
 * Returns 0 if the reading is successful and the result is stored in res, -1
 * in case of an error.
 */
int tracefs_instance_file_read_number(struct tracefs_instance *instance,
				      const char *file, long long *res)
{
	long long num;
	int ret = -1;
	int size = 0;
	char *endptr;
	char *str;

	str = tracefs_instance_file_read(instance, file, &size);
	if (size && str) {
		errno = 0;
		num = strtoll(str, &endptr, 0);
		if (errno == 0 && str != endptr) {
			*res = num;
			ret = 0;
		}
	}
	free(str);
	return ret;
}

/**
 * tracefs_instance_file_open - Open a trace file for reading and writing
 * @instance: ftrace instance, can be NULL for the top instance
 * @file: name of the file
 * @mode: file open flags, -1 for default O_RDWR
 *
 * Returns -1 in case of an error, or a valid file descriptor otherwise.
 * The returned FD must be closed with close()
 */
int tracefs_instance_file_open(struct tracefs_instance *instance,
			       const char *file, int mode)
{
	int flags = O_RDWR;
	int fd = -1;
	char *path;

	path = tracefs_instance_get_file(instance, file);
	if (!path)
		return -1;

	if (mode >= 0)
		flags = mode;
	fd = open(path, flags);
	tracefs_put_tracing_file(path);

	return fd;
}

static bool check_file_exists(struct tracefs_instance *instance,
			      const char *name, bool dir)
{
	char file[PATH_MAX];
	struct stat st;
	char *path;
	int ret;

	path = tracefs_instance_get_dir(instance);
	if (name)
		snprintf(file, PATH_MAX, "%s/%s", path, name);
	else
		snprintf(file, PATH_MAX, "%s", path);
	tracefs_put_tracing_file(path);
	ret = stat(file, &st);
	if (ret < 0)
		return false;

	return !dir == !S_ISDIR(st.st_mode);
}

/**
 * tracefs_instance_exists - Check an instance with given name exists
 * @name: name of the instance
 *
 * Returns true if the instance exists, false otherwise
 *
 */
bool tracefs_instance_exists(const char *name)
{
	char file[PATH_MAX];

	if (!name)
		return false;
	snprintf(file, PATH_MAX, "instances/%s", name);
	return check_file_exists(NULL, file, true);
}

/**
 * tracefs_file_exists - Check if a file with given name exists in given instance
 * @instance: ftrace instance, can be NULL for the top instance
 * @name: name of the file
 *
 * Returns true if the file exists, false otherwise
 *
 * If a directory with the given name exists, false is returned.
 */
bool tracefs_file_exists(struct tracefs_instance *instance, const char *name)
{
	return check_file_exists(instance, name, false);
}

/**
 * tracefs_dir_exists - Check if a directory with given name exists in given instance
 * @instance: ftrace instance, can be NULL for the top instance
 * @name: name of the directory
 *
 * Returns true if the directory exists, false otherwise
 */
bool tracefs_dir_exists(struct tracefs_instance *instance, const char *name)
{
	return check_file_exists(instance, name, true);
}

/**
 * tracefs_instances_walk - Iterate through all ftrace instances in the system
 * @callback: user callback, called for each instance. Instance name is passed
 *	      as input parameter. If the @callback returns non-zero,
 *	      the iteration stops.
 * @context: user context, passed to the @callback.
 *
 * Returns -1 in case of an error, 1 if the iteration was stopped because of the
 * callback return value or 0 otherwise.
 */
int tracefs_instances_walk(int (*callback)(const char *, void *), void *context)
{
	struct dirent *dent;
	char *path = NULL;
	DIR *dir = NULL;
	struct stat st;
	int fret = -1;
	int ret;

	path = tracefs_get_tracing_file("instances");
	if (!path)
		return -1;
	ret = stat(path, &st);
	if (ret < 0 || !S_ISDIR(st.st_mode))
		goto out;

	dir = opendir(path);
	if (!dir)
		goto out;
	fret = 0;
	while ((dent = readdir(dir))) {
		char *instance;

		if (strcmp(dent->d_name, ".") == 0 ||
		    strcmp(dent->d_name, "..") == 0)
			continue;
		instance = trace_append_file(path, dent->d_name);
		ret = stat(instance, &st);
		free(instance);
		if (ret < 0 || !S_ISDIR(st.st_mode))
			continue;
		if (callback(dent->d_name, context)) {
			fret = 1;
			break;
		}
	}

out:
	if (dir)
		closedir(dir);
	tracefs_put_tracing_file(path);
	return fret;
}

static inline bool match(const char *str, regex_t *re)
{
	if (!re)
		return true;
	return regexec(re, str, 0, NULL, 0) == 0;
}

struct instance_list {
	regex_t		*re;
	char		**list;
	int		failed;
};

static int build_list(const char *name, void *data)
{
	struct instance_list *list = (struct instance_list *)data; /* gpuvis change! */
	char **instances;
	int ret = -1;

	if (!match(name, list->re))
		return 0;

	instances = tracefs_list_add(list->list, name);
	if (!instances)
		goto out;

	list->list = instances;
	ret = 0;

 out:
	list->failed = ret;
	return ret;
}

/**
 * tracefs_instances - return a list of instance names
 * @regex: A regex of instances to filter on (NULL to match all)
 *
 * Returns a list of names of existing instances, that must be
 * freed with tracefs_list_free(). Note, if there are no matches
 * then an empty list will be returned (not NULL).
 * NULL on error.
 */
char **tracefs_instances(const char *regex)
{
	struct instance_list list = { .re = NULL, .list = NULL };
	regex_t re;
	int ret;

	if (regex) {
		ret = regcomp(&re, regex, REG_ICASE|REG_NOSUB);
		if (ret < 0)
			return NULL;
		list.re = &re;
	}

	ret = tracefs_instances_walk(build_list, &list);
	if (ret < 0 || list.failed) {
		tracefs_list_free(list.list);
		list.list = NULL;
	} else {
		/* No matches should produce an empty list */
		if (!list.list)
			list.list = trace_list_create_empty();
	}
	return list.list;
}

/**
 * tracefs_get_clock - Get the current trace clock
 * @instance: ftrace instance, can be NULL for the top instance
 *
 * Returns the current trace clock of the given instance, or NULL in
 * case of an error.
 * The return string must be freed by free()
 */
char *tracefs_get_clock(struct tracefs_instance *instance)
{
	char *all_clocks = NULL;
	char *ret = NULL;
	int bytes = 0;
	char *clock;
	char *cont;

	all_clocks  = tracefs_instance_file_read(instance, "trace_clock", &bytes);
	if (!all_clocks || !bytes)
		goto out;

	clock = strstr(all_clocks, "[");
	if (!clock)
		goto out;
	clock++;
	cont = strstr(clock, "]");
	if (!cont)
		goto out;
	*cont = '\0';

	ret = strdup(clock);
out:
	free(all_clocks);
	return ret;
}

/**
 * tracefs_instance_set_affinity_raw - write a hex bitmask into the affinity
 * @instance: The instance to set affinity to (NULL for top level)
 * @mask: String containing the hex value to set the tracing affinity to.
 *
 * Sets the tracing affinity CPU mask for @instance. The @mask is the raw
 * value that is used to write into the tracing system.
 *
 * Return 0 on success and -1 on error.
 */
int tracefs_instance_set_affinity_raw(struct tracefs_instance *instance,
				      const char *mask)
{
	return tracefs_instance_file_write(instance, "tracing_cpumask", mask);
}

/**
 * tracefs_instance_set_affinity_set - use a cpu_set to define tracing affinity
 * @instance: The instance to set affinity to (NULL for top level)
 * @set: A CPU set that describes the CPU affinity to set tracing to.
 * @set_size: The size in bytes of @set (use CPU_ALLOC_SIZE() to get this value)
 *
 * Sets the tracing affinity CPU mask for @instance. The bits in @set will be
 * used to set the CPUs to have tracing on.
 *
 * If @set is NULL, then all CPUs defined by sysconf(_SC_NPROCESSORS_CONF)
 * will be set, and @set_size is ignored.
 *
 * Return 0 on success and -1 on error.
 */
int tracefs_instance_set_affinity_set(struct tracefs_instance *instance,
				      cpu_set_t *set, size_t set_size)
{
	struct trace_seq seq;
	bool free_set = false;
	bool hit = false;
	int nr_cpus;
	int cpu;
	int ret = -1;
	int w, n, i;

	trace_seq_init(&seq);

	/* NULL set means all CPUs to be set */
	if (!set) {
		nr_cpus = sysconf(_SC_NPROCESSORS_CONF);
		set = CPU_ALLOC(nr_cpus);
		if (!set)
			goto out;
		set_size = CPU_ALLOC_SIZE(nr_cpus);
		CPU_ZERO_S(set_size, set);
		/* Set all CPUS */
		for (cpu = 0; cpu < nr_cpus; cpu++)
			CPU_SET_S(cpu, set_size, set);
		free_set = true;
	}
	/* Convert to a bitmask hex string */
	nr_cpus = (set_size + 1) * 8;
	if (nr_cpus < 1) {
		/* Must have at least one bit set */
		errno = EINVAL;
		goto out;
	}
	/* Start backwards from 32 bits */
	for (w = ((nr_cpus + 31) / 32) - 1; w >= 0; w--) {
		/* Now move one nibble at a time */
		for (n = 7; n >= 0; n--) {
			int nibble = 0;

			if ((n * 4) + (w * 32) >= nr_cpus)
				continue;

			/* One bit at a time */
			for (i = 3; i >= 0; i--) {
				cpu = (w * 32) + (n * 4) + i;
				if (cpu >= nr_cpus)
					continue;
				if (CPU_ISSET_S(cpu, set_size, set)) {
					nibble |= 1 << i;
					hit = true;
				}
			}
			if (hit && trace_seq_printf(&seq, "%x", nibble) < 0)
				goto out;
		}
		if (hit && w)
			if (trace_seq_putc(&seq, ',') < 0)
				goto out;
	}
	if (!hit) {
		errno = EINVAL;
		goto out;
	}
	trace_seq_terminate(&seq);
	ret = tracefs_instance_set_affinity_raw(instance, seq.buffer);
 out:
	trace_seq_destroy(&seq);
	if (free_set)
		CPU_FREE(set);
	return ret;
}

/**
 * tracefs_instance_set_affinity - Set the affinity defined by CPU values.
 * @instance: The instance to set affinity to (NULL for top level)
 * @cpu_str: A string of values that define what CPUs to set.
 *
 * Sets the tracing affinity CPU mask for @instance. The @cpu_str is a set
 * of decimal numbers used to state which CPU should be part of the affinity
 * mask. A range may also be specified via a hyphen.
 *
 * For example, "1,4,6-8"
 *
 * The numbers do not need to be in order.
 *
 * If @cpu_str is NULL, then all CPUs defined by sysconf(_SC_NPROCESSORS_CONF)
 * will be set.
 *
 * Return 0 on success and -1 on error.
 */
int tracefs_instance_set_affinity(struct tracefs_instance *instance,
				  const char *cpu_str)
{
	cpu_set_t *set = NULL;
	size_t set_size;
	char *word;
	char *cpus;
	char *del;
	char *c;
	int max_cpu = 0;
	int cpu1, cpu2;
	int len;
	int ret = -1;

	/* NULL cpu_str means to set all CPUs in the mask */
	if (!cpu_str)
		return tracefs_instance_set_affinity_set(instance, NULL, 0);

	/* First, find out how many CPUs are needed */
	cpus = strdup(cpu_str);
	if (!cpus)
		return -1;
	len = strlen(cpus) + 1;
	for (word = strtok_r(cpus, ",", &del); word; word = strtok_r(NULL, ",", &del)) {
		cpu1 = atoi(word);
		if (cpu1 < 0) {
			errno = EINVAL;
			goto out;
		}
		if (cpu1 > max_cpu)
			max_cpu = cpu1;
		cpu2 = -1;
		if ((c = strchr(word, '-'))) {
			c++;
			cpu2 = atoi(c);
			if (cpu2 < cpu1) {
				errno = EINVAL;
				goto out;
			}
			if (cpu2 > max_cpu)
				max_cpu = cpu2;
		}
	}
	/*
	 * Now ideally, cpus should fit cpu_str as it was orginally allocated
	 * by strdup(). But I'm paranoid, and can imagine someone playing tricks
	 * with threads, and changes cpu_str from another thread and messes
	 * with this. At least only copy what we know is allocated.
	 */
	strncpy(cpus, cpu_str, len);

	set = CPU_ALLOC(max_cpu + 1);
	if (!set)
		goto out;
	set_size = CPU_ALLOC_SIZE(max_cpu + 1);
	CPU_ZERO_S(set_size, set);

	for (word = strtok_r(cpus, ",", &del); word; word = strtok_r(NULL, ",", &del)) {
		cpu1 = atoi(word);
		if (cpu1 < 0 || cpu1 > max_cpu) {
			/* Someone playing games? */
			errno = EACCES;
			goto out;
		}
		cpu2 = cpu1;
		if ((c = strchr(word, '-'))) {
			c++;
			cpu2 = atoi(c);
			if (cpu2 < cpu1 || cpu2 > max_cpu) {
				errno = EACCES;
				goto out;
			}
		}
		for ( ; cpu1 <= cpu2; cpu1++)
			CPU_SET(cpu1, set);
	}
	ret = tracefs_instance_set_affinity_set(instance, set, set_size);
 out:
	free(cpus);
	CPU_FREE(set);
	return ret;
}

/**
 * tracefs_instance_get_affinity_raw - read the affinity instance file
 * @instance: The instance to get affinity of (NULL for top level)
 *
 * Reads the affinity file for @instance (or the top level if @instance
 * is NULL) and returns it. The returned string must be freed with free().
 *
 * Returns the affinity mask on success, and must be freed with free()
 *   or NULL on error.
 */
char *tracefs_instance_get_affinity_raw(struct tracefs_instance *instance)
{
	return tracefs_instance_file_read(instance, "tracing_cpumask", NULL);
}

static inline int update_cpu_set(int cpus, int cpu_set, int cpu,
				 cpu_set_t *set, size_t set_size)
{
	int bit = 1 << cpu;

	if (!(cpus & bit))
		return 0;

	CPU_SET_S(cpu_set + cpu, set_size, set);

	/*
	 * It is possible that the passed in set_size is not big enough
	 * to hold the cpu we just set. If that's the case, do not report
	 * it as being set.
	 *
	 * The CPU_ISSET_S() should return false if the CPU given to it
	 * is bigger than the set itself.
	 */
	return CPU_ISSET_S(cpu_set + cpu, set_size, set) ? 1 : 0;
}

/**
 * tracefs_instance_get_affinity_set - Retrieve the cpuset of an instance affinity
 * @instance: The instance to get affinity of (NULL for top level)
 * @set: A CPU set to put the affinity into.
 * @set_size: The size in bytes of @set (use CPU_ALLOC_SIZE() to get this value)
 *
 * Reads the affinity of a given instance and updates the CPU set by the
 * instance.
 *
 * Returns the number of CPUS that are set, or -1 on error.
 */
int tracefs_instance_get_affinity_set(struct tracefs_instance *instance,
				      cpu_set_t *set, size_t set_size)
{
	char *affinity;
	int cpu_set;
	int cpus;
	int cnt = 0;
	int ch;
	int i;

	if (!set || !set_size) {
		errno = -EINVAL;
		return -1;
	}

	affinity = tracefs_instance_get_affinity_raw(instance);
	if (!affinity)
		return -1;

	/*
	 * The returned affinity should be a comma delimited
	 * hex string. Work backwards setting the values.
	 */
	cpu_set = 0;
	i = strlen(affinity);
	for (i--; i >= 0; i--) {
		ch = affinity[i];
		if (isalnum(ch)) {
			ch = tolower(ch);
			if (isdigit(ch))
				cpus = ch - '0';
			else
				cpus = ch - 'a' + 10;

			cnt += update_cpu_set(cpus, cpu_set, 0, set, set_size);
			cnt += update_cpu_set(cpus, cpu_set, 1, set, set_size);
			cnt += update_cpu_set(cpus, cpu_set, 2, set, set_size);
			cnt += update_cpu_set(cpus, cpu_set, 3, set, set_size);
			/* Next nibble */
			cpu_set += 4;
		}
	}

	free(affinity);

	return cnt;
}

static inline int update_cpu(int cpus, int cpu_set, int cpu, int s, char **set)
{
	char *list;
	int bit = 1 << cpu;
	int ret;

	if (*set == (char *)-1)
		return s;

	if (cpus & bit) {
		/* If the previous CPU is set just return s */
		if (s >= 0)
			return s;
		/* Otherwise, return this cpu */
		return cpu_set + cpu;
	}

	/* If the last CPU wasn't set, just return s */
	if (s < 0)
		return s;

	/* Update the string */
	if (s == cpu_set + cpu - 1) {
		ret = asprintf(&list, "%s%s%d",
			       *set ? *set : "", *set ? "," : "", s);
	} else {
		ret = asprintf(&list, "%s%s%d-%d",
			       *set ? *set : "", *set ? "," : "",
			       s, cpu_set + cpu - 1);
	}
	free(*set);
	/* Force *set to be a failure */
	if (ret < 0)
		*set = (char *)-1;
	else
		*set = list;
	return -1;
}

/**
 * tracefs_instance_get_affinity - Retrieve a string of CPUs for instance affinity
 * @instance: The instance to get affinity of (NULL for top level)
 *
 * Reads the affinity of a given instance and returns a CPU count of the
 * instance. For example, if it reads "eb" it will return:
 *      "0-1,3,5-7"
 *
 * If no CPUs are set, an empty string is returned "\0", and it too needs
 * to be freed.
 *
 * Returns an allocated string containing the CPU affinity in "human readable"
 *  format which needs to be freed with free(), or NULL on error.
 */
char *tracefs_instance_get_affinity(struct tracefs_instance *instance)
{
	char *affinity;
	char *set = NULL;
	int cpu_set;
	int cpus;
	int ch;
	int s = -1;
	int i;

	affinity = tracefs_instance_get_affinity_raw(instance);
	if (!affinity)
		return NULL;

	/*
	 * The returned affinity should be a comma delimited
	 * hex string. Work backwards setting the values.
	 */
	cpu_set = 0;
	i = strlen(affinity);
	for (i--; i >= 0; i--) {
		ch = affinity[i];
		if (isalnum(ch)) {
			ch = tolower(ch);
			if (isdigit(ch))
				cpus = ch - '0';
			else
				cpus = ch - 'a' + 10;
			s = update_cpu(cpus, cpu_set, 0, s, &set);
			s = update_cpu(cpus, cpu_set, 1, s, &set);
			s = update_cpu(cpus, cpu_set, 2, s, &set);
			s = update_cpu(cpus, cpu_set, 3, s, &set);

			if (set == (char *)-1) {
				set = NULL;
				goto out;
			}
			/* Next nibble */
			cpu_set += 4;
		}
	}
	/* Clean up in case the last CPU is set */
	s = update_cpu(0, cpu_set, 0, s, &set);

	if (!set)
		set = strdup("");
 out:
	free(affinity);

	return set;
}
