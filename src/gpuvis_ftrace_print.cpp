/*
 * Copyright 2017 Valve Software
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
#include <stdio.h>
#include <string.h>

#include <array>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <string>

#include <SDL.h>

#include "imgui/imgui.h"
#include "gpuvis_macros.h"
#include "stlini.h"
#include "trace-cmd/trace-read.h"
#include "gpuvis_utils.h"
#include "gpuvis.h"

static const struct
{
    const char *leftstr;
    const char *rightstr;
} s_pairs[] =
{
    { "[Compositor Client] Submit Left", "[Compositor Client] Submit End" },
    { "[Compositor Client] Submit Right", "[Compositor Client] Submit End" },
    { "[Compositor Client] PostPresentHandoff Begin", "[Compositor Client] PostPresentHandoff End" },
    { "[Compositor] Before wait query", "[Compositor] After wait query" },
    { "[Compositor] Begin Present(wait)", "[Compositor] End Present" },
    { "[Compositor] Before flush", "[Compositor] After flush" },
    { "[Compositor] Sleep - begin: 0", "[Compositor] Sleep - end" },
    { "[Compositor] Sleep - begin: 6", "[Compositor] Sleep - end" },
    { "[Compositor] Begin Running Start", "[Compositor] End Running Start" },
    { "[Compositor] Mirror Begin Present", "[Compositor] Mirror End Present" },
};

static void init_ftrace_pairs( std::vector< TraceEvents::ftrace_pair_t > &ftrace_pairs )
{
    for ( size_t i = 0; i < ARRAY_SIZE( s_pairs ); i++ )
    {
        TraceEvents::ftrace_pair_t pair;

        pair.leftstr = s_pairs[ i ].leftstr;
        pair.rightstr = s_pairs[ i ].rightstr;

        pair.lefthashval = hashstr32( pair.leftstr );
        pair.righthashval = hashstr32( pair.rightstr );

        ftrace_pairs.push_back( pair );
    }

    // Sort ftrace print event IDs based on ts start locations
    auto cmp = [&]( const TraceEvents::ftrace_pair_t &lx, const TraceEvents::ftrace_pair_t &rx )
    {
        return ( lx.lefthashval < rx.lefthashval );
    };
    std::sort( ftrace_pairs.begin(), ftrace_pairs.end(), cmp );
}

/*
  [Compositor Client] Received Idx ###
  [Compositor Client] WaitGetPoses Begin ThreadId=####
  [Compositor Client] WaitGetPoses End ThreadId=####

  [Compositor] Detected dropped frames: ###
  [Compositor] frameTimeout( ### ms )
  [Compositor] NewFrame idx=####
  [Compositor] Predicting( ##.###### ms )
  [Compositor] Re-predicting( ##.###### ms )
  [Compositor] TimeSinceLastVSync: #.######(#####)
*/
static const char *s_buf_prefixes[] =
{
    "[Compositor Client] Received Idx ",
    "[Compositor Client] WaitGetPoses ",
    "[Compositor] frameTimeout( ",
    "[Compositor] Predicting( ",
    "[Compositor] Re-predicting( ",
    "[Compositor Client] PostPresentHandoff ",
    "[Compositor] Present() ",
};

// ftrace print event information
enum bufvar_t
{
    bufvar_duration,
    bufvar_begin_ctx,
    bufvar_end_ctx,
    bufvar_begin_gctx,
    bufvar_end_gctx,

    bufvar_equal,
    bufvar_prefix,

    bufvar_Max
};

static struct
{
    const char *var;
    size_t len;
} s_buf_vars[] =
{
    { "duration=", 9 },
    { "begin_ctx=", 10 },
    { "end_ctx=", 8 },
    { "begin_gctx=", 11 },
    { "end_gctx=", 9 },
};

static const char *find_buf_var( const char *buf, bufvar_t *bufvar )
{
    const char *var;

    // If we find any of our print variables, use that as buf end
    for ( size_t i = 0; i <= bufvar_end_ctx; i++ )
    {
        var = strncasestr( buf, s_buf_vars[ i ].var, s_buf_vars[ i ].len );

        if ( var )
        {
            *bufvar = ( bufvar_t )i;
            return var + s_buf_vars[ i ].len;
        }
    }

    // Search for : or =
    var = strpbrk( buf, ":=" );
    if ( var )
    {
        *bufvar = bufvar_equal;
        return var + 1;
    }

    // No colon - try to find one of our buf prefixes
    for ( size_t i = 0; i < ARRAY_SIZE( s_buf_prefixes ); i++ )
    {
        size_t prefixlen = strlen( s_buf_prefixes[ i ] );

        if ( !strncasecmp( buf, s_buf_prefixes[ i ], prefixlen ) )
        {
            *bufvar = bufvar_prefix;
            return buf + prefixlen;
        }
    }

    *bufvar = bufvar_Max;
    return NULL;
}

