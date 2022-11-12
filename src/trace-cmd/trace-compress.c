// SPDX-License-Identifier: LGPL-2.1
/*
 * Copyright (C) 2021, VMware, Tzvetomir Stoyanov tz.stoyanov@gmail.com>
 *
 */
#include <stdlib.h>
#include <sys/time.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

#include "trace-cmd-private.h"
#include "trace-cmd-local.h"

struct compress_proto {
	struct compress_proto *next;
	char *proto_name;
	char *proto_version;
	int weight;

	int (*compress_block)(void *ctx, const void *in, int in_bytes, void *out, int out_bytes);
	int (*uncompress_block)(void *ctx, const void *in,  int in_bytes, void *out, int out_bytes);
	unsigned int (*compress_size)(void *ctx, unsigned int bytes);
	bool (*is_supported)(const char *name, const char *version);
	void *(*new_context)(void);
	void (*free_context)(void *ctx);
};

static struct compress_proto *proto_list;

struct tracecmd_compression {
	int				fd;
	unsigned int			capacity;
	unsigned int			capacity_read;
	unsigned int			pointer;
	char				*buffer;
	struct compress_proto		*proto;
	struct tep_handle		*tep;
	struct tracecmd_msg_handle	*msg_handle;
	void				*context;
};

static int read_fd(int fd, char *dst, int len)
{
	size_t size = 0;
	int r;

	do {
		r = read(fd, dst+size, len);
		if (r > 0) {
			size += r;
			len -= r;
		} else
			break;
	} while (r > 0);

	if (len)
		return -1;
	return size;
}

static long long write_fd(int fd, const void *data, size_t size)
{
	long long tot = 0;
	long long w;

	do {
		w = write(fd, data + tot, size - tot);
		tot += w;

		if (!w)
			break;
		if (w < 0)
			return w;
	} while (tot != size);

	return tot;
}

static long long do_write(struct tracecmd_compression *handle,
			  const void *data, unsigned long long size)
{
	int ret;

	if (handle->msg_handle) {
		ret = tracecmd_msg_data_send(handle->msg_handle, data, size);
		if (ret)
			return -1;
		return size;
	}
	return write_fd(handle->fd, data, size);
}

static inline int buffer_extend(struct tracecmd_compression *handle, unsigned int size)
{
	int extend;
	char *buf;

	if (size <= handle->capacity)
		return 0;

	extend = (size / BUFSIZ + 1) * BUFSIZ;
	buf = realloc(handle->buffer, extend);
	if (!buf)
		return -1;
	handle->buffer = buf;
	handle->capacity = extend;

	return 0;
}

/**
 * tracecmd_compress_lseek - Move the read/write pointer into the compression buffer
 * @handle: compression handle
 * @offset: number of bytes to move the pointer, can be negative or positive
 * @whence: the starting position of the pointer movement,
 *
 * Returns the new file pointer on success, or -1 in case of an error.
 */
off64_t tracecmd_compress_lseek(struct tracecmd_compression *handle, off64_t offset, int whence)
{
	unsigned long p;

	if (!handle || !handle->buffer)
		return (off64_t)-1;

	switch (whence) {
	case SEEK_CUR:
		p = handle->pointer + offset;
		break;
	case SEEK_END:
		p = handle->capacity + offset;
		break;
	case SEEK_SET:
		p = offset;
		break;
	default:
		return (off64_t)-1;
	}

	if (buffer_extend(handle, p))
		return (off64_t)-1;

	handle->pointer = p;

	return p;
}

static int compress_read(struct tracecmd_compression *handle, char *dst, int len)
{

	if (handle->pointer > handle->capacity_read)
		return -1;

	if (handle->pointer + len > handle->capacity_read)
		len = handle->capacity_read - handle->pointer;

	memcpy(dst, handle->buffer + handle->pointer, len);

	return len;
}

