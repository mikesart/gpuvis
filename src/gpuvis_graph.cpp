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

#include <algorithm>
#include <future>
#include <set>
#include <sys/stat.h>
#include <sys/types.h>
#include <unordered_map>
#include <vector>
#include <array>

#include <SDL.h>

#include "GL/gl3w.h"
#include "gpuvis.h"

/*
  18:00:55 <Plagman> mikesart: i think using the same timeline logic for $name=print would be rad
  18:01:09 <Plagman> if it was multiline and staggered so it could print the event names on the actual timeline
  18:01:46 <Plagman> it should be quite a bit easier since the events are just points and not bars
  18:02:05 <Plagman> might not be able to print most of them until zoomed in way in though
  18:02:09 <Plagman> but that'd be fine
  18:02:27 <Plagman> i guess ideally you'd want recurring print events to always be on the same line
  18:02:36 <Plagman> that's where unique event IDs would be good, but need to change our code for that
  18:02:48 <Plagman> or do some best-effort string matching in the tool i guess
  18:03:19 <Plagman> but right now i have little use for all the lines under my $name=print row
  18:03:36 <Plagman> so if that took the remaining space and showed them in a timeline and printed what it could directly inline, that would be a good visualization
  18:04:20 <Plagman> oops, got to run for a bit

  19:07:32 <Plagman> mikesart: you think you can do fuzzy prefix matching for the ones with variable data in them?
  19:08:04 <Plagman> like matching all these together: "[Compositor] Re-Predicting( 20.02354345 )"
  19:08:40 <Plagman> an algo that matches as much as possible would probably match them together pretty well
  19:08:53 <Plagman> and then we could show offset since last identical event, highlight them together on hover, etc
  19:09:00 <Plagman> put them on separate graph rows too

  (09:03:30 PM) lostgoat: I think the gfx_1823_11105 info on the tooltip is not that useful
  (09:04:29 PM) lostgoat: and having the event names on the tooltip would be useful, although giving them short names seems hard
  (09:04:56 PM) lostgoat: Submission duration:
  (09:05:00 PM) lostgoat: Queued duration:
  (09:05:04 PM) lostgoat: Execution duration:
  (09:05:31 PM) Plagman: at the end of the day the name of the amd-specific events are implementation details and probably shoujld be hidden
  (09:05:54 PM) lostgoat: Although Submission is not really accurate
  (09:05:54 PM) Plagman: so i would agree with having the durations be expressed textually instead
  (09:06:08 PM) lostgoat: maybe 'SW queue duration' and 'HW queue duration'

  Add durations to regular event hovers?

  TODO mikesart: Hovering over "comp_1.1.1 hw" should highlight "comp_1.1.1" stuff, etc.

  TODO mikesart: Occasionally the hidden rows don't appear under the show menu?

  TODO mikesart: Try coloring the ftrace print events per the hash of the string?

  TODO mikesart: Check if entire rows are clipped when drawing...

  From DanG:
    * Add Browse button to load a trace file
*/

/*
  From conversations with Andres and Pierre-Loup...

  These are the important events:

  amdgpu_cs_ioctl:
    this event links a userspace submission with a kernel job
    it appears when a job is received from userspace
    dictates the userspace PID for the whole unit of work
      ie, the process that owns the work executing on the gpu represented by the bar
    only event executed within the context of the userspace process

  amdgpu_sched_run_job:
    links a job to a dma_fence object, the queue into the HW event
    start of the bar in the gpu timeline; either right now if no job is running, or when the currently running job finishes

  *fence_signaled:
    job completed
    dictates the end of the bar

  notes:
    amdgpu_cs_ioctl and amdgpu_sched_run_job have a common job handle

  We want to match: timeline, context, seqno.

    There are separate timelines for each gpu engine
    There are two dma timelines (one per engine)
    And 8 compute timelines (one per hw queue)
    They are all concurrently executed
      Most apps will probably only have a gfx timeline
      So if you populate those lazily it should avoid clogging the ui

  Andres warning:
    btw, expect to see traffic on some queues that was not directly initiated by an app
    There is some work the kernel submits itself and that won't be linked to any cs_ioctl

  Example:

  ; userspace submission
    SkinningApp-2837 475.1688: amdgpu_cs_ioctl:      sched_job=185904, timeline=gfx, context=249, seqno=91446, ring_name=ffff94d7a00d4694, num_ibs=3

  ; gpu starting job
            gfx-477  475.1689: amdgpu_sched_run_job: sched_job=185904, timeline=gfx, context=249, seqno=91446, ring_name=ffff94d7a00d4694, num_ibs=3

  ; job completed
         <idle>-0    475.1690: fence_signaled:       driver=amd_sched timeline=gfx context=249 seqno=91446
 */

class event_renderer_t
{
public:
    event_renderer_t( float y_in, float w_in, float h_in );

    void add_event( float x );
    void done();

    void set_y( float y_in, float h_in );

protected:
    void start( float x );
    void draw();

public:
    float x0, x1;
    uint32_t num_events;

    float y, w, h;
};

typedef std::function< uint32_t ( class graph_info_t &gi ) > RenderGraphRowCallback;

struct row_info_t
{
    uint32_t id;

    float row_y;
    float row_h;

    RenderGraphRowCallback render_cb;

    std::string comm;
    const std::vector< uint32_t > *plocs;
};

class graph_info_t
{
public:
    void init_row_info( TraceWin *win, const std::vector< GraphRows::graph_rows_info_t > &graph_rows );

    void init( TraceWin *win, float x, float w );
    void set_pos_y( float y, float h, const row_info_t *ri );

    float ts_to_x( int64_t ts );
    float ts_to_screenx( int64_t ts );

    int64_t screenx_to_ts( float x_in );
    int64_t dx_to_ts( float x_in );

    bool pt_in_graph( const ImVec2 &posin );
    bool mouse_pos_in_graph();
    bool mouse_pos_in_rect( float x, float w, float y, float h );

    row_info_t *find_row( const char *name );

public:
    float x, y, w, h;

    int64_t ts0;
    int64_t ts1;
    int64_t tsdx;
    double tsdxrcp;

    uint32_t eventstart;
    uint32_t eventend;

    bool mouse_over;
    ImVec2 mouse_pos;

    struct hovered_t
    {
        bool neg;
        int64_t dist_ts;
        uint32_t eventid;
    };
    const size_t hovered_max = 6;
    std::vector< hovered_t > hovered_items;

    uint32_t hovered_graph_event = INVALID_ID;

    bool do_zoom_gfx;
    bool timeline_render_user;
    bool graph_only_filtered;

    std::vector< row_info_t > row_info;
    const row_info_t *prinfo_cur = nullptr;
    const row_info_t *prinfo_gfx = nullptr;
    const row_info_t *prinfo_gfx_hw = nullptr;

    float text_h;
    float row_h;
    float visible_graph_height;
    float total_graph_height;
};

static void imgui_drawrect( float x, float w, float y, float h, ImU32 color )
{
    if ( w < 0.0f )
    {
        x += w;
        w = -w;
    }

    if ( w <= 1.0f )
        ImGui::GetWindowDrawList()->AddLine( ImVec2( x, y - 0.5f ), ImVec2( x, y + h - 0.5f ), color );
    else
        ImGui::GetWindowDrawList()->AddRectFilled( ImVec2( x, y ), ImVec2( x + w, y + h ), color );
}

static void imgui_draw_text( float x, float y, const char *text, ImU32 color )
{
#if 0
    //$ Don't draw background behind row label text - think it obscures the graph a bit.
    ImVec2 textsize = ImGui::CalcTextSize( text );

    ImGui::GetWindowDrawList()->AddRectFilled(
        ImVec2( x, y ), ImVec2( x + textsize.x, y + textsize.y ),
        col_get( col_RowLabelBk) );
#endif

    ImGui::GetWindowDrawList()->AddText( ImVec2( x, y ), color, text );
}

