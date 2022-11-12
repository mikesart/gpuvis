// SPDX-License-Identifier: LGPL-2.1
/*
 * trace-msg.c : define message protocol for communication between clients and
 *               a server
 *
 * Copyright (C) 2013 Hitachi, Ltd.
 * Created by Yoshihiro YUNOMAE <yoshihiro.yunomae.ez@hitachi.com>
 *
 */

#include <errno.h>
#include <poll.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <linux/types.h>

#include "trace-write-local.h"
#include "trace-cmd-local.h"
#include "trace-local.h"
#include "trace-msg.h"
#include "trace-cmd.h"

typedef __u32 u32;
typedef __be32 be32;

#define dprint(fmt, ...)	tracecmd_debug(fmt, ##__VA_ARGS__)

/* Two (4k) pages is the max transfer for now */
#define MSG_MAX_LEN			8192

#define MSG_HDR_LEN			sizeof(struct tracecmd_msg_header)

#define MSG_MAX_DATA_LEN		(MSG_MAX_LEN - MSG_HDR_LEN)

unsigned int page_size;

struct tracecmd_msg_tinit {
	be32 cpus;
	be32 page_size;
	be32 opt_num;
} __packed;

struct tracecmd_msg_rinit {
	be32 cpus;
} __packed;

#define TRACE_REQ_PARAM_SIZE  (2 * sizeof(int))
enum trace_req_params {
	TRACE_REQUEST_ARGS,
	TRACE_REQUEST_TSYNC_PROTOS,
};

struct tracecmd_msg_trace_req_param {
	int id;
	int length;
	char *value;
};

struct tracecmd_msg_trace_req {
	be32 flags;
	be32 argc;
	u64 trace_id;
} __packed;

struct tracecmd_msg_trace_proxy {
	struct tracecmd_msg_trace_req req;
	be32 cpus;
	be32 siblings;
} __packed;

struct tracecmd_msg_trace_resp {
	be32 flags;
	be32 cpus;
	be32 page_size;
	u64 trace_id;
	char tsync_proto_name[TRACECMD_TSYNC_PNAME_LENGTH];
	be32 tsync_port;
} __packed;

struct tracecmd_msg_tsync {
	char sync_protocol_name[TRACECMD_TSYNC_PNAME_LENGTH];
	be32 sync_msg_id;
} __packed;

struct tracecmd_msg_header {
	be32	size;
	be32	cmd;
	be32	cmd_size;
} __packed;

#define MSG_MAP								\
	C(CLOSE,	0,	0),					\
	C(TINIT,	1,	sizeof(struct tracecmd_msg_tinit)),	\
	C(RINIT,	2,	sizeof(struct tracecmd_msg_rinit)),	\
	C(SEND_DATA,	3,	0),					\
	C(FIN_DATA,	4,	0),					\
	C(NOT_SUPP,	5,	0),					\
	C(TRACE_REQ,	6,	sizeof(struct tracecmd_msg_trace_req)),	\
	C(TRACE_RESP,	7,	sizeof(struct tracecmd_msg_trace_resp)),\
	C(CLOSE_RESP,	8,	0),					\
	C(TIME_SYNC,	9,	sizeof(struct tracecmd_msg_tsync)),	\
	C(TRACE_PROXY,	10,	sizeof(struct tracecmd_msg_trace_proxy)), \
	C(CONT,		11,	0),

#undef C
#define C(a,b,c)	MSG_##a = b

enum tracecmd_msg_cmd {
	MSG_MAP
	MSG_NR_COMMANDS
};

#undef C
#define C(a,b,c)	c

static be32 msg_cmd_sizes[] = { MSG_MAP };

#undef C
#define C(a,b,c)	#a

static const char *msg_names[] = { MSG_MAP };

static const char *cmd_to_name(int cmd)
{
	if (cmd < 0 || cmd >= MSG_NR_COMMANDS)
		return "Unknown";
	return msg_names[cmd];
}

struct tracecmd_msg {
	struct tracecmd_msg_header		hdr;
	union {
		struct tracecmd_msg_tinit	tinit;
		struct tracecmd_msg_rinit	rinit;
		struct tracecmd_msg_trace_req	trace_req;
		struct tracecmd_msg_trace_proxy	trace_proxy;
		struct tracecmd_msg_trace_resp	trace_resp;
		struct tracecmd_msg_tsync	tsync;
	};
	char					*buf;
} __packed;

static inline int msg_buf_len(struct tracecmd_msg *msg)
{
	return ntohl(msg->hdr.size) - MSG_HDR_LEN - ntohl(msg->hdr.cmd_size);
}

static int __msg_write(int fd, struct tracecmd_msg *msg, bool network)
{
	int msg_size, data_size;
	int ret;
	int cmd;

	if (network) {
		cmd = ntohl(msg->hdr.cmd);
		if (cmd < 0 || cmd >= MSG_NR_COMMANDS)
			return -EINVAL;
		dprint("msg send: %d (%s) [%d]\n",
		       cmd, cmd_to_name(cmd), ntohl(msg->hdr.size));
	}
	msg_size = MSG_HDR_LEN + ntohl(msg->hdr.cmd_size);
	data_size = ntohl(msg->hdr.size) - msg_size;
	if (data_size < 0)
		return -EINVAL;

	if (network) {
		ret = __do_write_check(fd, msg, msg_size);
		if (ret < 0)
			return ret;
	}
	if (!data_size)
		return 0;

	return __do_write_check(fd, msg->buf, data_size);
}

__hidden off64_t msg_lseek(struct tracecmd_msg_handle *msg_handle, off64_t offset, int whence)
{
	off64_t cache_offset = msg_handle->cache_start_offset;
	off64_t ret;

	/*
	 * lseek works only if the handle is in cache mode,
	 * cannot seek on a network socket
	 */
	if (!msg_handle->cache || msg_handle->cfd < 0)
		return (off64_t)-1;

	if (whence == SEEK_SET) {
		if (offset < cache_offset)
			return (off64_t)-1;
		offset -= cache_offset;
	}

	ret = lseek64(msg_handle->cfd, offset, whence);
	if (ret == (off64_t)-1)
		return ret;

	return ret + cache_offset;
}

