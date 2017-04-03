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
#include <unistd.h>

#include <algorithm>
#include <future>
#include <getopt.h>
#include <set>
#include <sys/stat.h>
#include <sys/types.h>
#include <unordered_map>
#include <vector>

#include <SDL2/SDL.h>

#include "GL/gl3w.h"
#include "gpuvis.h"

class event_renderer_t
{
public:
    event_renderer_t( float y_in, float w_in, float h_in );

    void add_event( float x );
    void done();

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
    void init( int64_t start_ts, int64_t length_ts );
    void set_cursor_screen_pos( const ImVec2 &posin, const ImVec2 &size );

    float ts_to_x( int64_t ts );
    float ts_to_screenx( int64_t ts );

    int64_t screenx_to_ts( float x );
    int64_t dx_to_ts( float x );

    bool pt_in_graph( const ImVec2 &posin );
    bool mouse_pos_in_graph();

public:
    uint32_t num;
    ImVec2 pos;

    float h;
    float w;

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
void graph_info_t::init( int64_t start_ts, int64_t length_ts )
{
    num = 0;

    ts0 = start_ts;
    ts1 = start_ts + length_ts;

    tsdx = ts1 - ts0 + 1;
    tsdxrcp = 1.0 / tsdx;

    mouse_pos = ImGui::GetMousePos();

    hovered_items.clear();
}

void graph_info_t::set_cursor_screen_pos( const ImVec2 &posin, const ImVec2 &size )
{
    pos = posin;
    w = size.x;
    h = size.y;

    mouse_over =
        mouse_pos.x >= pos.x &&
        mouse_pos.x <= pos.x + w &&
        mouse_pos.y >= pos.y &&
        mouse_pos.y <= pos.y + h;

    num++;
}

float graph_info_t::ts_to_x( int64_t ts )
{
    return w * ( ts - ts0 ) * tsdxrcp;
}

float graph_info_t::ts_to_screenx( int64_t ts )
{
    return pos.x + ts_to_x( ts );
}

int64_t graph_info_t::screenx_to_ts( float x )
{
    double val = ( x - pos.x ) / w;

    return ts0 + val * tsdx;
}
int64_t graph_info_t::dx_to_ts( float x )
{
    return ( x / w ) * tsdx;
}

bool graph_info_t::pt_in_graph( const ImVec2 &posin )
{
    return ( posin.x >= pos.x && posin.x <= pos.x + w &&
             posin.y >= pos.y && posin.y <= pos.y + h );
}

bool graph_info_t::mouse_pos_in_graph()
{
    return pt_in_graph( mouse_pos );
}

void TraceWin::render_graph_row( const std::string &comm, std::vector< uint32_t > &locs, class graph_info_t *pgi )
{
    graph_info_t &gi = *pgi;

    // Draw background
    ImGui::GetWindowDrawList()->AddRectFilled(
        ImVec2( gi.pos.x, gi.pos.y ),
        ImVec2( gi.pos.x + gi.w, gi.pos.y + gi.h ),
        col_get( col_GraphRowBk ) );

    // Go through all event IDs for this process
    uint32_t num_events = 0;
    bool draw_selected_event = false;
    bool draw_hovered_event = false;
    event_renderer_t event_renderer( gi.pos.y, gi.w, gi.h );

    for ( size_t idx = vec_find_eventid( locs, gi.eventstart );
          idx < locs.size();
          idx++ )
    {
        uint32_t eventid = locs[ idx ];

        if ( eventid > gi.eventend )
            break;

        if ( eventid == m_hovered_eventid )
            draw_hovered_event = true;
        else if ( eventid == m_selected_eventid )
            draw_selected_event = true;

        num_events++;
        trace_event_t &event = m_trace_events->m_events[ eventid ];
        float x = gi.ts_to_screenx( event.ts );

        if ( gi.mouse_over )
        {
            float xdist_mouse = x - gi.mouse_pos.x;
            bool neg = xdist_mouse < 0.0f;

            if ( neg )
                xdist_mouse = -xdist_mouse;

            if ( xdist_mouse < imgui_scale( 8.0f ) )
            {
                bool inserted = false;
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
                    gi.hovered_items.push_back( { neg, dist_ts, event.id } );
                else if ( gi.hovered_items.size() > gi.hovered_max )
                    gi.hovered_items.pop_back();
            }
        }

        event_renderer.add_event( x );
    }
    event_renderer.done();

    if ( draw_hovered_event )
    {
        trace_event_t &event = m_trace_events->m_events[ m_hovered_eventid ];
        float x = gi.ts_to_screenx( event.ts );

        imgui_drawrect( x, imgui_scale( 3.0f ),
                        gi.pos.y, gi.h,
                        col_get( col_HovEvent ) );
    }
    if ( draw_selected_event )
    {
        trace_event_t &event = m_trace_events->m_events[ m_selected_eventid ];
        float x = gi.ts_to_screenx( event.ts );

        imgui_drawrect( x, imgui_scale( 3.0f ),
                        gi.pos.y, gi.h,
                        col_get( col_SelEvent ) );
    }

    std::string label;
    float x = gi.pos.x + ImGui::GetStyle().FramePadding.x;

    label = string_format( "%u) %s", gi.num, comm.c_str() );
    imgui_draw_text( x, gi.pos.y, label.c_str(),
                     col_get( col_RowLabel ) );

    label = string_format( "%u events", num_events );
    imgui_draw_text( x, gi.pos.y + ImGui::GetTextLineHeight(), label.c_str(),
                     col_get( col_RowLabel ) );
}

void TraceWin::render_graph_vblanks( class graph_info_t *pgi )
{
    graph_info_t &gi = *pgi;

    // Draw time ticks every millisecond
    int64_t tsstart = std::max< int64_t >( gi.ts0 / MSECS_PER_SEC - 1, 0 ) * MSECS_PER_SEC;
    float x0 = gi.ts_to_x( tsstart );
    float dx = gi.w * MSECS_PER_SEC * gi.tsdxrcp;

    if ( dx > imgui_scale( 4.0f ) )
    {
        for ( ; x0 <= gi.w; x0 += dx )
        {
            imgui_drawrect( gi.pos.x + x0, imgui_scale( 1.0f ),
                            gi.pos.y, imgui_scale( 16.0f ),
                            col_get( col_TimeTick ) );

            if ( dx >= imgui_scale( 35.0f ) )
            {
                for ( int i = 1; i < 4; i++ )
                {
                    imgui_drawrect( gi.pos.x + x0 + i * dx / 4, imgui_scale( 1.0f ),
                                    gi.pos.y, imgui_scale( 4.0f ),
                                    col_get( col_TimeTick ) );
                }
            }
        }
    }

    // Draw vblank events on every graph.
    const std::vector< uint32_t > &vblank_locs = m_trace_events->get_event_locs( "drm_vblank_event" );

    for ( size_t idx = vec_find_eventid( vblank_locs, gi.eventstart );
          idx < vblank_locs.size();
          idx++ )
    {
        uint32_t id = vblank_locs[ idx ];

        if ( id > gi.eventend )
            break;

        trace_event_t &event = m_trace_events->m_events[ id ];
        if ( ( ( size_t )event.crtc < m_loader.m_render_crtc.size() ) &&
             m_loader.m_render_crtc[ event.crtc ] )
        {
            // drm_vblank_event0: blue, drm_vblank_event1: red
            colors_t col = ( event.crtc > 0 ) ? col_VBlank1 : col_VBlank0;
            float x = gi.ts_to_screenx( event.ts );

            imgui_drawrect( x, imgui_scale( 2.0f ),
                            gi.pos.y, gi.h,
                            col_get( col ) );
        }
    }

    // Draw location line for mouse if mouse is over graph
    if ( m_mouse_over_graph &&
         gi.mouse_pos.x >= gi.pos.x &&
         gi.mouse_pos.x <= gi.pos.x + gi.w )
    {
        imgui_drawrect( gi.mouse_pos.x, imgui_scale( 2.0f ),
                        gi.pos.y, gi.h,
                        col_get( col_MousePos ) );
    }

    // Draw mouse selection location
    if ( m_mouse_captured == 1 )
    {
        float mousex0 = m_mouse_capture_pos.x;
        float mousex1 = gi.mouse_pos.x;

        imgui_drawrect( mousex0, mousex1 - mousex0,
                        gi.pos.y, gi.h,
                        col_get( col_ZoomSel ) );
    }

    if ( m_show_eventlist )
    {
        // Draw rectangle for visible event list contents
        if ( m_eventlist_start_eventid != ( uint32_t )-1 &&
             m_eventlist_end_eventid != ( uint32_t )-1 )
        {
            trace_event_t &event0 = m_trace_events->m_events[ m_eventlist_start_eventid ];
            trace_event_t &event1 = m_trace_events->m_events[ m_eventlist_end_eventid - 1 ];
            float xstart = gi.ts_to_screenx( event0.ts );
            float xend = gi.ts_to_screenx( event1.ts );

            imgui_drawrect( xstart, xend - xstart,
                            gi.pos.y, gi.h,
                            col_get( col_EventListSel ) );
        }
    }
}

void TraceWin::render_process_graphs()
{
    graph_info_t gi;
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

    gi.init( m_graph_start_ts + m_tsoffset, m_graph_length_ts );
    gi.eventstart = std::max( ts_to_eventid( gi.ts0 ), m_start_eventid );
    gi.eventend = std::min( ts_to_eventid( gi.ts1 ), m_end_eventid );

    int graph_row_count = 0;
    for ( const std::string &comm : m_graph_rows )
    {
        std::vector< uint32_t > &locs = m_trace_events->get_comm_locs( comm.c_str() );

        if ( locs.empty() )
        {
            locs = m_trace_events->get_event_locs( comm.c_str() );
            if ( locs.empty() )
                continue;
        }

        graph_row_count++;
    }

    // Get current count of rows. -1 means show all rows.
    int row_count = ( m_loader.m_graph_row_count < 1 ) ?
                graph_row_count : m_loader.m_graph_row_count;
    row_count = std::min< int >( row_count, graph_row_count );

    ImGui::SameLine();
    ImGui::Text( "Rows:" );
    ImGui::SameLine();
    ImGui::PushItemWidth( imgui_scale( 200.0f ) );
    if ( ImGui::SliderInt( "##GraphRowsCount", &row_count, 1, graph_row_count ) )
    {
        m_loader.m_graph_row_count = ( row_count >= graph_row_count ) ?
                    -1 : row_count;
    }

    {
        float graph_row_h = imgui_scale( 50.0f );
        float graph_padding = ImGui::GetStyle().FramePadding.y;
        float graph_row_h_total = graph_row_h + graph_padding;
        float graph_height = row_count * graph_row_h_total;

        graph_height = std::max( graph_height, graph_row_h_total );

        ImGui::BeginChild( "EventGraph", ImVec2( 0, graph_height ), true );

        ImVec2 windowpos = ImGui::GetWindowPos();
        ImVec2 windowsize = ImGui::GetWindowSize();

        m_graph_start_y = std::max( m_graph_start_y,
            ( row_count - graph_row_count ) * graph_row_h_total );
        m_graph_start_y = std::min( m_graph_start_y, 0.0f );

        float posy = windowpos.y + graph_padding + m_graph_start_y;
        float sizey = windowsize.y - 2 * graph_padding;

        // Draw graph background
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2( windowpos.x, posy ),
            ImVec2( windowpos.x + windowsize.x, posy + sizey ),
            col_get( col_GraphBk ) );

        gi.set_cursor_screen_pos( ImVec2( windowpos.x, posy ),
                                  ImVec2( windowsize.x, graph_row_h ) );
        for ( const std::string &comm : m_graph_rows )
        {
            std::vector< uint32_t > &locs = m_trace_events->get_comm_locs( comm.c_str() );

            if ( locs.empty() )
            {
                locs = m_trace_events->get_event_locs( comm.c_str() );
                if ( locs.empty() )
                    continue;
            }

            //$ TODO mikesart: Check if entire row is clipped...
            render_graph_row( comm, locs, &gi );

            gi.set_cursor_screen_pos( ImVec2( gi.pos.x, gi.pos.y + graph_row_h_total ),
                                      ImVec2( gi.w, gi.h ) );
        }

        // Render full graph lines: vblanks, mouse cursors, etc...
        gi.set_cursor_screen_pos( ImVec2( windowpos.x, windowpos.y ),
                                  ImVec2( windowsize.x, windowsize.y ) );
        render_graph_vblanks( &gi );

        ImGui::EndChild();
    }