const event_field_t *find_event_field( const std::vector< event_field_t > &fields, const char *name )
{
    for ( const event_field_t &field : fields )
    {
        if ( !strcmp( field.key, name ) )
            return &field;
    }

    return NULL;
}

const char *get_event_field_val( const std::vector< event_field_t > &fields, const char *name )
{
    const event_field_t *field = find_event_field( fields, name );

    return field ? field->value : "";
}

/*
 * event_renderer_t
 */
event_renderer_t::event_renderer_t( float y_in, float w_in, float h_in )
{
    y = y_in;
    w = w_in;
    h = h_in;

    start( -1.0f );
}

void event_renderer_t::set_y( float y_in, float h_in )
{
    if ( y != y_in || h != h_in )
    {
        done();

        y = y_in;
        h = h_in;
    }
}

void event_renderer_t::add_event( float x )
{
    if ( x0 < 0.0f )
    {
        // First event
        start( x );
    }
    else if ( x - x1 <= 1.0f )
    {
        // New event real close to last event
        x1 = x;
        num_events++;
    }
    else
    {
        // New event is away from current group, so draw.
        draw();

        // Start a new group
        start( x );
    }
}

void event_renderer_t::done()
{
    if ( x0 != -1 )
    {
        draw();
        start( -1.0f );
    }
}

void event_renderer_t::start( float x )
{
    num_events = 0;
    x0 = x;
    x1 = x + .0001f;
}

void event_renderer_t::draw()
{
    int index = std::min< int >( col_1Event + num_events, col_6Event );
    ImU32 color = col_get( ( colors_t )index );
    float min_width = std::min< float >( num_events + 1.0f, 4.0f );
    float width = std::max< float >( x1 - x0, min_width );

    imgui_drawrect( x0, width, y, h, color );
}

static option_id_t get_comm_option_id( const std::string &comm )
{
    if ( comm == "gfx" )
        return OPT_TimelineGfxSize;
    else if ( comm == "sdma0" )
        return OPT_TimelineSdma0Size;
    else if ( comm == "sdma1" )
        return OPT_TimelineSdma1Size;
    else if ( comm == "print" )
        return OPT_TimelinePrint;

    return OPT_Max;
}

/*
 * graph_info_t
 */
void graph_info_t::init_row_info( TraceWin *win, const std::vector< GraphRows::graph_rows_info_t > &graph_rows )
{
    uint32_t id = 0;
    TraceEvents *trace_events = win->m_trace_events;

    imgui_push_smallfont();

    float graph_row_padding = ImGui::GetStyle().FramePadding.y;

    text_h = ImGui::GetTextLineHeightWithSpacing();
    row_h = text_h * 2 + graph_row_padding;

    total_graph_height = graph_row_padding;

    imgui_pop_smallfont();

    for ( const GraphRows::graph_rows_info_t &grow : graph_rows )
    {
        row_info_t rinfo;
        TraceEvents::loc_type_t loc_type;
        const std::vector< uint32_t > *plocs;
        const std::string &comm = grow.name;

        if ( grow.hidden )
            continue;

        plocs = trace_events->get_locs( comm.c_str(), &loc_type );
        if ( !plocs )
            continue;

        rinfo.row_y = total_graph_height;
        rinfo.row_h = text_h * 2;
        rinfo.comm = comm;

        if ( loc_type == TraceEvents::LOC_TYPE_Print )
        {
            // ftrace print row
            rinfo.row_h = win->m_loader.get_opt( OPT_TimelinePrint ) * text_h;
            rinfo.render_cb = std::bind( &TraceWin::render_graph_print_timeline, win, _1 );
        }
        else if ( loc_type == TraceEvents::LOC_TYPE_Timeline )
        {
            option_id_t optid = get_comm_option_id( comm );
            int rows = ( optid != OPT_Max ) ?
                        Clamp< int >( win->m_loader.get_opt( optid ), 2, 50 ) : 4;

            rinfo.row_h = text_h * rows;
            rinfo.render_cb = std::bind( &TraceWin::render_graph_row_timeline, win, _1 );
        }
        else if ( loc_type == TraceEvents::LOC_TYPE_Timeline_hw )
        {
            rinfo.row_h = 2 * text_h;
            rinfo.render_cb = std::bind( &TraceWin::render_graph_hw_row_timeline, win, _1 );
        }
        else
        {
            // LOC_Type_Comm or LOC_TYPE_Tdopexpr
            rinfo.render_cb = std::bind( &TraceWin::render_graph_row_events, win, _1 );
        }

        rinfo.id = id++;
        rinfo.plocs = plocs;
        row_info.push_back( rinfo );

        if ( comm == "gfx" )
            prinfo_gfx = &row_info.back();
        else if ( comm == "gfx hw" )
            prinfo_gfx_hw = &row_info.back();

        total_graph_height += rinfo.row_h + graph_row_padding;
    }

    total_graph_height += imgui_scale( 2.0f );
    total_graph_height = std::max< float >( total_graph_height, 4 * row_h );
}

void graph_info_t::init( TraceWin *win, float x_in, float w_in )
{
    x = x_in;
    w = w_in;

    ts0 = win->m_graph_start_ts + win->m_tsoffset;
    ts1 = ts0 + win->m_graph_length_ts;

    eventstart = win->ts_to_eventid( ts0 );
    eventend = win->ts_to_eventid( ts1 );

    tsdx = ts1 - ts0 + 1;
    tsdxrcp = 1.0 / tsdx;

    mouse_pos = ImGui::IsRootWindowOrAnyChildFocused() ?
                ImGui::GetMousePos() : ImVec2( -1024, -1024 );

    // Check if we're supposed to render filtered events only
    graph_only_filtered = win->m_loader.m_options[ OPT_GraphOnlyFiltered ].val &&
                          !win->m_filtered_events.empty();

    timeline_render_user = !!win->m_loader.get_opt( OPT_TimelineRenderUserSpace );

    row_info_t *pri = find_row( "gfx" );
    if ( pri )
    {
        size_t index = pri - &row_info[ 0 ];

        // Draw the gfx row last so we can highlight items in it based on other row hovering.
        row_info.push_back( row_info[ index ] );
        row_info.erase( row_info.begin() + index );

        prinfo_gfx = &row_info.back();
    }

    prinfo_gfx_hw = find_row( "gfx hw" );
}

void graph_info_t::set_pos_y( float y_in, float h_in, const row_info_t *ri )
{
    y = y_in;
    h = h_in;

    prinfo_cur = ri;

    mouse_over = mouse_pos.x >= x &&
            mouse_pos.x <= x + w &&
            mouse_pos.y >= y &&
            mouse_pos.y <= y + h;
}

float graph_info_t::ts_to_x( int64_t ts )
{
    return w * ( ts - ts0 ) * tsdxrcp;
}

float graph_info_t::ts_to_screenx( int64_t ts )
{
    return x + ts_to_x( ts );
}

int64_t graph_info_t::screenx_to_ts( float x_in )
{
    double val = ( x_in - x ) / w;

    return ts0 + val * tsdx;
}
int64_t graph_info_t::dx_to_ts( float x_in )
{
    return ( x_in / w ) * tsdx;
}

bool graph_info_t::pt_in_graph( const ImVec2 &posin )
{
    return ( posin.x >= x && posin.x <= x + w &&
             posin.y >= y && posin.y <= y + h );
}

bool graph_info_t::mouse_pos_in_graph()
{
    return pt_in_graph( mouse_pos );
}