static int msg_write(struct tracecmd_msg_handle *msg_handle, struct tracecmd_msg *msg)
{
	if (msg_handle->cache && msg_handle->cfd >= 0)
		return __msg_write(msg_handle->cfd, msg, false);


	return __msg_write(msg_handle->fd, msg, true);
}

enum msg_trace_flags {
	MSG_TRACE_USE_FIFOS = 1 << 0,
};

static int make_tinit(struct tracecmd_msg_handle *msg_handle,
		      struct tracecmd_msg *msg)
{
	int cpu_count = msg_handle->cpu_count;
	int opt_num = 0;
	int data_size = 0;

	if (msg_handle->flags & (TRACECMD_MSG_FL_USE_TCP |
				 TRACECMD_MSG_FL_USE_VSOCK)) {
		msg->buf = msg_handle->flags & TRACECMD_MSG_FL_USE_TCP ?
			strdup("tcp") : strdup("vsock");
		if (!msg->buf)
			return -1;
		opt_num++;
		data_size += strlen(msg->buf) + 1;
	}

	msg->tinit.cpus = htonl(cpu_count);
	msg->tinit.page_size = htonl(page_size);
	msg->tinit.opt_num = htonl(opt_num);

	msg->hdr.size = htonl(ntohl(msg->hdr.size) + data_size);

	return 0;
}

/* test a to u */
static int tatou(const char *s, unsigned int *res)
{
        long r;

        r = atol(s);
        if (r >= 0 && r <= UINT_MAX) {
                *res = (unsigned int)r;
                return 0;
        }
        return -1;
}

static int write_uints(char *buf, size_t buf_len,
		       unsigned int *arr, int arr_len)
{
	int i, ret, tot = 0;

	for (i = 0; i < arr_len; i++) {
		ret = snprintf(buf, buf_len, "%u", arr[i]);
		if (ret < 0)
			return ret;

		/* Count the '\0' byte */
		ret++;
		tot += ret;
		if (buf)
			buf += ret;
		if (buf_len >= ret)
			buf_len -= ret;
		else
			buf_len = 0;
	}

	return tot;
}

static int make_rinit(struct tracecmd_msg *msg, int cpus, unsigned int *ports)
{
	int data_size;

	data_size = write_uints(NULL, 0, ports, cpus);
	msg->buf = malloc(data_size);
	if (!msg->buf)
		return -ENOMEM;
	write_uints(msg->buf, data_size, ports, cpus);

	msg->rinit.cpus = htonl(cpus);
	msg->hdr.size = htonl(ntohl(msg->hdr.size) + data_size);

	return 0;
}

static void tracecmd_msg_init(u32 cmd, struct tracecmd_msg *msg)
{
	memset(msg, 0, sizeof(*msg));
	msg->hdr.size = htonl(MSG_HDR_LEN + msg_cmd_sizes[cmd]);
	msg->hdr.cmd = htonl(cmd);
	msg->hdr.cmd_size = htonl(msg_cmd_sizes[cmd]);
}

static void msg_free(struct tracecmd_msg *msg)
{
	free(msg->buf);
	memset(msg, 0, sizeof(*msg));
}

static int tracecmd_msg_send(struct tracecmd_msg_handle *msg_handle, struct tracecmd_msg *msg)
{
	int ret = 0;

	ret = msg_write(msg_handle, msg);
	if (ret < 0)
		ret = -ECOMM;

	msg_free(msg);

	return ret;
}

static int msg_send_nofree(struct tracecmd_msg_handle *msg_handle, struct tracecmd_msg *msg)
{
	int ret = 0;

	ret = msg_write(msg_handle, msg);
	if (ret < 0)
		ret = -ECOMM;

	return ret;
}

static int msg_read(int fd, void *buf, u32 size, int *n)
{
	ssize_t r;

	while (size) {
		r = read(fd, buf + *n, size);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		} else if (!r)
			return -ENOTCONN;
		size -= r;
		*n += r;
	}

	return 0;
}

static char scratch_buf[MSG_MAX_LEN];

static int msg_read_extra(int fd, struct tracecmd_msg *msg,
			  int *n, int size)
{
	int cmd, cmd_size, rsize;
	int ret;

	cmd = ntohl(msg->hdr.cmd);
	if (cmd < 0 || cmd >= MSG_NR_COMMANDS)
		return -EINVAL;

	cmd_size = ntohl(msg->hdr.cmd_size);
	if (cmd_size < 0)
		return -EINVAL;

	if (cmd_size > 0) {
		rsize = cmd_size;
		if (rsize > msg_cmd_sizes[cmd])
			rsize = msg_cmd_sizes[cmd];

		ret = msg_read(fd, msg, rsize, n);
		if (ret < 0)
			return ret;

		ret = msg_read(fd, scratch_buf, cmd_size - rsize, n);
		if (ret < 0)
			return ret;
	}

	if (size > *n) {
		size -= *n;
		msg->buf = malloc(size);
		if (!msg->buf)
			return -ENOMEM;

		*n = 0;
		return msg_read(fd, msg->buf, size, n);
	}

	return 0;
}

/*
 * Read header information of msg first, then read all data
 */
static int tracecmd_msg_recv(int fd, struct tracecmd_msg *msg)
{
	u32 size = 0;
	int n = 0;
	int ret;

	ret = msg_read(fd, msg, MSG_HDR_LEN, &n);
	if (ret < 0)
		return ret;

	dprint("msg received: %d (%s) [%d]\n",
	       ntohl(msg->hdr.cmd), cmd_to_name(ntohl(msg->hdr.cmd)),
	       ntohl(msg->hdr.size));

	size = ntohl(msg->hdr.size);
	if (size > MSG_MAX_LEN)
		/* too big */
		goto error;
	else if (size < MSG_HDR_LEN)
		/* too small */
		goto error;
	else if (size > MSG_HDR_LEN)
		return msg_read_extra(fd, msg, &n, size);

	return 0;
error:
	tracecmd_plog("Receive an invalid message(size=%d)\n", size);
	return -ENOMSG;
}

