/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2009, 2010 Red Hat Inc, Steven Rostedt <srostedt@redhat.com>
 *
 */
#ifndef __TRACE_LOCAL_H
#define __TRACE_LOCAL_H

#include <sys/types.h>
#include <dirent.h>	/* for DIR */
#include <ctype.h>	/* for isdigit() */
#include <errno.h>
#include <limits.h>

#include "trace-cmd-private.h"
#include "event-utils.h"

#define TRACE_AGENT_DEFAULT_PORT	823

#define DEFAULT_INPUT_FILE	"trace.dat"
#define GUEST_PIPE_NAME		"trace-pipe-cpu"
#define GUEST_DIR_FMT		"/var/lib/trace-cmd/virt/%s"
#define GUEST_FIFO_FMT		GUEST_DIR_FMT "/" GUEST_PIPE_NAME "%d"
#define VIRTIO_FIFO_FMT		"/dev/virtio-ports/" GUEST_PIPE_NAME "%d"

/* fix stupid glib guint64 typecasts and printf formats */
typedef unsigned long long u64;

struct buffer_instance;

#define __printf(a, b) __attribute__((format(printf,a,b)))

__printf(1,2)
void warning(const char *fmt, ...);

/* for local shared information with trace-cmd executable */

void usage(char **argv);

extern int silence_warnings;
extern int show_status;

int trace_set_verbose(char *level);

enum port_type {
	USE_UDP			= 0,	/* Default setting */
	USE_TCP,
	USE_VSOCK
};

struct pid_record_data {
	int			pid;
	int			brass[2];
	int			cpu;
	int			closed;
	struct tracecmd_input	*stream;
	struct buffer_instance	*instance;
	struct tep_record	*record;
};

void show_file(const char *name);

struct tracecmd_input *read_trace_header(const char *file, int flags);
int read_trace_files(void);

void trace_record(int argc, char **argv);

void trace_stop(int argc, char **argv);

void trace_restart(int argc, char **argv);

void trace_reset(int argc, char **argv);

void trace_start(int argc, char **argv);

void trace_set(int argc, char **argv);

void trace_extract(int argc, char **argv);

void trace_stream(int argc, char **argv);

void trace_profile(int argc, char **argv);

void trace_report(int argc, char **argv);

void trace_split(int argc, char **argv);

void trace_listen(int argc, char **argv);

void trace_agent(int argc, char **argv);

void trace_setup_guest(int argc, char **argv);

void trace_restore(int argc, char **argv);

void trace_clear(int argc, char **argv);

void trace_check_events(int argc, char **argv);

void trace_stack(int argc, char **argv);

void trace_option(int argc, char **argv);

void trace_hist(int argc, char **argv);

void trace_snapshot(int argc, char **argv);

void trace_mem(int argc, char **argv);

void trace_stat(int argc, char **argv);

void trace_show(int argc, char **argv);

void trace_list(int argc, char **argv);

void trace_usage(int argc, char **argv);

void trace_dump(int argc, char **argv);

void trace_convert(int argc, char **argv);

int trace_record_agent(struct tracecmd_msg_handle *msg_handle,
		       int cpus, int *fds,
		       int argc, char **argv,
		       bool use_fifos, struct tracecmd_time_sync *tsync,
		       unsigned long long trace_id, int rcid, const char *host);

struct hook_list;

void trace_init_profile(struct tracecmd_input *handle, struct hook_list *hooks,
			int global);
int do_trace_profile(void);
void trace_profile_set_merge_like_comms(void);

struct tracecmd_input *
trace_stream_init(struct buffer_instance *instance, int cpu, int fd, int cpus,
		  struct hook_list *hooks,
		  tracecmd_handle_init_func handle_init, int global);
int trace_stream_read(struct pid_record_data *pids, int nr_pids, struct timeval *tv);

void trace_show_data(struct tracecmd_input *handle, struct tep_record *record);

/* --- event interation --- */

/*
 * Use this to iterate through the event directories
 */


enum event_process {
	PROCESSED_NONE,
	PROCESSED_EVENT,
	PROCESSED_SYSTEM
};

enum process_type {
	PROCESS_EVENT,
	PROCESS_SYSTEM
};

struct event_iter {
	DIR *system_dir;
	DIR *event_dir;
	struct dirent *system_dent;
	struct dirent *event_dent;
};

enum event_iter_type {
	EVENT_ITER_NONE,
	EVENT_ITER_SYSTEM,
	EVENT_ITER_EVENT
};

