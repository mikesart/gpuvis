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

#include <assert.h>
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

#if USE_I915_PERF
static uint32_t record_timestamp( const struct drm_i915_perf_record_header *record )
{
    const uint32_t *data = ( const uint32_t * )( record + 1 );
    return data[ 1 ];
}
#endif

void load_i915_perf_counter_values( struct intel_perf_data_reader *reader,
                                    struct intel_perf_logical_counter *counter,
                                    const trace_event_t &event, I915CounterCallback &cb )
{
#if USE_I915_PERF
    assert( event.i915_perf_timeline < reader->n_timelines );

    const struct intel_perf_timeline_item *item = &reader->timelines[ event.i915_perf_timeline ];
    const struct drm_i915_perf_record_header *first_record = reader->records[ item->record_start ];
    for ( uint32_t j = item->record_start; j < item->record_end; j++ )
    {
        const struct drm_i915_perf_record_header *record = reader->records[j];
        int64_t ts = item->cpu_ts_start +
            ( record_timestamp( record ) - record_timestamp( first_record ) ) *
            ( item->cpu_ts_end - item->cpu_ts_start ) / ( item->ts_end - item->ts_start );
        struct intel_perf_accumulator acc;

        intel_perf_accumulate_reports( &acc, reader->metric_set->perf_oa_format,
                                       reader->records[j], reader->records[j + 1] );

        float value;
        if ( counter->storage == INTEL_PERF_LOGICAL_COUNTER_STORAGE_DOUBLE ||
             counter->storage == INTEL_PERF_LOGICAL_COUNTER_STORAGE_FLOAT )
        {
            value = counter->read_float( reader->perf, reader->metric_set, acc.deltas );
        }
        else
        {
            value = counter->read_uint64( reader->perf, reader->metric_set, acc.deltas );
        }

        cb( event, ts, value / 1000000.0 ); // Report the frequency in MHz, not Hz
    }
#endif
}

struct intel_perf_logical_counter *get_i915_perf_frequency_counter( struct intel_perf_data_reader *reader )
{
#if USE_I915_PERF
    struct intel_perf_metric_set *metric_set = reader->metric_set;

    for ( uint32_t i = 0; i < metric_set->n_counters; i++ )
    {
        struct intel_perf_logical_counter *counter = &metric_set->counters[ i ];

        if ( strcmp( counter->symbol_name, "AvgGpuCoreFrequency" ) == 0 )
        {
            return counter;
        }
    }

    return NULL;
#else
    return NULL;
#endif
}