#define MSG_WAIT_MSEC	5000
static int msg_wait_to = MSG_WAIT_MSEC;

bool tracecmd_msg_done(struct tracecmd_msg_handle *msg_handle)
{
	return (volatile int)msg_handle->done;
}

void tracecmd_msg_set_done(struct tracecmd_msg_handle *msg_handle)
{
	msg_handle->done = true;
}

static void error_operation(struct tracecmd_msg *msg)
{
	tracecmd_warning("Message: cmd=%d size=%d", ntohl(msg->hdr.cmd), ntohl(msg->hdr.size));
}

/*
 * A return value of 0 indicates time-out
 */
static int tracecmd_msg_recv_wait(int fd, struct tracecmd_msg *msg)
{
	struct pollfd pfd;
	int ret;

	pfd.fd = fd;
	pfd.events = POLLIN;
	ret = poll(&pfd, 1, tracecmd_get_debug() ? -1 : msg_wait_to);
	if (ret < 0)
		return -errno;
	else if (ret == 0)
		return -ETIMEDOUT;

	return tracecmd_msg_recv(fd, msg);
}

static int tracecmd_msg_wait_for_msg(int fd, struct tracecmd_msg *msg)
{
	u32 cmd;
	int ret;

	ret = tracecmd_msg_recv_wait(fd, msg);
	if (ret < 0) {
		if (ret == -ETIMEDOUT)
			tracecmd_warning("Connection timed out");
		return ret;
	}

	cmd = ntohl(msg->hdr.cmd);
	if (cmd == MSG_CLOSE)
		return -ECONNABORTED;

	return 0;
}

static int tracecmd_msg_send_notsupp(struct tracecmd_msg_handle *msg_handle)
{
	struct tracecmd_msg msg;

	tracecmd_msg_init(MSG_NOT_SUPP, &msg);
	return tracecmd_msg_send(msg_handle, &msg);
}

static int handle_unexpected_msg(struct tracecmd_msg_handle *msg_handle,
				 struct tracecmd_msg *msg)
{
	/* Don't send MSG_NOT_SUPP back if we just received one */
	if (ntohl(msg->hdr.cmd) == MSG_NOT_SUPP)
		return 0;

	return tracecmd_msg_send_notsupp(msg_handle);

}

int tracecmd_msg_send_init_data(struct tracecmd_msg_handle *msg_handle,
				unsigned int **client_ports)
{
	struct tracecmd_msg msg;
	unsigned int *ports;
	int i, cpus, ret;
	char *p, *buf_end;
	ssize_t buf_len;

	*client_ports = NULL;

	tracecmd_msg_init(MSG_TINIT, &msg);
	ret = make_tinit(msg_handle, &msg);
	if (ret < 0)
		goto out;

	ret = tracecmd_msg_send(msg_handle, &msg);
	if (ret < 0)
		goto out;

	msg_free(&msg);

	ret = tracecmd_msg_wait_for_msg(msg_handle->fd, &msg);
	if (ret < 0)
		goto out;

	if (ntohl(msg.hdr.cmd) != MSG_RINIT) {
		ret = -EOPNOTSUPP;
		goto error;
	}

	buf_len = msg_buf_len(&msg);
	if (buf_len <= 0) {
		ret = -EINVAL;
		goto error;
	}

	if (msg.buf[buf_len-1] != '\0') {
		ret = -EINVAL;
		goto error;
	}

	cpus = ntohl(msg.rinit.cpus);
	ports = malloc(sizeof(*ports) * cpus);
	if (!ports) {
		ret = -ENOMEM;
		goto out;
	}

	buf_end = msg.buf + buf_len;
	for (i = 0, p = msg.buf; i < cpus; i++, p++) {
		if (p >= buf_end || tatou(p, &ports[i])) {
			free(ports);
			ret = -EINVAL;
			goto error;
		}
		p = strchr(p, '\0');
	}

	*client_ports = ports;

	msg_free(&msg);
	return 0;

error:
	error_operation(&msg);
	if (ret == -EOPNOTSUPP)
		handle_unexpected_msg(msg_handle, &msg);
out:
	msg_free(&msg);
	return ret;
}

static bool process_option(struct tracecmd_msg_handle *msg_handle,
			   const char *opt)
{
	if (strcmp(opt, "tcp") == 0) {
		msg_handle->flags |= TRACECMD_MSG_FL_USE_TCP;
		return true;
	}
	if (strcmp(opt, "vsock") == 0) {
		msg_handle->flags |= TRACECMD_MSG_FL_USE_VSOCK;
		return true;
	}
	return false;
}

struct tracecmd_msg_handle *
tracecmd_msg_handle_alloc(int fd, unsigned long flags)
{
	struct tracecmd_msg_handle *handle;

	handle = calloc(1, sizeof(struct tracecmd_msg_handle));
	if (!handle)
		return NULL;

	handle->fd = fd;
	handle->flags = flags;
	handle->cfd = -1;
	handle->cache = false;
	return handle;
}

int tracecmd_msg_handle_cache(struct tracecmd_msg_handle *msg_handle)
{
	if (msg_handle->cfd < 0) {
#ifdef HAVE_MEMFD_CREATE
		msg_handle->cfd = memfd_create("trace_msg_cache", 0);
		if (msg_handle->cfd < 0)
			return -1;
#else
		strcpy(msg_handle->cfile, MSG_CACHE_FILE);
		msg_handle->cfd = mkstemp(msg_handle->cfile);
		if (msg_handle->cfd < 0)
			return -1;
		unlink(msg_handle->cfile);
#endif
	}
	msg_handle->cache = true;
	return 0;
}