/**
 * tracecmd_compress_pread - pread() on compression buffer
 * @handle: compression handle
 * @dst: return, store the read data
 * @len: length of data to be read
 * @offset: offset in the buffer of data to be read
 *
 * Read a @len of data from the compression buffer at given @offset,
 * without updating the buffer pointer.
 *
 * On success returns the number of bytes read, or -1 on failure.
 */
int tracecmd_compress_pread(struct tracecmd_compression *handle, char *dst, int len, off_t offset)
{
	int ret;

	if (!handle || !handle->buffer || offset > handle->capacity_read)
		return -1;

	ret = tracecmd_compress_lseek(handle, offset, SEEK_SET);
	if (ret < 0)
		return ret;
	return compress_read(handle, dst, len);
}

/**
 * tracecmd_compress_buffer_read - read() from compression buffer
 * @handle: compression handle
 * @dst: return, store the read data
 * @len: length of data to be read
 *
 * Read a @len of data from the compression buffer
 *
 * On success returns the number of bytes read, or -1 on failure.
 */
int tracecmd_compress_buffer_read(struct tracecmd_compression *handle, char *dst, int len)
{
	int ret;

	if (!handle || !handle->buffer)
		return -1;

	ret = compress_read(handle, dst, len);
	if (ret > 0)
		handle->pointer += ret;

	return ret;
}

/**
 * tracecmd_compress_reset - Reset the compression buffer
 * @handle: compression handle
 *
 * Reset the compression buffer, any data currently in the buffer
 * will be destroyed.
 *
 */
void tracecmd_compress_reset(struct tracecmd_compression *handle)
{
	if (!handle)
		return;

	free(handle->buffer);
	handle->buffer = NULL;
	handle->pointer = 0;
	handle->capacity_read = 0;
	handle->capacity = 0;
}

/**
 * tracecmd_uncompress_block - uncompress a memory block
 * @handle: compression handle
 *
 * Read compressed memory block from the file and uncompress it into
 * internal buffer. The tracecmd_compress_buffer_read() can be used
 * to read the uncompressed data from the buffer.
 *
 * Returns 0 on success, or -1 in case of an error.
 */
int tracecmd_uncompress_block(struct tracecmd_compression *handle)
{
	unsigned int s_uncompressed;
	unsigned int s_compressed;
	char *bytes = NULL;
	char buf[4];
	int size;
	int ret;

	if (!handle || !handle->proto || !handle->proto->uncompress_block)
		return -1;

	tracecmd_compress_reset(handle);

	if (read(handle->fd, buf, 4) != 4)
		return -1;

	s_compressed = tep_read_number(handle->tep, buf, 4);
	if (read(handle->fd, buf, 4) != 4)
		return -1;

	s_uncompressed = tep_read_number(handle->tep, buf, 4);
	size = s_uncompressed > s_compressed ? s_uncompressed : s_compressed;

	handle->buffer = malloc(size);
	if (!handle->buffer)
		return -1;

	bytes = malloc(s_compressed);
	if (!bytes)
		goto error;

	if (read_fd(handle->fd, bytes, s_compressed) < 0)
		goto error;

	ret = handle->proto->uncompress_block(handle->context,
					      bytes, s_compressed, handle->buffer, size);
	if (ret < 0)
		goto error;

	free(bytes);
	handle->pointer = 0;
	handle->capacity_read = ret;
	handle->capacity = size;
	return 0;
error:
	tracecmd_compress_reset(handle);
	free(bytes);
	return -1;
}

/**
 * tracecmd_compress_block - compress a memory block
 * @handle: compression handle
 *
 * Compress the content of the internal memory buffer and write
 * the compressed data in the file. The tracecmd_compress_buffer_write()
 * can be used to write data into the internal memory buffer,
 * before calling this API.
 *
 * Returns 0 on success, or -1 in case of an error.
 */