static const char *trim_ftrace_print_buf(
        char ( &newbuf )[ TRACE_BUF_SIZE ],
        const char *buf, const char *buf_end, size_t len )
{
    const char *tok1 = buf_end;
    const char *tok0 = buf_end - len;

    // Read to end of token value.
    //   duration=-1234.5 ms

    if ( *tok1 == '-' )
        tok1++;
    while ( isdigit( *tok1 ) )
        tok1++;
    if ( *tok1 == '.' )
    {
        tok1++;
        while ( isdigit( *tok1 ) )
            tok1++;
    }

    if ( tok1[ 0 ] == 'm' && tok1[ 1 ] == 's' )
        tok1 += 2;
    else  if ( tok1[ 0 ] == ' ' && tok1[ 1 ] == 'm' && tok1[ 2 ] == 's' )
        tok1 += 3;

    if ( ( tok0 > buf ) && ( tok0[ -1 ] == '(' ) && ( *tok1 == ')' ) )
    {
        tok0--;
        tok1++;
    }
    if ( ( tok0 > buf ) && isspace( tok0[ -1 ] ) )
        tok0--;

    memmove( newbuf, buf, tok0 - buf );
    memmove( newbuf + ( tok0 - buf ), tok1, strlen( tok1 ) + 1 );
    return newbuf;
}

ftrace_row_info_t *TraceEvents::get_ftrace_row_info_pid( int pid, bool add )
{
    ftrace_row_info_t *row_info = m_ftrace.row_info.get_val( pid );

    if ( !row_info && add )
    {
        row_info = m_ftrace.row_info.get_val_create( pid );

        if ( pid & 0xffff )
            row_info->pid = pid;
        else
            row_info->tgid = pid >> 16;
    }

    return row_info;
}

ftrace_row_info_t *TraceEvents::get_ftrace_row_info_tgid( int tgid, bool add )
{
    return get_ftrace_row_info_pid( tgid << 16, add );
}

ftrace_row_info_t *TraceEvents::get_ftrace_row_info( const char *row_name )
{
    ftrace_row_info_t *ftrace_row_info = NULL;

    if ( !strcmp( row_name, "print" ) )
        ftrace_row_info = get_ftrace_row_info_pid( -1 );
    else if ( !strncmp( row_name, "print pid:", 10 ) )
        ftrace_row_info = get_ftrace_row_info_pid( atoi( row_name + 10 ) );
    else if ( !strncmp( row_name, "print tgid:", 11 ) )
        ftrace_row_info = get_ftrace_row_info_tgid( atoi( row_name + 11 ) );

    return ftrace_row_info;
}