static int flush_cache(struct tracecmd_msg_handle *msg_handle)
{
	char buf[MSG_MAX_DATA_LEN];
	int fd = msg_handle->cfd;
	int ret;

	if (!msg_handle->cache || fd < 0)
		return 0;
	msg_handle->cache = false;
	if (lseek64(fd, 0, SEEK_SET) == (off64_t)-1)
		return -1;
	do {
		ret = read(fd, buf, MSG_MAX_DATA_LEN);
		if (ret <= 0)
			break;
		ret = tracecmd_msg_data_send(msg_handle, buf, ret);
		if (ret < 0)
			break;
	} while (ret >= 0);

	msg_handle->cache_start_offset = lseek64(fd, 0, SEEK_CUR);
	if (msg_handle->cache_start_offset == (off64_t)-1)
		return -1;

	close(fd);
	msg_handle->cfd = -1;
	return ret;
}

void tracecmd_msg_handle_close(struct tracecmd_msg_handle *msg_handle)
{
	if (msg_handle->fd >= 0)
		close(msg_handle->fd);
	if (msg_handle->cfd >= 0)
		close(msg_handle->cfd);
	free(msg_handle);
}

#define MAX_OPTION_SIZE 4096

int tracecmd_msg_initial_setting(struct tracecmd_msg_handle *msg_handle)
{
	struct tracecmd_msg msg;
	char *p, *buf_end;
	ssize_t buf_len;
	int pagesize;
	int options, i;
	int cpus;
	int ret;

	memset(&msg, 0, sizeof(msg));
	ret = tracecmd_msg_recv_wait(msg_handle->fd, &msg);
	if (ret < 0) {
		if (ret == -ETIMEDOUT)
			tracecmd_warning("Connection timed out");
		return ret;
	}

	if (ntohl(msg.hdr.cmd) != MSG_TINIT) {
		ret = -EOPNOTSUPP;
		goto error;
	}

	cpus = ntohl(msg.tinit.cpus);
	tracecmd_plog("cpus=%d\n", cpus);
	if (cpus < 0) {
		ret = -EINVAL;
		goto error;
	}

	msg_handle->cpu_count = cpus;

	pagesize = ntohl(msg.tinit.page_size);
	tracecmd_plog("pagesize=%d\n", pagesize);
	if (pagesize <= 0) {
		ret = -EINVAL;
		goto error;
	}

	buf_len = msg_buf_len(&msg);
	if (buf_len < 0) {
		ret = -EINVAL;
		goto error;
	}

	if (buf_len == 0)
		goto no_options;

	if (msg.buf[buf_len-1] != '\0') {
		ret = -EINVAL;
		goto error;
	}

	buf_end = msg.buf + buf_len;
	options = ntohl(msg.tinit.opt_num);
	for (i = 0, p = msg.buf; i < options; i++, p++) {
		if (p >= buf_end) {
			ret = -EINVAL;
			goto error;
		}

		/* do we understand this option? */
		if (!process_option(msg_handle, p))
			tracecmd_plog("Cannot understand option '%s'\n", p);

		p = strchr(p, '\0');
	}

no_options:
	msg_free(&msg);
	return pagesize;

error:
	error_operation(&msg);
	if (ret == -EOPNOTSUPP)
		handle_unexpected_msg(msg_handle, &msg);
	msg_free(&msg);
	return ret;
}

int tracecmd_msg_send_port_array(struct tracecmd_msg_handle *msg_handle,
				 unsigned int *ports)
{
	struct tracecmd_msg msg;
	int ret;

	tracecmd_msg_init(MSG_RINIT, &msg);
	ret = make_rinit(&msg, msg_handle->cpu_count, ports);
	if (ret < 0)
		return ret;

	ret = tracecmd_msg_send(msg_handle, &msg);
	if (ret < 0)
		return ret;

	return 0;
}

int tracecmd_msg_send_close_msg(struct tracecmd_msg_handle *msg_handle)
{
	struct tracecmd_msg msg;

	tracecmd_msg_init(MSG_CLOSE, &msg);
	return tracecmd_msg_send(msg_handle, &msg);
}

int tracecmd_msg_send_close_resp_msg(struct tracecmd_msg_handle *msg_handle)
{
	struct tracecmd_msg msg;

	tracecmd_msg_init(MSG_CLOSE_RESP, &msg);
	return tracecmd_msg_send(msg_handle, &msg);
}

int tracecmd_msg_cont(struct tracecmd_msg_handle *msg_handle)
{
	struct tracecmd_msg msg;

	tracecmd_msg_init(MSG_CONT, &msg);
	return tracecmd_msg_send(msg_handle, &msg);
}

int tracecmd_msg_data_send(struct tracecmd_msg_handle *msg_handle,
			   const char *buf, int size)
{
	struct tracecmd_msg msg;
	int n;
	int ret;
	int count = 0;

	/* Don't bother doing anything if there's nothing to do */
	if (!size)
		return 0;

	tracecmd_msg_init(MSG_SEND_DATA, &msg);

	msg.buf = malloc(MSG_MAX_DATA_LEN);
	if (!msg.buf)
		return -ENOMEM;

	msg.hdr.size = htonl(MSG_MAX_LEN);

	n = size;
	while (n) {
		if (n > MSG_MAX_DATA_LEN) {
			memcpy(msg.buf, buf + count, MSG_MAX_DATA_LEN);
			n -= MSG_MAX_DATA_LEN;
			count += MSG_MAX_DATA_LEN;
		} else {
			msg.hdr.size = htonl(MSG_HDR_LEN + n);
			memcpy(msg.buf, buf + count, n);
			n = 0;
		}
		ret = msg_write(msg_handle, &msg);
		if (ret < 0)
			break;
	}

	msg_free(&msg);
	return ret;
}

/**
 * tracecmd_msg_send_options - Send options over the network
 * @msg_handle: message handle, holding the communication context
 * @handle: The output file that has the options to send
 *
 * Send options over the network. This is used when the output handle
 * has more options to send over the network after the trace. Some
 * options are sent before, and some sent afterward. Since the receiving
 * side needs to know the location to update the indexes, it will
 * handle the section header. This just sends out the raw content to
 * the receiver (requires that both sides have the same endianess, as
 * no conversion is made of the content of the options).
 *
 * Returns 0 on success and -1 on error.
 */