int tracecmd_compress_block(struct tracecmd_compression *handle)
{
	unsigned int size, real_size;
	char *buf;
	int endian4;
	int ret;

	if (!handle || !handle->proto ||
	    !handle->proto->compress_size || !handle->proto->compress_block)
		return -1;

	size = handle->proto->compress_size(handle->context, handle->pointer);

	buf = malloc(size);
	if (!buf)
		return -1;

	real_size = handle->proto->compress_block(handle->context, handle->buffer, handle->pointer, buf, size);
	if (real_size < 0) {
		ret = real_size;
		goto out;
	}

	/* Write compressed data size */
	endian4 = tep_read_number(handle->tep, &real_size, 4);
	ret = do_write(handle, &endian4, 4);
	if (ret != 4)
		goto out;

	/* Write uncompressed data size */
	endian4 = tep_read_number(handle->tep, &handle->pointer, 4);
	ret = do_write(handle, &endian4, 4);
	if (ret != 4) {
		ret = -1;
		goto out;
	}

	/* Write compressed data */
	ret = do_write(handle, buf, real_size);
	if (ret != real_size) {
		ret = -1;
		goto out;
	}

	ret = 0;
	tracecmd_compress_reset(handle);
out:
	free(buf);
	return ret;
}

/**
 * tracecmd_compress_buffer_write - write() to compression buffer
 * @handle: compression handle
 * @data: data to be written
 * @size: size of @data
 *
 * Write @data of @size in the compression buffer
 *
 * Returns 0 on success, or -1 on failure.
 */
int tracecmd_compress_buffer_write(struct tracecmd_compression *handle,
				   const void *data, unsigned long long size)
{
	if (!handle)
		return -1;

	if (buffer_extend(handle, handle->pointer + size))
		return -1;

	memcpy(&handle->buffer[handle->pointer], data, size);
	handle->pointer += size;
	if (handle->capacity_read < handle->pointer)
		handle->capacity_read = handle->pointer;

	return 0;
}

/**
 * tracecmd_compress_init - initialize the library with available compression algorithms
 */
void tracecmd_compress_init(void)
{
	struct timeval time;

	gettimeofday(&time, NULL);
	srand((time.tv_sec * 1000) + (time.tv_usec / 1000));

#ifdef HAVE_ZLIB
	tracecmd_zlib_init();
#endif
	tracecmd_zstd_init();
}

static struct compress_proto *compress_proto_select(void)
{
	struct compress_proto *proto = proto_list;
	struct compress_proto *selected = NULL;

	while (proto) {
		if (!selected || selected->weight > proto->weight)
			selected = proto;
		proto = proto->next;
	}

	return selected;
}

/**
 * tracecmd_compress_alloc - Allocate a new compression context
 * @name: name of the compression algorithm.
 *        If NULL - auto select the best available algorithm
 * @version: version of the compression algorithm, can be NULL
 * @fd: file descriptor for reading / writing data
 * @tep: tep handle, used to encode the data
 * @msg_handle: message handle, use it for reading / writing data instead of @fd
 *
 * Returns NULL on failure or pointer to allocated compression context.
 * The returned context must be freed by tracecmd_compress_destroy()
 */
struct tracecmd_compression *tracecmd_compress_alloc(const char *name, const char *version,
						     int fd, struct tep_handle *tep,
						     struct tracecmd_msg_handle *msg_handle)
{
	struct tracecmd_compression *new;
	struct compress_proto *proto;

	if (name) {
		proto = proto_list;
		while (proto) {
			if (proto->is_supported && proto->is_supported(name, version))
				break;
			proto = proto->next;
		}
	} else {
		proto = compress_proto_select();
	}
	if (!proto)
		return NULL;

	new = calloc(1, sizeof(*new));
	if (!new)
		return NULL;

	new->fd = fd;
	new->tep = tep;
	new->msg_handle = msg_handle;
	new->proto = proto;
	if (proto->new_context)
		new->context = proto->new_context();

	return new;
}

