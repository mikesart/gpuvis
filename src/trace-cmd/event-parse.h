/*
 * Copyright (C) 2009, 2010 Red Hat Inc, Steven Rostedt <srostedt@redhat.com>
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License (not later!)
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not,  see <http://www.gnu.org/licenses>
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */
#ifndef _PARSE_EVENTS_H
#define _PARSE_EVENTS_H

#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef __GNUC__
#define ATTRIBUTE_PRINTF( _x, _y ) __attribute__( ( __format__( __printf__, _x, _y ) ) )

#ifndef __maybe_unused
#define __maybe_unused __attribute__((unused))
#endif
#else
#define ATTRIBUTE_PRINTF( _x, _y )

#ifndef __maybe_unused
#define __maybe_unused
#endif
#endif


/* ----------------------- trace_seq ----------------------- */


#ifndef TRACE_SEQ_BUF_SIZE
#define TRACE_SEQ_BUF_SIZE 4096
#endif

struct pevent_record {
	unsigned long long	ts;
	unsigned long long	offset;
	long long		missed_events;	/* buffer dropped events before */
	int			record_size;	/* size of binary record */
	int			size;		/* size of data */
	void			*data;
	int			cpu;
	int			ref_count;
	int			locked;		/* Do not free, even if ref_count is zero */
	void			*priv;
};

/*
 * Trace sequences are used to allow a function to call several other functions
 * to create a string of data to use (up to a max of PAGE_SIZE).
 */

struct trace_seq {
	char			*buffer;
	unsigned int		buffer_size;
	unsigned int		len;

	char			buf[TRACE_SEQ_BUF_SIZE];
};

void trace_seq_init(struct trace_seq *s);
void trace_seq_reset(struct trace_seq *s);
void trace_seq_destroy(struct trace_seq *s);

extern int trace_seq_printf(struct trace_seq *s, const char *fmt, ...) ATTRIBUTE_PRINTF( 2, 3 );
extern int trace_seq_vprintf(struct trace_seq *s, const char *fmt, va_list args) ATTRIBUTE_PRINTF( 2, 0 );

extern int trace_seq_puts(struct trace_seq *s, const char *str);
extern int trace_seq_putc(struct trace_seq *s, unsigned char c);

int trace_seq_put_sval(struct trace_seq *s, long long val);
int trace_seq_put_uval(struct trace_seq *s, unsigned long long val);

extern void trace_seq_terminate(struct trace_seq *s);

extern int trace_seq_do_fprintf(struct trace_seq *s, FILE *fp);
extern int trace_seq_do_printf(struct trace_seq *s);


/* ----------------------- pevent ----------------------- */

struct pevent;
struct event_format;

typedef int (*pevent_event_handler_func)(struct trace_seq *s,
					 struct pevent_record *record,
					 struct event_format *event,
					 void *context);

#define NSECS_PER_SEC		1000000000ULL
#define NSECS_PER_USEC		1000ULL

#define USECS_PER_SEC		1000000ULL

enum format_flags {
	FIELD_IS_ARRAY		= 1,
	FIELD_IS_POINTER	= 2,
	FIELD_IS_SIGNED		= 4,
	FIELD_IS_STRING		= 8,
	FIELD_IS_DYNAMIC	= 16,
	FIELD_IS_LONG		= 32,
	FIELD_IS_FLAG		= 64,
	FIELD_IS_SYMBOLIC	= 128,
};

struct format_field {
	struct format_field	*next;
	struct event_format	*event;
	char			*type;
	char			*name;
	char			*alias;
	int			offset;
	int			size;
	unsigned int		arraylen;
	unsigned int		elementsize;
	unsigned long		flags;
};

struct format {
	int			nr_common;
	int			nr_fields;
	struct format_field	*common_fields;
	struct format_field	*fields;
};

struct print_arg_atom {
	char			*atom;
};

struct print_arg_string {
	char			*string;
	int			offset;
};

struct print_arg_bitmask {
	char			*bitmask;
	int			offset;
};