struct event_iter *trace_event_iter_alloc(const char *path);
enum event_iter_type trace_event_iter_next(struct event_iter *iter,
					   const char *path, const char *system);
void trace_event_iter_free(struct event_iter *iter);

char *append_file(const char *dir, const char *name);
char *get_file_content(const char *file);

char *strstrip(char *str);

/* --- instance manipulation --- */

enum buffer_instance_flags {
	BUFFER_FL_KEEP		= 1 << 0,
	BUFFER_FL_PROFILE	= 1 << 1,
	BUFFER_FL_GUEST		= 1 << 2,
	BUFFER_FL_AGENT		= 1 << 3,
	BUFFER_FL_HAS_CLOCK	= 1 << 4,
	BUFFER_FL_TSC2NSEC	= 1 << 5,
	BUFFER_FL_NETWORK	= 1 << 6,
	BUFFER_FL_PROXY		= 1 << 7,
};

struct func_list {
	struct func_list *next;
	const char *func;
	const char *mod;
};

struct pid_addr_maps {
	struct pid_addr_maps		*next;
	struct tracecmd_proc_addr_map	*lib_maps;
	unsigned int			nr_lib_maps;
	char				*proc_name;
	int				pid;
};

struct opt_list {
	struct opt_list *next;
	const char	*option;
};

struct filter_pids {
	struct filter_pids *next;
	int pid;
	int exclude;
};

struct tsc_nsec {
	int mult;
	int shift;
	unsigned long long offset;
};

struct buffer_instance {
	struct buffer_instance	*next;
	char			*name;
	struct tracefs_instance	*tracefs;
	unsigned long long	trace_id;
	char			*cpumask;
	char			*output_file;
	const char		*temp_dir;
	char			*temp_file;
	struct event_list	*events;
	struct event_list	**event_next;
	bool			delete;

	struct event_list	*sched_switch_event;
	struct event_list	*sched_wakeup_event;
	struct event_list	*sched_wakeup_new_event;

	const char		*plugin;
	char			*filter_mod;
	struct func_list	*filter_funcs;
	struct func_list	*notrace_funcs;

	struct opt_list		*options;
	struct filter_pids	*filter_pids;
	struct filter_pids	*process_pids;
	char			*common_pid_filter;
	int			nr_filter_pids;
	int			len_filter_pids;
	int			nr_process_pids;
	bool			ptrace_child;

	int			have_set_event_pid;
	int			have_event_fork;
	int			have_func_fork;
	int			get_procmap;

	const char		*clock;
	unsigned int		*client_ports;

	struct trace_seq	*s_save;
	struct trace_seq	*s_print;

	struct tracecmd_input	*handle;

	struct tracecmd_msg_handle *msg_handle;
	struct tracecmd_output *network_handle;
	const char		*host;

	struct pid_addr_maps	*pid_maps;

	char			*max_graph_depth;

	int			flags;
	int			tracing_on_init_val;
	int			tracing_on_fd;
	int			buffer_size;
	int			cpu_count;

	int			proxy_fd;

	int			argc;
	char			**argv;

	struct addrinfo		*result;
	unsigned int		cid;
	unsigned int		port;
	int			*fds;
	bool			use_fifos;

	enum port_type		port_type;	/* Default to USE_UDP (zero) */
	int			tsync_loop_interval;
	struct tracecmd_time_sync *tsync;
};

void init_top_instance(void);

extern struct buffer_instance top_instance;
extern struct buffer_instance *buffer_instances;
extern struct buffer_instance *first_instance;

#define for_each_instance(i) for (i = buffer_instances; i; i = (i)->next)
#define for_all_instances(i) for (i = first_instance; i; \
				  i = i == &top_instance ? buffer_instances : (i)->next)

#define is_agent(instance)	((instance)->flags & BUFFER_FL_AGENT)
#define is_guest(instance)	((instance)->flags & BUFFER_FL_GUEST)
#define is_proxy(instance)	((instance)->flags & BUFFER_FL_PROXY)
#define is_network(instance)	((instance)->flags & BUFFER_FL_NETWORK)
#define is_proxy_server(instance)					\
	((instance)->msg_handle &&					\
	 (instance)->msg_handle->flags & TRACECMD_MSG_FL_PROXY)

#define START_PORT_SEARCH 1500
#define MAX_PORT_SEARCH 6000

struct sockaddr_storage;

