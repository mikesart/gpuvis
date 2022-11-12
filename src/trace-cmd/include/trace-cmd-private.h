/* SPDX-License-Identifier: LGPL-2.1 */
/*
 * Copyright (C) 2008, 2009, 2010 Red Hat Inc, Steven Rostedt <srostedt@redhat.com>
 *
 */
#ifndef _TRACE_CMD_PRIVATE_H
#define _TRACE_CMD_PRIVATE_H

#include <fcntl.h> /* for iovec */
#include <sys/types.h>
#include "event-parse.h"
#include "trace-cmd/trace-cmd.h"

#define __packed __attribute__((packed))
#define __hidden __attribute__((visibility ("hidden")))

#define TRACECMD_MAGIC { 23, 8, 68 }

#define ARRAY_SIZE(_a) (sizeof(_a) / sizeof((_a)[0]))
#define __weak __attribute__((weak))
#define __noreturn __attribute__((noreturn))

#define TRACECMD_ERR_MSK	((unsigned long)(-1) & ~((1UL << 14) - 1))
#define TRACECMD_ISERR(ptr)	((unsigned long)(ptr) > TRACECMD_ERR_MSK)
#define TRACECMD_ERROR(ret)	((void *)((unsigned long)(ret) | TRACECMD_ERR_MSK))
#define TRACECMD_PTR2ERR(ptr)	((unisgned long)(ptr) & ~TRACECMD_ERR_MSK)

#define TSCNSEC_CLOCK	"tsc2nsec"

struct tep_plugin_list *trace_load_plugins(struct tep_handle *tep, int flags);

int *tracecmd_add_id(int *list, int id, int len);

#define FILE_VERSION_MIN		6
#define FILE_VERSION_MAX		7

#define FILE_VERSION_SECTIONS		7
#define FILE_VERSION_COMPRESSION	7

enum {
	RINGBUF_TYPE_PADDING		= 29,
	RINGBUF_TYPE_TIME_EXTEND	= 30,
	RINGBUF_TYPE_TIME_STAMP		= 31,
};

/* Can be overridden */
void tracecmd_debug(const char *fmt, ...);

void tracecmd_record_ref(struct tep_record *record);

void tracecmd_set_debug(bool set_debug);
bool tracecmd_get_debug(void);

bool tracecmd_is_version_supported(unsigned int version);
int tracecmd_default_file_version(void);

struct tracecmd_output;
struct tracecmd_recorder;
struct hook_list;

/* --- tracecmd plugins --- */

enum tracecmd_context {
	TRACECMD_INPUT,
	TRACECMD_OUTPUT,
};

enum tracecmd_plugin_flag {
	TRACECMD_DISABLE_SYS_PLUGINS	= 1,
	TRACECMD_DISABLE_PLUGINS	= 1 << 1,
};

struct trace_plugin_context;

struct trace_plugin_context *
tracecmd_plugin_context_create(enum tracecmd_context context, void *data);

void tracecmd_plugin_set_flag(struct trace_plugin_context *context,
			      enum tracecmd_plugin_flag flag);

#define TRACECMD_PLUGIN_LOADER tracecmd_plugin_loader
#define TRACECMD_PLUGIN_UNLOADER tracecmd_plugin_unloader
#define TRACECMD_PLUGIN_ALIAS tracecmd_plugin_alias
#define _MAKE_STR(x)	#x
#define MAKE_STR(x)	_MAKE_STR(x)
#define TRACECMD_PLUGIN_LOADER_NAME MAKE_STR(TRACECMD_PLUGIN_LOADER)
#define TRACECMD_PLUGIN_UNLOADER_NAME MAKE_STR(TRACECMD_PLUGIN_UNLOADER)
#define TRACECMD_PLUGIN_ALIAS_NAME MAKE_STR(TRACECMD_PLUGIN_ALIAS)