/**
 * tracecmd_compress_destroy - Free a compression context
 * @handle: handle to the compression context that will be freed
 */
void tracecmd_compress_destroy(struct tracecmd_compression *handle)
{
	if (!handle)
		return;

	tracecmd_compress_reset(handle);

	if (handle->proto && handle->proto->free_context)
		handle->proto->free_context(handle->context);

	free(handle);
}

/**
 * tracecmd_compress_is_supported - check if compression algorithm is supported
 * @name: name of the compression algorithm.
 * @version: version of the compression algorithm.
 *
 * Checks if compression algorithm with given name and version is supported.
 * Returns true if the algorithm is supported or false if it is not.
 */
bool tracecmd_compress_is_supported(const char *name, const char *version)
{
	struct compress_proto *proto = proto_list;

	if (!name)
		return NULL;

	while (proto) {
		if (proto->is_supported && proto->is_supported(name, version))
			return true;
		proto = proto->next;
	}
	return false;
}

/**
 * tracecmd_compress_proto_get_name - get name and version of compression algorithm
 * @compress: compression handle.
 * @name: return, name of the compression algorithm.
 * @version: return, version of the compression algorithm.
 *
 * Returns 0 on success, or -1 in case of an error. If 0 is returned, the name
 * and version of the algorithm are stored in @name and @version. The returned
 * strings must *not* be freed.
 */
int tracecmd_compress_proto_get_name(struct tracecmd_compression *compress,
				     const char **name, const char **version)
{
	if (!compress || !compress->proto)
		return -1;

	if (name)
		*name = compress->proto->proto_name;
	if (version)
		*version = compress->proto->proto_version;

	return 0;
}

/**
 * tracecmd_compress_proto_register - register a new compression algorithm
 * @name: name of the compression algorithm.
 * @version: version of the compression algorithm.
 * @weight: weight of the compression algorithm, lower is better.
 * @compress: compression hook, called to compress a memory block.
 * @uncompress: uncompression hook, called to uncompress a memory block.
 * @compress_size: hook, called to get the required minimum size of the buffer
 *		   for compression given number of bytes.
 * @is_supported: check hook, called to check if compression with given name and
 *		   version is supported by this plugin.
 *
 * Returns 0 on success, or -1 in case of an error. If algorithm with given name
 * and version is already registered, -1 is returned.
 */
int tracecmd_compress_proto_register(struct tracecmd_compression_proto *proto)
{
	struct compress_proto *new;

	if (!proto || !proto->name || !proto->compress || !proto->uncompress)
		return -1;

	if (tracecmd_compress_is_supported(proto->name, proto->version))
		return -1;

	new = calloc(1, sizeof(*new));
	if (!new)
		return -1;

	new->proto_name = strdup(proto->name);
	if (!new->proto_name)
		goto error;

	new->proto_version = strdup(proto->version);
	if (!new->proto_version)
		goto error;

	new->compress_block = proto->compress;
	new->uncompress_block = proto->uncompress;
	new->compress_size = proto->compress_size;
	new->is_supported = proto->is_supported;
	new->weight = proto->weight;
	new->next = proto_list;
	new->new_context = proto->new_context;
	new->free_context = proto->free_context;
	proto_list = new;
	return 0;

error:
	free(new->proto_name);
	free(new->proto_version);
	free(new);
	return -1;
}

/**
 * tracecmd_compress_free - free the library resources, related to available compression algorithms
 *
 */
void tracecmd_compress_free(void)
{
	struct compress_proto *proto = proto_list;
	struct compress_proto *del;

	while (proto) {
		del = proto;
		proto = proto->next;
		free(del->proto_name);
		free(del->proto_version);
		free(del);
	}
	proto_list = NULL;
}

