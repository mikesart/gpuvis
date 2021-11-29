/* SPDX-License-Identifier: LGPL-2.1 */
/*
 * Copyright (C) 2019, VMware, Tzvetomir Stoyanov <tz.stoyanov@gmail.com>
 *
 */
#ifndef _TRACE_FS_H
#define _TRACE_FS_H

#include <fcntl.h>
#include <sched.h>
#include <event-parse.h>

char *tracefs_get_tracing_file(const char *name);
void tracefs_put_tracing_file(char *name);

/* the returned string must *not* be freed */
const char *tracefs_tracing_dir(void);

/* ftrace instances */
struct tracefs_instance;

void tracefs_instance_free(struct tracefs_instance *instance);
struct tracefs_instance *tracefs_instance_create(const char *name);
struct tracefs_instance *tracefs_instance_alloc(const char *tracing_dir,
						const char *name);
int tracefs_instance_destroy(struct tracefs_instance *instance);
bool tracefs_instance_is_new(struct tracefs_instance *instance);
const char *tracefs_instance_get_name(struct tracefs_instance *instance);
const char *tracefs_instance_get_trace_dir(struct tracefs_instance *instance);
char *
tracefs_instance_get_file(struct tracefs_instance *instance, const char *file);
char *tracefs_instance_get_dir(struct tracefs_instance *instance);
int tracefs_instance_file_write(struct tracefs_instance *instance,
				const char *file, const char *str);
int tracefs_instance_file_append(struct tracefs_instance *instance,
				 const char *file, const char *str);
int tracefs_instance_file_clear(struct tracefs_instance *instance,
				const char *file);
char *tracefs_instance_file_read(struct tracefs_instance *instance,
				 const char *file, int *psize);
int tracefs_instance_file_read_number(struct tracefs_instance *instance,
				      const char *file, long long *res);
int tracefs_instance_file_open(struct tracefs_instance *instance,
			       const char *file, int mode);
int tracefs_instances_walk(int (*callback)(const char *, void *), void *context);
char **tracefs_instances(const char *regex);

bool tracefs_instance_exists(const char *name);
bool tracefs_file_exists(struct tracefs_instance *instance, const char *name);
bool tracefs_dir_exists(struct tracefs_instance *instance, const char *name);

int tracefs_trace_is_on(struct tracefs_instance *instance);
int tracefs_trace_on(struct tracefs_instance *instance);
int tracefs_trace_off(struct tracefs_instance *instance);
int tracefs_trace_on_fd(int fd);
int tracefs_trace_off_fd(int fd);

int tracefs_event_enable(struct tracefs_instance *instance, const char *system, const char *event);
int tracefs_event_disable(struct tracefs_instance *instance, const char *system, const char *event);

char *tracefs_error_last(struct tracefs_instance *instance);
char *tracefs_error_all(struct tracefs_instance *instance);
int tracefs_error_clear(struct tracefs_instance *instance);

void tracefs_list_free(char **list);
char **tracefs_list_add(char **list, const char *string);
int tracefs_list_size(char **list);
int tracefs_list_pop(char **list);

/**
 * tracefs_trace_on_get_fd - Get a file descriptor of "tracing_on" in given instance
 * @instance: ftrace instance, can be NULL for the top instance
 *
 * Returns -1 in case of an error, or a valid file descriptor to "tracing_on"
 * file for reading and writing.The returned FD must be closed with close().
 */
static inline int tracefs_trace_on_get_fd(struct tracefs_instance *instance)
{
	return tracefs_instance_file_open(instance, "tracing_on", O_RDWR);
}

/* trace print string*/
int tracefs_print_init(struct tracefs_instance *instance);
int tracefs_printf(struct tracefs_instance *instance, const char *fmt, ...);
int tracefs_vprintf(struct tracefs_instance *instance, const char *fmt, va_list ap);
void tracefs_print_close(struct tracefs_instance *instance);

/* trace write binary data*/
int tracefs_binary_init(struct tracefs_instance *instance);
int tracefs_binary_write(struct tracefs_instance *instance, void *data, int len);
void tracefs_binary_close(struct tracefs_instance *instance);

/* events */
char **tracefs_event_systems(const char *tracing_dir);
char **tracefs_system_events(const char *tracing_dir, const char *system);
int tracefs_iterate_raw_events(struct tep_handle *tep,
				struct tracefs_instance *instance,
				cpu_set_t *cpus, int cpu_size,
				int (*callback)(struct tep_event *,
						struct tep_record *,
						int, void *),
				void *callback_context);
