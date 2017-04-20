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

  TODO mikesart: Try coloring the ftrace print events per the hash of the string?

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

class graph_info_t
{
public:
    void init( float x, float w, int64_t start_ts, int64_t length_ts );
    void set_pos_y( float y, float h );

    float ts_to_x( int64_t ts );
    float ts_to_screenx( int64_t ts );

    int64_t screenx_to_ts( float x_in );
    int64_t dx_to_ts( float x_in );

    bool pt_in_graph( const ImVec2 &posin );
    bool mouse_pos_in_graph();
    bool mouse_pos_in_rect( float x, float w, float y, float h );

public:
    uint32_t row_num;
    float x, y, w, h;

    int64_t ts0;
    int64_t ts1;
    int64_t tsdx;
    double tsdxrcp;

    bool mouse_over;
    ImVec2 mouse_pos;

    uint32_t eventstart;
    uint32_t eventend;

    struct hovered_t
    {
        bool neg;
        int64_t dist_ts;
        uint32_t eventid;
    };
    const size_t hovered_max = 6;
    std::vector< hovered_t > hovered_items;

    uint32_t hovered_graph_event;

    bool is_timeline;
    bool timeline_render_user;
    bool graph_only_filtered;
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
    ImVec2 textsize = ImGui::CalcTextSize( text );

    ImGui::GetWindowDrawList()->AddRectFilled(
        ImVec2( x, y ), ImVec2( x + textsize.x, y + textsize.y ),
        col_get( col_RowLabelBk) );

    ImGui::GetWindowDrawList()->AddText( ImVec2( x, y ), color, text );
}

const event_field_t *find_event_field( std::vector< event_field_t > &fields, const char *name )
{
    for ( const event_field_t &field : fields )
    {
        if ( !strcmp( field.key, name ) )
            return &field;
    }

    return NULL;
}

const char *get_event_field_val( std::vector< event_field_t > &fields, const char *name )
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

/*
 * graph_info_t
 */
void graph_info_t::init( float x_in, float w_in, int64_t start_ts, int64_t length_ts )
{
    row_num = 0;

    x = x_in;
    w = w_in;

    ts0 = start_ts;
    ts1 = start_ts + length_ts;

    tsdx = ts1 - ts0 + 1;
    tsdxrcp = 1.0 / tsdx;

    mouse_pos = ImGui::IsRootWindowOrAnyChildFocused() ?
            ImGui::GetMousePos() : ImVec2( -1024, -1024 );

    hovered_items.clear();
    hovered_graph_event = INVALID_ID;

    is_timeline = false;
}