struct print_arg_field {
	char			*name;
	struct format_field	*field;
};

struct print_flag_sym {
	struct print_flag_sym	*next;
	char			*value;
	char			*str;
};

struct print_arg_typecast {
	char 			*type;
	struct print_arg	*item;
};

struct print_arg_flags {
	struct print_arg	*field;
	char			*delim;
	struct print_flag_sym	*flags;
};

struct print_arg_symbol {
	struct print_arg	*field;
	struct print_flag_sym	*symbols;
};

struct print_arg_hex {
	struct print_arg	*field;
	struct print_arg	*size;
};

struct print_arg_int_array {
	struct print_arg	*field;
	struct print_arg	*count;
	struct print_arg	*el_size;
};

struct print_arg_dynarray {
	struct format_field	*field;
	struct print_arg	*index;
};

struct print_arg;

struct print_arg_op {
	char			*op;
	int			prio;
	struct print_arg	*left;
	struct print_arg	*right;
};

struct pevent_function_handler;

struct print_arg_func {
	struct pevent_function_handler	*func;
	struct print_arg		*args;
};

enum print_arg_type {
	PRINT_NULL,
	PRINT_ATOM,
	PRINT_FIELD,
	PRINT_FLAGS,
	PRINT_SYMBOL,
	PRINT_HEX,
	PRINT_INT_ARRAY,
	PRINT_TYPE,
	PRINT_STRING,
	PRINT_BSTRING,
	PRINT_DYNAMIC_ARRAY,
	PRINT_OP,
	PRINT_FUNC,
	PRINT_BITMASK,
	PRINT_DYNAMIC_ARRAY_LEN,
	PRINT_HEX_STR,
};

struct print_arg {
	struct print_arg		*next;
	enum print_arg_type		type;
	union {
		struct print_arg_atom		atom;
		struct print_arg_field		field;
		struct print_arg_typecast	typecast;
		struct print_arg_flags		flags;
		struct print_arg_symbol		symbol;
		struct print_arg_hex		hex;
		struct print_arg_int_array	int_array;
		struct print_arg_func		func;
		struct print_arg_string		string;
		struct print_arg_bitmask	bitmask;
		struct print_arg_op		op;
		struct print_arg_dynarray	dynarray;
	};
};

struct print_fmt {
	char			*format;
	struct print_arg	*args;
};

struct event_format {
	struct pevent		*pevent;
	char			*name;
	int			id;
	int			flags;
	struct format		format;
	struct print_fmt	print_fmt;
	char			*system;
	pevent_event_handler_func handler;
	void			*context;
};

enum {
	EVENT_FL_ISFTRACE	= 0x01,
	EVENT_FL_ISPRINT	= 0x02,
	EVENT_FL_ISBPRINT	= 0x04,
	EVENT_FL_ISFUNCENT	= 0x10,
	EVENT_FL_ISFUNCRET	= 0x20,
	EVENT_FL_NOHANDLE	= 0x40,
	EVENT_FL_PRINTRAW	= 0x80,

	EVENT_FL_FAILED		= 0x80000000
};

enum event_sort_type {
	EVENT_SORT_ID,
	EVENT_SORT_NAME,
	EVENT_SORT_SYSTEM,
};

enum event_type {
	EVENT_ERROR,
	EVENT_NONE,
	EVENT_SPACE,
	EVENT_NEWLINE,
	EVENT_OP,
	EVENT_DELIM,
	EVENT_ITEM,
	EVENT_DQUOTE,
	EVENT_SQUOTE,
};

typedef unsigned long long (*pevent_func_handler)(struct trace_seq *s,
					     unsigned long long *args);

enum pevent_func_arg_type {
	PEVENT_FUNC_ARG_VOID,
	PEVENT_FUNC_ARG_INT,
	PEVENT_FUNC_ARG_LONG,
	PEVENT_FUNC_ARG_STRING,
	PEVENT_FUNC_ARG_PTR,
	PEVENT_FUNC_ARG_MAX_TYPES
};