typedef int (*tracecmd_plugin_load_func)(struct trace_plugin_context *trace);
typedef int (*tracecmd_plugin_unload_func)(struct trace_plugin_context *trace);

struct tracecmd_input *
tracecmd_plugin_context_input(struct trace_plugin_context *trace_context);
struct tracecmd_output *
tracecmd_plugin_context_output(struct trace_plugin_context *trace_context);

void tracecmd_set_quiet(struct tracecmd_output *handle, bool set_quiet);
bool tracecmd_get_quiet(struct tracecmd_output *handle);
void tracecmd_set_out_clock(struct tracecmd_output *handle, const char *clock);
const char *tracecmd_get_trace_clock(struct tracecmd_input *handle);

const char *tracecmd_get_cpustats(struct tracecmd_input *handle);
const char *tracecmd_get_uname(struct tracecmd_input *handle);
const char *tracecmd_get_version(struct tracecmd_input *handle);
off64_t tracecmd_get_cpu_file_size(struct tracecmd_input *handle, int cpu);

static inline int tracecmd_host_bigendian(void)
{
	unsigned char str[] = { 0x1, 0x2, 0x3, 0x4 };
	unsigned int *ptr;

	ptr = (unsigned int *)str;
	return *ptr == 0x01020304;
}

/* --- Opening and Reading the trace.dat file --- */

enum tracecmd_file_states {
	TRACECMD_FILE_ALLOCATED = 0,
	TRACECMD_FILE_INIT,
	TRACECMD_FILE_HEADERS,
	TRACECMD_FILE_FTRACE_EVENTS,
	TRACECMD_FILE_ALL_EVENTS,
	TRACECMD_FILE_KALLSYMS,
	TRACECMD_FILE_PRINTK,
	TRACECMD_FILE_CMD_LINES,
	TRACECMD_FILE_CPU_COUNT,
	TRACECMD_FILE_OPTIONS,
	TRACECMD_FILE_CPU_LATENCY,
	TRACECMD_FILE_CPU_FLYRECORD,
};

enum {
	TRACECMD_OPTION_DONE,
	TRACECMD_OPTION_DATE,
	TRACECMD_OPTION_CPUSTAT,
	TRACECMD_OPTION_BUFFER,
	TRACECMD_OPTION_TRACECLOCK,
	TRACECMD_OPTION_UNAME,
	TRACECMD_OPTION_HOOK,
	TRACECMD_OPTION_OFFSET,
	TRACECMD_OPTION_CPUCOUNT,
	TRACECMD_OPTION_VERSION,
	TRACECMD_OPTION_PROCMAPS,
	TRACECMD_OPTION_TRACEID,
	TRACECMD_OPTION_TIME_SHIFT,
	TRACECMD_OPTION_GUEST,
	TRACECMD_OPTION_TSC2NSEC,
	TRACECMD_OPTION_STRINGS,
	TRACECMD_OPTION_HEADER_INFO,
	TRACECMD_OPTION_FTRACE_EVENTS,
	TRACECMD_OPTION_EVENT_FORMATS,
	TRACECMD_OPTION_KALLSYMS,
	TRACECMD_OPTION_PRINTK,
	TRACECMD_OPTION_CMDLINES,
	TRACECMD_OPTION_BUFFER_TEXT,
	TRACECMD_OPTION_MAX,
};

enum {
	TRACECMD_FL_IGNORE_DATE		= (1 << 0),
	TRACECMD_FL_BUFFER_INSTANCE	= (1 << 1),
	TRACECMD_FL_IN_USECS		= (1 << 2),
	TRACECMD_FL_RAW_TS		= (1 << 3),
	TRACECMD_FL_SECTIONED		= (1 << 4),
	TRACECMD_FL_COMPRESSION		= (1 << 5),
};

struct tracecmd_ftrace {
	struct tracecmd_input		*handle;
	struct tep_event *fgraph_ret_event;
	int fgraph_ret_id;
	int long_size;
};