int tracecmd_msg_send_options(struct tracecmd_msg_handle *msg_handle,
			      struct tracecmd_output *handle)
{
	struct tracecmd_msg msg;
	size_t len;
	void *buf;
	int ret;

	buf = trace_get_options(handle, &len);
	if (!buf)
		return -1;

	ret = tracecmd_msg_data_send(msg_handle, buf, len);
	free(buf);
	if (ret < 0)
		return ret;

	tracecmd_msg_init(MSG_FIN_DATA, &msg);
	return tracecmd_msg_send(msg_handle, &msg);
}

/**
 * tracecmd_msg_flush_data - Send the current cache data over the network
 * @msg_handle: message handle, holding the communication context
 *
 * Send the content in the cache file over the nework, reset the file
 * and start the cache up again (with nothing in it).
 */
int tracecmd_msg_flush_data(struct tracecmd_msg_handle *msg_handle)
{
	struct tracecmd_msg msg;
	int ret;

	flush_cache(msg_handle);
	tracecmd_msg_init(MSG_FIN_DATA, &msg);
	ret = tracecmd_msg_send(msg_handle, &msg);
	if (ret < 0)
		return ret;
	return tracecmd_msg_handle_cache(msg_handle);
}

int tracecmd_msg_finish_sending_data(struct tracecmd_msg_handle *msg_handle)
{
	struct tracecmd_msg msg;
	int ret;

	flush_cache(msg_handle);
	tracecmd_msg_init(MSG_FIN_DATA, &msg);
	ret = tracecmd_msg_send(msg_handle, &msg);
	if (ret < 0)
		return ret;
	return 0;
}

static int read_msg_data(struct tracecmd_msg_handle *msg_handle,
			 struct tracecmd_msg *msg)
{
	int cmd;
	int ret;

	ret = tracecmd_msg_recv_wait(msg_handle->fd, msg);
	if (ret < 0) {
		tracecmd_warning("reading client %d (%s)", ret, strerror(ret));
		return ret;
	}

	cmd = ntohl(msg->hdr.cmd);
	if (cmd == MSG_FIN_DATA) {
		/* Finish receiving data */
		return 0;
	} else if (cmd != MSG_SEND_DATA) {
		ret = handle_unexpected_msg(msg_handle, msg);
		if (ret < 0)
			return -1;
		return 0;
	}

	return msg_buf_len(msg);
}

/**
 * tracecmd_msg_read_options - Receive options from over the network
 * @msg_handle: message handle, holding the communication context
 * @handle: The output file to write the options to.
 *
 * Receive the options sent by tracecmd_msg_send_options().
 * See that function's documentation for mor details.
 *
 * Returns 0 on success and -1 on error.
 */
int tracecmd_msg_read_options(struct tracecmd_msg_handle *msg_handle,
			      struct tracecmd_output *handle)
{
	struct tracecmd_msg msg;
	size_t len = 0;
	void *buf = NULL;
	void *tmp;
	int ret;
	int n;

	memset(&msg, 0, sizeof(msg));
	while (!tracecmd_msg_done(msg_handle)) {
		n = read_msg_data(msg_handle, &msg);
		if (n <= 0)
			break;

		tmp = realloc(buf, n + len);
		if (!tmp)
			goto error;
		buf = tmp;
		memcpy(buf + len, msg.buf, n);
		len += n;
		msg_free(&msg);
	}
	msg_free(&msg);

	ret = trace_append_options(handle, buf, len);
	free(buf);

	return ret;
 error:
	msg_free(&msg);
	free(buf);
	return -1;
}

int tracecmd_msg_read_data(struct tracecmd_msg_handle *msg_handle, int ofd)
{
	struct tracecmd_msg msg;
	int t, n;
	ssize_t s;
	int ret;

	memset(&msg, 0, sizeof(msg));

	while (!tracecmd_msg_done(msg_handle)) {
		n = read_msg_data(msg_handle, &msg);
		if (n <= 0)
			break;
		t = n;
		s = 0;
		while (t > 0) {
			s = write(ofd, msg.buf+s, t);
			if (s < 0) {
				if (errno == EINTR)
					continue;
				tracecmd_warning("writing to file");
				ret = -errno;
				goto error;
			}
			t -= s;
			s = n - t;
		}
		msg_free(&msg);
	}
	msg_free(&msg);

	return 0;

error:
	error_operation(&msg);
	msg_free(&msg);
	return ret;
}

int tracecmd_msg_collect_data(struct tracecmd_msg_handle *msg_handle, int ofd)
{
	int ret;

	ret = tracecmd_msg_read_data(msg_handle, ofd);
	if (ret)
		return ret;

	return tracecmd_msg_wait_close(msg_handle);
}

static int tracecmd_msg_wait_for_cmd(struct tracecmd_msg_handle *msg_handle, enum tracecmd_msg_cmd cmd)
{
	struct tracecmd_msg msg;
	int ret = -1;

	memset(&msg, 0, sizeof(msg));
	while (!tracecmd_msg_done(msg_handle)) {
		ret = tracecmd_msg_recv(msg_handle->fd, &msg);
		if (ret < 0)
			goto error;

		if (ntohl(msg.hdr.cmd) == cmd)
			return 0;

		error_operation(&msg);
		ret = handle_unexpected_msg(msg_handle, &msg);
		if (ret < 0)
			goto error;

		msg_free(&msg);
	}

error:
	msg_free(&msg);
	return ret;
}

int tracecmd_msg_wait(struct tracecmd_msg_handle *msg_handle)
{
	return tracecmd_msg_wait_for_cmd(msg_handle, MSG_CONT);
}

int tracecmd_msg_wait_close(struct tracecmd_msg_handle *msg_handle)
{
	return tracecmd_msg_wait_for_cmd(msg_handle, MSG_CLOSE);
}

