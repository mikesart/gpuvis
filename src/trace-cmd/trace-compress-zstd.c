// SPDX-License-Identifier: LGPL-2.1
/*
 * Copyright (C) 2022, Sebastian Andrzej Siewior <sebastian@breakpoint.cc>
 *
 */
#include <stdlib.h>
#include <zstd.h>
#include <errno.h>

#include "trace-cmd-private.h"

#define __ZSTD_NAME		"zstd"
#define __ZSTD_WEIGTH		5

struct zstd_context {
	ZSTD_CCtx *ctx_c;
	ZSTD_DCtx *ctx_d;
};

static int zstd_compress(void *ctx, const void *in, int in_bytes, void *out, int out_bytes)
{
	struct zstd_context *context = ctx;
	size_t ret;

	if (!ctx)
		return -1;

	ret = ZSTD_compress2(context->ctx_c, out, out_bytes, in, in_bytes);
	if (ZSTD_isError(ret))
		return -1;

	return ret;
}

static int zstd_decompress(void *ctx, const void *in, int in_bytes, void *out, int out_bytes)
{
	struct zstd_context *context = ctx;
	size_t ret;

	if (!ctx)
		return -1;

	ret = ZSTD_decompressDCtx(context->ctx_d, out, out_bytes, in, in_bytes);
	if (ZSTD_isError(ret)) {
		errno = -EINVAL;
		return -1;
	}

	return ret;
}

static unsigned int zstd_compress_bound(void *ctx, unsigned int in_bytes)
{
	return ZSTD_compressBound(in_bytes);
}

static bool zstd_is_supported(const char *name, const char *version)
{
	if (!name)
		return false;
	if (strcmp(name, __ZSTD_NAME))
		return false;

	return true;
}

static void *new_zstd_context(void)
{
	struct zstd_context *context;
	size_t r;

	context = calloc(1, sizeof(*context));
	if (!context)
		return NULL;

	context->ctx_c = ZSTD_createCCtx();
	context->ctx_d = ZSTD_createDCtx();
	if (!context->ctx_c || !context->ctx_d)
		goto err;

	r = ZSTD_CCtx_setParameter(context->ctx_c, ZSTD_c_contentSizeFlag, 0);
	if (ZSTD_isError(r))
		goto err;

	return context;
err:
	ZSTD_freeCCtx(context->ctx_c);
	ZSTD_freeDCtx(context->ctx_d);
	free(context);
	return NULL;
}
static void free_zstd_context(void *ctx)
{
	struct zstd_context *context = ctx;

	if (!ctx)
		return;

	ZSTD_freeCCtx(context->ctx_c);
	ZSTD_freeDCtx(context->ctx_d);
	free(context);
}

int tracecmd_zstd_init(void)
{
	struct tracecmd_compression_proto proto;

	memset(&proto, 0, sizeof(proto));
	proto.name = __ZSTD_NAME;
	proto.version = ZSTD_versionString();
	proto.weight = __ZSTD_WEIGTH;
	proto.compress = zstd_compress;
	proto.uncompress = zstd_decompress;
	proto.is_supported = zstd_is_supported;
	proto.compress_size = zstd_compress_bound;
	proto.new_context = new_zstd_context;
	proto.free_context = free_zstd_context;

	return tracecmd_compress_proto_register(&proto);
}