void tracefs_iterate_stop(struct tracefs_instance *instance);

char *tracefs_event_get_file(struct tracefs_instance *instance,
			     const char *system, const char *event,
			     const char *file);
char *tracefs_event_file_read(struct tracefs_instance *instance,
			      const char *system, const char *event,
			      const char *file, int *psize);
int tracefs_event_file_write(struct tracefs_instance *instance,
			     const char *system, const char *event,
			     const char *file, const char *str);
int tracefs_event_file_append(struct tracefs_instance *instance,
			      const char *system, const char *event,
			      const char *file, const char *str);
int tracefs_event_file_clear(struct tracefs_instance *instance,
			     const char *system, const char *event,
			     const char *file);
bool tracefs_event_file_exists(struct tracefs_instance *instance,
			       const char *system, const char *event,
			       const char *file);

char **tracefs_tracers(const char *tracing_dir);

struct tep_handle *tracefs_local_events(const char *tracing_dir);
struct tep_handle *tracefs_local_events_system(const char *tracing_dir,
					       const char * const *sys_names);
int tracefs_fill_local_events(const char *tracing_dir,
			       struct tep_handle *tep, int *parsing_failures);

int tracefs_load_cmdlines(const char *tracing_dir, struct tep_handle *tep);

char *tracefs_get_clock(struct tracefs_instance *instance);

enum tracefs_option_id {
	TRACEFS_OPTION_INVALID = 0,
	TRACEFS_OPTION_ANNOTATE,
	TRACEFS_OPTION_BIN,
	TRACEFS_OPTION_BLK_CGNAME,
	TRACEFS_OPTION_BLK_CGROUP,
	TRACEFS_OPTION_BLK_CLASSIC,
	TRACEFS_OPTION_BLOCK,
	TRACEFS_OPTION_CONTEXT_INFO,
	TRACEFS_OPTION_DISABLE_ON_FREE,
	TRACEFS_OPTION_DISPLAY_GRAPH,
	TRACEFS_OPTION_EVENT_FORK,
	TRACEFS_OPTION_FGRAPH_ABSTIME,
	TRACEFS_OPTION_FGRAPH_CPU,
	TRACEFS_OPTION_FGRAPH_DURATION,
	TRACEFS_OPTION_FGRAPH_IRQS,
	TRACEFS_OPTION_FGRAPH_OVERHEAD,
	TRACEFS_OPTION_FGRAPH_OVERRUN,
	TRACEFS_OPTION_FGRAPH_PROC,
	TRACEFS_OPTION_FGRAPH_TAIL,
	TRACEFS_OPTION_FUNC_STACKTRACE,
	TRACEFS_OPTION_FUNCTION_FORK,
	TRACEFS_OPTION_FUNCTION_TRACE,
	TRACEFS_OPTION_GRAPH_TIME,
	TRACEFS_OPTION_HEX,
	TRACEFS_OPTION_IRQ_INFO,
	TRACEFS_OPTION_LATENCY_FORMAT,
	TRACEFS_OPTION_MARKERS,
	TRACEFS_OPTION_OVERWRITE,
	TRACEFS_OPTION_PAUSE_ON_TRACE,
	TRACEFS_OPTION_PRINTK_MSG_ONLY,
	TRACEFS_OPTION_PRINT_PARENT,
	TRACEFS_OPTION_RAW,
	TRACEFS_OPTION_RECORD_CMD,
	TRACEFS_OPTION_RECORD_TGID,
	TRACEFS_OPTION_SLEEP_TIME,
	TRACEFS_OPTION_STACKTRACE,
	TRACEFS_OPTION_SYM_ADDR,
	TRACEFS_OPTION_SYM_OFFSET,
	TRACEFS_OPTION_SYM_USEROBJ,
	TRACEFS_OPTION_TRACE_PRINTK,
	TRACEFS_OPTION_USERSTACKTRACE,
	TRACEFS_OPTION_VERBOSE,
};
#define TRACEFS_OPTION_MAX (TRACEFS_OPTION_VERBOSE + 1)

struct tracefs_options_mask;
bool tracefs_option_mask_is_set(const struct tracefs_options_mask *options,
				enum tracefs_option_id id);