int tracecmd_msg_wait_close_resp(struct tracecmd_msg_handle *msg_handle)
{
	return tracecmd_msg_wait_for_cmd(msg_handle, MSG_CLOSE_RESP);
}

static int make_trace_req_protos(char **buf, int *size,
				 struct tracecmd_tsync_protos *protos)
{
	int protos_size = 1;
	size_t buf_size;
	char **names;
	char *nbuf;
	char *p;

	names = protos->names;
	while (*names) {
		protos_size += strlen(*names) + 1;
		names++;
	}

	buf_size = TRACE_REQ_PARAM_SIZE + protos_size;
	nbuf = realloc(*buf, *size + buf_size);
	if (!nbuf)
		return -1;

	p = nbuf + *size;
	memset(p, 0, buf_size);

	*(unsigned int *)p = htonl(TRACE_REQUEST_TSYNC_PROTOS);
	p += sizeof(int);
	*(unsigned int *)p = htonl(protos_size);
	p += sizeof(int);

	names = protos->names;
	while (*names) {
		strcpy(p, *names);
		p += strlen(*names) + 1;
		names++;
	}
	p = NULL;

	*size += buf_size;
	*buf = nbuf;
	return 0;
}

static int make_trace_req_args(char **buf, int *size, int argc, char **argv)
{
	size_t args_size;
	size_t buf_size;
	char *nbuf;
	char *p;
	int i;

	args_size = sizeof(int);
	for (i = 0; i < argc; i++)
		args_size += strlen(argv[i]) + 1;

	buf_size = TRACE_REQ_PARAM_SIZE + args_size;
	nbuf = realloc(*buf, *size + buf_size);
	if (!nbuf)
		return -1;

	p = nbuf + *size;
	memset(p, 0, buf_size);

	*(unsigned int *)p = htonl(TRACE_REQUEST_ARGS);
	p += sizeof(int);
	*(unsigned int *)p = htonl(args_size);
	p += sizeof(int);

	*(unsigned int *)p = htonl(argc);
	p += sizeof(int);
	for (i = 0; i < argc; i++)
		p = stpcpy(p, argv[i]) + 1;

	*size += buf_size;
	*buf = nbuf;
	return 0;
}

static int make_trace_req(struct tracecmd_msg *msg, int argc, char **argv,
			  bool use_fifos, unsigned long long trace_id,
			  struct tracecmd_tsync_protos *protos)
{
	int size = 0;
	char *buf = NULL;

	msg->trace_req.flags = 0;
	if (use_fifos)
		msg->trace_req.flags |= MSG_TRACE_USE_FIFOS;
	msg->trace_req.flags = htonl(msg->trace_req.flags);
	msg->trace_req.trace_id = htonll(trace_id);

	if (argc && argv)
		make_trace_req_args(&buf, &size, argc, argv);
	if (protos && protos->names)
		make_trace_req_protos(&buf, &size, protos);

	msg->buf = buf;
	msg->hdr.size = htonl(ntohl(msg->hdr.size) + size);

	return size;
}

int tracecmd_msg_send_trace_req(struct tracecmd_msg_handle *msg_handle,
				int argc, char **argv, bool use_fifos,
				unsigned long long trace_id,
				struct tracecmd_tsync_protos *protos)
{
	struct tracecmd_msg msg;
	int ret;

	tracecmd_msg_init(MSG_TRACE_REQ, &msg);
	ret = make_trace_req(&msg, argc, argv, use_fifos, trace_id, protos);
	if (ret < 0)
		return ret;

	return tracecmd_msg_send(msg_handle, &msg);
}

int tracecmd_msg_send_trace_proxy(struct tracecmd_msg_handle *msg_handle,
				  int argc, char **argv, bool use_fifos,
				  unsigned long long trace_id,
				  struct tracecmd_tsync_protos *protos,
				  unsigned int nr_cpus,
				  unsigned int siblings)
{
	struct tracecmd_msg msg;
	int ret;

	tracecmd_msg_init(MSG_TRACE_PROXY, &msg);
	ret = make_trace_req(&msg, argc, argv, use_fifos, trace_id, protos);
	if (ret < 0)
		return ret;

	msg.trace_proxy.cpus = htonl(nr_cpus);
	msg.trace_proxy.siblings = htonl(siblings);
	return tracecmd_msg_send(msg_handle, &msg);
}

static int get_trace_req_protos(char *buf, int length,
				struct tracecmd_tsync_protos **protos)
{
	struct tracecmd_tsync_protos *plist = NULL;
	int count = 0;
	char *p;
	int i, j;

	i = length;
	p = buf;
	while (i > 0) {
		i -= strlen(p) + 1;
		count++;
		p += strlen(p) + 1;
	}

	plist = calloc(1, sizeof(struct tracecmd_tsync_protos));
	if (!plist)
		goto error;
	plist->names = calloc(count + 1, sizeof(char *));
	if (!plist->names)
		goto error;
	i = length;
	p = buf;
	j = 0;
	while (i > 0 && j < (count - 1)) {
		i -= strlen(p) + 1;
		plist->names[j++] = strdup(p);
		p += strlen(p) + 1;
	}

	*protos = plist;
	return 0;
error:
	if (plist) {
		free(plist->names);
		free(plist);
	}
	return -1;
}

static int get_trace_req_args(char *buf, int length, int *argc, char ***argv)
{
	unsigned int nr_args;
	char *p, *buf_end;
	char **args = NULL;
	char *vagrs = NULL;
	int ret;
	int i;

	if (length <= sizeof(int) || buf[length - 1] != '\0') {
		ret = -EINVAL;
		goto out;
	}

	nr_args = ntohl(*(unsigned int *)buf);
	buf += sizeof(int);
	length -= sizeof(int);

	args = calloc(nr_args, sizeof(*args));
	if (!args) {
		ret = -ENOMEM;
		goto out;
	}

	vagrs = calloc(length, sizeof(char));
	if (!vagrs) {
		ret = -ENOMEM;
		goto out;
	}

	memcpy(vagrs, buf, length);
	buf_end = vagrs + length;
	for (i = 0, p = vagrs; i < nr_args; i++, p++) {
		if (p >= buf_end) {
			ret = -EINVAL;
			goto out;
		}
		args[i] = p;
		p = strchr(p, '\0');
	}

	*argc = nr_args;
	*argv = args;
	return 0;

out:
	free(args);
	free(vagrs);
	return ret;

}