void graph_info_t::set_pos_y( float y_in, float h_in )
{
    y = y_in;
    h = h_in;

    mouse_over = mouse_pos.x >= x &&
            mouse_pos.x <= x + w &&
            mouse_pos.y >= y &&
            mouse_pos.y <= y + h;

    row_num++;
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

void TraceWin::render_graph_hw_row_timeline( graph_info_t &gi )
{
    struct row_t
    {
        const char *name;
        const std::vector< uint32_t > *plocs;
    };
    const std::array< row_t, 1 > rows =
    {
        "gfx", m_trace_events->get_timeline_locs( rows[ 0 ].name )
    };

    imgui_push_smallfont();

    ImU32 col_event = col_get( col_1Event );
    float row_h = gi.h;

    for ( size_t irow = 0; irow < rows.size(); irow++ )
    {
        ImRect hov_rect;
        ImU32 last_color = 0;
        float y = gi.y + irow * row_h;
        const std::vector< uint32_t > *plocs = rows[ irow ].plocs;

        if ( !plocs )
            continue;

        for ( size_t idx = vec_find_eventid( *plocs, gi.eventstart );
              idx < plocs->size();
              idx++ )
        {
            const trace_event_t &fence_signaled = get_event( plocs->at( idx ) );

            if ( fence_signaled.is_fence_signaled() &&
                 is_valid_id( fence_signaled.id_start ) &&
                 ( fence_signaled.ts - fence_signaled.duration < gi.ts1 ) )
            {
                float x0 = gi.ts_to_screenx( fence_signaled.ts - fence_signaled.duration );
                float x1 = gi.ts_to_screenx( fence_signaled.ts );

                // Get a color based on the user command line
                uint32_t hashval = fnv_hashstr32( fence_signaled.user_comm );
                float h = ( hashval & 0xffffff ) / 16777215.0f;
                float v = ( hashval >> 24 ) / ( 2.0f * 255.0f ) + 0.5f;
                ImU32 col = imgui_hsv( h, .9f, v, 1.0f );

                imgui_drawrect( x0, x1 - x0, y, row_h, col );

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
                if ( last_color == col )
                    imgui_drawrect( x0, 1.0, y, row_h, col_event );
                else
                    last_color = col;

                // Check if we're hovering this event.
                if ( !is_valid_id( gi.hovered_graph_event ) &&
                     gi.mouse_pos_in_rect( x0, x1 - x0, y, row_h ) )
                {
                    gi.hovered_graph_event = fence_signaled.id;
                    hov_rect = { x0, y, x1, y + row_h };
                }
            }
        }

        if ( hov_rect.Min.x < gi.x + gi.w )
            ImGui::GetWindowDrawList()->AddRect( hov_rect.Min, hov_rect.Max, col_get( col_BarSelRect ) );
    }

    imgui_pop_smallfont();
}

void TraceWin::render_graph_row_timeline( const std::vector< uint32_t > &locs, graph_info_t &gi )
{
    imgui_push_smallfont();

    ImRect hov_rect;
    ImU32 col_hwrunning = col_get( col_BarHwRunning );
    ImU32 col_userspace = col_get( col_BarUserspace );
    ImU32 col_hwqueue = col_get( col_BarHwQueue );
    ImU32 color_1event = col_get( col_1Event );
    float text_h = ImGui::GetTextLineHeightWithSpacing();

    uint32_t timeline_row_count = gi.h / text_h;

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
                float y = gi.y + ( fence_signaled.graph_row_id % timeline_row_count ) * text_h;

                // amdgpu_cs_ioctl  amdgpu_sched_run_job   |   fence_signaled
                //       |-----------------|---------------|--------|
                //       |user-->          |hwqueue-->     |hw->    |
                float x_user_start = gi.ts_to_screenx( cs_ioctl.ts );
                float x_hwqueue_start = gi.ts_to_screenx( sched_run_job.ts );
                float x_hwqueue_end = gi.ts_to_screenx( fence_signaled.ts - fence_signaled.duration );
                float x_hw_end = gi.ts_to_screenx( fence_signaled.ts );
                float xleft = gi.timeline_render_user ? x_user_start : x_hwqueue_start;

                if ( ( gi.hovered_graph_event == fence_signaled.id ) ||
                    gi.mouse_pos_in_rect( xleft, x_hw_end - xleft, y, text_h ) )
                {
                    // Mouse is hovering over this fence_signaled.
                    hovered = true;
                    hov_rect = { x_user_start, y, x_hw_end, y + text_h };

                    if ( !is_valid_id( gi.hovered_graph_event ) )
                        gi.hovered_graph_event = fence_signaled.id;
                }

                // Draw user bar
                if ( hovered || gi.timeline_render_user )
                    imgui_drawrect( x_user_start, x_hwqueue_start - x_user_start, y, text_h, col_userspace );

                // Draw hw queue bar
                if ( x_hwqueue_end != x_hwqueue_start )
                    imgui_drawrect( x_hwqueue_start, x_hwqueue_end - x_hwqueue_start, y, text_h, col_hwqueue );

                // Draw hw running bar
                imgui_drawrect( x_hwqueue_end, x_hw_end - x_hwqueue_end, y, text_h, col_hwrunning );

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
                        imgui_drawrect( x_user_start, 1.0, y, text_h, color_1event );

                        // Check if we're mouse hovering starting event
                        if ( gi.mouse_over && gi.mouse_pos.y >= y && gi.mouse_pos.y <= y + text_h )
                        {
                            // If we are hovering, and no selection bar is set, do it.
                            if ( add_mouse_hovered_event( x_user_start, gi, cs_ioctl ) && ( hov_rect.Min.x == FLT_MAX ) )
                            {
                                hov_rect = { x_user_start, y, x_hw_end, y + text_h };

                                // Draw user bar for hovered events if they weren't already drawn
                                if ( !hovered && !gi.timeline_render_user )
                                    imgui_drawrect( x_user_start, x_hwqueue_start - x_user_start, y, text_h, col_userspace );
                            }
                        }
                    }

                    // Draw event line for hwqueue start and hw end
                    imgui_drawrect( x_hwqueue_start, 1.0, y, text_h, color_1event );
                    imgui_drawrect( x_hw_end, 1.0, y, text_h, color_1event );
                }
            }
        }
    }

    if ( hov_rect.Min.x < gi.x + gi.w )
        ImGui::GetWindowDrawList()->AddRect( hov_rect.Min, hov_rect.Max, col_get( col_BarSelRect ) );

    imgui_pop_smallfont();
}