int trace_net_make(int port, enum port_type type);
int trace_net_search(int start_port, int *sfd, enum port_type type);
int trace_net_print_connection(int fd);
bool trace_net_cmp_connection(struct sockaddr_storage *addr, const char *name);
bool trace_net_cmp_connection_fd(int fd, const char *name);

struct buffer_instance *allocate_instance(const char *name);
void add_instance(struct buffer_instance *instance, int cpu_count);
void update_first_instance(struct buffer_instance *instance, int topt);

void show_instance_file(struct buffer_instance *instance, const char *name);
void show_options(const char *prefix, struct buffer_instance *buffer);

struct trace_guest {
	struct tracefs_instance *instance;
	char *name;
	unsigned long long trace_id;
	int cid;
	int pid;
	int cpu_max;
	int *cpu_pid;
	int *task_pids;
};
struct trace_guest *trace_get_guest(unsigned int cid, const char *name);
bool trace_have_guests_pid(void);
void read_qemu_guests(void);
int get_guest_pid(unsigned int guest_cid);
int get_guest_vcpu_pid(unsigned int guest_cid, unsigned int guest_vcpu);
void trace_add_guest_info(struct tracecmd_output *handle, struct buffer_instance *instance);

struct tracecmd_time_sync *
trace_tsync_as_host(int fd, unsigned long long trace_id,
		    int loop_interval, int guest_id,
		    int guest_cpus, const char *proto_name,
		    const char *clock);

struct tracecmd_time_sync *
trace_tsync_as_guest(int fd, const char *tsync_proto, const char *clock,
	       unsigned int remote_id, unsigned int local_id);

/* moved from trace-cmd.h */
void tracecmd_remove_instances(void);
int tracecmd_add_event(const char *event_str, int stack);
void tracecmd_enable_events(void);
void tracecmd_disable_all_tracing(int disable_tracer);
void tracecmd_disable_tracing(void);
void tracecmd_enable_tracing(void);
void tracecmd_stat_cpu(struct trace_seq *s, int cpu);

int tracecmd_host_tsync(struct buffer_instance *instance,
			 unsigned int tsync_port);
void tracecmd_host_tsync_complete(struct buffer_instance *instance);
const char *tracecmd_guest_tsync(struct tracecmd_tsync_protos *tsync_protos,
				 char *clock, unsigned int *tsync_port,
				 pthread_t *thr_id);

int trace_make_vsock(unsigned int port);
int trace_get_vsock_port(int sd, unsigned int *port);
int trace_open_vsock(unsigned int cid, unsigned int port);

int get_local_cid(unsigned int *cid);

char *trace_get_guest_file(const char *file, const char *guest);

#ifdef VSOCK
int trace_vsock_open(unsigned int cid, unsigned int port);
int trace_vsock_make(unsigned int port);
int trace_vsock_make_any(void);
int get_vsocket_params(int fd, unsigned int *lcid, unsigned int *rcid);
int trace_vsock_get_port(int sd, unsigned int *port);
bool trace_vsock_can_splice_read(void);
int trace_vsock_local_cid(void);
int trace_vsock_print_connection(int fd);
#else
static inline int trace_vsock_open(unsigned int cid, unsigned int port)
{
	return -ENOTSUP;
}

static inline int trace_vsock_make(unsigned int port)
{
	return -ENOTSUP;

}

static inline int trace_vsock_make_any(void)
{
	return -ENOTSUP;

}

static inline int get_vsocket_params(int fd, unsigned int *lcid, unsigned int *rcid)
{
	return -ENOTSUP;
}

static inline int trace_vsock_get_port(int sd, unsigned int *port)
{
	return -ENOTSUP;
}

static inline bool trace_vsock_can_splice_read(void)
{
	return false;
}

static inline int trace_vsock_local_cid(void)
{
	return -ENOTSUP;
}
static inline int trace_vsock_print_connection(int fd)
{
	return -1;
}
#endif /* VSOCK */

/* No longer in event-utils.h */
__printf(1,2)
void __noreturn die(const char *fmt, ...); /* Can be overriden */
void *malloc_or_die(unsigned int size); /* Can be overridden */
__printf(1,2)
void __noreturn __die(const char *fmt, ...);
void __noreturn _vdie(const char *fmt, va_list ap);

static inline bool is_digits(const char *s)
{
	for (; *s; s++)
		if (!isdigit(*s))
			return false;
	return true;
}

bool trace_tsc2nsec_is_supported(void);

#endif /* __TRACE_LOCAL_H */