const struct tracefs_options_mask *tracefs_options_get_supported(struct tracefs_instance *instance);
bool tracefs_option_is_supported(struct tracefs_instance *instance, enum tracefs_option_id id);
const struct tracefs_options_mask *tracefs_options_get_enabled(struct tracefs_instance *instance);
bool tracefs_option_is_enabled(struct tracefs_instance *instance, enum tracefs_option_id id);
int tracefs_option_enable(struct tracefs_instance *instance, enum tracefs_option_id id);
int tracefs_option_disable(struct tracefs_instance *instance, enum tracefs_option_id id);
const char *tracefs_option_name(enum tracefs_option_id id);
enum tracefs_option_id tracefs_option_id(const char *name);

/*
 * RESET	- Reset on opening filter file (O_TRUNC)
 * CONTINUE	- Do not close filter file on return.
 * FUTURE	- For kernels that support this feature, enable filters for
 *		  a module that has yet to be loaded.
 */
enum {
	TRACEFS_FL_RESET	= (1 << 0),
	TRACEFS_FL_CONTINUE	= (1 << 1),
	TRACEFS_FL_FUTURE	= (1 << 2),
};

int tracefs_function_filter(struct tracefs_instance *instance, const char *filter,
			    const char *module, unsigned int flags);
int tracefs_function_notrace(struct tracefs_instance *instance, const char *filter,
			     const char *module, unsigned int flags);
int tracefs_filter_functions(const char *filter, const char *module, char ***list);


/* Control library logs */
void tracefs_set_loglevel(enum tep_loglevel level);

enum tracefs_tracers {
	TRACEFS_TRACER_NOP = 0,
	TRACEFS_TRACER_CUSTOM,
	TRACEFS_TRACER_FUNCTION,
	TRACEFS_TRACER_FUNCTION_GRAPH,
	TRACEFS_TRACER_IRQSOFF,
	TRACEFS_TRACER_PREEMPTOFF,
	TRACEFS_TRACER_PREEMPTIRQSOFF,
	TRACEFS_TRACER_WAKEUP,
	TRACEFS_TRACER_WAKEUP_RT,
	TRACEFS_TRACER_WAKEUP_DL,
	TRACEFS_TRACER_MMIOTRACE,
	TRACEFS_TRACER_HWLAT,
	TRACEFS_TRACER_BRANCH,
	TRACEFS_TRACER_BLOCK,
};

int tracefs_tracer_set(struct tracefs_instance *instance, enum tracefs_tracers tracer, ...);

int tracefs_tracer_clear(struct tracefs_instance *instance);

ssize_t tracefs_trace_pipe_stream(int fd, struct tracefs_instance *instance, int flags);
ssize_t tracefs_trace_pipe_print(struct tracefs_instance *instance, int flags);
void tracefs_trace_pipe_stop(struct tracefs_instance *instance);

enum tracefs_kprobe_type {
	TRACEFS_ALL_KPROBES,
	TRACEFS_KPROBE,
	TRACEFS_KRETPROBE,
};

int tracefs_kprobe_raw(const char *system, const char *event,
		       const char *addr, const char *format);
int tracefs_kretprobe_raw(const char *system, const char *event,
			  const char *addr, const char *format);
char **tracefs_get_kprobes(enum tracefs_kprobe_type type);
enum tracefs_kprobe_type tracefs_kprobe_info(const char *group, const char *event,
					     char **type, char **addr, char **format);
int tracefs_kprobe_clear_all(bool force);
int tracefs_kprobe_clear_probe(const char *system, const char *event, bool force);

enum tracefs_hist_key_type {
	TRACEFS_HIST_KEY_NORMAL = 0,
	TRACEFS_HIST_KEY_HEX,
	TRACEFS_HIST_KEY_SYM,
	TRACEFS_HIST_KEY_SYM_OFFSET,
	TRACEFS_HIST_KEY_SYSCALL,
	TRACEFS_HIST_KEY_EXECNAME,
	TRACEFS_HIST_KEY_LOG,
	TRACEFS_HIST_KEY_USECS,
	TRACEFS_HIST_KEY_MAX
};

enum tracefs_hist_sort_direction {
	TRACEFS_HIST_SORT_ASCENDING,
	TRACEFS_HIST_SORT_DESCENDING,
};