bool graph_info_t::mouse_pos_in_rect( float x0, float width, float y0, float height )
{
    return ( mouse_pos.x >= x0 &&
             mouse_pos.x <= x0 + width &&
             mouse_pos.y >= y0 &&
             mouse_pos.y <= y0 + height );
}

row_info_t *graph_info_t::find_row( const char *name )
{
    for ( row_info_t &ri : row_info )
    {
        if ( ri.comm == name )
            return &ri;
    }
    return NULL;
}

bool TraceWin::add_mouse_hovered_event( float x, class graph_info_t &gi, const trace_event_t &event )
{
    bool inserted = false;
    float xdist_mouse = x - gi.mouse_pos.x;
    bool neg = xdist_mouse < 0.0f;

    if ( neg )
        xdist_mouse = -xdist_mouse;

    if ( xdist_mouse < imgui_scale( 8.0f ) )
    {
        int64_t dist_ts = gi.dx_to_ts( xdist_mouse );

        for ( auto it = gi.hovered_items.begin(); it != gi.hovered_items.end(); it++ )
        {
            if ( dist_ts < it->dist_ts )
            {
                gi.hovered_items.insert( it, { neg, dist_ts, event.id } );
                inserted = true;
                break;
            }
        }

        if ( !inserted && ( gi.hovered_items.size() < gi.hovered_max ) )
        {
            gi.hovered_items.push_back( { neg, dist_ts, event.id } );
            inserted = true;
        }
        else if ( gi.hovered_items.size() > gi.hovered_max )
        {
            gi.hovered_items.pop_back();
        }
    }

    return inserted;
}

uint32_t TraceWin::render_graph_print_timeline( graph_info_t &gi )
{
    imgui_push_smallfont();

    struct row_draw_info_t
    {
        float x = 0.0f;
        const trace_event_t *event = nullptr;
        const TraceEvents::event_print_info_t *print_info = nullptr;
    };
    std::vector< row_draw_info_t > row_draw_info;

    uint32_t num_events = 0;
    const std::vector< uint32_t > &locs = *gi.prinfo_cur->plocs;
    bool timeline_labels = m_loader.get_opt( OPT_PrintTimelineLabels );
    uint32_t row_count = std::max< uint32_t >( 1, gi.h / gi.text_h - 1 );

    row_draw_info.resize( row_count + 1 );

    // We need to start drawing to the left of 0 for timeline_labels
    int64_t ts = timeline_labels ? gi.screenx_to_ts( gi.x - m_trace_events->m_buf_size_max_x ) : gi.ts0;
    uint32_t eventstart = ts_to_eventid( ts );

    for ( size_t idx = vec_find_eventid( locs, eventstart );
          idx < locs.size();
          idx++ )
    {
        uint32_t eventid = locs[ idx ];
        const trace_event_t &event = get_event( eventid );
        uint32_t row_id = event.graph_row_id ? ( event.graph_row_id % row_count + 1 ) : 0;
        float x = gi.ts_to_screenx( event.ts );
        float y = gi.y + row_id * gi.text_h;

        if ( eventid > gi.eventend )
            break;
        else if ( gi.graph_only_filtered && event.is_filtered_out )
            continue;

        // Check if we drew something on this row already
        if ( row_draw_info[ row_id ].print_info )
        {
            float x0 = row_draw_info[ row_id ].x;
            const TraceEvents::event_print_info_t *print_info = row_draw_info[ row_id ].print_info;

            // If we did and there is room, draw the ftrace print buf
            if ( x - x0 > print_info->buf_size.x )
            {
                imgui_drawrect( x0, print_info->buf_size.x, y + imgui_scale( 2.0f ), print_info->buf_size.y, event.color );
                imgui_draw_text( x0, y + imgui_scale( 2.0f ), print_info->buf, IM_COL32_WHITE );
            }
        }

        // Otherwise draw a little tick for it
        imgui_drawrect( x, imgui_scale( 2.0f ), y, gi.text_h, event.color );

        // Check if we're mouse hovering this event
        if ( gi.mouse_over && gi.mouse_pos.y >= y && gi.mouse_pos.y <= y + gi.text_h )
            add_mouse_hovered_event( x, gi, event );

        num_events++;

        if ( timeline_labels )
        {
            row_draw_info[ row_id ].x = x;
            row_draw_info[ row_id ].print_info = m_trace_events->m_print_buf_info.get_val( event.id );
            row_draw_info[ row_id ].event = &event;
        }
    }

    for ( uint32_t row_id = 0; row_id < row_draw_info.size(); row_id++ )
    {
        const row_draw_info_t &draw_info = row_draw_info[ row_id ];
        const TraceEvents::event_print_info_t *print_info = draw_info.print_info;

        if ( print_info )
        {
            float x0 = draw_info.x;
            float y = gi.y + row_id * gi.text_h;
            const trace_event_t *event = draw_info.event;

            imgui_drawrect( x0, print_info->buf_size.x, y + imgui_scale( 2.0f ), print_info->buf_size.y, event->color );
            imgui_draw_text( x0, y + imgui_scale( 2.0f ), print_info->buf, IM_COL32_WHITE );
        }
    }

    imgui_pop_smallfont();

    return num_events;
}

uint32_t TraceWin::render_graph_hw_row_timeline( graph_info_t &gi )
{
    imgui_push_smallfont();

    float row_h = gi.h;
    uint32_t num_events = 0;
    ImU32 col_event = col_get( col_1Event );

    ImRect hov_rect;
    ImU32 last_color = 0;
    float y = gi.y;
    const std::vector< uint32_t >& locs = *gi.prinfo_cur->plocs;

    for ( size_t idx = vec_find_eventid( locs, gi.eventstart );
          idx < locs.size();
          idx++ )
    {
        const trace_event_t &fence_signaled = get_event( locs.at( idx ) );

        if ( fence_signaled.is_fence_signaled() &&
             is_valid_id( fence_signaled.id_start ) &&
             ( fence_signaled.ts - fence_signaled.duration < gi.ts1 ) )
        {
            float x0 = gi.ts_to_screenx( fence_signaled.ts - fence_signaled.duration );
            float x1 = gi.ts_to_screenx( fence_signaled.ts );

            imgui_drawrect( x0, x1 - x0, y, row_h, fence_signaled.color );

            // Draw a label if we have room.
            const char *label = fence_signaled.user_comm;
            ImVec2 size = ImGui::CalcTextSize( label );

            if ( size.x + imgui_scale( 4 ) >= x1 - x0 )
            {
                // No room for the comm, try just the pid.
                label = strrchr( label, '-' );
                if ( label )
                    size = ImGui::CalcTextSize( ++label );
            }
            if ( size.x + imgui_scale( 4 ) < x1 - x0 )
            {
                ImGui::GetWindowDrawList()->AddText(
                            ImVec2( x0 + imgui_scale( 2.0f ), y + imgui_scale( 2.0f ) ),
                            col_get( col_BarText ), label );
            }

            // If we drew the same color last time, draw a separator.
            if ( last_color == fence_signaled.color )
                imgui_drawrect( x0, 1.0, y, row_h, col_event );
            else
                last_color = fence_signaled.color;

            // Check if we're hovering this event.
            if ( !is_valid_id( gi.hovered_graph_event ) &&
                 gi.mouse_pos_in_rect( x0, x1 - x0, y, row_h ) )
            {
                gi.hovered_graph_event = fence_signaled.id;
                hov_rect = { x0, y, x1, y + row_h };
            }

            num_events++;
        }
    }

    if ( hov_rect.Min.x < gi.x + gi.w )
        ImGui::GetWindowDrawList()->AddRect( hov_rect.Min, hov_rect.Max, col_get( col_BarSelRect ) );

    imgui_pop_smallfont();

    return num_events;
}