// Called by TraceEvents::new_event_cb() when adding new events to m_events array
void TraceEvents::new_event_ftrace_print( trace_event_t &event )
{
    int pid = event.pid;
    int64_t ts_offset = 0;
    bool do_find_buf_var = true;
    bufvar_t bufvar = bufvar_Max;
    char newbuf[ TRACE_BUF_SIZE ];
    trace_event_t *add_event = &event;
    const char *orig_buf = get_event_field_val( event, "buf" );
    const char *buf = orig_buf;
    const char *ts_offset_str = strncasestr( buf, "offset=", 7 );

    if ( m_ftrace.ftrace_pairs.empty() )
        init_ftrace_pairs( m_ftrace.ftrace_pairs );

    // Default color for ctx events without sibling
    event.color = 0xffff00ff;

    event.color_index = 0;
    event.seqno = UINT32_MAX;

    if ( ts_offset_str )
    {
        ts_offset_str += 7;
        ts_offset = ( int64_t )( atof( ts_offset_str ) * NSECS_PER_MSEC );

        buf = trim_ftrace_print_buf( newbuf, buf, ts_offset_str, 7 );
    }
    else
    {
        // Hash the buf string
        uint32_t hashval = hashstr32( buf );
        uint64_t key = ( ( uint64_t )event.pid << 32 );

        // Try to find this hash+pid in the pairs_ctx map
        uint32_t *event0id = m_ftrace.pairs_ctx.get_val( key | hashval );

        if ( event0id )
        {
            // Found hash+pid in duration map. Value is start event id.
            trace_event_t &event0 = m_events[ *event0id ];

            event0.id_start = event.id;
            event0.duration = event.ts - event0.ts;
            event0.color_index = hashval;
            event.color_index = hashval;

            m_ftrace.pairs_ctx.erase_key( key | hashval  );
            m_ftrace.print_ts_max = std::max< int64_t >( m_ftrace.print_ts_max, event0.duration );

            // Don't add event (we added event0 already)
            add_event = NULL;
            do_find_buf_var = false;
        }
        else
        {
            ftrace_pair_t x = { hashval };
            auto cmp = [&]( const ftrace_pair_t &lx, const ftrace_pair_t &rx )
            {
                return ( lx.lefthashval < rx.lefthashval );
            };
            auto it = std::lower_bound( m_ftrace.ftrace_pairs.begin(), m_ftrace.ftrace_pairs.end(), x, cmp );

            // Try to find this starting hashval in our ftrace_pairs array
            if ( ( it != m_ftrace.ftrace_pairs.end() ) && ( it->lefthashval == hashval ) )
            {
                // Found - add right hashval or'd with pid pointing to this event id
                m_ftrace.pairs_ctx.m_map[ key | it->righthashval ] = event.id;
                do_find_buf_var = false;
            }
        }
    }

    const char *var = do_find_buf_var ? find_buf_var( buf, &bufvar ) : NULL;

    if ( bufvar <= bufvar_end_gctx )
    {
        // This is a duration or ctx print event...

        // Remove "duration=XXX", etc from buf
        buf = trim_ftrace_print_buf( newbuf, buf, var, s_buf_vars[ bufvar ].len );

        // Set color index to hash of new string
        event.color_index = hashstr32( buf );

        if ( bufvar == bufvar_duration )
        {
            event.duration = ( int64_t )( atof( var ) * NSECS_PER_MSEC );
        }
        else
        {
            uint64_t key = 0;

            event.seqno = strtoul( var, 0, 10 );

            if ( ( bufvar == bufvar_begin_ctx ) || ( bufvar == bufvar_end_ctx ) )
                key = ( uint64_t )pid << 32;

            key |= event.seqno;

            if ( ( bufvar == bufvar_begin_ctx ) || ( bufvar == bufvar_begin_gctx ) )
                m_ftrace.begin_ctx.get_val( key, event.id );
            else
                m_ftrace.end_ctx.get_val( key, event.id );

            // We're only going to add a single event for the begin/end ctx pairs
            add_event = NULL;

            uint32_t *begin_eventid = m_ftrace.begin_ctx.get_val( key );
            uint32_t *end_eventid = m_ftrace.end_ctx.get_val( key );

            if ( begin_eventid && end_eventid  )
            {
                // We have a begin/end pair for this ctx
                trace_event_t &event0 = m_events[ *begin_eventid ];
                const trace_event_t &event1 = m_events[ *end_eventid ];

                event0.id_start = event1.id;
                event0.duration = event1.ts - event0.ts;
                event0.color_index = event.color_index;

                // Erase all knowledge of this ctx so it can be reused
                m_ftrace.begin_ctx.erase_key( event.seqno );
                m_ftrace.end_ctx.erase_key( event.seqno );

                add_event = &event0;
            }
        }
    }
    else if ( bufvar < bufvar_Max )
    {
        event.color_index = hashstr32( buf );
    }

    if ( buf == newbuf )
    {
        event_field_t *field = get_event_field( event, "buf" );

        buf = m_strpool.getstr( newbuf );
        field->value = buf;
#if 0
        // Add orig_buf which points to original buf data
        event_field_t *fields = new event_field_t[ event.numfields + 1 ];
        memcpy( fields, event.fields, event.numfields * sizeof( fields[ 0 ] ) );

        fields[ event.numfields ].key = m_strpool.getstr( "orig_buf" );
        fields[ event.numfields ].value = orig_buf;

        delete [] event.fields;
        event.fields = fields;
        event.numfields++;
#endif
    }

    if ( add_event )
    {
        print_info_t print_info;
        const tgid_info_t *tgid_info = tgid_from_pid( pid );

        print_info.ts = add_event->ts + ts_offset;
        print_info.tgid = tgid_info ? tgid_info->tgid : 0;
        print_info.graph_row_id_pid = 0;
        print_info.graph_row_id_tgid = 0;
        print_info.buf = buf;
        print_info.size = ImVec2( 0, 0 );

        if ( add_event->duration < 0 )
        {
            print_info.ts += add_event->duration;
            add_event->duration = -add_event->duration;
        }

        // Add cached print info for this event
        m_ftrace.print_info.get_val( add_event->id, print_info );

        m_ftrace.print_locs.push_back( add_event->id );

        if ( add_event->has_duration() )
            m_ftrace.print_ts_max = std::max< int64_t >( m_ftrace.print_ts_max, add_event->duration );
    }
}

