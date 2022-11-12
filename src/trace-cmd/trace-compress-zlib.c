// SPDX-License-Identifier: LGPL-2.1
/*
 * Copyright (C) 2021, VMware, Tzvetomir Stoyanov tz.stoyanov@gmail.com>
 *
 */
#include <stdlib.h>
#include <dlfcn.h>
#include <zlib.h>
#include <errno.h>

#include "trace-cmd-private.h"

#define __ZLIB_NAME		"zlib"
#define __ZLIB_WEIGTH		10

static int zlib_compress(void *ctx, const void *in, int in_bytes, void *out, int out_bytes)
{
	unsigned long obytes = out_bytes;
	int ret;

	ret = compress2((unsigned char *)out, &obytes,
			(unsigned char *)in, (unsigned long)in_bytes, Z_BEST_COMPRESSION);
	switch (ret) {
	case Z_OK:
		return obytes;
	case Z_BUF_ERROR:
		errno = -ENOBUFS;
		break;
	case Z_MEM_ERROR:
		errno = -ENOMEM;
		break;
	case Z_STREAM_ERROR:
		errno = -EINVAL;
		break;
	case Z_ERRNO:
		break;
	default:
		errno = -EFAULT;
		break;
	}

	return -1;
}

static int zlib_decompress(void *ctx, const void *in, int in_bytes, void *out, int out_bytes)
{
	unsigned long obytes = out_bytes;
	int ret;

	ret = uncompress((unsigned char *)out, &obytes,
			 (unsigned char *)in, (unsigned long)in_bytes);
	switch (ret) {
	case Z_OK:
		return obytes;
	case Z_BUF_ERROR:
		errno = -ENOBUFS;
		break;
	case Z_MEM_ERROR:
		errno = -ENOMEM;
		break;
	case Z_DATA_ERROR:
		errno = -EINVAL;
		break;
	case Z_ERRNO:
		break;
	default:
		errno = -EFAULT;
		break;
	}

	return -1;
}

static unsigned int zlib_compress_bound(void *ctx, unsigned int in_bytes)
{
	return compressBound(in_bytes);
}

static bool zlib_is_supported(const char *name, const char *version)
{
	const char *zver;

	if (!name)
		return false;
	if (strlen(name) != strlen(__ZLIB_NAME) || strcmp(name, __ZLIB_NAME))
		return false;

	if (!version)
		return true;

	zver = zlibVersion();
	if (!zver)
		return false;

	/* Compare the major version number */
	if (atoi(version) <= atoi(zver))
		return true;

	return false;
}

int tracecmd_zlib_init(void)
{
	struct tracecmd_compression_proto proto;

	memset(&proto, 0, sizeof(proto));
	proto.name = __ZLIB_NAME;
	proto.version = zlibVersion();
	proto.weight = __ZLIB_WEIGTH;
	proto.compress = zlib_compress;
	proto.uncompress = zlib_decompress;
	proto.is_supported = zlib_is_supported;
	proto.compress_size = zlib_compress_bound;

	return tracecmd_compress_proto_register(&proto);
}