struct tracecmd_proc_addr_map {
	unsigned long long	start;
	unsigned long long	end;
	char			*lib_name;
};

typedef void (*tracecmd_show_data_func)(struct tracecmd_input *handle,
					struct tep_record *record);
typedef void (*tracecmd_handle_init_func)(struct tracecmd_input *handle,
					  struct hook_list *hook, int global);

struct tracecmd_input *tracecmd_alloc(const char *file, int flags);
struct tracecmd_input *tracecmd_alloc_fd(int fd, int flags);
void tracecmd_ref(struct tracecmd_input *handle);
int tracecmd_read_headers(struct tracecmd_input *handle,
			  enum tracecmd_file_states state);
int tracecmd_get_parsing_failures(struct tracecmd_input *handle);
int tracecmd_long_size(struct tracecmd_input *handle);
int tracecmd_page_size(struct tracecmd_input *handle);
int tracecmd_cpus(struct tracecmd_input *handle);
int tracecmd_copy_headers(struct tracecmd_input *in_handle,
			  struct tracecmd_output *out_handle,
			  enum tracecmd_file_states start_state,
			  enum tracecmd_file_states end_state);
int tracecmd_copy_buffer_descr(struct tracecmd_input *in_handle,
			       struct tracecmd_output *out_handle);
int tracecmd_copy_options(struct tracecmd_input *in_handle,
			  struct tracecmd_output *out_handle);
int tracecmd_copy_trace_data(struct tracecmd_input *in_handle,
			     struct tracecmd_output *out_handle);
void tracecmd_set_flag(struct tracecmd_input *handle, int flag);
void tracecmd_clear_flag(struct tracecmd_input *handle, int flag);
unsigned long tracecmd_get_flags(struct tracecmd_input *handle);
enum tracecmd_file_states tracecmd_get_file_state(struct tracecmd_input *handle);
int tracecmd_enable_tsync(struct tracecmd_input *handle, bool enable);

void tracecmd_parse_trace_clock(struct tracecmd_input *handle, char *file, int size);

int tracecmd_make_pipe(struct tracecmd_input *handle, int cpu, int fd, int cpus);

int tracecmd_is_buffer_instance(struct tracecmd_input *handle);

void tracecmd_set_ts_offset(struct tracecmd_input *handle, long long offset);
void tracecmd_set_ts2secs(struct tracecmd_input *handle, unsigned long long hz);

void tracecmd_print_events(struct tracecmd_input *handle, const char *regex);

struct hook_list *tracecmd_hooks(struct tracecmd_input *handle);

void tracecmd_print_stats(struct tracecmd_input *handle);
void tracecmd_print_uname(struct tracecmd_input *handle);
void tracecmd_print_version(struct tracecmd_input *handle);

struct tep_record *
tracecmd_peek_data(struct tracecmd_input *handle, int cpu);

static inline struct tep_record *
tracecmd_peek_data_ref(struct tracecmd_input *handle, int cpu)
{
	struct tep_record *rec = tracecmd_peek_data(handle, cpu);
	if (rec)
		rec->ref_count++;
	return rec;
}

int tracecmd_latency_data_read(struct tracecmd_input *handle, char **buf, size_t *size);

struct tep_record *
tracecmd_read_prev(struct tracecmd_input *handle, struct tep_record *record);

struct tep_record *
tracecmd_read_next_data(struct tracecmd_input *handle, int *rec_cpu);

struct tep_record *
tracecmd_peek_next_data(struct tracecmd_input *handle, int *rec_cpu);

struct tep_record *
tracecmd_translate_data(struct tracecmd_input *handle,
			void *ptr, int size);
struct tep_record *
tracecmd_read_cpu_last(struct tracecmd_input *handle, int cpu);
int tracecmd_refresh_record(struct tracecmd_input *handle,
			    struct tep_record *record);

int tracecmd_set_cpu_to_timestamp(struct tracecmd_input *handle,
				  int cpu, unsigned long long ts);