// Called by TraceEvents::init() after m_events is filled for second initialization pass.
void TraceEvents::calculate_event_print_info()
{
    if ( m_ftrace.print_locs.empty() )
        return;

    // Sort ftrace print event IDs based on ts start locations
    auto cmp_ts = [&]( const uint32_t lx, const uint32_t rx )
    {
        const print_info_t *lval = m_ftrace.print_info.get_val( lx );
        const print_info_t *rval = m_ftrace.print_info.get_val( rx );

        return ( lval->ts < rval->ts );
    };
    std::sort( m_ftrace.print_locs.begin(), m_ftrace.print_locs.end(), cmp_ts );

    // Sort ftrace print event IDs based on duration
    auto cmp_dur = [&]( const uint32_t lx, const uint32_t rx )
    {
        const trace_event_t &lval = m_events[ lx ];
        const trace_event_t &rval = m_events[ rx ];
        int64_t ldur = lval.has_duration() ? lval.duration : 0;
        int64_t rdur = rval.has_duration() ? rval.duration : 0;

        return ( ldur > rdur );
    };
    std::vector< uint32_t > locs_duration = m_ftrace.print_locs;
    std::sort( locs_duration.begin(), locs_duration.end(), cmp_dur );

    row_pos_t row_pos;
    ftrace_row_info_t *row_info;
    util_umap< int, row_pos_t > row_pos_pid;
    util_umap< int, row_pos_t > row_pos_tgid;

    // Go through all the ftrace print events with largest durations first
    for ( uint32_t idx : locs_duration )
    {
        row_pos_t *prow_pos;
        trace_event_t &event = m_events[ idx ];
        print_info_t *print_info = m_ftrace.print_info.get_val( event.id );
        int64_t min_ts = print_info->ts;
        uint32_t duration = ( event.has_duration() ? event.duration : 0 );
        int64_t max_ts = min_ts + std::max< int64_t >( duration, 1 * NSECS_PER_MSEC );

        // Global print row id
        event.graph_row_id = row_pos.get_row( min_ts, max_ts );

        // Pid print row id
        prow_pos = row_pos_pid.get_val_create( event.pid );
        print_info->graph_row_id_pid = prow_pos->get_row( min_ts, max_ts );

        row_info = get_ftrace_row_info_pid( event.pid, true );
        row_info->rows = std::max< uint32_t >( row_info->rows, prow_pos->m_rows );
        row_info->count++;

        if ( print_info->tgid )
        {
            // Tgid print row id
            prow_pos = row_pos_tgid.get_val_create( print_info->tgid );
            print_info->graph_row_id_tgid = prow_pos->get_row( min_ts, max_ts );

            row_info = get_ftrace_row_info_tgid( print_info->tgid, true );
            row_info->rows = std::max< uint32_t >( row_info->rows, prow_pos->m_rows );
            row_info->count++;
        }
    }

    // Add info for special pid=-1 (all ftrace print events)
    row_info = get_ftrace_row_info_pid( -1, true );
    row_info->rows = row_pos.m_rows;
    row_info->count = m_ftrace.print_locs.size();
}

void TraceEvents::invalidate_ftraceprint_colors()
{
    m_ftrace.text_size_max = -1.0f;
}

// Called by TraceWin::graph_render_print_timeline() to recalculate text sizes and colors
void TraceEvents::update_ftraceprint_colors()
{
    float label_sat = s_clrs().getalpha( col_Graph_PrintLabelSat );
    float label_alpha = s_clrs().getalpha( col_Graph_PrintLabelAlpha );
    ImU32 color = s_clrs().get( col_FtracePrintText, label_alpha * 255 );

    m_ftrace.text_size_max = 0.0f;

    for ( auto &entry : m_ftrace.print_info.m_map )
    {
        trace_event_t &event = m_events[ entry.first ];
        print_info_t &print_info = entry.second;

        print_info.size = ImGui::CalcTextSize( print_info.buf );
        m_ftrace.text_size_max = std::max< float >( print_info.size.x, m_ftrace.text_size_max );

        // Mark this event as autogen'd color so it doesn't get overwritten
        event.flags |= TRACE_FLAG_AUTOGEN_COLOR;

        if ( event.color_index )
        {
            // If we have a graph row id, use the hashval stored in color_index
            event.color = imgui_col_from_hashval( event.color_index, label_sat, label_alpha );

            if ( is_valid_id( event.id_start ) )
            {
                m_events[ event.id_start ].color = event.color;
                m_events[ event.id_start ].flags |= TRACE_FLAG_AUTOGEN_COLOR;
            }
        }
        else
        {
            event.color = color;
        }
    }
}