void TraceWin::render_graph_row( const std::string &comm, const std::vector< uint32_t > &locs, graph_info_t &gi )
{
    // Draw background
    ImGui::GetWindowDrawList()->AddRectFilled(
        ImVec2( gi.x, gi.y ),
        ImVec2( gi.x + gi.w, gi.y + gi.h ),
        col_get( col_GraphRowBk ) );

    // Go through all event IDs for this process
    uint32_t num_events = 0;
    bool draw_selected_event = false;
    bool draw_hovered_event = false;
    bool is_gfx_hw = ( comm == "gfx hw" );

    if ( is_gfx_hw )
    {
        render_graph_hw_row_timeline( gi );
    }
    else if ( gi.is_timeline )
    {
        render_graph_row_timeline( locs, gi );
    }
    else
    {
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
    }

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

    // Draw row label
    std::string label = string_format( "%u) %s", gi.row_num, comm.c_str() );
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
    if ( m_mouse_captured == 1 )
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
    if ( m_locations.size() < 9 )
        m_locations.resize( 9 );

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
                        m_locations[ index ] = std::make_pair( m_graph_start_ts, m_graph_length_ts );
                    }
                    else if ( m_locations[ index ].second )
                    {
                        // ctrl+#: goto location
                        m_graph_start_ts = m_locations[ index ].first;
                        m_graph_length_ts = m_locations[ index ].second;
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

void TraceWin::render_process_graph()
{
    struct row_info_t
    {
        float row_y;
        float row_h;
        bool is_timeline;
        std::string comm;
        const std::vector< uint32_t > *plocs;
    };
    std::vector< row_info_t > row_info;

    uint32_t timeline_gfx_index = INVALID_ID;

    imgui_push_smallfont();

    float text_h = ImGui::GetTextLineHeightWithSpacing();
    float graph_row_padding = ImGui::GetStyle().FramePadding.y;
    float total_graph_height = graph_row_padding;

    imgui_pop_smallfont();

    for ( const std::string &comm : m_graph_rows )
    {
        row_info_t rinfo;

        rinfo.row_y = total_graph_height;
        rinfo.row_h = text_h * 2;
        rinfo.is_timeline = false;
        rinfo.comm = comm;

        const std::vector< uint32_t > *plocs;
        const char *comm_str = comm.c_str();

        plocs = m_trace_events->get_comm_locs( comm_str );
        if ( !plocs )
            plocs = m_trace_events->get_tdopexpr_locs( comm_str );
        if ( !plocs )
        {
            int rows = 4;

            if ( comm == "gfx hw" )
            {
                rows = 2;
                comm_str = "gfx";
            }
            else if ( comm == "gfx" )
                rows = m_loader.get_opt( OPT_TimelineGfxRowCount );
            else if ( comm == "sdma0" )
                rows = m_loader.get_opt( OPT_TimelineSdma0RowCount );
            else if ( comm == "sdma1" )
                rows = m_loader.get_opt( OPT_TimelineSdma1RowCount );

            rows = Clamp< int >( rows, 2, 50 );

            rinfo.is_timeline = true;
            rinfo.row_h = text_h * rows;

            plocs = m_trace_events->get_timeline_locs( comm_str );
        }

        if ( plocs )
        {
            rinfo.plocs = plocs;

            row_info.push_back( rinfo );
            if ( comm == "gfx" )
                timeline_gfx_index = row_info.size() - 1;

            total_graph_height += rinfo.row_h + graph_row_padding;
        }
    }

    total_graph_height += imgui_scale( 2.0f );

    if ( row_info.empty() )
        return;

    // Max graph row count is the number of rows.
    m_loader.m_options[ OPT_GraphRowCount ].val_max = row_info.size();

    // Get current count of rows. 0 means show all rows.
    int graph_row_count = m_loader.get_opt( OPT_GraphRowCount );
    int row_count = ( graph_row_count < 1 ) ? row_info.size() : graph_row_count;
    row_count = Clamp< int >( row_count, 1, row_info.size() );

    // Slider to set the number of graph rows
    ImGui::SameLine();
    ImGui::PushItemWidth( imgui_scale( 200.0f ) );
    if ( ImGui::SliderInt( "##GraphRows", &row_count, 1, row_info.size(), "Graph Rows: %.0f" ) )
    {
        m_loader.m_options[ OPT_GraphRowCount ].val =
                ( ( uint32_t )row_count >= row_info.size() ) ? 0 : row_count;
    }

    bool gfx_timeline_zoom = is_valid_id( timeline_gfx_index ) ?
                !!m_loader.get_opt( OPT_TimelineZoomGfx ) : false;

    {
        bool zoom = !!m_loader.get_opt( OPT_TimelineZoomGfx );

        ImGui::SameLine();
        if ( ImGui::Checkbox( "Zoom gfx timeline", &zoom ) )
            m_loader.m_options[ OPT_TimelineZoomGfx ].val = zoom;
    }

    // Make sure our ts start and length values are sane
    range_check_graph_location();

    {
        graph_info_t gi;

        float visible_graph_height = ( ( size_t )row_count >= row_info.size() ) ?
            total_graph_height : row_info[ row_count ].row_y;

        ImGui::BeginChild( "EventGraph", ImVec2( 0, visible_graph_height ), true );
        {
            ImVec2 windowpos = ImGui::GetWindowClipRectMin();
            ImVec2 cliprectmax = ImGui::GetWindowClipRectMax();
            ImVec2 windowsize = ImVec2( cliprectmax.x - windowpos.x, cliprectmax.y - windowpos.y );

            // Clear graph background
            imgui_drawrect( windowpos.x, windowsize.x,
                            windowpos.y, windowsize.y, col_get( col_GraphBk ) );

            // Initialize x / width and ts values
            gi.init( windowpos.x, windowsize.x,
                     m_graph_start_ts + m_tsoffset, m_graph_length_ts );

            // Check if we're supposed to render filtered events only
            gi.graph_only_filtered =
                    m_loader.m_options[ OPT_GraphOnlyFiltered ].val &&
                    !m_filtered_events.empty();

            // Initialize eventstart / end
            gi.eventstart = ts_to_eventid( gi.ts0 );
            gi.eventend = ts_to_eventid( gi.ts1 );

            // Range check our mouse pan values
            m_graph_start_y = Clamp< float >( m_graph_start_y,
                                              visible_graph_height - total_graph_height, 0.0f );

            if ( !gfx_timeline_zoom )
            {
                // Go through and render all the rows
                for ( size_t i = 0; i < row_info.size(); i++ )
                {
                    const row_info_t &ri = row_info[ i ];

                    // Draw the gfx row last so we can select things based on hovering other timelines
                    if ( i == timeline_gfx_index )
                        continue;

                    gi.is_timeline = ri.is_timeline;
                    gi.timeline_render_user = !!m_loader.get_opt( OPT_TimelineRenderUserSpace );

                    gi.set_pos_y( windowpos.y + ri.row_y + m_graph_start_y, ri.row_h );
                    if ( gi.mouse_over )
                        m_mouse_over_row_name = ri.comm;

                    //$ TODO mikesart: Check if entire row is clipped...
                    render_graph_row( ri.comm, *ri.plocs, gi );
                }

                // Draw the gfx row now if we've got one
                if ( is_valid_id( timeline_gfx_index ) )
                {
                    const row_info_t &ri = row_info[ timeline_gfx_index ];

                    gi.is_timeline = ri.is_timeline;
                    gi.timeline_render_user = !!m_loader.get_opt( OPT_TimelineRenderUserSpace );

                    gi.set_pos_y( windowpos.y + ri.row_y + m_graph_start_y, ri.row_h );
                    if ( gi.mouse_over )
                        m_mouse_over_row_name = ri.comm;

                    render_graph_row( ri.comm, *ri.plocs, gi );
                }
            }

            // Render full graph lines: vblanks, mouse cursors, etc...
            gi.set_pos_y( windowpos.y, windowsize.y );

            if ( gfx_timeline_zoom )
            {
                gi.is_timeline = true;
                gi.timeline_render_user = true;

                render_graph_row( row_info[ timeline_gfx_index ].comm, *row_info[ timeline_gfx_index ].plocs, gi );
            }

            render_graph_vblanks( gi );
        }

        // Handle right, left, pgup, pgdown, etc in graph
        handle_graph_keyboard_scroll();
        handle_graph_hotkeys();

        // Render mouse tooltips, mouse selections, etc
        handle_mouse_graph( gi );

        ImGui::EndChild();
    }

    if ( !m_graph_popup )
        m_mouse_over_row_name = "";
}

bool TraceWin::render_graph_popup( graph_info_t &gi )
{
    if ( !ImGui::BeginPopup( "GraphPopup" ) )
        return false;

    auto get_location_label_lambda = [this]( size_t i )
    {
        auto &pair = m_locations[ i ];
        std::string start = ts_to_timestr( pair.first );
        std::string len = ts_to_timestr( pair.second );
        return string_format( "Start:%s Length:%s", start.c_str(), len.c_str() );
    };

    imgui_text_bg( "Options", ImGui::GetColorVec4( ImGuiCol_Header ) );
    ImGui::Separator();

    if ( !m_mouse_over_row_name.empty() )
    {
        std::string label = string_format( "Hide row '%s'", m_mouse_over_row_name.c_str() );

        if ( ImGui::MenuItem( label.c_str() ) )
            graph_rows_show( m_mouse_over_row_name, HIDE_ROW );

        label = string_format( "Hide row '%s' and below", m_mouse_over_row_name.c_str() );
        if ( ImGui::MenuItem( label.c_str() ) )
            graph_rows_show( m_mouse_over_row_name, HIDE_ROW_AND_ALL_BELOW );
    }

    if ( !m_graph_rows_hidden_rows.empty() )
    {
        if ( ImGui::BeginMenu( "Show row" ) )
        {
            for ( const std::string &entry : m_graph_rows_hidden_rows )
            {
                std::string label = entry;
                const std::vector< uint32_t > *plocs = m_trace_events->get_comm_locs( entry.c_str() );

                if ( plocs )
                    label += string_format( " (%lu events)", plocs->size() );

                if ( ImGui::MenuItem( label.c_str() ) )
                {
                    graph_rows_show( entry.c_str(), SHOW_ROW );
                }
            }

            ImGui::EndMenu();
        }
    }

    ImGui::Separator();

    if ( ImGui::BeginMenu( "Save Location" ) )
    {
        for ( size_t i = 0; i < m_locations.size(); i++ )
        {
            std::string label = get_location_label_lambda( i );
            std::string shortcut = string_format( "Ctrl+Shift+%c", ( int )( i + '1' ) );

            if ( ImGui::MenuItem( label.c_str(), shortcut.c_str() ) )
            {
                m_locations[ i ] = std::make_pair( m_graph_start_ts, m_graph_length_ts );
                break;
            }
        }

        ImGui::EndMenu();
    }

    if ( ImGui::BeginMenu( "Restore Location" ) )
    {
        for ( size_t i = 0; i < m_locations.size(); i++ )
        {
            if ( m_locations[ i ].second )
            {
                std::string label = get_location_label_lambda( i );
                std::string shortcut = string_format( "Ctrl+%c", ( int )( i + '1' ) );

                if ( ImGui::MenuItem( label.c_str(), shortcut.c_str() ) )
                {
                    m_graph_start_ts = m_locations[ i ].first;
                    m_graph_length_ts = m_locations[ i ].second;
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

        if ( ( i >= OPT_RenderCrtc0 ) &&
             ( i <= OPT_RenderCrtc9 ) )
        {
            if ( i - OPT_RenderCrtc0 > m_loader.m_crtc_max )
                continue;
        }

        ImGui::PushID( i );

        if ( opt.val_min == 0 && opt.val_max == 1 )
        {
            bool val = !!opt.val;
            if ( ImGui::MenuItem( opt.desc.c_str(), "", &val ) )
                opt.val = val;
        }
        else
        {
            ImGui::PushItemWidth( imgui_scale( 200.0f ) );
            ImGui::SliderInt( "##slider", &opt.val, opt.val_min, opt.val_max, opt.desc.c_str() );
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
    if ( m_mouse_captured &&
         imgui_key_pressed( ImGuiKey_Escape ) )
    {
        m_mouse_captured = 0;
        ImGui::CaptureMouseFromApp( false );

        return;
    }

    if ( m_mouse_captured == 1 )
    {
        // shift + click: zoom area
        int64_t event_ts0 = gi.screenx_to_ts( m_mouse_capture_pos.x );
        int64_t event_ts1 = gi.screenx_to_ts( gi.mouse_pos.x );

        if ( event_ts0 > event_ts1 )
            std::swap( event_ts0, event_ts1 );

        if ( ImGui::IsMouseDown( 0 ) )
        {
            std::string time_buf0 = ts_to_timestr( event_ts0, m_tsoffset );
            std::string time_buf1 = ts_to_timestr( event_ts1 - event_ts0 );

            // Show tooltip with starting time and length of selected area.
            imgui_set_tooltip( string_format( "%s (%s ms)", time_buf0.c_str(), time_buf1.c_str() ) );
        }
        else
        {
            // Mouse is no longer down, uncapture mouse...
            m_mouse_captured = 0;
            ImGui::CaptureMouseFromApp( false );

            m_graph_start_ts = event_ts0 - m_tsoffset;
            m_graph_length_ts = event_ts1 - event_ts0;
            m_do_graph_start_timestr = true;
            m_do_graph_length_timestr = true;
        }
    }
    else if ( m_mouse_captured == 2 )
    {
        // click: pan
        if ( ImGui::IsMouseDown( 0 ) )
        {
            float dx = gi.mouse_pos.x - m_mouse_capture_pos.x;
            int64_t tsdiff = gi.dx_to_ts( dx );

            m_graph_start_ts -= tsdiff;
            m_do_graph_start_timestr = true;

            m_graph_start_y += gi.mouse_pos.y - m_mouse_capture_pos.y;

            m_mouse_capture_pos = gi.mouse_pos;
        }
        else
        {
            m_mouse_captured = 0;
            ImGui::CaptureMouseFromApp( false );
        }
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
            if ( ImGui::GetIO().KeyShift )
            {
                // shift + click: zoom
                m_mouse_captured = 1;
                ImGui::CaptureMouseFromApp( true );
                m_mouse_capture_pos = gi.mouse_pos;
            }
            else
            {
                // click: pan
                m_mouse_captured = 2;
                ImGui::CaptureMouseFromApp( true );
                m_mouse_capture_pos = gi.mouse_pos;
            }
        }
        else if ( ImGui::IsMouseClicked( 1 ) )
        {
            // right click: popup menu
            m_graph_popup = true;
            m_graph_rows_hidden_rows = graph_rows_get_hidden_rows();

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
