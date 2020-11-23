/*
 * Copyright 2020 Intel Corporation
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

#include <array>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <string>

#include <SDL.h>

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"   // BeginColumns(), EndColumns(), PushColumnClipRect()

#include "gpuvis_macros.h"
#include "stlini.h"
#include "trace-cmd/trace-read.h"
#include "gpuvis_utils.h"
#include "gpuvis.h"

#ifdef USE_I915_PERF

#include <perf.h>
#include <perf_data_reader.h>

static void
pretty_print_value(intel_perf_logical_counter_unit_t unit,
                   double value,
                   char *buffer, size_t length)
{
    static const char *times[] = { "ns", "us", "ms", "s" };
    static const char *bytes[] = { "B", "KiB", "MiB", "GiB" };
    static const char *freqs[] = { "Hz", "KHz", "MHz", "GHz" };
    static const char *texels[] = { "texels", "K texels", "M texels", "G texels" };
    static const char *pixels[] = { "pixels", "K pixels", "M pixels", "G pixels" };
    static const char *cycles[] = { "cycles", "K cycles", "M cycles", "G cycles" };
    static const char *threads[] = { "threads", "K threads", "M threads", "G threads" };
    const char **scales = NULL;

    switch (unit) {
    case INTEL_PERF_LOGICAL_COUNTER_UNIT_BYTES:   scales = bytes; break;
    case INTEL_PERF_LOGICAL_COUNTER_UNIT_HZ:      scales = freqs; break;
    case INTEL_PERF_LOGICAL_COUNTER_UNIT_NS:
    case INTEL_PERF_LOGICAL_COUNTER_UNIT_US:      scales = times; break;
    case INTEL_PERF_LOGICAL_COUNTER_UNIT_PIXELS:  scales = pixels; break;
    case INTEL_PERF_LOGICAL_COUNTER_UNIT_TEXELS:  scales = texels; break;
    case INTEL_PERF_LOGICAL_COUNTER_UNIT_THREADS: scales = threads; break;
    case INTEL_PERF_LOGICAL_COUNTER_UNIT_CYCLES:  scales = cycles; break;
    default: break;
    }

    if (scales) {
        const double base = unit == INTEL_PERF_LOGICAL_COUNTER_UNIT_BYTES ? 1024 : 1000;

        if (unit == INTEL_PERF_LOGICAL_COUNTER_UNIT_US)
            value *= 1000;

        int i = 0;
        while (value >= base && i < 3) {
            value /= base;
            i++;
        }
        snprintf(buffer, length, "%.4g %s", value, scales ? scales[i] : "");
    } else {
        if (unit == INTEL_PERF_LOGICAL_COUNTER_UNIT_PERCENT)
            snprintf(buffer, length, "%.3g %%", value);
        else
            snprintf(buffer, length, "%.2f", value);
    }
}

void I915PerfCounters::init( TraceEvents &trace_events )
{
    m_trace_events = &trace_events;

    m_counters.clear();

    if (!m_trace_events->i915_perf_reader)
        return;

    const struct intel_perf_metric_set *metric_set =
        m_trace_events->i915_perf_reader->metric_set;

    for ( uint32_t c = 0; c < metric_set->n_counters; c++ )
    {
        struct intel_perf_logical_counter *counter = &metric_set->counters[c];

        i915_perf_counter_t dcounter;

        dcounter.name = std::string(counter->name);
        dcounter.desc = std::string(counter->desc);
        dcounter.type =
            (counter->storage == INTEL_PERF_LOGICAL_COUNTER_STORAGE_FLOAT ||
             counter->storage == INTEL_PERF_LOGICAL_COUNTER_STORAGE_DOUBLE) ?
            i915_perf_counter_t::type::FLOAT :
            i915_perf_counter_t::type::INTEGER;

        m_counters.push_back(dcounter);
    }
}

void I915PerfCounters::set_event( const trace_event_t &event )
{
    if ( m_event_id == event.id || event.id == INVALID_ID )
        return;

    m_event_id = event.id;

    assert( event.i915_perf_timeline != INVALID_ID );

    const struct intel_perf_metric_set *metric_set =
        m_trace_events->i915_perf_reader->metric_set;
    const struct intel_perf_timeline_item *timeline_item =
        &m_trace_events->i915_perf_reader->timelines[event.i915_perf_timeline];
    const struct drm_i915_perf_record_header *record_start =
        m_trace_events->i915_perf_reader->records[timeline_item->record_start];
    const struct drm_i915_perf_record_header *record_end =
        m_trace_events->i915_perf_reader->records[timeline_item->record_end];

    struct intel_perf_accumulator accu;
    intel_perf_accumulate_reports( &accu, metric_set->perf_oa_format,
                                   record_start, record_end );

    m_n_reports = timeline_item->record_end - timeline_item->record_start;

    for ( uint32_t c = 0; c < metric_set->n_counters; c++ )
    {
        struct intel_perf_logical_counter *counter = &metric_set->counters[c];
        struct i915_perf_counter_t &dcounter = m_counters[c];

        if ( m_counters[c].type == i915_perf_counter_t::type::FLOAT )
        {
            dcounter.value.f = counter->read_float( m_trace_events->i915_perf_reader->perf,
                                                    metric_set, accu.deltas );
            if ( counter->max_float )
            {
                dcounter.max_value.f = counter->max_float( m_trace_events->i915_perf_reader->perf,
                                                           metric_set, accu.deltas );
            }
            else
            {
                dcounter.max_value.f = 0.0f;
            }
            pretty_print_value( counter->unit, dcounter.value.f,
                                dcounter.pretty_value, sizeof(dcounter.pretty_value) );
        }
        else
        {
            dcounter.value.u = counter->read_uint64( m_trace_events->i915_perf_reader->perf,
                                                     metric_set, accu.deltas );
            if ( counter->max_uint64 )
            {
                dcounter.max_value.u = counter->max_uint64( m_trace_events->i915_perf_reader->perf,
                                                            metric_set, accu.deltas );
            }
            else
            {
                dcounter.max_value.u = 0;
            }
            pretty_print_value( counter->unit, dcounter.value.u,
                                dcounter.pretty_value, sizeof(dcounter.pretty_value) );
        }
    }
}

I915PerfCounters::i915_perf_process
I915PerfCounters::get_process( const trace_event_t &i915_perf_event )
{
    i915_perf_process process;
    process.label = "<unknown>";
    process.color = s_clrs().get( col_Graph_i915Perf );

    uint32_t *req_event_id = m_trace_events->m_i915.perf_to_req_in.get_val( i915_perf_event.id );

    if ( req_event_id )
    {
        const trace_event_t &req_event = m_trace_events->m_events[ *req_event_id ];
        process.label = req_event.comm;

        const std::vector< uint32_t > *sched_plocs =
            m_trace_events->get_sched_switch_locs( req_event.pid,
                                                   TraceEvents::SCHED_SWITCH_PREV );
        if ( sched_plocs )
        {
            const trace_event_t &e = m_trace_events->m_events[ sched_plocs->back() ];
            process.color = e.color;
        }
    }

    return process;
}

void I915PerfCounters::render()
{
    if ( m_event_id == INVALID_ID )
        return;

    m_filter.Draw();
    ImGui::SameLine();

    i915_perf_process process = get_process( m_trace_events->m_events[ m_event_id ] );
    ImGui::Text( "Process: %s", process.label );
    ImGui::SameLine();
    ImGui::ColorButton( "##process_color", ImColor( process.color ),
                        ImGuiColorEditFlags_NoInputs |
                        ImGuiColorEditFlags_NoTooltip |
                        ImGuiColorEditFlags_NoLabel );
    ImGui::SameLine();
    ImGui::Text( "Reports: %u", m_n_reports );

    const ImVec2 content_avail = ImGui::GetContentRegionAvail();
    ImGui::BeginChild( "i915-counters-listbox", ImVec2( 0.0f, content_avail.y ) );

    if ( imgui_begin_columns( "i915_counters", { "Percent", "Name", "Value", "Description" } ) )
        ImGui::SetColumnWidth( 0, imgui_scale( 250.0f ) );

    float lineh = ImGui::GetTextLineHeightWithSpacing() - 4;
    for ( const i915_perf_counter_t &c : m_counters )
    {
        const i915_perf_count_value_t &c_val = c.value;
        const i915_perf_count_value_t &c_max_val = c.max_value;

        if ( !m_filter.PassFilter( c.name.c_str() ) )
            continue;

        if ( c.type == i915_perf_counter_t::type::INTEGER )
        {
            ImGui::ProgressBar( c_max_val.u == 0 ? 0 : ((double) c_val.u / c_max_val.u), ImVec2(-1, lineh) );
        }
        else
        {
            ImGui::ProgressBar( c_max_val.f == 0.f ? 0.f : c_val.f / c_max_val.f, ImVec2(-1, lineh) );
        }
        ImGui::NextColumn();
        ImGui::Text( "%s", c.name.c_str() );
        ImGui::NextColumn();
        if ( c.type == i915_perf_counter_t::type::INTEGER )
        {
            ImGui::Text( "%lu -- %s", c_val.u, c.pretty_value );
        }
        else
        {
            ImGui::Text( "%f -- %s", c_val.f, c.pretty_value );
        }
        ImGui::NextColumn();
        ImGui::Text( "%s", c.desc.c_str() );
        ImGui::NextColumn();
    }

    ImGui::EndColumns();

    ImGui::EndChild();
}

#else

void I915PerfCounters::init( TraceEvents &trace_events )
{
}

void I915PerfCounters::set_event( const trace_event_t &event )
{
}

I915PerfCounters::i915_perf_process
I915PerfCounters::get_process( const trace_event_t &i915_perf_event )
{
    i915_perf_process ret = { "", 0 };

    return ret;
}

void I915PerfCounters::render()
{
}

#endif
