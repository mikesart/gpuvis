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

#ifndef I915_PERF_READ_H_
#define I915_PERF_READ_H_

#include <functional>

#define __STDC_FORMAT_MACROS
#include <inttypes.h>

extern "C" {
    struct intel_perf_data_reader;
    struct intel_perf_logical_counter;
}

typedef std::function< void ( const trace_event_t &event, int64_t ts, float value ) > I915CounterCallback;

int read_i915_perf_file( const char *file, StrPool &strpool, trace_info_t &trace_info, struct intel_perf_data_reader **reader, EventCallback &cb );
struct intel_perf_logical_counter *get_i915_perf_frequency_counter( struct intel_perf_data_reader *reader );
void load_i915_perf_counter_values( struct intel_perf_data_reader *reader,
                                    struct intel_perf_logical_counter *counter,
                                    const trace_event_t &event, I915CounterCallback &cb );

#endif // I915_PERF_READ_H_