uint32_t TraceWin::render_graph_row_timeline( graph_info_t &gi )
{
    imgui_push_smallfont();

    ImRect hov_rect;
    uint32_t num_events = 0;
    ImU32 col_hwrunning = col_get( col_BarHwRunning );
    ImU32 col_userspace = col_get( col_BarUserspace );
    ImU32 col_hwqueue = col_get( col_BarHwQueue );
    ImU32 color_1event = col_get( col_1Event );
    const std::vector< uint32_t > &locs = *gi.prinfo_cur->plocs;

    uint32_t timeline_row_count = gi.h / gi.text_h;

    bool render_timeline_events = !!m_loader.get_opt( OPT_TimelineEvents );
    bool render_timeline_labels = !!m_loader.get_opt( OPT_TimelineLabels );

    for ( size_t idx = vec_find_eventid( locs, gi.eventstart );
          idx < locs.size();
          idx++ )
    {
        const trace_event_t &fence_signaled = get_event( locs[ idx ] );

        if ( fence_signaled.is_fence_signaled() &&
             is_valid_id( fence_signaled.id_start ) )
        {
            const trace_event_t &sched_run_job = get_event( fence_signaled.id_start );
            const trace_event_t &cs_ioctl = is_valid_id( sched_run_job.id_start ) ?
                        get_event( sched_run_job.id_start ) : sched_run_job;

            //$ TODO mikesart: can we bail out of this loop at some point if
            //  our start times for all the graphs are > gi.ts1?
            if ( cs_ioctl.ts < gi.ts1 )
            {
                bool hovered = false;
                float y = gi.y + ( fence_signaled.graph_row_id % timeline_row_count ) * gi.text_h;

                // amdgpu_cs_ioctl  amdgpu_sched_run_job   |   fence_signaled
                //       |-----------------|---------------|--------|
                //       |user-->          |hwqueue-->     |hw->    |
                float x_user_start = gi.ts_to_screenx( cs_ioctl.ts );
                float x_hwqueue_start = gi.ts_to_screenx( sched_run_job.ts );
                float x_hwqueue_end = gi.ts_to_screenx( fence_signaled.ts - fence_signaled.duration );
                float x_hw_end = gi.ts_to_screenx( fence_signaled.ts );
                float xleft = gi.timeline_render_user ? x_user_start : x_hwqueue_start;

                if ( ( gi.hovered_graph_event == fence_signaled.id ) ||
                    gi.mouse_pos_in_rect( xleft, x_hw_end - xleft, y, gi.text_h ) )
                {
                    // Mouse is hovering over this fence_signaled.
                    hovered = true;
                    hov_rect = { x_user_start, y, x_hw_end, y + gi.text_h };

                    if ( !is_valid_id( gi.hovered_graph_event ) )
                        gi.hovered_graph_event = fence_signaled.id;
                }

                // Draw user bar
                if ( hovered || gi.timeline_render_user )
                    imgui_drawrect( x_user_start, x_hwqueue_start - x_user_start, y, gi.text_h, col_userspace );

                // Draw hw queue bar
                if ( x_hwqueue_end != x_hwqueue_start )
                    imgui_drawrect( x_hwqueue_start, x_hwqueue_end - x_hwqueue_start, y, gi.text_h, col_hwqueue );

                // Draw hw running bar
                imgui_drawrect( x_hwqueue_end, x_hw_end - x_hwqueue_end, y, gi.text_h, col_hwrunning );

                if ( render_timeline_labels )
                {
                    const ImVec2 size = ImGui::CalcTextSize( cs_ioctl.user_comm );
                    float x_text = std::max< float >( x_hwqueue_start, gi.x ) + imgui_scale( 2.0f );

                    if ( x_hw_end - x_text >= size.x )
                    {
                        ImGui::GetWindowDrawList()->AddText( ImVec2( x_text, y + imgui_scale( 1.0f ) ),
                                                             col_get( col_BarText ), cs_ioctl.user_comm );
                    }
                }

                if ( render_timeline_events )
                {
                    if ( cs_ioctl.id != sched_run_job.id )
                    {
                        // Draw event line for start of user
                        imgui_drawrect( x_user_start, 1.0, y, gi.text_h, color_1event );

                        // Check if we're mouse hovering starting event
                        if ( gi.mouse_over && gi.mouse_pos.y >= y && gi.mouse_pos.y <= y + gi.text_h )
                        {
                            // If we are hovering, and no selection bar is set, do it.
                            if ( add_mouse_hovered_event( x_user_start, gi, cs_ioctl ) && ( hov_rect.Min.x == FLT_MAX ) )
                            {
                                hov_rect = { x_user_start, y, x_hw_end, y + gi.text_h };

                                // Draw user bar for hovered events if they weren't already drawn
                                if ( !hovered && !gi.timeline_render_user )
                                    imgui_drawrect( x_user_start, x_hwqueue_start - x_user_start, y, gi.text_h, col_userspace );
                            }
                        }
                    }

                    // Draw event line for hwqueue start and hw end
                    imgui_drawrect( x_hwqueue_start, 1.0, y, gi.text_h, color_1event );
                    imgui_drawrect( x_hw_end, 1.0, y, gi.text_h, color_1event );
                }

                num_events++;
            }
        }
    }

    if ( hov_rect.Min.x < gi.x + gi.w )
        ImGui::GetWindowDrawList()->AddRect( hov_rect.Min, hov_rect.Max, col_get( col_BarSelRect ) );

    imgui_pop_smallfont();

    return num_events;
}

uint32_t TraceWin::render_graph_row_events( graph_info_t &gi )
{
    uint32_t num_events = 0;
    bool draw_hovered_event = false;
    bool draw_selected_event = false;
    const std::vector< uint32_t > &locs = *gi.prinfo_cur->plocs;
    event_renderer_t event_renderer( gi.y + 4, gi.w, gi.h - 8 );

    for ( size_t idx = vec_find_eventid( locs, gi.eventstart );
          idx < locs.size();
          idx++ )
    {
        uint32_t eventid = locs[ idx ];
        const trace_event_t &event = get_event( eventid );

        if ( eventid > gi.eventend )
            break;
        else if ( gi.graph_only_filtered && event.is_filtered_out )
            continue;

        float x = gi.ts_to_screenx( event.ts );

        if ( eventid == m_hovered_eventlist_eventid )
            draw_hovered_event = true;
        else if ( eventid == m_selected_eventid )
            draw_selected_event = true;

        // Check if we're mouse hovering this event
        if ( gi.mouse_over )
            add_mouse_hovered_event( x, gi, event );

        event_renderer.add_event( x );
        num_events++;
    }

    event_renderer.done();

    if ( draw_hovered_event )
    {
        trace_event_t &event = get_event( m_hovered_eventlist_eventid );
        float x = gi.ts_to_screenx( event.ts );

        ImGui::GetWindowDrawList()->AddCircleFilled(
                    ImVec2( x, gi.y + gi.h / 2.0f ),
                    imgui_scale( 5.0f ),
                    col_get( col_HovEvent ) );
    }

    if ( draw_selected_event )
    {
        trace_event_t &event = get_event( m_selected_eventid );
        float x = gi.ts_to_screenx( event.ts );

        ImGui::GetWindowDrawList()->AddCircleFilled(
                    ImVec2( x, gi.y + gi.h / 2.0f ),
                    imgui_scale( 5.0f ),
                    col_get( col_SelEvent ) );
    }

    return num_events;
}