enum pevent_flag {
	PEVENT_NSEC_OUTPUT		= 1,	/* output in NSECS */
	PEVENT_DISABLE_SYS_PLUGINS	= 1 << 1,
	PEVENT_DISABLE_PLUGINS		= 1 << 2,
	PEVENT_SHOW_TGIDS            = 1 << 3,
};

#define PEVENT_ERRORS 							      \
	_PE(MEM_ALLOC_FAILED,	"failed to allocate memory"),		      \
	_PE(PARSE_EVENT_FAILED,	"failed to parse event"),		      \
	_PE(READ_ID_FAILED,	"failed to read event id"),		      \
	_PE(READ_FORMAT_FAILED,	"failed to read event format"),		      \
	_PE(READ_PRINT_FAILED,	"failed to read event print fmt"), 	      \
	_PE(OLD_FTRACE_ARG_FAILED,"failed to allocate field name for ftrace"),\
	_PE(INVALID_ARG_TYPE,	"invalid argument type"),		      \
	_PE(INVALID_EXP_TYPE,	"invalid expression type"),		      \
	_PE(INVALID_OP_TYPE,	"invalid operator type"),		      \
	_PE(INVALID_EVENT_NAME,	"invalid event name"),			      \
	_PE(EVENT_NOT_FOUND,	"no event found"),			      \
	_PE(SYNTAX_ERROR,	"syntax error"),			      \
	_PE(ILLEGAL_RVALUE,	"illegal rvalue"),			      \
	_PE(ILLEGAL_LVALUE,	"illegal lvalue for string comparison"),      \
	_PE(INVALID_REGEX,	"regex did not compute"),		      \
	_PE(ILLEGAL_STRING_CMP,	"illegal comparison for string"), 	      \
	_PE(ILLEGAL_INTEGER_CMP,"illegal comparison for integer"), 	      \
	_PE(REPARENT_NOT_OP,	"cannot reparent other than OP"),	      \
	_PE(REPARENT_FAILED,	"failed to reparent filter OP"),	      \
	_PE(BAD_FILTER_ARG,	"bad arg in filter tree"),		      \
	_PE(UNEXPECTED_TYPE,	"unexpected type (not a value)"),	      \
	_PE(ILLEGAL_TOKEN,	"illegal token"),			      \
	_PE(INVALID_PAREN,	"open parenthesis cannot come here"), 	      \
	_PE(UNBALANCED_PAREN,	"unbalanced number of parenthesis"),	      \
	_PE(UNKNOWN_TOKEN,	"unknown token"),			      \
	_PE(FILTER_NOT_FOUND,	"no filter found"),			      \
	_PE(NOT_A_NUMBER,	"must have number field"),		      \
	_PE(NO_FILTER,		"no filters exists"),			      \
	_PE(FILTER_MISS,	"record does not match to filter")

#undef _PE
#define _PE(__code, __str) PEVENT_ERRNO__ ## __code
enum pevent_errno {
	PEVENT_ERRNO__SUCCESS			= 0,
	PEVENT_ERRNO__FILTER_MATCH		= PEVENT_ERRNO__SUCCESS,

	/*
	 * Choose an arbitrary negative big number not to clash with standard
	 * errno since SUS requires the errno has distinct positive values.
	 * See 'Issue 6' in the link below.
	 *
	 * http://pubs.opengroup.org/onlinepubs/9699919799/basedefs/errno.h.html
	 */
	__PEVENT_ERRNO__START			= -100000,

	PEVENT_ERRORS,

	__PEVENT_ERRNO__END,
};
#undef _PE

struct cmdline;
struct cmdline_list;
struct func_map;
struct func_list;
struct event_handler;
struct func_resolver;

typedef char *(pevent_func_resolver_t)(void *priv,
				       unsigned long long *addrp, char **modp);

struct cmdline {
	char *comm;
	int pid;
};

struct cmdline_list {
	struct cmdline_list	*next;
	char			*comm;
	int			pid;
};

