/* SPDX-License-Identifier: LGPL-2.1 */
#ifndef _TRACE_MSG_H_
#define _TRACE_MSG_H_

#include <stdbool.h>

#define UDP_MAX_PACKET	(65536 - 20)
#define V3_MAGIC	"766679\0"
#define V3_CPU		"-1V3"

#define V1_PROTOCOL	1
#define V3_PROTOCOL	3

extern unsigned int page_size;

#endif /* _TRACE_MSG_H_ */