void TraceWin::render_graph_row( graph_info_t &gi )
{
    const std::string comm = gi.prinfo_cur->comm;

    if ( gi.mouse_over )
        m_mouse_over_row_name = comm;

    // Draw background
    ImGui::GetWindowDrawList()->AddRectFilled(
        ImVec2( gi.x, gi.y ),
        ImVec2( gi.x + gi.w, gi.y + gi.h ),
        col_get( col_GraphRowBk ) );

    // Call the render callback function
    uint32_t num_events = gi.prinfo_cur->render_cb( gi );

    // Draw row label
    std::string label = string_format( "%u) %s", gi.prinfo_cur->id, comm.c_str() );
    imgui_draw_text( gi.x, gi.y, label.c_str(),
                     col_get( col_RowLabel ) );

    if ( num_events )
    {
        label = string_format( "%u events", num_events );
        imgui_draw_text( gi.x, gi.y + ImGui::GetTextLineHeight(), label.c_str(),
                         col_get( col_RowLabel ) );
    }
}

void TraceWin::render_graph_vblanks( graph_info_t &gi )
{
    // Draw time ticks every millisecond
    int64_t tsstart = std::max< int64_t >( gi.ts0 / MSECS_PER_SEC - 1, 0 ) * MSECS_PER_SEC;
    float x0 = gi.ts_to_x( tsstart );
    float dx = gi.w * MSECS_PER_SEC * gi.tsdxrcp;

    if ( dx > imgui_scale( 4.0f ) )
    {
        for ( ; x0 <= gi.w; x0 += dx )
        {
            imgui_drawrect( gi.x + x0, imgui_scale( 1.0f ),
                            gi.y, imgui_scale( 16.0f ),
                            col_get( col_TimeTick ) );

            if ( dx >= imgui_scale( 35.0f ) )
            {
                for ( int i = 1; i < 4; i++ )
                {
                    imgui_drawrect( gi.x + x0 + i * dx / 4, imgui_scale( 1.0f ),
                                    gi.y, imgui_scale( 4.0f ),
                                    col_get( col_TimeTick ) );
                }
            }
        }
    }

    // Draw vblank events on every graph.
    const std::vector< uint32_t > *vblank_locs = m_trace_events->get_tdopexpr_locs( "$name=drm_vblank_event" );

    if ( vblank_locs )
    {
        for ( size_t idx = vec_find_eventid( *vblank_locs, gi.eventstart );
              idx < vblank_locs->size();
              idx++ )
        {
            uint32_t id = vblank_locs->at( idx );

            if ( id > gi.eventend )
                break;

            trace_event_t &event = get_event( id );

            if ( m_loader.get_opt_crtc( event.crtc ) )
            {
                // drm_vblank_event0: blue, drm_vblank_event1: red
                colors_t col = ( event.crtc > 0 ) ? col_VBlank1 : col_VBlank0;
                float x = gi.ts_to_screenx( event.ts );

                imgui_drawrect( x, imgui_scale( 1.0f ),
                                gi.y, gi.h,
                                col_get( col ) );
            }
        }
    }

    // Draw location line for mouse if mouse is over graph
    if ( m_mouse_over_graph &&
         gi.mouse_pos.x >= gi.x &&
         gi.mouse_pos.x <= gi.x + gi.w )
    {
        imgui_drawrect( gi.mouse_pos.x, imgui_scale( 2.0f ),
                        gi.y, gi.h,
                        col_get( col_MousePos ) );
    }

    if ( is_valid_id( m_hovered_eventlist_eventid ) )
    {
        trace_event_t &event = get_event( m_hovered_eventlist_eventid );

        if ( event.ts >= gi.ts0 && event.ts <= gi.ts1 )
        {
            float x = gi.ts_to_screenx( event.ts );

            imgui_drawrect( x, imgui_scale( 1.0f ),
                            gi.y, gi.h,
                            col_get( col_HovEvent, 120 ) );
        }
    }

    if ( is_valid_id( m_selected_eventid ) )
    {
        trace_event_t &event = get_event( m_selected_eventid );

        if ( event.ts >= gi.ts0 && event.ts <= gi.ts1 )
        {
            float x = gi.ts_to_screenx( event.ts );

            imgui_drawrect( x, imgui_scale( 1.0f ),
                            gi.y, gi.h,
                            col_get( col_SelEvent, 120 ) );
        }
    }

    // Draw mouse selection location
    if ( ( m_mouse_captured == MOUSE_CAPTURED_ZOOM ) ||
         ( m_mouse_captured == MOUSE_CAPTURED_SELECT_AREA ) )
    {
        float mousex0 = m_mouse_capture_pos.x;
        float mousex1 = gi.mouse_pos.x;

        imgui_drawrect( mousex0, mousex1 - mousex0,
                        gi.y, gi.h,
                        col_get( col_ZoomSel ) );
    }

    if ( m_show_eventlist )
    {
        // Draw rectangle for visible event list contents
        if ( is_valid_id( m_eventlist_start_eventid ) &&
             is_valid_id( m_eventlist_end_eventid ) )
        {
            trace_event_t &event0 = get_event( m_eventlist_start_eventid );
            trace_event_t &event1 = get_event( m_eventlist_end_eventid - 1 );
            float xstart = gi.ts_to_screenx( event0.ts );
            float xend = gi.ts_to_screenx( event1.ts );

            ImGui::GetWindowDrawList()->AddRect(
                        ImVec2( xstart, gi.y + imgui_scale( 20 ) ),
                        ImVec2( xend, gi.y + gi.h - imgui_scale( 30 ) ),
                        col_get( col_EventListSel ) );
        }
    }
}

void TraceWin::range_check_graph_location()
{
    std::vector< trace_event_t > &events = m_trace_events->m_events;

    if ( m_graph_length_ts < g_min_graph_length )
    {
        m_graph_length_ts = g_min_graph_length;
        m_do_graph_length_timestr = true;
    }
    else if ( m_graph_length_ts > g_max_graph_length )
    {
        m_graph_length_ts = g_max_graph_length;
        m_do_graph_length_timestr = true;
    }

    // Sanity check the graph start doesn't go completely off the rails.
    if ( m_graph_start_ts + m_tsoffset < events.front().ts - 1 * MSECS_PER_SEC )
    {
        m_graph_start_ts = events.front().ts - m_tsoffset - 1 * MSECS_PER_SEC;
        m_do_graph_start_timestr = true;
    }
    else if ( m_graph_start_ts + m_tsoffset > events.back().ts )
    {
        m_graph_start_ts = events.back().ts - m_tsoffset;
        m_do_graph_start_timestr = true;
    }
}

void TraceWin::handle_graph_hotkeys()
{
    if ( m_saved_graph_locs.size() < 9 )
        m_saved_graph_locs.resize( 9 );

    if ( ImGui::GetIO().KeyCtrl )
    {
        bool keyshift = ImGui::GetIO().KeyShift;

        if ( keyshift && ImGui::IsKeyPressed( 'z' ) )
        {
            m_loader.m_options[ OPT_TimelineZoomGfx ].val ^= 1;
        }
        else
        {
            for ( int key = '1'; key <= '9'; key++ )
            {
                if ( ImGui::IsKeyPressed( key ) )
                {
                    int index = key - '1';

                    if ( keyshift )
                    {
                        // ctrl+shift+#: save location
                        m_saved_graph_locs[ index ] = std::make_pair( m_graph_start_ts, m_graph_length_ts );
                    }
                    else if ( m_saved_graph_locs[ index ].second )
                    {
                        // ctrl+#: goto location
                        m_graph_start_ts = m_saved_graph_locs[ index ].first;
                        m_graph_length_ts = m_saved_graph_locs[ index ].second;
                        m_do_graph_start_timestr = true;
                        m_do_graph_length_timestr = true;
                    }
                    break;
                }
            }
        }
    }
}