void
tracecmd_set_all_cpus_to_timestamp(struct tracecmd_input *handle,
				   unsigned long long time);

int tracecmd_set_cursor(struct tracecmd_input *handle,
			int cpu, unsigned long long offset);
unsigned long long
tracecmd_get_cursor(struct tracecmd_input *handle, int cpu);

unsigned long tracecmd_get_in_file_version(struct tracecmd_input *handle);
size_t tracecmd_get_options_offset(struct tracecmd_input *handle);
int tracecmd_get_file_compress_proto(struct tracecmd_input *handle,
				     const char **name, const char **version);

int tracecmd_ftrace_overrides(struct tracecmd_input *handle, struct tracecmd_ftrace *finfo);
bool tracecmd_get_use_trace_clock(struct tracecmd_input *handle);
tracecmd_show_data_func
tracecmd_get_show_data_func(struct tracecmd_input *handle);
void tracecmd_set_show_data_func(struct tracecmd_input *handle,
				 tracecmd_show_data_func func);

int tracecmd_record_at_buffer_start(struct tracecmd_input *handle, struct tep_record *record);
unsigned long long tracecmd_page_ts(struct tracecmd_input *handle,
				    struct tep_record *record);
unsigned int tracecmd_record_ts_delta(struct tracecmd_input *handle,
				      struct tep_record *record);

struct tracecmd_proc_addr_map *
tracecmd_search_task_map(struct tracecmd_input *handle,
			 int pid, unsigned long long addr);
#ifndef SWIG
/* hack for function graph work around */
extern __thread struct tracecmd_input *tracecmd_curr_thread_handle;
#endif


/* --- Creating and Writing the trace.dat file --- */

struct tracecmd_event_list {
	struct tracecmd_event_list	*next;
	const char			*glob;
};

struct tracecmd_option;
struct tracecmd_msg_handle;

int tracecmd_output_set_msg(struct tracecmd_output *handle,
			    struct tracecmd_msg_handle *msg_handle);
int tracecmd_output_set_trace_dir(struct tracecmd_output *handle, const char *tracing_dir);
int tracecmd_output_set_kallsyms(struct tracecmd_output *handle, const char *kallsyms);
int tracecmd_output_set_from_input(struct tracecmd_output *handle, struct tracecmd_input *ihandle);
int tracecmd_output_set_version(struct tracecmd_output *handle, int file_version);
int tracecmd_output_set_compression(struct tracecmd_output *handle, const char *compression);
int tracecmd_output_write_headers(struct tracecmd_output *handle,
				  struct tracecmd_event_list *list);

struct tracecmd_output *tracecmd_output_create(const char *output_file);
struct tracecmd_output *tracecmd_output_create_fd(int fd);
struct tracecmd_output *tracecmd_create_file_latency(const char *output_file, int cpus,
						     int file_version, const char *compression);

struct tracecmd_option *tracecmd_add_option(struct tracecmd_output *handle,
					    unsigned short id, int size,
					    const void *data);
struct tracecmd_option *
tracecmd_add_option_v(struct tracecmd_output *handle,
		      unsigned short id, const struct iovec *vector, int count);

int tracecmd_add_buffer_info(struct tracecmd_output *handle, const char *name, int cpus);
int tracecmd_write_buffer_info(struct tracecmd_output *handle);

int tracecmd_write_cpus(struct tracecmd_output *handle, int cpus);
int tracecmd_write_cmdlines(struct tracecmd_output *handle);
int tracecmd_prepare_options(struct tracecmd_output *handle, off64_t offset, int whence);
int tracecmd_write_options(struct tracecmd_output *handle);
int tracecmd_write_meta_strings(struct tracecmd_output *handle);
int tracecmd_append_options(struct tracecmd_output *handle);
void tracecmd_output_close(struct tracecmd_output *handle);
void tracecmd_output_free(struct tracecmd_output *handle);
struct tracecmd_output *tracecmd_copy(struct tracecmd_input *ihandle, const char *file,
				      enum tracecmd_file_states state, int file_version,
				      const char *compression);

