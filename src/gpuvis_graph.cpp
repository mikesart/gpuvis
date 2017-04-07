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

#include <SDL.h>

#include "GL/gl3w.h"
#include "gpuvis.h"

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

static const event_field_t *find_event_field( std::vector< event_field_t > &fields, const char *name )
{
    for ( const event_field_t &field : fields )
    {
        if ( !strcmp( field.key, name ) )
            return &field;
    }

    return NULL;
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
    float min_width = std::min( num_events + 1.0f, 4.0f );
    float width = std::max( x1 - x0, min_width );

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

    mouse_pos = ImGui::GetMousePos();

    hovered_items.clear();
    hovered_graph_event = ( uint32_t )-1;

    is_timeline = false;
}

void graph_info_t::set_pos_y( float y_in, float h_in )
{
    y = y_in;
    h = h_in;

    mouse_over =
        mouse_pos.x >= x &&
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

//$ TODO: Add timeline view which renders on single line and only displays the red parts
// and color codes the app names...

void TraceWin::render_graph_row_timeline( const std::string &comm, const std::vector< uint32_t > &locs,
                                          graph_info_t &gi )
{
    imgui_push_smallfont();

    ImVec2 hov_p0 = { FLT_MAX, FLT_MAX };
    ImVec2 hov_p1 = { FLT_MAX, FLT_MAX };
    float last_fence_signaled_x = -1.0f;
    ImU32 col_hwrunning = col_get( col_BarHwRunning );
    ImU32 col_userspace = col_get( col_BarUserspace );
    ImU32 col_hwqueue = col_get( col_BarHwQueue );
    float text_h = ImGui::GetTextLineHeightWithSpacing();

    uint32_t timeline_row_count = gi.h / text_h;

    bool render_timeline_events = !!m_loader.get_opt( TraceLoader::OPT_TimelineEvents );
    bool render_timeline_labels = !!m_loader.get_opt( TraceLoader::OPT_TimelineLabels );

    for ( size_t idx = vec_find_eventid( locs, gi.eventstart );
          idx < locs.size();
          idx++ )
    {
        uint32_t eventid = locs[ idx ];
        const trace_event_t &event = get_event( eventid );

        if ( event.is_fence_signaled() && ( event.id_start != ( uint32_t )-1 ) )
        {
            const trace_event_t &event1 = get_event( event.id_start );
            const trace_event_t &event0 = ( event1.id_start != ( uint32_t )-1 ) ?
                        get_event( event1.id_start ) : event1;

            //$ TODO mikesart: can we bail out of this loop at some point if
            //  our start times for all the graphs are > gi.ts1?
            if ( event0.ts < gi.ts1 )
            {
                float x0 = gi.ts_to_screenx( event0.ts );
                float x1 = gi.ts_to_screenx( event1.ts );
                float x2 = gi.ts_to_screenx( event.ts );
                float xleft = gi.timeline_render_user ? x0 : x1;
                float dx = x2 - xleft;
                float y = gi.y + ( event1.graph_row_id % timeline_row_count ) * text_h;

                if ( dx < imgui_scale( 2.0f ) )
                {
                    imgui_drawrect( x1, dx, y, text_h, col_hwrunning );
                }
                else
                {
                    bool hovered = false;

                    if ( gi.hovered_graph_event == ( uint32_t )-1 )
                    {
                        if ( gi.mouse_pos.x >= xleft &&
                             gi.mouse_pos.x <= x2 &&
                             gi.mouse_pos.y >= y &&
                             gi.mouse_pos.y <= y + text_h )
                        {
                            hovered = true;
                            gi.hovered_graph_event = event0.id;

                            hov_p0.x = x0;
                            hov_p0.y = y;
                            hov_p1.x = x2;
                            hov_p1.y = y + text_h;
                        }
                    }

                    // Current job doesn't start until the last one finishes.
                    if ( ( last_fence_signaled_x > x1 ) && ( last_fence_signaled_x < x2 ) )
                    {
                        if ( hovered || gi.timeline_render_user )
                            imgui_drawrect( x0, x1 - x0, y, text_h, col_userspace );
                        imgui_drawrect( x1, last_fence_signaled_x - x1, y, text_h, col_hwqueue );
                        imgui_drawrect( last_fence_signaled_x, x2 - last_fence_signaled_x, y, text_h, col_hwrunning );
                    }
                    else
                    {
                        if ( hovered || gi.timeline_render_user )
                            imgui_drawrect( x0, x1 - x0, y, text_h, col_userspace );
                        imgui_drawrect( x1, x2 - x1, y, text_h, col_hwrunning );
                    }

                    last_fence_signaled_x = x2;

                    if ( render_timeline_labels )
                    {
                        const ImVec2& size = ImGui::CalcTextSize( event0.user_comm );

                        if ( dx >= size.x )
                        {
                            float x = std::max( x1, gi.x ) + imgui_scale( 2.0f );

                            ImGui::GetWindowDrawList()->AddText( ImVec2( x, y + imgui_scale( 1.0f ) ),
                                                                 col_get( col_BarText ), event0.user_comm );
                        }
                    }
                }

                if ( render_timeline_events )
                {
                    ImU32 color = col_get( col_1Event );

                    if ( event0.id != event1.id )
                    {
                        imgui_drawrect( x0, 1.0, y, text_h, color );

                        //$ TODO: If we're hovering over this event and it's not selected,
                        // set hov_p0 and hov_p1 to draw the entire bar select?

                        // Check if we're mouse hovering starting event
                        if ( gi.mouse_over && gi.mouse_pos.y >= y && gi.mouse_pos.y <= y + text_h )
                        {
                            // If we are hovering, and no selection bar is set, do it.
                            if ( add_mouse_hovered_event( x0, gi, event0 ) && ( hov_p0.x == FLT_MAX ) )
                            {
                                hov_p0.x = x0;
                                hov_p0.y = y;
                                hov_p1.x = x2;
                                hov_p1.y = y + text_h;

                                imgui_drawrect( x0, x1 - x0, y, text_h, col_userspace );
                            }
                        }
                    }
                    imgui_drawrect( x1, 1.0, y, text_h, color );
                    imgui_drawrect( x2, 1.0, y, text_h, color );
                }
            }
        }
    }

    if ( hov_p0.x < gi.x + gi.w )
    {
        ImGui::GetWindowDrawList()->AddRect( hov_p0, hov_p1, col_get( col_BarSelRect ) );
    }

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

    if ( gi.is_timeline )
    {
        render_graph_row_timeline( comm, locs, gi );
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
    const std::vector< uint32_t > *vblank_locs = m_trace_events->get_event_locs( "drm_vblank_event" );

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

    if ( m_hovered_eventlist_eventid != ( uint32_t )-1 )
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
    if ( m_selected_eventid != ( uint32_t )-1 )
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
        if ( m_eventlist_start_eventid != ( uint32_t )-1 &&
             m_eventlist_end_eventid != ( uint32_t )-1 )
        {
            trace_event_t &event0 = get_event( m_eventlist_start_eventid );
            trace_event_t &event1 = get_event( m_eventlist_end_eventid - 1 );
            float xstart = gi.ts_to_screenx( event0.ts );
            float xend = gi.ts_to_screenx( event1.ts );

            imgui_drawrect( xstart, xend - xstart,
                            gi.y, gi.h,
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
        if ( ImGui::IsKeyPressed( 'z' ) )
        {
            m_loader.m_options[ TraceLoader::OPT_TimelineZoomGfx ].val ^= 1;
        }
        else
        {
            bool keyshift = ImGui::GetIO().KeyShift;

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

    uint32_t timeline_gfx_index = ( uint32_t )-1;

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

        plocs = m_trace_events->get_comm_locs( comm.c_str() );
        if ( !plocs )
            plocs = m_trace_events->get_event_locs( comm.c_str() );
        if ( !plocs )
            plocs = m_trace_events->get_gfxcontext_locs( comm.c_str() );
        if ( !plocs )
        {
            int rows = 4;

            if ( comm == "gfx" )
                rows = m_loader.get_opt( TraceLoader::OPT_TimelineGfxRowCount );
            else if ( comm == "sdma0" )
                rows = m_loader.get_opt( TraceLoader::OPT_TimelineSdma0RowCount );
            else if ( comm == "sdma1" )
                rows = m_loader.get_opt( TraceLoader::OPT_TimelineSdma1RowCount );

            rows = Clamp( rows, 2, 50 );

            rinfo.is_timeline = true;
            rinfo.row_h = text_h * rows;

            plocs = m_trace_events->get_timeline_locs( comm.c_str() );
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

    handle_graph_hotkeys();

    // Get current count of rows. 0 means show all rows.
    int row_count = ( m_loader.m_graph_row_count < 1 ) ? row_info.size() : m_loader.m_graph_row_count;
    row_count = Clamp< int >( row_count, 1, row_info.size() );

    // Slider to set the number of graph rows
    ImGui::SameLine();
    ImGui::Text( "Rows:" );
    ImGui::SameLine();
    ImGui::PushItemWidth( imgui_scale( 200.0f ) );
    if ( ImGui::SliderInt( "##GraphRowsCount", &row_count, 1, row_info.size() ) )
    {
        m_loader.m_graph_row_count = ( ( uint32_t )row_count >= row_info.size() ) ? 0 : row_count;
    }

    bool gfx_timeline_zoom = ( timeline_gfx_index != ( uint32_t )-1 ) ?
                m_loader.get_opt( TraceLoader::OPT_TimelineZoomGfx ) : false;

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

            // Initialize eventstart / end
            gi.eventstart = std::max( ts_to_eventid( gi.ts0 ), m_start_eventid );
            gi.eventend = std::min( ts_to_eventid( gi.ts1 ), m_end_eventid );

            // Range check our mouse pan values
            m_graph_start_y = Clamp< float >( m_graph_start_y,
                                              visible_graph_height - total_graph_height, 0.0f );

            if ( !gfx_timeline_zoom )
            {
                // Go through and render all the rows
                for ( const row_info_t &ri : row_info )
                {
                    gi.is_timeline = ri.is_timeline;
                    gi.timeline_render_user = m_loader.get_opt( TraceLoader::OPT_TimelineRenderUserSpace );

                    gi.set_pos_y( windowpos.y + ri.row_y + m_graph_start_y, ri.row_h );

                    //$ TODO mikesart: Check if entire row is clipped...
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
        ImGui::EndChild();

        // Render mouse tooltips, mouse selections, etc
        handle_mouse_graph( gi );
    }
}

bool TraceWin::render_graph_popup()
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

    ImGui::Text( "Options" );
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

        if ( ( i >= TraceLoader::OPT_RenderCrtc0 ) &&
             ( i <= TraceLoader::OPT_RenderCrtc9 ) )
        {
            if ( i - TraceLoader::OPT_RenderCrtc0 > m_loader.m_crtc_max )
                continue;
        }

        if ( opt.val_min == 0 && opt.val_max == 1 )
        {
            bool val = opt.val;
            if ( ImGui::MenuItem( opt.desc.c_str(), "", &val ) )
                opt.val = val;
        }
        else
        {
            ImGui::PushItemWidth( imgui_scale( 150.0f ) );
            ImGui::SliderInt( opt.desc.c_str(), &opt.val, opt.val_min, opt.val_max );
            ImGui::PopItemWidth();
        }
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
            ImGui::SetTooltip( "%s (%s ms)", time_buf0.c_str(), time_buf1.c_str() );
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

    const std::vector< uint32_t > *vblank_locs = m_trace_events->get_event_locs( "drm_vblank_event" );
    if ( vblank_locs )
    {
        int64_t prev_vblank_ts = INT64_MAX;
        int64_t next_vblank_ts = INT64_MAX;
        int eventid = ts_to_eventid( mouse_ts );
        size_t idx = vec_find_eventid( *vblank_locs, eventid );
        size_t idxmax = std::min( idx + 20, vblank_locs->size() );

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
            time_buf += "\nPrev vblank: " + ts_to_timestr( prev_vblank_ts, 0, 2 );
        if ( next_vblank_ts != INT64_MAX )
            time_buf += "\nNext vblank: " + ts_to_timestr( next_vblank_ts, 0, 2 );
    }

    if ( m_loader.get_opt( TraceLoader::OPT_SyncEventListToGraph ) &&
         m_show_eventlist &&
         !gi.hovered_items.empty() )
    {
        m_do_gotoevent = true;
        m_goto_eventid = gi.hovered_items[ 0 ].eventid;
    }

    if ( gi.hovered_graph_event != ( uint32_t )-1 )
    {
        const trace_event_t &event_hov = get_event( gi.hovered_graph_event );
        std::string context = get_event_gfxcontext_str( event_hov );
        const std::vector< uint32_t > *plocs = m_trace_events->get_gfxcontext_locs( context.c_str() );

        time_buf += string_format( "\n%s [%s]", event_hov.user_comm, context.c_str() );

        int64_t total_ts = get_event( plocs->back() ).ts - get_event( plocs->front() ).ts;
        time_buf += ": ";
        time_buf += ts_to_timestr( total_ts );

        int64_t ts0 = -1;
        for ( uint32_t id : *plocs )
        {
            const trace_event_t &event = get_event( id );

            time_buf += string_format( "\n  %u %s", event.id, event.name );

            if ( ts0 >= 0 )
            {
                time_buf += ": ";
                time_buf += ts_to_timestr( event.ts - ts0 );
            }
            ts0 = event.ts;
        }
    }

    // Show tooltip with the closest events we could drum up
    for ( graph_info_t::hovered_t &hov : gi.hovered_items )
    {
        trace_event_t &event = get_event( hov.eventid );
        std::string gfxcontext_str = get_event_gfxcontext_str( event );

        time_buf += string_format( "\n%u %c%s %s",
                                   hov.eventid, hov.neg ? '-' : ' ',
                                   ts_to_timestr( hov.dist_ts ).c_str(),
                                   event.name );

        if ( event.crtc >= 0 )
        {
            time_buf += " ";
            time_buf += std::to_string( event.crtc );
        }

        if ( !gfxcontext_str.empty() )
            time_buf += string_format( " [%s] %s", gfxcontext_str.c_str(), event.user_comm );

        if ( !strcmp( event.system, "ftrace-print" ) )
        {
            const event_field_t *field = find_event_field( event.fields, "buf" );

            if ( field )
            {
                time_buf += " ";
                time_buf += field->value;
            }
        }
    }

    ImGui::SetTooltip( "%s", time_buf.c_str() );
}

void TraceWin::handle_mouse_graph( graph_info_t &gi )
{
    // If we've got an active popup menu, render it.
    if ( m_graph_popup )
    {
        m_graph_popup = TraceWin::render_graph_popup();
        return;
    }

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