void TraceWin::handle_graph_keyboard_scroll()
{
    if ( !ImGui::IsWindowFocused() )
        return;

    int64_t start_ts = m_graph_start_ts + m_tsoffset;
    std::vector< trace_event_t > &events = m_trace_events->m_events;

    if ( imgui_key_pressed( ImGuiKey_UpArrow ) )
    {
        m_graph_start_y += ImGui::GetTextLineHeightWithSpacing() * 4;
    }
    else if ( imgui_key_pressed( ImGuiKey_DownArrow ) )
    {
        m_graph_start_y -= ImGui::GetTextLineHeightWithSpacing() * 4;
    }
    else if ( imgui_key_pressed( ImGuiKey_LeftArrow ) )
    {
        start_ts = std::max< int64_t >( start_ts - 9 * m_graph_length_ts / 10,
                                        -MSECS_PER_SEC );
    }
    else if ( imgui_key_pressed( ImGuiKey_RightArrow ) )
    {
        start_ts = std::min< int64_t >( start_ts + 9 * m_graph_length_ts / 10,
                                        events.back().ts - m_graph_length_ts + MSECS_PER_SEC );
    }
    else if ( imgui_key_pressed( ImGuiKey_Home ) )
    {
        start_ts = events.front().ts - MSECS_PER_SEC;
    }
    else if ( imgui_key_pressed( ImGuiKey_End ) )
    {
        start_ts = events.back().ts - m_graph_length_ts + MSECS_PER_SEC;
    }

    start_ts -= m_tsoffset;
    if ( start_ts != m_graph_start_ts )
    {
        m_graph_start_ts = start_ts;
        m_do_graph_start_timestr = true;
    }
}

static void calc_process_graph_height( TraceWin *win, graph_info_t &gi )
{
    // Zoom mode if we have a gfx row and zoom option is set
    option_id_t optid;
    float max_graph_size;

    gi.do_zoom_gfx = gi.prinfo_gfx && win->m_loader.get_opt( OPT_TimelineZoomGfx );

    if ( gi.do_zoom_gfx )
    {
        optid = OPT_GraphHeightZoomed;
        max_graph_size = imgui_scale( 60.0f ) * gi.row_h;
    }
    else
    {
        optid = OPT_GraphHeight;
        max_graph_size = gi.total_graph_height;
    }

    TraceLoader::option_t &opt = win->m_loader.m_options[ optid ];

    // First time initialization - start with about 15 rows
    if ( !opt.valf )
        opt.valf = 15.0f * gi.row_h;

    // Set up min / max sizes and clamp value in that range
    opt.valf_min = 4.0f * gi.row_h;
    opt.valf_max = Clamp< float >( max_graph_size, opt.valf_min, ImGui::GetWindowHeight() );
    opt.valf = Clamp< float >( opt.valf, opt.valf_min, opt.valf_max );

    gi.visible_graph_height = opt.valf;
}

void TraceWin::render_process_graph()
{
    graph_info_t gi;

    // Initialize our row size, location, etc information based on our graph rows
    gi.init_row_info( this, m_graphrows.m_graph_rows_list );

    // Checkbox to toggle zooming gfx timeline view
    ImGui::SameLine();
    ImGui::CheckboxInt( "Zoom gfx timeline", &m_loader.m_options[ OPT_TimelineZoomGfx ].val );

    // Figure out gi.visible_graph_height
    calc_process_graph_height( this, gi );

    // Make sure ts start and length values are mostly sane
    range_check_graph_location();

    ImGui::BeginChild( "EventGraph", ImVec2( 0, gi.visible_graph_height ), true );
    {
        ImVec2 windowpos = ImVec2( ImGui::GetWindowClipRectMin().x, ImGui::GetWindowPos().y );
        ImVec2 windowsize = ImGui::GetWindowSize();

        // Clear graph background
        imgui_drawrect( windowpos.x, windowsize.x,
                        windowpos.y, windowsize.y, col_get( col_GraphBk ) );

        // Initialize our graphics info struct
        gi.init( this, windowpos.x, windowsize.x );

        // Range check mouse pan values
        m_graph_start_y = Clamp< float >( m_graph_start_y,
                                          gi.visible_graph_height - gi.total_graph_height, 0.0f );

        // If we don't have a popup menu, clear the mouse over row name
        if ( !m_graph_popup )
        {
            m_mouse_over_row_name = "";
            m_rename_comm_buf[ 0 ] = 0;
        }

        // If we have a gfx graph and we're zoomed, render only that
        if ( gi.do_zoom_gfx )
        {
            float gfx_hw_row_h = 0;

            if ( gi.prinfo_gfx_hw )
            {
                const row_info_t &ri = *gi.prinfo_gfx_hw;
                gfx_hw_row_h = ri.row_h + ImGui::GetStyle().FramePadding.y;

                gi.set_pos_y( windowpos.y + windowsize.y - ri.row_h, ri.row_h, &ri );
                render_graph_row( gi );
            }

            gi.timeline_render_user = true;
            gi.set_pos_y( windowpos.y, windowsize.y - gfx_hw_row_h, gi.prinfo_gfx );
            render_graph_row( gi );
        }
        else
        {
            // Go through and render all the rows
            for ( const row_info_t &ri : gi.row_info )
            {
                gi.set_pos_y( windowpos.y + ri.row_y + m_graph_start_y, ri.row_h, &ri );
                render_graph_row( gi );
            }
        }

        // Render full graph lines: vblanks, mouse cursors, etc...
        gi.set_pos_y( windowpos.y, windowsize.y, NULL );
        render_graph_vblanks( gi );

        // Handle right, left, pgup, pgdown, etc in graph
        handle_graph_keyboard_scroll();

        // Handle hotkeys. Ie: Ctrl+Shift+1, etc
        handle_graph_hotkeys();

        // Render mouse tooltips, mouse selections, etc
        handle_mouse_graph( gi );
    }
    ImGui::EndChild();

    ImGui::Button( "##resize_graph", ImVec2( ImGui::GetContentRegionAvailWidth(), imgui_scale( 4.0f ) ) );
    if ( ImGui::IsItemActive() )
    {
        option_id_t opt = gi.do_zoom_gfx ? OPT_GraphHeightZoomed : OPT_GraphHeight;

        if ( ImGui::IsMouseClicked( 0 ) )
            m_resize_graph_click_pos = m_loader.m_options[ opt ].valf;

        m_loader.m_options[ opt ].valf = m_resize_graph_click_pos + ImGui::GetMouseDragDelta( 0 ).y;
    }
}