int tracecmd_write_cpu_data(struct tracecmd_output *handle,
			    int cpus, char * const *cpu_data_files, const char *buff_name);
int tracecmd_append_cpu_data(struct tracecmd_output *handle,
			     int cpus, char * const *cpu_data_files);
int tracecmd_append_buffer_cpu_data(struct tracecmd_output *handle,
				    const char *name, int cpus, char * const *cpu_data_files);
struct tracecmd_output *tracecmd_get_output_handle_fd(int fd);
unsigned long tracecmd_get_out_file_version(struct tracecmd_output *handle);
unsigned long long tracecmd_get_out_file_offset(struct tracecmd_output *handle);

/* --- Reading the Fly Recorder Trace --- */

enum {
	TRACECMD_RECORD_NOSPLICE	= (1 << 0),	/* Use read instead of splice */
	TRACECMD_RECORD_SNAPSHOT	= (1 << 1),	/* Extract from snapshot */
	TRACECMD_RECORD_BLOCK_SPLICE	= (1 << 2),	/* Block on splice write */
	TRACECMD_RECORD_NOBRASS		= (1 << 3),	/* Splice directly without a brass pipe */
	TRACECMD_RECORD_POLL		= (1 << 4),	/* Use O_NONBLOCK, poll trace buffers */
};

void tracecmd_free_recorder(struct tracecmd_recorder *recorder);
struct tracecmd_recorder *tracecmd_create_recorder(const char *file, int cpu, unsigned flags);
struct tracecmd_recorder *tracecmd_create_recorder_fd(int fd, int cpu, unsigned flags);
struct tracecmd_recorder *tracecmd_create_recorder_virt(const char *file, int cpu, unsigned flags, int trace_fd);
struct tracecmd_recorder *tracecmd_create_recorder_maxkb(const char *file, int cpu, unsigned flags, int maxkb);
struct tracecmd_recorder *tracecmd_create_buffer_recorder_fd(int fd, int cpu, unsigned flags, const char *buffer);
struct tracecmd_recorder *tracecmd_create_buffer_recorder(const char *file, int cpu, unsigned flags, const char *buffer);
struct tracecmd_recorder *tracecmd_create_buffer_recorder_maxkb(const char *file, int cpu, unsigned flags, const char *buffer, int maxkb);

int tracecmd_start_recording(struct tracecmd_recorder *recorder, unsigned long sleep);
void tracecmd_stop_recording(struct tracecmd_recorder *recorder);
long tracecmd_flush_recording(struct tracecmd_recorder *recorder);

enum tracecmd_msg_flags {
	TRACECMD_MSG_FL_USE_TCP		= 1 << 0,
	TRACECMD_MSG_FL_USE_VSOCK	= 1 << 1,
	TRACECMD_MSG_FL_PROXY		= 1 << 2,
};

#define MSG_CACHE_FILE "/tmp/trace_msg_cacheXXXXXX"

/* for both client and server */
struct tracecmd_msg_handle {
	int			fd;
	short			cpu_count;
	short			version;	/* Current protocol version */
	unsigned long		flags;
	off64_t			cache_start_offset;
	bool			done;
	bool			cache;
	int			cfd;
#ifndef HAVE_MEMFD_CREATE
	char			cfile[sizeof(MSG_CACHE_FILE)];
#endif
};

struct tracecmd_tsync_protos {
	char **names;
};

struct tracecmd_msg_handle *
tracecmd_msg_handle_alloc(int fd, unsigned long flags);
int tracecmd_msg_handle_cache(struct tracecmd_msg_handle *msg_handle);

/* Closes the socket and frees the handle */
void tracecmd_msg_handle_close(struct tracecmd_msg_handle *msg_handle);