/**
 * tracecmd_compress_protos_get - get a list of all supported compression algorithms and versions
 * @names: return, array with names of all supported compression algorithms
 * @versions: return, array with versions of all supported compression algorithms
 *
 * On success, the size of @names and @versions arrays is returned.
 * Those arrays are allocated by the API and must be freed with free() by the
 * caller. Both arrays are with same size, each name from @names corresponds to
 * a version from @versions. The last element in both arrays is a NULL pointer.
 * On error -1 is returned and @names and @versions arrays are not allocated.
 */
int tracecmd_compress_protos_get(char ***names, char ***versions)
{
	struct compress_proto *proto = proto_list;
	char **n = NULL;
	char **v = NULL;
	int c, i;

	for (c = 0; proto; proto = proto->next)
		c++;

	if (c < 1)
		return c;

	n = calloc(c + 1, sizeof(char *));
	if (!n)
		goto error;
	v = calloc(c + 1, sizeof(char *));
	if (!v)
		goto error;

	proto = proto_list;
	for (i = 0; i < c && proto; i++) {
		n[i] = proto->proto_name;
		v[i] = proto->proto_version;
		proto = proto->next;
	}

	n[i] = NULL;
	v[i] = NULL;
	*names = n;
	*versions = v;
	return c;

error:
	free(n);
	free(v);
	return -1;
}

/**
 * tracecmd_compress_copy_from - Copy and compress data from a file
 * @handle: compression handle
 * @fd: file descriptor to uncompressed data to copy from
 * @chunk_size: size of one compression chunk
 * @read_size: Pointer to max bytes to read from. The pointer is updated
 *	       with the actual size of compressed data read. If 0 is passed,
 *	       read until the EOF is reached.
 * @write_size: return, size of the compressed data written into @handle
 *
 * This function reads uncompressed data from given @fd, compresses the data
 * using the @handle compression context and writes the compressed data into the
 * fd associated with the @handle. The data is compressed on chunks with given
 * @chunk_size size. The compressed data is written in the format:
 *  - 4 bytes, chunks count
 *  - for each chunk:
 *    - 4 bytes, size of compressed data in this chunk
 *    - 4 bytes, uncompressed size of the data in this chunk
 *    - data, bytes of <size of compressed data in this chunk>
 *
 * On success 0 is returned, @read_size and @write_size are updated with the size of
 * read and written data.
 */
int tracecmd_compress_copy_from(struct tracecmd_compression *handle, int fd, int chunk_size,
				unsigned long long *read_size, unsigned long long *write_size)
{
	unsigned int rchunk = 0;
	unsigned int chunks = 0;
	unsigned int wsize = 0;
	unsigned int rsize = 0;
	unsigned int rmax = 0;
	unsigned int csize;
	unsigned int size;
	unsigned int all;
	unsigned int r;
	off64_t offset;
	char *buf_from;
	char *buf_to;
	int endian4;
	int ret;

	if (!handle || !handle->proto ||
	    !handle->proto->compress_block || !handle->proto->compress_size)
		return 0;

	if (read_size)
		rmax = *read_size;
	csize = handle->proto->compress_size(handle->context, chunk_size);
	buf_from = malloc(chunk_size);
	if (!buf_from)
		return -1;

	buf_to = malloc(csize);
	if (!buf_to)
		return -1;

	/* save the initial offset and write 0 as initial chunk count */
	offset = lseek64(handle->fd, 0, SEEK_CUR);
	write_fd(handle->fd, &chunks, 4);

	do {
		all = 0;
		if (rmax > 0 && (rmax - rsize) < chunk_size)
			rchunk = (rmax - rsize);
		else
			rchunk = chunk_size;

		do {
			r = read(fd, buf_from + all, rchunk - all);
			all += r;

			if (r <= 0)
				break;
		} while (all != rchunk);


		if (r < 0 || (rmax > 0 && rsize >= rmax))
			break;
		rsize += all;
		size = csize;
		if (all > 0) {
			ret = handle->proto->compress_block(handle->context,
							    buf_from, all, buf_to, size);
			if (ret < 0) {
				if (errno == EINTR)
					continue;
				break;
			}
			size = ret;
			/* Write compressed data size */
			endian4 = tep_read_number(handle->tep, &size, 4);
			ret = write_fd(handle->fd, &endian4, 4);
			if (ret != 4)
				break;

			/* Write uncompressed data size */
			endian4 = tep_read_number(handle->tep, &all, 4);
			ret = write_fd(handle->fd, &endian4, 4);
			if (ret != 4)
				break;

			/* Write the compressed data */
			ret = write_fd(handle->fd, buf_to, size);
			if (ret != size)
				break;
			/* data + compress header */
			wsize += (size + 8);
			chunks++;
		}
	} while (all > 0);

	free(buf_from);
	free(buf_to);

	if (all)
		return -1;

	if (lseek64(handle->fd, offset, SEEK_SET) == (off_t)-1)
		return -1;

	endian4 = tep_read_number(handle->tep, &chunks, 4);
	/* write chunks count*/
	write_fd(handle->fd, &chunks, 4);
	if (lseek64(handle->fd, 0, SEEK_END) == (off_t)-1)
		return -1;

	if (read_size)
		*read_size = rsize;
	if (write_size)
		*write_size = wsize;
	return 0;
}