static int msg_recv_trace_req_proxy(struct tracecmd_msg_handle *msg_handle,
				    int *argc, char ***argv, bool *use_fifos,
				    unsigned long long *trace_id,
				    struct tracecmd_tsync_protos **protos,
				    unsigned int *cpus,
				    unsigned int *siblings)
{
	struct tracecmd_msg msg;
	unsigned int param_id;
	int param_length;
	ssize_t buf_len;
	char *p;
	int ret;

	ret = tracecmd_msg_recv(msg_handle->fd, &msg);
	if (ret < 0)
		return ret;

	switch (ntohl(msg.hdr.cmd)) {
	case MSG_TRACE_PROXY:
		if (cpus)
			*cpus = ntohl(msg.trace_proxy.cpus);
		if (siblings)
			*siblings = ntohl(msg.trace_proxy.siblings);
		/* fall through */
	case MSG_TRACE_REQ:
		break;
	default:
		ret = -ENOTSUP;
		goto out;
	}

	buf_len = ntohl(msg.hdr.size) - MSG_HDR_LEN - ntohl(msg.hdr.cmd_size);
	if (buf_len < 0) {
		ret = -EINVAL;
		goto out;
	}

	*use_fifos = ntohl(msg.trace_req.flags) & MSG_TRACE_USE_FIFOS;
	*trace_id = ntohll(msg.trace_req.trace_id);
	p = msg.buf;
	while (buf_len > 2 * sizeof(int)) {
		param_id = ntohl(*((unsigned int *)p));
		p += sizeof(int);
		buf_len -= sizeof(int);
		param_length = ntohl(*((unsigned int *)p));
		p += sizeof(int);
		buf_len -= sizeof(int);
		if (buf_len < param_length)
			break;
		ret = 0;
		switch (param_id) {
		case TRACE_REQUEST_ARGS:
			ret = get_trace_req_args(p, param_length, argc, argv);
			break;
		case TRACE_REQUEST_TSYNC_PROTOS:
			ret = get_trace_req_protos(p, param_length, protos);
			break;
		default:
			break;
		}
		if (ret)
			break;
		buf_len -= param_length;
		p += param_length;
	}

	msg_free(&msg);
	return 0;

out:
	error_operation(&msg);
	if (ret == -EOPNOTSUPP)
		handle_unexpected_msg(msg_handle, &msg);
	msg_free(&msg);
	return ret;
}

/*
 * NOTE: On success, the returned `argv` should be freed with:
 *     free(argv[0]);
 *     free(argv);
 * and `tsync_protos` with free(tsync_protos);
 */
int tracecmd_msg_recv_trace_req(struct tracecmd_msg_handle *msg_handle,
				int *argc, char ***argv, bool *use_fifos,
				unsigned long long *trace_id,
				struct tracecmd_tsync_protos **protos)
{
	return msg_recv_trace_req_proxy(msg_handle, argc, argv, use_fifos,
					trace_id, protos, NULL, NULL);
}

/*
 * NOTE: On success, the returned `argv` should be freed with:
 *     free(argv[0]);
 *     free(argv);
 * and `tsync_protos` with free(tsync_protos);
 */
int tracecmd_msg_recv_trace_proxy(struct tracecmd_msg_handle *msg_handle,
				  int *argc, char ***argv, bool *use_fifos,
				  unsigned long long *trace_id,
				  struct tracecmd_tsync_protos **protos,
				  unsigned int *cpus, unsigned int *siblings)
{
	return msg_recv_trace_req_proxy(msg_handle, argc, argv, use_fifos,
					trace_id, protos, cpus, siblings);
}

/**
 * tracecmd_msg_send_time_sync - Send a time sync packet
 * @msg_handle: message handle, holding the communication context
 * @sync_protocol: name of the time synch protocol, string up to
 *		   TRACECMD_TSYNC_PNAME_LENGTH characters length.
 * @sync_msg_id: id if the time synch message, protocol dependent
 * @payload_size: size of the packet payload, 0 in case of no payload
 * @payload: pointer to the packet payload, or NULL in case of no payload
 *
 * Returns 0 if packet is sent successfully, or negative error otherwise.
 */
int tracecmd_msg_send_time_sync(struct tracecmd_msg_handle *msg_handle,
				char *sync_protocol, unsigned int sync_msg_id,
				unsigned int payload_size, char *payload)
{
	struct tracecmd_msg msg;

	tracecmd_msg_init(MSG_TIME_SYNC, &msg);
	strncpy(msg.tsync.sync_protocol_name, sync_protocol, TRACECMD_TSYNC_PNAME_LENGTH);
	msg.tsync.sync_msg_id = htonl(sync_msg_id);
	msg.hdr.size = htonl(ntohl(msg.hdr.size) + payload_size);

	msg.buf = payload;
	return msg_send_nofree(msg_handle, &msg);
}

/**
 * tracecmd_msg_recv_time_sync - Receive a time sync packet
 * @msg_handle: message handle, holding the communication context
 * @sync_protocol: return the name of the packet's time synch protocol.
 *		   It must point to a prealocated buffer with size
 *		   TRACECMD_TSYNC_PNAME_LENGTH
 * @sync_msg_id: return the id of the packet's time synch message
 * @payload_size: size of the packet's payload, can be:
 *		 NULL - the payload is not interested and should be ignored
 *		 pointer to int, with value 0 - update with the size of the payload
 *						allocate memory and cpy the payload
 *						into it
 *		 pointer to int, with value greater than 0 - expected size of the
 *							     payload, preallocated
 *							     memory is passed to the API
 *							     with that size
 *@payload: pointer to the packet payload, can be:
 *	     NULL - the payload is not interested and should be ignored
 *	     pointer to char *, with value NULL - a new memory is allocated and returned
 *						  here, containing the packet's payload
 *						  the @payload_size is updated with the
 *						  size of the allocated memory. It must be
 *						  freed by free()
 *	     pointer to char *, with no-NULL value - A prealocated array is passed, with size
 *						     @payload_size. If payload's size is equal
 *						     or less, it will be copied here.
 *
 * Returns 0 if packet is received successfully, or negative error otherwise.
 */