#define TRACEFS_HIST_TIMESTAMP		"common_timestamp"
#define TRACEFS_HIST_TIMESTAMP_USECS	"common_timestamp.usecs"
#define TRACEFS_HIST_CPU		"cpu"

#define TRACEFS_HIST_COUNTER		"__COUNTER__"

#define TRACEFS_HIST_HITCOUNT		"hitcount"

struct tracefs_hist;

enum tracefs_hist_command {
	TRACEFS_HIST_CMD_START = 0,
	TRACEFS_HIST_CMD_PAUSE,
	TRACEFS_HIST_CMD_CONT,
	TRACEFS_HIST_CMD_CLEAR,
	TRACEFS_HIST_CMD_DESTROY,
};

enum tracefs_filter {
	TRACEFS_FILTER_COMPARE,
	TRACEFS_FILTER_AND,
	TRACEFS_FILTER_OR,
	TRACEFS_FILTER_NOT,
	TRACEFS_FILTER_OPEN_PAREN,
	TRACEFS_FILTER_CLOSE_PAREN,
};

enum tracefs_compare {
	TRACEFS_COMPARE_EQ,
	TRACEFS_COMPARE_NE,
	TRACEFS_COMPARE_GT,
	TRACEFS_COMPARE_GE,
	TRACEFS_COMPARE_LT,
	TRACEFS_COMPARE_LE,
	TRACEFS_COMPARE_RE,
	TRACEFS_COMPARE_AND,
};

void tracefs_hist_free
(struct tracefs_hist *hist);
struct tracefs_hist *
tracefs_hist_alloc(struct tep_handle *tep,
		   const char *system, const char *event,
		   const char *key, enum tracefs_hist_key_type type);
int tracefs_hist_add_key(struct tracefs_hist *hist, const char *key,
			 enum tracefs_hist_key_type type);
int tracefs_hist_add_value(struct tracefs_hist *hist, const char *value);
int tracefs_hist_add_sort_key(struct tracefs_hist *hist,
			      const char *sort_key, ...);
int tracefs_hist_sort_key_direction(struct tracefs_hist *hist,
				    const char *sort_key,
				    enum tracefs_hist_sort_direction dir);
int tracefs_hist_add_name(struct tracefs_hist *hist, const char *name);
int tracefs_hist_append_filter(struct tracefs_hist *hist,
			       enum tracefs_filter type,
			       const char *field,
			       enum tracefs_compare compare,
			       const char *val);
int tracefs_hist_show(struct trace_seq *seq,  struct tracefs_instance *instance,
		      struct tracefs_hist *hist, enum tracefs_hist_command command);
int tracefs_hist_command(struct tracefs_instance *instance,
			 struct tracefs_hist *hist, enum tracefs_hist_command cmd);

/**
 * tracefs_hist_start - enable a histogram
 * @instance: The instance the histogram will be in (NULL for toplevel)
 * @hist: The histogram to start
 *
 * Starts executing a histogram.
 *
 * Returns 0 on success, -1 on error.
 */
static inline int tracefs_hist_start(struct tracefs_instance *instance,
				     struct tracefs_hist *hist)
{
	return tracefs_hist_command(instance, hist, TRACEFS_HIST_CMD_START); /* gpuvis change! */
}

/**
 * tracefs_hist_pause - pause a histogram
 * @instance: The instance the histogram is in (NULL for toplevel)
 * @hist: The histogram to pause
 *
 * Pause a histogram.
 *
 * Returns 0 on success, -1 on error.
 */
static inline int tracefs_hist_pause(struct tracefs_instance *instance,
				     struct tracefs_hist *hist)
{
	return tracefs_hist_command(instance, hist, TRACEFS_HIST_CMD_PAUSE);
}

/**
 * tracefs_hist_continue - continue a paused histogram
 * @instance: The instance the histogram is in (NULL for toplevel)
 * @hist: The histogram to continue
 *
 * Continue a histogram.
 *
 * Returns 0 on success, -1 on error.
 */
static inline int tracefs_hist_continue(struct tracefs_instance *instance,
					struct tracefs_hist *hist)
{
	return tracefs_hist_command(instance, hist, TRACEFS_HIST_CMD_CONT);
}

/**
 * tracefs_hist_reset - clear a histogram
 * @instance: The instance the histogram is in (NULL for toplevel)
 * @hist: The histogram to reset
 *
 * Resets a histogram.
 *
 * Returns 0 on success, -1 on error.
 */