/**
 * tracecmd_load_chunks_info - Read compression chunks information from the file
 * @handle: compression handle
 * @chunks_info: return, array with compression chunks information
 *
 * This function reads information of all compression chunks in the current
 * compression block from the file and fills that information in a newly
 * allocated array @chunks_info which is returned.
 *
 * On success count of compression chunks is returned. Array of that count is
 * allocated and returned in @chunks_info. Each entry describes one compression
 * chunk. On error -1 is returned. In case of success, @chunks_info must be
 * freed by free().
 */
int tracecmd_load_chunks_info(struct tracecmd_compression *handle,
			      struct tracecmd_compress_chunk **chunks_info)
{
	struct tracecmd_compress_chunk *chunks = NULL;
	unsigned long long size = 0;
	unsigned int count = 0;
	off64_t offset;
	int ret = -1;
	char buf[4];
	int i;

	if (!handle)
		return -1;

	offset = lseek64(handle->fd, 0, SEEK_CUR);
	if (offset == (off64_t)-1)
		return -1;

	if (read(handle->fd, buf, 4) != 4)
		return -1;

	count = tep_read_number(handle->tep, buf, 4);
	if (!count) {
		ret = 0;
		goto out;
	}

	chunks = calloc(count, sizeof(struct tracecmd_compress_chunk));
	if (!chunks)
		goto out;

	for (i = 0; i < count; i++) {
		chunks[i].zoffset = lseek64(handle->fd, 0, SEEK_CUR);
		if (chunks[i].zoffset == (off_t)-1)
			goto out;
		if (read(handle->fd, buf, 4) != 4)
			goto out;
		chunks[i].zsize = tep_read_number(handle->tep, buf, 4);
		chunks[i].offset = size;
		if (read(handle->fd, buf, 4) != 4)
			goto out;
		chunks[i].size = tep_read_number(handle->tep, buf, 4);
		size += chunks[i].size;
		if (lseek64(handle->fd, chunks[i].zsize, SEEK_CUR) == (off64_t)-1)
			goto out;
	}

	ret = count;
out:
	if (lseek64(handle->fd, offset, SEEK_SET) == (off64_t)-1)
		ret = -1;

	if (ret > 0 && chunks_info)
		*chunks_info = chunks;
	else
		free(chunks);

	return ret;
}

/**
 * tracecmd_uncompress_chunk - Uncompress given compression chunk.
 * @handle: compression handle
 * @chunk: chunk, that will be uncompressed in @data
 * @data: Preallocated memory for uncompressed data. Must have enough space
 * to hold the uncompressed data.
 *
 * This function uncompresses the chunk described by @chunk and stores
 * the uncompressed data in the preallocated memory @data.
 *
 * On success 0 is returned and the uncompressed data is stored in @data.
 * On error -1 is returned.
 */