/* for clients */
int tracecmd_msg_send_init_data(struct tracecmd_msg_handle *msg_handle,
				unsigned int **client_ports);
int tracecmd_msg_data_send(struct tracecmd_msg_handle *msg_handle,
			       const char *buf, int size);
int tracecmd_msg_finish_sending_data(struct tracecmd_msg_handle *msg_handle);
int tracecmd_msg_flush_data(struct tracecmd_msg_handle *msg_handle);
int tracecmd_msg_send_close_msg(struct tracecmd_msg_handle *msg_handle);
int tracecmd_msg_send_close_resp_msg(struct tracecmd_msg_handle *msg_handle);
int tracecmd_msg_wait_close(struct tracecmd_msg_handle *msg_handle);
int tracecmd_msg_wait_close_resp(struct tracecmd_msg_handle *msg_handle);
int tracecmd_msg_cont(struct tracecmd_msg_handle *msg_handle);
int tracecmd_msg_wait(struct tracecmd_msg_handle *msg_handle);

/* for server */
int tracecmd_msg_initial_setting(struct tracecmd_msg_handle *msg_handle);
int tracecmd_msg_send_port_array(struct tracecmd_msg_handle *msg_handle,
				 unsigned *ports);
int tracecmd_msg_read_data(struct tracecmd_msg_handle *msg_handle, int ofd);
int tracecmd_msg_collect_data(struct tracecmd_msg_handle *msg_handle, int ofd);
bool tracecmd_msg_done(struct tracecmd_msg_handle *msg_handle);
void tracecmd_msg_set_done(struct tracecmd_msg_handle *msg_handle);
int tracecmd_msg_read_options(struct tracecmd_msg_handle *msg_handle,
			      struct tracecmd_output *handle);
int tracecmd_msg_send_options(struct tracecmd_msg_handle *msg_handle,
			      struct tracecmd_output *handle);

int tracecmd_msg_send_trace_req(struct tracecmd_msg_handle *msg_handle,
				int argc, char **argv, bool use_fifos,
				unsigned long long trace_id,
				struct tracecmd_tsync_protos *protos);
int tracecmd_msg_send_trace_proxy(struct tracecmd_msg_handle *msg_handle,
				  int argc, char **argv, bool use_fifos,
				  unsigned long long trace_id,
				  struct tracecmd_tsync_protos *protos,
				  unsigned int nr_cpus,
				  unsigned int siblings);
int tracecmd_msg_recv_trace_req(struct tracecmd_msg_handle *msg_handle,
				int *argc, char ***argv, bool *use_fifos,
				unsigned long long *trace_id,
				struct tracecmd_tsync_protos **protos);
int tracecmd_msg_recv_trace_proxy(struct tracecmd_msg_handle *msg_handle,
				  int *argc, char ***argv, bool *use_fifos,
				  unsigned long long *trace_id,
				  struct tracecmd_tsync_protos **protos,
				  unsigned int *cpus, unsigned int *siblings);

int tracecmd_msg_send_trace_resp(struct tracecmd_msg_handle *msg_handle,
				 int nr_cpus, int page_size,
				 unsigned int *ports, bool use_fifos,
				 unsigned long long trace_id,
				 const char *tsync_proto, unsigned int tsync_port);
int tracecmd_msg_recv_trace_resp(struct tracecmd_msg_handle *msg_handle,
				 int *nr_cpus, int *page_size,
				 unsigned int **ports, bool *use_fifos,
				 unsigned long long *trace_id,
				 char **tsync_proto,
				 unsigned int *tsync_port);

int tracecmd_msg_send_time_sync(struct tracecmd_msg_handle *msg_handle,
				char *sync_protocol, unsigned int sync_msg_id,
				unsigned int payload_size, char *payload);
int tracecmd_msg_recv_time_sync(struct tracecmd_msg_handle *msg_handle,
				char *sync_protocol,
				unsigned int *sync_msg_id,
				unsigned int *payload_size, char **payload);