struct pevent {
	int ref_count;

	int header_page_ts_offset;
	int header_page_ts_size;
	int header_page_size_offset;
	int header_page_size_size;
	int header_page_data_offset;
	int header_page_data_size;
	int header_page_overwrite;

	int file_bigendian;
	int host_bigendian;

	int latency_format;

	int old_format;

	int cpus;
	int long_size;
	int page_size;

	struct cmdline *cmdlines;
	struct cmdline_list *cmdlist;
	int cmdline_count;

	struct func_map *func_map;
	struct func_resolver *func_resolver;
	struct func_list *funclist;
	unsigned int func_count;

	struct printk_map *printk_map;
	struct printk_list *printklist;
	unsigned int printk_count;

	int *tgid_map;
	unsigned int tgid_count;

	struct event_format **events;
	int nr_events;
	struct event_format **sort_events;
	enum event_sort_type last_type;

	int type_offset;
	int type_size;

	int pid_offset;
	int pid_size;

 	int pc_offset;
	int pc_size;

	int flags_offset;
	int flags_size;

	int ld_offset;
	int ld_size;

	int print_raw;

	int test_filters;

	int flags;

	struct format_field *bprint_ip_field;
	struct format_field *bprint_fmt_field;
	struct format_field *bprint_buf_field;

	struct event_handler *handlers;
	struct pevent_function_handler *func_handlers;

	int parsing_failures;

	/* cache */
	struct event_format *last_event;

	char *trace_clock;
};

static inline void pevent_set_flag(struct pevent *pevent, int flag)
{
	pevent->flags |= flag;
}

static inline unsigned short
__data2host2(struct pevent *pevent, unsigned short data)
{
	unsigned short swap;

	if (pevent->host_bigendian == pevent->file_bigendian)
		return data;

	swap = ((data & 0xffULL) << 8) |
		((data & (0xffULL << 8)) >> 8);

	return swap;
}

static inline unsigned int
__data2host4(struct pevent *pevent, unsigned int data)
{
	unsigned int swap;

	if (pevent->host_bigendian == pevent->file_bigendian)
		return data;

	swap = ((data & 0xffULL) << 24) |
		((data & (0xffULL << 8)) << 8) |
		((data & (0xffULL << 16)) >> 8) |
		((data & (0xffULL << 24)) >> 24);

	return swap;
}

static inline unsigned long long
__data2host8(struct pevent *pevent, unsigned long long data)
{
	unsigned long long swap;

	if (pevent->host_bigendian == pevent->file_bigendian)
		return data;

	swap = ((data & 0xffULL) << 56) |
		((data & (0xffULL << 8)) << 40) |
		((data & (0xffULL << 16)) << 24) |
		((data & (0xffULL << 24)) << 8) |
		((data & (0xffULL << 32)) >> 8) |
		((data & (0xffULL << 40)) >> 24) |
		((data & (0xffULL << 48)) >> 40) |
		((data & (0xffULL << 56)) >> 56);

	return swap;
}

#define data2host2(pevent, ptr)		__data2host2(pevent, *(unsigned short *)(ptr))
#define data2host4(pevent, ptr)		__data2host4(pevent, *(unsigned int *)(ptr))

static inline unsigned long long data2host8(struct pevent *pevent, const void *ptr)
{
    unsigned long long __val;

    memcpy(&__val, ptr, sizeof(unsigned long long));
    return __data2host8(pevent, __val);
}

static inline int traceevent_host_bigendian(void)
{
	unsigned char str[] = { 0x1, 0x2, 0x3, 0x4 };
	unsigned int val;

	memcpy(&val, str, 4);
	return val == 0x01020304;
}

/* taken from kernel/trace/trace.h */
enum trace_flag_type {
    TRACE_FLAG_IRQS_OFF		= 0x01, // interrupts were disabled
    TRACE_FLAG_IRQS_NOSUPPORT	= 0x02,
    TRACE_FLAG_NEED_RESCHED		= 0x04,
    TRACE_FLAG_HARDIRQ		= 0x08, // inside an interrupt handler
    TRACE_FLAG_SOFTIRQ		= 0x10, // inside a softirq handler
};