bool TraceWin::render_graph_popup( graph_info_t &gi )
{
    option_id_t optid = OPT_Max;

    if ( !ImGui::BeginPopup( "GraphPopup" ) )
        return false;

    auto get_location_label_lambda = [this]( size_t i )
    {
        auto &pair = m_saved_graph_locs[ i ];
        std::string start = ts_to_timestr( pair.first );
        std::string len = ts_to_timestr( pair.second );
        return string_format( "Start:%s Length:%s", start.c_str(), len.c_str() );
    };

    imgui_text_bg( "Options", ImGui::GetColorVec4( ImGuiCol_Header ) );
    ImGui::Separator();

    if ( !m_mouse_over_row_name.empty() )
    {
        optid = get_comm_option_id( m_mouse_over_row_name.c_str() );

        std::string label = string_format( "Hide row '%s'", m_mouse_over_row_name.c_str() );

        if ( ImGui::MenuItem( label.c_str() ) )
            m_graphrows.show_row( m_mouse_over_row_name, GraphRows::HIDE_ROW );

        label = string_format( "Hide row '%s' and below", m_mouse_over_row_name.c_str() );
        if ( ImGui::MenuItem( label.c_str() ) )
            m_graphrows.show_row( m_mouse_over_row_name, GraphRows::HIDE_ROW_AND_ALL_BELOW );
    }

    if ( !m_graph_rows_hidden_rows.empty() )
    {
        if ( ImGui::BeginMenu( "Show row" ) )
        {
            if ( ImGui::MenuItem( "All Rows" ) )
                m_graphrows.show_row( "", GraphRows::SHOW_ALL_ROWS );

            ImGui::Separator();

            for ( const GraphRows::hidden_row_t &entry : m_graph_rows_hidden_rows )
            {
                const std::string label = string_format( "%s (%lu events)",
                                                         entry.name.c_str(), entry.num_events );

                if ( ImGui::MenuItem( label.c_str() ) )
                {
                    m_graphrows.show_row( entry.name.c_str(), GraphRows::SHOW_ROW );
                }
            }

            ImGui::EndMenu();
        }
    }

    if ( m_trace_events->get_comm_locs( m_mouse_over_row_name.c_str() ) )
    {
        if ( !m_rename_comm_buf[ 0 ] )
        {
            strcpy_safe( m_rename_comm_buf, m_mouse_over_row_name.c_str() );

            char *slash = strrchr( m_rename_comm_buf, '-' );
            if ( slash )
                *slash = 0;
        }

        ImGui::AlignFirstTextHeightToWidgets();
        ImGui::Text( "Rename '%s':", m_mouse_over_row_name.c_str() );

        ImGui::SameLine();
        if ( ImGui::InputText( "##rename_comm", m_rename_comm_buf, sizeof( m_rename_comm_buf ),
                               ImGuiInputTextFlags_EnterReturnsTrue ) )
        {
            if ( rename_comm_event( m_mouse_over_row_name.c_str(), m_rename_comm_buf ) )
                ImGui::CloseCurrentPopup();
        }
    }

    if ( optid != OPT_Max )
    {
        TraceLoader::option_t &opt = m_loader.m_options[ optid ];

        ImGui::AlignFirstTextHeightToWidgets();
        ImGui::Text( "%s", opt.desc.c_str() );
        ImGui::SameLine();
        ImGui::PushItemWidth( imgui_scale( 150.0f ) );
        ImGui::SliderInt( "##row_size", &opt.val, opt.val_min, opt.val_max );
    }

    ImGui::Separator();

    if ( ImGui::BeginMenu( "Save Location" ) )
    {
        for ( size_t i = 0; i < m_saved_graph_locs.size(); i++ )
        {
            std::string label = get_location_label_lambda( i );
            std::string shortcut = string_format( "Ctrl+Shift+%c", ( int )( i + '1' ) );

            if ( ImGui::MenuItem( label.c_str(), shortcut.c_str() ) )
            {
                m_saved_graph_locs[ i ] = std::make_pair( m_graph_start_ts, m_graph_length_ts );
                break;
            }
        }

        ImGui::EndMenu();
    }

    if ( ImGui::BeginMenu( "Restore Location" ) )
    {
        for ( size_t i = 0; i < m_saved_graph_locs.size(); i++ )
        {
            if ( m_saved_graph_locs[ i ].second )
            {
                std::string label = get_location_label_lambda( i );
                std::string shortcut = string_format( "Ctrl+%c", ( int )( i + '1' ) );

                if ( ImGui::MenuItem( label.c_str(), shortcut.c_str() ) )
                {
                    m_graph_start_ts = m_saved_graph_locs[ i ].first;
                    m_graph_length_ts = m_saved_graph_locs[ i ].second;
                    m_do_graph_start_timestr = true;
                    m_do_graph_length_timestr = true;
                }
            }
        }

        ImGui::EndMenu();
    }

    ImGui::Separator();

    for ( size_t i = 0; i < m_loader.m_options.size(); i++ )
    {
        TraceLoader::option_t &opt = m_loader.m_options[ i ];

        if ( opt.hidden )
            continue;

        if ( ( i >= OPT_RenderCrtc0 ) &&
             ( i <= OPT_RenderCrtc9 ) )
        {
            if ( i - OPT_RenderCrtc0 > m_loader.m_crtc_max )
                continue;
        }

        ImGui::PushID( i );

        if ( opt.type == TraceLoader::OPT_Bool )
        {
            bool val = !!opt.val;
            if ( ImGui::MenuItem( opt.desc.c_str(), "", &val ) )
                opt.val = val;
        }
        else if ( opt.type == TraceLoader::OPT_Int )
        {
            ImGui::PushItemWidth( imgui_scale( 200.0f ) );
            ImGui::SliderInt( "##slider_int", &opt.val, opt.val_min, opt.val_max, opt.desc.c_str() );
            ImGui::PopItemWidth();
        }
        else
        {
            ImGui::PushItemWidth( imgui_scale( 200.0f ) );
            ImGui::SliderFloat( "##slider_float", &opt.valf, opt.valf_min, opt.valf_max, opt.desc.c_str() );
            ImGui::PopItemWidth();
        }

        ImGui::PopID();
    }

    ImGui::EndPopup();
    return true;
}

void TraceWin::handle_mouse_graph_captured( graph_info_t &gi )
{
    // Uncapture mouse if user hits escape
    if ( m_mouse_captured && imgui_key_pressed( ImGuiKey_Escape ) )
    {
        m_mouse_captured = MOUSE_NOT_CAPTURED;
        ImGui::CaptureMouseFromApp( false );

        return;
    }

    bool is_mouse_down = ImGui::IsMouseDown( 0 );

    if ( ( m_mouse_captured == MOUSE_CAPTURED_ZOOM ) ||
         ( m_mouse_captured == MOUSE_CAPTURED_SELECT_AREA ) )
    {
        // shift + click: zoom area
        int64_t event_ts0 = gi.screenx_to_ts( m_mouse_capture_pos.x );
        int64_t event_ts1 = gi.screenx_to_ts( gi.mouse_pos.x );

        if ( event_ts0 > event_ts1 )
            std::swap( event_ts0, event_ts1 );

        if ( is_mouse_down )
        {
            std::string time_buf0 = ts_to_timestr( event_ts0, m_tsoffset );
            std::string time_buf1 = ts_to_timestr( event_ts1 - event_ts0 );

            // Show tooltip with starting time and length of selected area.
            imgui_set_tooltip( string_format( "%s (%s ms)", time_buf0.c_str(), time_buf1.c_str() ) );
        }
        else if ( m_mouse_captured == MOUSE_CAPTURED_ZOOM )
        {
            m_graph_start_ts = event_ts0 - m_tsoffset;
            m_graph_length_ts = event_ts1 - event_ts0;
            m_do_graph_start_timestr = true;
            m_do_graph_length_timestr = true;
        }
    }
    else if ( m_mouse_captured == MOUSE_CAPTURED_PAN )
    {
        // click: pan
        if ( is_mouse_down )
        {
            float dx = gi.mouse_pos.x - m_mouse_capture_pos.x;
            int64_t tsdiff = gi.dx_to_ts( dx );

            m_graph_start_ts -= tsdiff;
            m_do_graph_start_timestr = true;

            m_graph_start_y += gi.mouse_pos.y - m_mouse_capture_pos.y;

            m_mouse_capture_pos = gi.mouse_pos;
        }
    }

    if ( !is_mouse_down )
    {
        // Mouse is no longer down, uncapture mouse...
        m_mouse_captured = MOUSE_NOT_CAPTURED;
        ImGui::CaptureMouseFromApp( false );
    }

}