enum tracecmd_clocks {
	TRACECMD_CLOCK_UNKNOWN	= 0,
	TRACECMD_CLOCK_LOCAL	= 1,
	TRACECMD_CLOCK_GLOBAL	= 1 << 1,
	TRACECMD_CLOCK_COUNTER	= 1 << 2,
	TRACECMD_CLOCK_UPTIME	= 1 << 3,
	TRACECMD_CLOCK_PERF	= 1 << 4,
	TRACECMD_CLOCK_MONO	= 1 << 5,
	TRACECMD_CLOCK_MONO_RAW	= 1 << 6,
	TRACECMD_CLOCK_BOOT	= 1 << 7,
	TRACECMD_CLOCK_X86_TSC	= 1 << 8
};

enum tracecmd_clocks tracecmd_clock_str2id(const char *clock);
const char *tracecmd_clock_id2str(enum tracecmd_clocks clock);

/* --- Timestamp synchronization --- */

struct tracecmd_time_sync;
#define TRACECMD_TSYNC_PNAME_LENGTH	16
#define TRACECMD_TSYNC_PROTO_NONE	"none"

enum{
	TRACECMD_TIME_SYNC_CMD_PROBE	= 1,
	TRACECMD_TIME_SYNC_CMD_STOP	= 2,
};

enum tracecmd_time_sync_role {
	TRACECMD_TIME_SYNC_ROLE_HOST	= (1 << 0),
	TRACECMD_TIME_SYNC_ROLE_GUEST	= (1 << 1),
	TRACECMD_TIME_SYNC_ROLE_CLIENT	= (1 << 2),
	TRACECMD_TIME_SYNC_ROLE_SERVER	= (1 << 3),
};

/* Timestamp synchronization flags */
#define TRACECMD_TSYNC_FLAG_INTERPOLATE	0x1

void tracecmd_tsync_init(void);
int tracecmd_tsync_proto_getall(struct tracecmd_tsync_protos **protos, const char *clock, int role);
bool tsync_proto_is_supported(const char *proto_name);
struct tracecmd_time_sync *
tracecmd_tsync_with_host(int fd, const char *proto, const char *clock,
			 int remote_id, int local_id);
int tracecmd_tsync_with_host_stop(struct tracecmd_time_sync *tsync);
struct tracecmd_time_sync *
tracecmd_tsync_with_guest(unsigned long long trace_id, int loop_interval,
			  unsigned int fd, int guest_pid,
			  int guest_cpus, const char *proto_name, const char *clock);
int tracecmd_tsync_with_guest_stop(struct tracecmd_time_sync *tsync);
int tracecmd_tsync_get_offsets(struct tracecmd_time_sync *tsync, int cpu,
			       int *count, long long **ts,
			       long long **offsets, long long **scalings, long long **frac);
const char *tracecmd_tsync_get_proto(const struct tracecmd_tsync_protos *protos,
			 const char *clock, enum tracecmd_time_sync_role role);
void tracecmd_tsync_free(struct tracecmd_time_sync *tsync);
int tracecmd_write_guest_time_shift(struct tracecmd_output *handle,
				    struct tracecmd_time_sync *tsync);

/* --- Compression --- */
struct tracecmd_compress_chunk {
	unsigned int		size;
	unsigned int		zsize;
	off64_t			zoffset;
	off64_t			offset;
};
struct tracecmd_compression;
struct tracecmd_compression_proto {
	int weight;
	const char *name;
	const char *version;
	int (*compress)(void *ctx, const void *in, int in_bytes, void *out, int out_bytes);
	int (*uncompress)(void *ctx, const void *in, int in_bytes, void *out, int out_bytes);
	unsigned int (*compress_size)(void *ctx, unsigned int bytes);
	bool (*is_supported)(const char *name, const char *version);
	void *(*new_context)(void);
	void (*free_context)(void *ctx);
};