int pevent_set_function_resolver(struct pevent *pevent,
				 pevent_func_resolver_t *func, void *priv);
void pevent_reset_function_resolver(struct pevent *pevent);
int pevent_register_comm(struct pevent *pevent, const char *comm, int pid);
int pevent_register_tgid(struct pevent *pevent, int tgid, int pid);
int pevent_register_trace_clock(struct pevent *pevent, const char *trace_clock);
int pevent_register_function(struct pevent *pevent, char *name,
			     unsigned long long addr, char *mod);
int pevent_register_print_string(struct pevent *pevent, const char *fmt,
				 unsigned long long addr);
int pevent_pid_is_registered(struct pevent *pevent, int pid);

void pevent_print_event_task(struct pevent *pevent, struct trace_seq *s,
			     struct event_format *event,
			     struct pevent_record *record);
void pevent_print_event_time(struct pevent *pevent, struct trace_seq *s,
			     struct event_format *event,
			     struct pevent_record *record,
			     bool use_trace_clock);
void pevent_print_event_data(struct pevent *pevent, struct trace_seq *s,
			     struct event_format *event,
			     struct pevent_record *record);
void pevent_print_event(struct pevent *pevent, struct trace_seq *s,
			struct pevent_record *record, bool use_trace_clock);

int pevent_parse_header_page(struct pevent *pevent, char *buf, unsigned long size,
			     int long_size);

enum pevent_errno pevent_parse_event(struct pevent *pevent, const char *buf,
				     unsigned long size, const char *sys);
enum pevent_errno pevent_parse_format(struct pevent *pevent,
				      struct event_format **eventp,
				      const char *buf,
				      unsigned long size, const char *sys);
void pevent_free_format(struct event_format *event);
void pevent_free_format_field(struct format_field *field);

void *pevent_get_field_raw(struct trace_seq *s, struct event_format *event,
			   const char *name, struct pevent_record *record,
			   int *len, int err);

int pevent_get_field_val(struct trace_seq *s, struct event_format *event,
			 const char *name, struct pevent_record *record,
			 unsigned long long *val, int err);
int pevent_get_common_field_val(struct trace_seq *s, struct event_format *event,
				const char *name, struct pevent_record *record,
				unsigned long long *val, int err);
int pevent_get_any_field_val(struct trace_seq *s, struct event_format *event,
			     const char *name, struct pevent_record *record,
			     unsigned long long *val, int err);

int pevent_print_num_field(struct trace_seq *s, const char *fmt,
			   struct event_format *event, const char *name,
			   struct pevent_record *record, int err);

int pevent_print_func_field(struct trace_seq *s, const char *fmt,
			   struct event_format *event, const char *name,
			   struct pevent_record *record, int err);

int pevent_register_event_handler(struct pevent *pevent, int id,
				  const char *sys_name, const char *event_name,
				  pevent_event_handler_func func, void *context);
int pevent_unregister_event_handler(struct pevent *pevent, int id,
				    const char *sys_name, const char *event_name,
				    pevent_event_handler_func func, void *context);
int pevent_register_print_function(struct pevent *pevent,
				   pevent_func_handler func,
				   enum pevent_func_arg_type ret_type,
				   char *name, ...);
int pevent_unregister_print_function(struct pevent *pevent,
				     pevent_func_handler func, char *name);

struct format_field *pevent_find_common_field(struct event_format *event, const char *name);
struct format_field *pevent_find_field(struct event_format *event, const char *name);
struct format_field *pevent_find_any_field(struct event_format *event, const char *name);

const char *pevent_find_function(struct pevent *pevent, unsigned long long addr);
unsigned long long
pevent_find_function_address(struct pevent *pevent, unsigned long long addr);
unsigned long long pevent_read_number(struct pevent *pevent, const void *ptr, int size);
int pevent_read_number_field(struct format_field *field, const void *data,
			     unsigned long long *value);