int tracecmd_uncompress_chunk(struct tracecmd_compression *handle,
			      struct tracecmd_compress_chunk *chunk, char *data)
{
	char *bytes_in = NULL;
	int ret = -1;

	if (!handle || !handle->proto || !handle->proto->uncompress_block || !chunk || !data)
		return -1;

	if (lseek64(handle->fd, chunk->zoffset + 8, SEEK_SET) == (off_t)-1)
		return -1;

	bytes_in = malloc(chunk->zsize);
	if (!bytes_in)
		return -1;

	if (read_fd(handle->fd, bytes_in, chunk->zsize) < 0)
		goto out;

	if (handle->proto->uncompress_block(handle->context,
					    bytes_in, chunk->zsize, data, chunk->size) < 0)
		goto out;

	ret = 0;
out:
	free(bytes_in);
	return ret;
}

/**
 * tracecmd_uncompress_copy_to - Uncompress data and copy to a file
 * @handle: compression handle
 * @fd: file descriptor to uncompressed data to copy into
 * @read_size: return, size of the compressed data read from @handle
 * @write_size: return, size of the uncompressed data written into @fd
 *
 * This function reads compressed data from the fd, associated with @handle,
 * uncompresses it using the @handle compression context and writes
 * the uncompressed data into the fd. The compressed data must be in the format:
 *  - 4 bytes, chunks count
 *  - for each chunk:
 *    - 4 bytes, size of compressed data in this chunk
 *    - 4 bytes, uncompressed size of the data in this chunk
 *    - data, bytes of <size of compressed data in this chunk>
 *
 * On success 0 is returned, @read_size and @write_size are updated with
 * the size of read and written data.
 */
int tracecmd_uncompress_copy_to(struct tracecmd_compression *handle, int fd,
				unsigned long long *read_size, unsigned long long *write_size)
{
	unsigned int s_uncompressed;
	unsigned int s_compressed;
	unsigned int rsize = 0;
	unsigned int wsize = 0;
	char *bytes_out = NULL;
	char *bytes_in = NULL;
	int size_out = 0;
	int size_in = 0;
	int chunks;
	char buf[4];
	char *tmp;
	int ret;

	if (!handle || !handle->proto || !handle->proto->uncompress_block)
		return -1;

	if (read(handle->fd, buf, 4) != 4)
		return -1;

	chunks = tep_read_number(handle->tep, buf, 4);
	rsize += 4;

	while (chunks) {
		if (read(handle->fd, buf, 4) != 4)
			break;

		s_compressed = tep_read_number(handle->tep, buf, 4);
		rsize += 4;
		if (read(handle->fd, buf, 4) != 4)
			break;

		s_uncompressed = tep_read_number(handle->tep, buf, 4);
		rsize += 4;
		if (!bytes_in || size_in < s_compressed) {
			tmp = realloc(bytes_in, s_compressed);
			if (!tmp)
				break;

			bytes_in = tmp;
			size_in = s_compressed;
		}

		if (!bytes_out || size_out < s_uncompressed) {
			tmp = realloc(bytes_out, s_uncompressed);
			if (!tmp)
				break;
			bytes_out = tmp;
			size_out = s_uncompressed;
		}

		if (read_fd(handle->fd, bytes_in, s_compressed) < 0)
			break;

		rsize += s_compressed;
		ret = handle->proto->uncompress_block(handle->context, bytes_in, s_compressed,
						      bytes_out, s_uncompressed);
		if (ret < 0)
			break;

		write_fd(fd, bytes_out, ret);
		wsize += ret;
		chunks--;
	}
	free(bytes_in);
	free(bytes_out);
	if (chunks)
		return -1;

	if (read_size)
		*read_size = rsize;
	if (write_size)
		*write_size = wsize;

	return 0;
}