struct tracecmd_compression *tracecmd_compress_alloc(const char *name, const char *version,
						     int fd, struct tep_handle *tep,
						     struct tracecmd_msg_handle *msg_handle);
void tracecmd_compress_destroy(struct tracecmd_compression *handle);
int tracecmd_compress_block(struct tracecmd_compression *handle);
int tracecmd_uncompress_block(struct tracecmd_compression *handle);
void tracecmd_compress_reset(struct tracecmd_compression *handle);
int tracecmd_compress_buffer_read(struct tracecmd_compression *handle, char *dst, int len);
int tracecmd_compress_pread(struct tracecmd_compression *handle, char *dst, int len, off_t offset);
int tracecmd_compress_buffer_write(struct tracecmd_compression *handle,
				   const void *data, unsigned long long size);
off64_t tracecmd_compress_lseek(struct tracecmd_compression *handle, off64_t offset, int whence);
int tracecmd_compress_proto_get_name(struct tracecmd_compression *compress,
				     const char **name, const char **version);
bool tracecmd_compress_is_supported(const char *name, const char *version);
int tracecmd_compress_protos_get(char ***names, char ***versions);
int tracecmd_compress_proto_register(struct tracecmd_compression_proto *proto);
int tracecmd_compress_copy_from(struct tracecmd_compression *handle, int fd, int chunk_size,
				unsigned long long *read_size, unsigned long long *write_size);
int tracecmd_uncompress_copy_to(struct tracecmd_compression *handle, int fd,
				unsigned long long *read_size, unsigned long long *write_size);
int tracecmd_uncompress_chunk(struct tracecmd_compression *handle,
			      struct tracecmd_compress_chunk *chunk, char *data);
int tracecmd_load_chunks_info(struct tracecmd_compression *handle,
			      struct tracecmd_compress_chunk **chunks_info);
/* --- Plugin handling --- */
extern struct tep_plugin_option trace_ftrace_options[];

char **trace_util_find_plugin_files(const char *suffix);
void trace_util_free_plugin_files(char **files);

/* Used for trace-cmd list */
void tracecmd_ftrace_load_options(void);

/* event hooks */

struct hook_list {
	struct hook_list	*next;
	struct buffer_instance	*instance;
	const char		*hook;
	char			*str;
	char			*start_system;
	char			*start_event;
	char			*start_match;
	char			*end_system;
	char			*end_event;
	char			*end_match;
	char			*pid;
	int			migrate;
	int			global;
	int			stack;
};

struct hook_list *tracecmd_create_event_hook(const char *arg);
void tracecmd_free_hooks(struct hook_list *hooks);

void tracecmd_plog(const char *fmt, ...);
void tracecmd_plog_error(const char *fmt, ...);
int tracecmd_set_logfile(char *logfile);

/* --- System --- */
unsigned long long tracecmd_generate_traceid(void);
int tracecmd_count_cpus(void);

/* --- Hack! --- */
int tracecmd_blk_hack(struct tracecmd_input *handle);

/* --- Stack tracer functions --- */
int tracecmd_stack_tracer_status(int *status);

/* --- Debugging --- */
struct kbuffer *tracecmd_record_kbuf(struct tracecmd_input *handle,
				     struct tep_record *record);
void *tracecmd_record_page(struct tracecmd_input *handle,
			   struct tep_record *record);
void *tracecmd_record_offset(struct tracecmd_input *handle,
			     struct tep_record *record);
#ifdef PERF

#include <linux/perf_event.h>

/* trace-cmd Perf */
struct trace_perf {
	int fd;
	int cpu;
	int pid;
	int pages;
	struct perf_event_attr pe;
	struct perf_event_mmap_page *mmap;
};
int trace_perf_init(struct trace_perf *perf, int pages, int cpu, int pid);
int trace_perf_open(struct trace_perf *perf);
void trace_perf_close(struct trace_perf *perf);
#endif

#endif /* _TRACE_CMD_PRIVATE_H */