void TraceWin::set_mouse_graph_tooltip( class graph_info_t &gi, int64_t mouse_ts )
{
    std::string time_buf = "Time: " + ts_to_timestr( mouse_ts, m_tsoffset );

    const std::vector< uint32_t > *vblank_locs = m_trace_events->get_tdopexpr_locs( "$name=drm_vblank_event" );
    if ( vblank_locs )
    {
        int64_t prev_vblank_ts = INT64_MAX;
        int64_t next_vblank_ts = INT64_MAX;
        int eventid = ts_to_eventid( mouse_ts );
        size_t idx = vec_find_eventid( *vblank_locs, eventid );
        size_t idxmax = std::min< size_t >( idx + 20, vblank_locs->size() );

        for ( idx = ( idx > 10 ) ? ( idx - 10 ) : 0; idx < idxmax; idx++ )
        {
            trace_event_t &event = get_event( vblank_locs->at( idx ) );

            if ( m_loader.get_opt_crtc( event.crtc ) )
            {
                if ( event.ts < mouse_ts )
                {
                    if ( mouse_ts - event.ts < prev_vblank_ts )
                        prev_vblank_ts = mouse_ts - event.ts;
                }
                if ( event.ts > mouse_ts )
                {
                    if ( event.ts - mouse_ts < next_vblank_ts )
                        next_vblank_ts = event.ts - mouse_ts;
                }
            }
        }

        if ( prev_vblank_ts != INT64_MAX )
            time_buf += "\nPrev vblank: -" + ts_to_timestr( prev_vblank_ts, 0, 2 ) + "ms";
        if ( next_vblank_ts != INT64_MAX )
            time_buf += "\nNext vblank: " + ts_to_timestr( next_vblank_ts, 0, 2 ) + "ms";
    }

    bool sync_event_list_to_graph = m_loader.get_opt( OPT_SyncEventListToGraph ) && m_show_eventlist;

    if ( is_valid_id( gi.hovered_graph_event ) )
    {
        const trace_event_t &event_hov = get_event( gi.hovered_graph_event );
        std::string context = get_event_gfxcontext_str( event_hov );
        const std::vector< uint32_t > *plocs = m_trace_events->get_gfxcontext_locs( context.c_str() );

        if ( sync_event_list_to_graph )
        {
            // Sync event list to first event id in this context
            m_do_gotoevent = true;
            m_goto_eventid = plocs->at( 0 );
        }

        time_buf += string_format( "\n%s [%s]", event_hov.user_comm, context.c_str() );

        for ( uint32_t id : *plocs )
        {
            const trace_event_t &event = get_event( id );

            time_buf += string_format( "\n  %u %s %sms", event.id, event.name,
                                       ts_to_timestr( event.duration, 0, 4 ).c_str() );
        }
    }

    m_highlight_ids.clear();

    if ( !gi.hovered_items.empty() )
    {
        // Sort hovered items array by id
        std::sort( gi.hovered_items.begin(), gi.hovered_items.end(),
                   [=]( const graph_info_t::hovered_t& lx, const graph_info_t::hovered_t &rx )
        {
            return lx.eventid < rx.eventid;
        } );

        // Show tooltip with the closest events we could drum up
        for ( graph_info_t::hovered_t &hov : gi.hovered_items )
        {
            trace_event_t &event = get_event( hov.eventid );
            std::string gfxcontext_str = get_event_gfxcontext_str( event );

            m_highlight_ids.push_back( event.id );

            // Add event id and distance from cursor to this event
            time_buf += string_format( "\n%u %c%sms",
                                       hov.eventid, hov.neg ? '-' : ' ',
                                       ts_to_timestr( hov.dist_ts, 0, 4 ).c_str() );

            // If this isn't an ftrace print event, add the event name
            if ( !event.is_ftrace_print() )
                time_buf += std::string( " " ) + event.name;

            // If this is a vblank event, add the crtc
            if ( event.crtc >= 0 )
                time_buf += std::to_string( event.crtc );

            // If this is a timeline gfx context, add it plus the user comm
            if ( !gfxcontext_str.empty() )
                time_buf += string_format( " [%s] %s", gfxcontext_str.c_str(), event.user_comm );

            // Add yellow string for ftrace print events
            if ( event.is_ftrace_print() )
            {
                const event_field_t *field = find_event_field( event.fields, "buf" );

                if ( field )
                    time_buf += " " +  multi_text_color::yellow.m_str( field->value );
            }
        }

        if ( sync_event_list_to_graph && !m_do_gotoevent )
        {
            m_do_gotoevent = true;
            m_goto_eventid = gi.hovered_items[ 0 ].eventid;
        }
    }

    imgui_set_tooltip( time_buf );
}

void TraceWin::handle_mouse_graph( graph_info_t &gi )
{
    // If we've got an active popup menu, render it.
    if ( m_graph_popup )
    {
        m_graph_popup = TraceWin::render_graph_popup( gi );
        return;
    }

    m_ts_marker = -1;

    // Check if mouse if over our graph and we've got focus
    m_mouse_over_graph = gi.mouse_pos_in_graph() &&
                         ImGui::IsRootWindowOrAnyChildFocused();

    // If we don't own the mouse and we don't have focus, bail.
    if ( !m_mouse_captured && !m_mouse_over_graph )
        return;

    if ( m_mouse_captured )
    {
        handle_mouse_graph_captured( gi );
        return;
    }

    // Mouse is over our active graph window
    {
        int64_t mouse_ts = gi.screenx_to_ts( gi.mouse_pos.x );

        m_ts_marker = mouse_ts;

        // Set the tooltip
        set_mouse_graph_tooltip( gi, mouse_ts );

        // Check for clicking, wheeling, etc.
        if ( ImGui::IsMouseClicked( 0 ) )
        {
            if ( ImGui::GetIO().KeyCtrl )
            {
                // ctrl + click: select area
                m_mouse_captured = MOUSE_CAPTURED_SELECT_AREA;
                ImGui::CaptureMouseFromApp( true );
                m_mouse_capture_pos = gi.mouse_pos;
            }
            else if ( ImGui::GetIO().KeyShift )
            {
                // shift + click: zoom
                m_mouse_captured = MOUSE_CAPTURED_ZOOM;
                ImGui::CaptureMouseFromApp( true );
                m_mouse_capture_pos = gi.mouse_pos;
            }
            else
            {
                // click: pan
                m_mouse_captured = MOUSE_CAPTURED_PAN;
                ImGui::CaptureMouseFromApp( true );
                m_mouse_capture_pos = gi.mouse_pos;
            }
        }
        else if ( ImGui::IsMouseClicked( 1 ) )
        {
            // right click: popup menu
            m_graph_popup = true;
            m_graph_rows_hidden_rows = m_graphrows.get_hidden_rows_list( m_trace_events );

            ImGui::OpenPopup( "GraphPopup" );
        }
        else
        {
            float mousewheel = ImGui::GetIO().MouseWheel;

            if ( mousewheel )
            {
                bool zoomin = ( mousewheel > 0.0f );
                int64_t len0 = m_graph_length_ts;
                int64_t amt = zoomin ? -( m_graph_length_ts / 2 ) : ( m_graph_length_ts / 2 );
                int64_t len1 = len0 + amt;

                if ( ( len1 > g_min_graph_length ) && ( len1 < g_max_graph_length ) )
                {
                    //$ TODO mikesart: we've gotten overflow error here:
                    // runtime error: signed integer overflow: 2023691192 * 4676142294 cannot be represented in type 'long int'
                    m_graph_start_ts = mouse_ts - len1 * ( mouse_ts - gi.ts0 ) / len0 - m_tsoffset;
                    m_graph_length_ts = len1;

                    m_do_graph_start_timestr = true;
                    m_do_graph_length_timestr = true;
                }
            }
        }
    }
}
