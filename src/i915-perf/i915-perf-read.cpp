/*
 * Copyright 2019 Intel Corporation
 *
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef _LARGEFILE64_SOURCE
#define _LARGEFILE64_SOURCE
#endif

#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <string>
#include <vector>
#include <forward_list>
#include <csetjmp>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <future>
#include <string.h>

#ifdef USE_I915_PERF
#include <perf.h>
#include <perf_data_reader.h>
#endif

#include "../gpuvis_macros.h"
#include "../trace-cmd/trace-read.h"
#include "i915-perf-read.h"

void logf( const char *fmt, ... ) ATTRIBUTE_PRINTF( 1, 2 );

int read_i915_perf_file( const char *file, StrPool &strpool, trace_info_t &trace_info, struct intel_perf_data_reader **out_reader, EventCallback &cb )
{
    GPUVIS_TRACE_BLOCK( __func__ );

#ifdef USE_I915_PERF
    int fd = open( file, O_RDONLY );
    if ( fd < 0 )
    {
        logf( "[Error] %s: opening i915-perf file failed: %s.\n", __func__, strerror( errno ) );
        return -1;
    }

    *out_reader = new intel_perf_data_reader;
    struct intel_perf_data_reader *reader = *out_reader;

    if ( !intel_perf_data_reader_init( reader, fd ) )
    {
        logf( "[Error] %s: initializing i915-perf reader failed: %s.\n", __func__, reader->error_msg );
        return -1;
    }

    trace_info.file = std::string(file);

    for (uint32_t i = 0; i < reader->n_timelines; i++)
    {
        // Skip the idle time
        if ( reader->timelines[i].hw_id == 0xffffffff )
            continue;

        trace_event_t event;

        if ( reader->timelines[i].cpu_ts_start < trace_info.min_file_ts )
            continue;

        // Abuse PID, this is kind of a similar concept...
        event.flags = TRACE_FLAG_I915_PERF;
        event.pid = reader->timelines[i].hw_id;
        event.cpu = 0;
        event.comm = strpool.getstr( "i915-perf" );
        event.system = strpool.getstr( "i915-perf" );
        event.user_comm = strpool.getstrf( "[i915-perf hw_id=0x%x]", reader->timelines[i].hw_id );

        event.name = strpool.getstr( "i915-perf-begin" );
        event.ts = reader->timelines[i].cpu_ts_start;
        event.duration = reader->timelines[i].cpu_ts_end - reader->timelines[i].cpu_ts_start;

        // This will track the timeline number and allow us to compute counter
        // deltas.
        event.i915_perf_timeline = i;

        cb( event );

        event.name = strpool.getstr( "i915-perf-end" );
        // The GPU context switch event happens on a single timestamp. Make
        // begin/end look like they happen at different time so that events
        // are ordered properly.
        event.ts = reader->timelines[i].cpu_ts_end - 1;
        event.duration = INT64_MAX;

        cb( event );
    }
#endif

    return 0;
}