struct event_format *pevent_find_event(struct pevent *pevent, int id);

struct event_format *
pevent_find_event_by_name(struct pevent *pevent, const char *sys, const char *name);

struct event_format *
pevent_find_event_by_record(struct pevent *pevent, struct pevent_record *record);

void pevent_data_lat_fmt(struct pevent *pevent,
			 struct trace_seq *s, struct pevent_record *record);
int pevent_data_type(struct pevent *pevent, struct pevent_record *rec);
struct event_format *pevent_data_event_from_type(struct pevent *pevent, int type);
int pevent_data_pid(struct pevent *pevent, struct pevent_record *rec);
int pevent_data_pc(struct pevent *pevent, struct pevent_record *rec);
int pevent_data_flags(struct pevent *pevent, struct pevent_record *rec);
int pevent_data_tgid_from_pid(struct pevent *pevent, int pid);
const char *pevent_data_comm_from_pid(struct pevent *pevent, int pid);
struct cmdline;
struct cmdline *pevent_data_pid_from_comm(struct pevent *pevent, const char *comm,
					  struct cmdline *next);
int pevent_cmdline_pid(struct pevent *pevent, struct cmdline *cmdline);

void pevent_print_field(struct trace_seq *s, void *data,
			struct format_field *field);
void pevent_print_fields(struct trace_seq *s, void *data,
			 int size __maybe_unused, struct event_format *event);
void pevent_event_info(struct trace_seq *s, struct event_format *event,
		       struct pevent_record *record);
int pevent_strerror(struct pevent *pevent, enum pevent_errno errnum,
		    char *buf, size_t buflen);

struct event_format **pevent_list_events(struct pevent *pevent, enum event_sort_type);
struct format_field **pevent_event_common_fields(struct event_format *event);
struct format_field **pevent_event_fields(struct event_format *event);

static inline int pevent_get_cpus(struct pevent *pevent)
{
	return pevent->cpus;
}

static inline void pevent_set_cpus(struct pevent *pevent, int cpus)
{
	pevent->cpus = cpus;
}

static inline int pevent_get_long_size(struct pevent *pevent)
{
	return pevent->long_size;
}

static inline void pevent_set_long_size(struct pevent *pevent, int long_size)
{
	pevent->long_size = long_size;
}

static inline int pevent_get_page_size(struct pevent *pevent)
{
	return pevent->page_size;
}

static inline void pevent_set_page_size(struct pevent *pevent, int _page_size)
{
	pevent->page_size = _page_size;
}

static inline int pevent_is_file_bigendian(struct pevent *pevent)
{
	return pevent->file_bigendian;
}

static inline void pevent_set_file_bigendian(struct pevent *pevent, int endian)
{
	pevent->file_bigendian = endian;
}

static inline int pevent_is_host_bigendian(struct pevent *pevent)
{
	return pevent->host_bigendian;
}

static inline void pevent_set_host_bigendian(struct pevent *pevent, int endian)
{
	pevent->host_bigendian = endian;
}

static inline int pevent_is_latency_format(struct pevent *pevent)
{
	return pevent->latency_format;
}

static inline void pevent_set_latency_format(struct pevent *pevent, int lat)
{
	pevent->latency_format = lat;
}

struct pevent *pevent_alloc(void);
void pevent_free(struct pevent *pevent);
void pevent_ref(struct pevent *pevent);
void pevent_unref(struct pevent *pevent);

/* access to the internal parser */
void pevent_buffer_init(const char *buf, unsigned long long size);
enum event_type pevent_read_token(char **tok);
void pevent_free_token(char *token);
int pevent_peek_char(void);
const char *pevent_get_input_buf(void);
unsigned long long pevent_get_input_buf_ptr(void);

/* for debugging */
void pevent_print_funcs(struct pevent *pevent);
void pevent_print_printk(struct pevent *pevent);
void pevent_print_tgids(struct pevent *pevent);

#endif /* _PARSE_EVENTS_H */