int tracecmd_msg_recv_time_sync(struct tracecmd_msg_handle *msg_handle,
				char *sync_protocol,
				unsigned int *sync_msg_id,
				unsigned int *payload_size, char **payload)
{
	struct tracecmd_msg msg;
	int ret = -1;
	int buf_size;

	memset(&msg, 0, sizeof(msg));
	ret = tracecmd_msg_recv(msg_handle->fd, &msg);
	if (ret < 0)
		goto out;

	if (ntohl(msg.hdr.cmd) != MSG_TIME_SYNC) {
		ret = -EOPNOTSUPP;
		goto out;
	}

	if (sync_protocol)
		strncpy(sync_protocol, msg.tsync.sync_protocol_name,
				TRACECMD_TSYNC_PNAME_LENGTH);
	if (sync_msg_id)
		*sync_msg_id = ntohl(msg.tsync.sync_msg_id);

	buf_size = msg_buf_len(&msg);
	if (buf_size < 0) {
		ret = -EINVAL;
		goto out;
	}

	if (buf_size && payload && payload_size) {
		if (*payload_size) {
			if (*payload_size < buf_size || *payload == NULL) {
				ret = -ENOMEM;
				goto out;
			}
			memcpy(*payload, msg.buf, buf_size);
			goto out;
		}

		*payload = malloc(buf_size);
		if (*payload == NULL) {
			ret = -ENOMEM;
			goto out;
		}
		*payload_size = buf_size;
		memcpy(*payload, msg.buf, buf_size);
	}

out:
	msg_free(&msg);
	return ret;
}

static int make_trace_resp(struct tracecmd_msg *msg, int page_size, int nr_cpus,
			   unsigned int *ports, bool use_fifos,
			   unsigned long long trace_id,
			   const char *tsync_proto,
			   unsigned int tsync_port)
{
	int data_size;

	if (!tsync_proto)
		tsync_proto = "";

	data_size = write_uints(NULL, 0, ports, nr_cpus);
	msg->buf = malloc(data_size);
	if (!msg->buf)
		return -ENOMEM;
	write_uints(msg->buf, data_size, ports, nr_cpus);

	msg->hdr.size = htonl(ntohl(msg->hdr.size) + data_size);
	msg->trace_resp.flags = use_fifos ? MSG_TRACE_USE_FIFOS : 0;
	msg->trace_resp.flags = htonl(msg->trace_resp.flags);
	strncpy(msg->trace_resp.tsync_proto_name, tsync_proto, TRACECMD_TSYNC_PNAME_LENGTH);
	msg->trace_resp.tsync_port = htonl(tsync_port);

	msg->trace_resp.cpus = htonl(nr_cpus);
	msg->trace_resp.page_size = htonl(page_size);
	msg->trace_resp.trace_id = htonll(trace_id);

	return 0;
}

int tracecmd_msg_send_trace_resp(struct tracecmd_msg_handle *msg_handle,
				 int nr_cpus, int page_size,
				 unsigned int *ports, bool use_fifos,
				 unsigned long long trace_id,
				 const char *tsync_proto, unsigned int tsync_port)
{
	struct tracecmd_msg msg;
	int ret;

	tracecmd_msg_init(MSG_TRACE_RESP, &msg);
	ret = make_trace_resp(&msg, page_size, nr_cpus, ports,
			      use_fifos, trace_id, tsync_proto, tsync_port);
	if (ret < 0)
		return ret;

	return tracecmd_msg_send(msg_handle, &msg);
}

int tracecmd_msg_recv_trace_resp(struct tracecmd_msg_handle *msg_handle,
				 int *nr_cpus, int *page_size,
				 unsigned int **ports, bool *use_fifos,
				 unsigned long long *trace_id,
				 char **tsync_proto,
				 unsigned int *tsync_port)
{
	struct tracecmd_msg msg;
	char *p, *buf_end;
	ssize_t buf_len;
	int i, ret;

	ret = tracecmd_msg_recv(msg_handle->fd, &msg);
	if (ret < 0)
		return ret;

	if (ntohl(msg.hdr.cmd) != MSG_TRACE_RESP) {
		ret = -ENOTSUP;
		goto out;
	}

	buf_len = msg_buf_len(&msg);
	if (buf_len <= 0) {
		ret = -EINVAL;
		goto out;
	}

	*use_fifos = ntohl(msg.trace_resp.flags) & MSG_TRACE_USE_FIFOS;
	*nr_cpus = ntohl(msg.trace_resp.cpus);
	*page_size = ntohl(msg.trace_resp.page_size);
	*trace_id = ntohll(msg.trace_resp.trace_id);
	*tsync_proto = strdup(msg.trace_resp.tsync_proto_name);
	*tsync_port = ntohl(msg.trace_resp.tsync_port);
	*ports = calloc(*nr_cpus, sizeof(**ports));
	if (!*ports) {
		ret = -ENOMEM;
		goto out;
	}

	buf_end = msg.buf + buf_len;
	for (i = 0, p = msg.buf; i < *nr_cpus; i++, p++) {
		if (p >= buf_end || tatou(p, &(*ports)[i])) {
			free(*ports);
			ret = -EINVAL;
			goto out;
		}
		p = strchr(p, '\0');
	}

	msg_free(&msg);
	return 0;

out:
	error_operation(&msg);
	if (ret == -EOPNOTSUPP)
		handle_unexpected_msg(msg_handle, &msg);
	msg_free(&msg);
	return ret;
}
