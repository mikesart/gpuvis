/* SPDX-License-Identifier: LGPL-2.1 */
/*
 * Copyright (C) 2010 Red Hat Inc, Steven Rostedt <srostedt@redhat.com>
 *
 */
#ifndef _TRACE_CMD_LOCAL_H
#define _TRACE_CMD_LOCAL_H

#include <byteswap.h>
#include "trace-cmd-private.h"

/* Can be overridden */
void tracecmd_warning(const char *fmt, ...);
void tracecmd_critical(const char *fmt, ...);
void tracecmd_info(const char *fmt, ...);

/* trace.dat file format version */
#define FILE_VERSION 6

#define _STR(x)	#x
#define STR(x)	_STR(x)
#define FILE_VERSION_STRING STR(FILE_VERSION)

#ifndef htonll
# if __BYTE_ORDER == __LITTLE_ENDIAN
#define htonll(x) __bswap_64(x)
#define ntohll(x) __bswap_64(x)
#else
#define htonll(x) (x)
#define ntohll(x) (x)
#endif
#endif


#endif /* _TRACE_CMD_LOCAL_H */