    render_mouse_graph( &gi );
}

void TraceWin::render_mouse_graph( class graph_info_t *pgi )
{
    graph_info_t &gi = *pgi;

    // If we've got an active popup menu, render it.
    if ( m_graph_popup )
    {
        //$ TODO mikesart: this needs to be render_graph_popup()
        m_graph_popup = TraceWin::render_events_list_popup();
        return;
    }

    m_mouse_over_graph = gi.mouse_pos_in_graph() &&
                         ImGui::IsRootWindowOrAnyChildFocused();

    // If we don't own the mouse and we don't have focus, bail.
    if ( !m_mouse_captured && !m_mouse_over_graph )
        return;

    // Uncapture mouse if user hits escape
    if ( m_mouse_captured &&
         imgui_key_pressed( ImGuiKey_Escape ) )
    {
        m_mouse_captured = 0;
        ImGui::CaptureMouseFromApp( false );
    }

    if ( m_mouse_captured )
    {
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

                // And zoom into the selected area.
                m_graph_location_stack.push_back( { m_graph_start_ts, m_graph_length_ts } );
                if ( m_graph_location_stack.size() > 64 )
                    m_graph_location_stack.erase( m_graph_location_stack.begin() );

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
    else if ( m_mouse_over_graph )
    {
        bool mouse_clicked = ImGui::IsMouseClicked( 0 );
        int64_t event_ts = gi.screenx_to_ts( gi.mouse_pos.x );
        std::string time_buf = "Time: " + ts_to_timestr( event_ts, m_tsoffset );

        const std::vector< uint32_t > &vblank_locs = m_trace_events->get_event_locs( "drm_vblank_event" );
        if ( !vblank_locs.empty() )
        {
            int64_t prev_vblank_ts = INT64_MAX;
            int64_t next_vblank_ts = INT64_MAX;
            int eventid = ts_to_eventid( event_ts );
            size_t idx = vec_find_eventid( vblank_locs, eventid );
            size_t idxmax = std::min( idx + 20, vblank_locs.size() );

            for ( idx = ( idx > 10 ) ? ( idx - 10 ) : 0; idx < idxmax; idx++ )
            {
                trace_event_t &event = m_trace_events->m_events[ vblank_locs[ idx ] ];

                if ( ( ( size_t )event.crtc < m_loader.m_render_crtc.size() ) &&
                        m_loader.m_render_crtc[ event.crtc ] )
                {
                    if ( event.ts < event_ts )
                    {
                        if ( event_ts - event.ts < prev_vblank_ts )
                            prev_vblank_ts = event_ts - event.ts;
                    }
                    if ( event.ts > event_ts )
                    {
                        if ( event.ts - event_ts < next_vblank_ts )
                            next_vblank_ts = event.ts - event_ts;
                    }
                }
            }

            if ( prev_vblank_ts != INT64_MAX )
                time_buf += "\nPrev vblank: " + ts_to_timestr( prev_vblank_ts, 0, 2 );
            if ( next_vblank_ts != INT64_MAX )
                time_buf += "\nNext vblank: " + ts_to_timestr( next_vblank_ts, 0, 2 );
        }

        if ( m_loader.m_sync_eventlist_to_graph &&
             m_show_eventlist &&
             !gi.hovered_items.empty() )
        {
            m_do_gotoevent = true;
            m_goto_eventid = gi.hovered_items[ 0 ].eventid;
        }

        // Show tooltip with the closest events we could drum up
        for ( graph_info_t::hovered_t &hov : gi.hovered_items )
        {
            std::string crtc;
            trace_event_t &event = m_trace_events->m_events[ hov.eventid ];

            if ( event.crtc >= 0 )
                crtc = std::to_string( event.crtc );

            time_buf += string_format( "\n%u %c%s %s%s",
                hov.eventid, hov.neg ? '-' : ' ',
                ts_to_timestr( hov.dist_ts ).c_str(),
                event.name, crtc.c_str() );

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

        if ( mouse_clicked && ImGui::GetIO().KeyShift )
        {
            // shift + click: zoom
            m_mouse_captured = 1;
            ImGui::CaptureMouseFromApp( true );
            m_mouse_capture_pos = gi.mouse_pos;
        }
        else if ( mouse_clicked )
        {
            // click: pan
            m_mouse_captured = 2;
            ImGui::CaptureMouseFromApp( true );
            m_mouse_capture_pos = gi.mouse_pos;
        }
        else if ( ImGui::IsMouseClicked( 1 ) )
        {
            // right click: popup menu
            m_graph_popup = true;
            ImGui::OpenPopup( "EventsListPopup" );
#if 0
            //$ TODO: Add this to right click menu or a button?
            if ( !m_graph_location_stack.empty() )
            {
                // Right click restores previous graph location
                std::pair< int64_t, int64_t > &locs = m_graph_location_stack.back();

                m_graph_start_ts = locs.first;
                m_graph_length_ts = locs.second;

                m_do_graph_start_timestr = true;
                m_do_graph_length_timestr = true;

                m_graph_location_stack.pop_back();
            }
#endif
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
                    m_graph_start_ts = event_ts - len1 * ( event_ts - gi.ts0 ) / len0 - m_tsoffset;
                    m_graph_length_ts = len1;

                    m_do_graph_start_timestr = true;
                    m_do_graph_length_timestr = true;
                }
            }
        }
    }
}