static inline int tracefs_hist_reset(struct tracefs_instance *instance,
				     struct tracefs_hist *hist)
{
	return tracefs_hist_command(instance, hist, TRACEFS_HIST_CMD_CLEAR);
}

/**
 * tracefs_hist_destroy - deletes a histogram (needs to be enabled again)
 * @instance: The instance the histogram is in (NULL for toplevel)
 * @hist: The histogram to delete
 *
 * Deletes (removes) a running histogram. This is different than
 * clear, as clear only clears the data but the histogram still exists.
 * This deletes the histogram and should be called before
 * tracefs_hist_free() to clean up properly.
 *
 * Returns 0 on success, -1 on error.
 */
static inline int tracefs_hist_destroy(struct tracefs_instance *instance,
				       struct tracefs_hist *hist)
{
	return tracefs_hist_command(instance, hist, TRACEFS_HIST_CMD_DESTROY);
}

struct tracefs_synth;

/*
 * DELTA_END	- end_field - start_field
 * DELTA_START	- start_field - end_field
 * ADD		- start_field + end_field
 */
enum tracefs_synth_calc {
	TRACEFS_SYNTH_DELTA_END,
	TRACEFS_SYNTH_DELTA_START,
	TRACEFS_SYNTH_ADD,
};

int tracefs_event_append_filter(struct tep_event *event, char **filter,
				enum tracefs_filter type,
				const char *field, enum tracefs_compare compare,
				const char *val);
int tracefs_event_verify_filter(struct tep_event *event, const char *filter,
				char **err);

#define TRACEFS_TIMESTAMP "common_timestamp"
#define TRACEFS_TIMESTAMP_USECS "common_timestamp.usecs"

enum tracefs_synth_handler {
	TRACEFS_SYNTH_HANDLE_NONE	= 0,
	TRACEFS_SYNTH_HANDLE_MATCH,
	TRACEFS_SYNTH_HANDLE_MAX,
	TRACEFS_SYNTH_HANDLE_CHANGE,
};

struct tracefs_synth *tracefs_synth_init(struct tep_handle *tep,
					 const char *name,
					 const char *start_system,
					 const char *start_event,
					 const char *end_system,
					 const char *end_event,
					 const char *start_match_field,
					 const char *end_match_field,
					 const char *match_name);
int tracefs_synth_add_match_field(struct tracefs_synth *synth,
				  const char *start_match_field,
				  const char *end_match_field,
				  const char *name);
int tracefs_synth_add_compare_field(struct tracefs_synth *synth,
				    const char *start_compare_field,
				    const char *end_compare_field,
				    enum tracefs_synth_calc calc,
				    const char *name);
int tracefs_synth_add_start_field(struct tracefs_synth *synth,
				  const char *start_field,
				  const char *name);
int tracefs_synth_add_end_field(struct tracefs_synth *synth,
				const char *end_field,
				const char *name);
int tracefs_synth_append_start_filter(struct tracefs_synth *synth,
				      enum tracefs_filter type,
				      const char *field,
				      enum tracefs_compare compare,
				      const char *val);
int tracefs_synth_append_end_filter(struct tracefs_synth *synth,
				    enum tracefs_filter type,
				    const char *field,
				    enum tracefs_compare compare,
				    const char *val);
int tracefs_synth_trace(struct tracefs_synth *synth,
			enum tracefs_synth_handler type, const char *field);
int tracefs_synth_snapshot(struct tracefs_synth *synth,
			   enum tracefs_synth_handler type, const char *field);
int tracefs_synth_save(struct tracefs_synth *synth,
		       enum tracefs_synth_handler type, const char *field,
		       char **save_fields);
bool tracefs_synth_complete(struct tracefs_synth *synth);
struct tracefs_hist *tracefs_synth_get_start_hist(struct tracefs_synth *synth);
int tracefs_synth_create(struct tracefs_instance *instance,
			 struct tracefs_synth *synth);
int tracefs_synth_destroy(struct tracefs_instance *instance,
			  struct tracefs_synth *synth);
void tracefs_synth_free(struct tracefs_synth *synth);
int tracefs_synth_show(struct trace_seq *seq, struct tracefs_instance *instance,
		       struct tracefs_synth *synth);

struct tracefs_synth *tracefs_sql(struct tep_handle *tep, const char *name,
				  const char *sql_buffer, char **err);

#endif /* _TRACE_FS_H */
