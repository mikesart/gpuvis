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
#include <unistd.h>

#include <future>
#include <vector>
#include <array>
#include <algorithm>
#include <unordered_map>

#include <SDL2/SDL.h>

#include "GL/gl3w.h"
#include "imgui/imgui.h"
#include "imgui/imgui_impl_sdl_gl3.h"
#include "gpuvis.h"

//$ TODO: Restore window size and position
//$ TODO: Restore column sizes?
//$ TODO: Small font for events?

class TraceEventWin
{
public:
    TraceEventWin() {}
    ~TraceEventWin() {}

public:
    bool render( const char *name, TraceEvents &trace_events );

protected:
    void render_time_delta_button_init( TraceEvents &trace_events );
    void render_time_delta_button( TraceEvents &trace_events );

    bool render_time_goto_button( TraceEvents &trace_events );

public:
    bool m_inited = false;

    bool m_do_gotoevent = false;
    int m_gotoevent = 0;
    int m_eventstart = 0;
    int m_eventend = INT32_MAX;
    bool m_open = false;
    uint32_t m_selected = ( uint32_t )-1;

    char m_timegoto_buf[ 32 ];

    char m_timedelta_buf[ 32 ] = { 0 };
    unsigned long long m_tsdelta = ( unsigned long long )-1;
};

static bool imgui_input_int( int *val, float w, const char *label, const char *label2 )
{
    bool ret = ImGui::Button( label );

    ImGui::SameLine();
    ImGui::PushItemWidth( w );
    ret |= ImGui::InputInt( label2, val, 0, 0 );
    ImGui::PopItemWidth();

    return ret;
}

static bool imgui_key_pressed( ImGuiKey key )
{
    return ImGui::IsKeyPressed( ImGui::GetKeyIndex( key ) );
}

void TraceEventWin::render_time_delta_button_init( TraceEvents &trace_events )
{
    // Default to minimum time stamp.
    unsigned long long ts = trace_events.m_ts_min;

    // Try to grab all the vblank event locations.
    std::vector< uint32_t > *vblank_locs = trace_events.get_event_locs( "drm_vblank_event" );

    if ( vblank_locs )
    {
        std::vector< trace_event_t > &events = trace_events.m_trace_events;

        // Use first vblank, but then try to find first vblank where pid != 0.
        ts = vblank_locs->at( 0 );

        for ( uint32_t i : *vblank_locs )
        {
            if ( events[ i ].pid )
            {
                ts = events[ i ].ts;
                break;
            }
        }
    }

    unsigned long msecs = ts / MSECS_PER_SEC;
    unsigned long nsecs = ts - msecs * MSECS_PER_SEC;

    snprintf( m_timedelta_buf, sizeof( m_timedelta_buf), "%lu.%06lu", msecs, nsecs );
    m_tsdelta = ts;
}

void TraceEventWin::render_time_delta_button( TraceEvents &trace_events )
{
    if ( m_tsdelta == ( unsigned long long )-1 )
        render_time_delta_button_init( trace_events );

    bool time_delta = ImGui::Button( "Time Delta:" );

    ImGui::SameLine();
    ImGui::PushItemWidth( 150 );
    time_delta |= ImGui::InputText( "##TimeDelta", m_timedelta_buf, sizeof( m_timedelta_buf ), 0, 0 );
    ImGui::PopItemWidth();

    if ( time_delta )
    {
        const char *dot = strchr( m_timedelta_buf, '.' );
        unsigned long msecs = strtoull( m_timedelta_buf, NULL, 10 );
        unsigned long nsecs = dot ? strtoul( dot + 1, NULL, 10 ) : 0;

        while ( nsecs && ( nsecs * 10 < MSECS_PER_SEC ) )
            nsecs *= 10;

        m_tsdelta = msecs / MSECS_PER_SEC + nsecs + msecs * MSECS_PER_SEC;
    }
}

bool TraceEventWin::render_time_goto_button( TraceEvents &trace_events )
{
    bool time_goto = ImGui::Button( "Goto Time:" );

    if ( !m_inited )
        snprintf( m_timegoto_buf, sizeof( m_timegoto_buf ), "0.0" );

    ImGui::SameLine();
    ImGui::PushItemWidth( 150 );
    time_goto |= ImGui::InputText( "##TimeGoto", m_timegoto_buf, sizeof( m_timegoto_buf ), 0, 0 );
    ImGui::PopItemWidth();

    if ( time_goto )
    {
        trace_event_t x;
        const char *dot = strchr( m_timegoto_buf, '.' );
        long msecs = strtol( m_timegoto_buf, NULL, 10 );
        unsigned long nsecs = dot ? strtoul( dot + 1, NULL, 10 ) : 0;
        long neg = ( msecs < 0 ) ? -1 : +1;
        std::vector< trace_event_t > &events = trace_events.m_trace_events;

        while ( nsecs && ( nsecs * 10 < MSECS_PER_SEC ) )
            nsecs *= 10;
        if ( neg < 0 )
            msecs = -msecs;

        long delta = ( msecs / MSECS_PER_SEC + nsecs + msecs * MSECS_PER_SEC );

        x.ts = m_tsdelta + neg * delta;

        auto eventidx = std::lower_bound( events.begin(), events.end(), x,
                          [](const trace_event_t& f1, const trace_event_t& f2) { return f1.ts < f2.ts; } );

        m_gotoevent = eventidx - events.begin();
        if ( ( neg < 0 ) && ( m_gotoevent > 0 ) )
            m_gotoevent--;
    }

    return time_goto;
}

static void draw_minimap_marker( float x, float y, float width, float height, ImU32 color,
                                 float rounding = 0.0f )
{
    ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2( x, y ),
                ImVec2( x + width, y + height ),
                color, rounding, 0xf );
}

bool TraceEventWin::render( const char *name, TraceEvents &trace_events )
{
    std::vector< trace_event_t > &events = trace_events.m_trace_events;
    size_t event_count = events.size();

    ImGuiWindowFlags winflags = ImGuiWindowFlags_MenuBar;
    ImGui::SetNextWindowSize( ImVec2( 0, 0 ), ImGuiSetCond_FirstUseEver );
    ImGui::Begin( name, &m_open, winflags );

    if ( m_selected != ( uint32_t )-1 )
        ImGui::Text( "Selected: %u\n", m_selected );
    ImGui::Text( "Events: %lu\n", event_count );

    imgui_input_int( &m_eventstart, 75.0f, "Event Start:", "##EventStart" );
    ImGui::SameLine();
    imgui_input_int( &m_eventend, 75.0f, "Event End:", "##EventEnd" );
    ImGui::SameLine();
    render_time_delta_button( trace_events );

    m_do_gotoevent |= imgui_input_int( &m_gotoevent, 75.0f, "Goto Event:", "##GotoEvent" );
    ImGui::SameLine();
    m_do_gotoevent |= render_time_goto_button( trace_events );

    m_gotoevent = std::min< uint32_t >( m_gotoevent, event_count - 1 );
    m_eventstart = std::min< uint32_t >( m_eventstart, event_count - 1 );
    m_eventend = std::min< uint32_t >( std::max< uint32_t >( m_eventend, m_eventstart ), event_count - 1 );

    event_count = m_eventend - m_eventstart + 1;

    // Set focus on event list first time we open.
    if ( !m_inited && ImGui::IsWindowFocused() )
        ImGui::SetNextWindowFocus();

    // Events list
    ImVec2 avail = ImGui::GetContentRegionAvail();
    const float map_width = 16.0f;
    {
        // Set the child window size to hold count of items + header + separator
        float lineh = ImGui::GetTextLineHeightWithSpacing();
        ImGui::SetNextWindowContentSize( { 0.0f, ( event_count + 1 ) * lineh + 1 } );
        ImGui::BeginChild( "eventlistbox", ImVec2( avail.x - ( map_width + 6.0f ), 0.0f ) );

        float winh = ImGui::GetWindowHeight();

        if ( ImGui::IsWindowFocused() )
        {
            int scroll_lines = 0;

            if ( imgui_key_pressed( ImGuiKey_PageDown ) )
                scroll_lines = ( winh / lineh - 5 );
            else if ( imgui_key_pressed( ImGuiKey_PageUp ) )
                scroll_lines = -( winh / lineh - 5 );
            else if ( imgui_key_pressed( ImGuiKey_DownArrow ) )
                scroll_lines = 1;
            else if ( imgui_key_pressed( ImGuiKey_UpArrow ) )
                scroll_lines = -1;
            else if ( imgui_key_pressed( ImGuiKey_Home ) )
                scroll_lines = -event_count;
            else if ( imgui_key_pressed( ImGuiKey_End ) )
                scroll_lines = event_count;

            if ( scroll_lines )
                ImGui::SetScrollY( ImGui::GetScrollY() + scroll_lines * lineh );
        }

        if ( m_do_gotoevent )
        {
            ImGui::SetScrollY( std::max< int >( 0, m_gotoevent - m_eventstart ) * lineh );
            m_do_gotoevent = false;
        }

        float scrolly = ImGui::GetScrollY();
        uint32_t start_idx = ( scrolly >= lineh ) ? ( uint32_t )( scrolly / lineh - 1 ) : 0;
        uint32_t rows = ( winh + 1 ) / lineh;
        uint32_t end_idx = std::min< uint32_t >( start_idx + 2 + rows, event_count );

        // Draw columns
        std::array< const char *, 5 > columns = { "Id", "CPU", "Time Stamp", "Task", "Event" };
        ImGui::Columns( columns.size(), "events" );
        for ( const char *str : columns )
        {
            ImGui::TextColored( ImVec4( 1, 1, 0, 1 ), "%s", str );
            ImGui::NextColumn();
        }
        ImGui::Separator();

        if ( start_idx > 0 )
        {
            // Move cursor position down to where we've scrolled.
            ImGui::SetCursorPosY( ImGui::GetCursorPosY() + lineh * ( start_idx - 1 ) );

            // Scoot to next row (fixes top row occasionally drawing half).
            for ( size_t i = 0; i < columns.size(); i++ )
                ImGui::NextColumn();
        }

        // Draw events
        for ( uint32_t i = start_idx; i < end_idx; i++ )
        {
            char label[ 32 ];
            int colors_pushed = 0;
            trace_event_t &event = events[ m_eventstart + i ];
            bool selected = ( m_selected == i );
            bool ts_negative = ( m_tsdelta > event.ts );
            unsigned long long ts = ts_negative ? ( m_tsdelta - event.ts ) : ( event.ts - m_tsdelta );
            unsigned long msecs = ts / MSECS_PER_SEC;
            unsigned long nsecs = ts - msecs * MSECS_PER_SEC;
            bool is_vblank = !strcmp( event.name, "drm_vblank_event" );

            if ( is_vblank && !selected )
            {
                // If this is a vblank and it's not selected, draw a blue background by
                //  pretending this row is selected.
                ImGui::PushStyleColor( ImGuiCol_Header, ImVec4( 0.0f, 0.0f, 1.0f, 1.0f ) );
                selected = true;
                colors_pushed++;
            }

            snprintf( label, sizeof( label ), "%u", event.id );
            if ( ImGui::Selectable( label, selected, ImGuiSelectableFlags_SpanAllColumns ) )
                m_selected = i;
            ImGui::NextColumn();

            ImGui::Text( "%u", event.cpu ); ImGui::NextColumn();
            ImGui::Text( "%s%lu.%06lu", ts_negative ? "-" : "", msecs, nsecs ); ImGui::NextColumn();
            ImGui::Text( "%s", event.comm ); ImGui::NextColumn();
            ImGui::Text( "%s", event.name ); ImGui::NextColumn();

            ImGui::PopStyleColor( colors_pushed );
        }

        ImGui::Columns( 1 );
        ImGui::EndChild();

        // Draw a zoomed minimap off to the right to help locate events
        {
            ImGui::SameLine();

            const float fw = 3.0f;
            ImVec2 pos = ImGui::GetCursorScreenPos();
            uint32_t event0 = std::max< int >( ( int )start_idx + m_eventstart - rows * 50, m_eventstart );
            uint32_t event1 = std::min< uint32_t >( event0 + rows * 100, m_eventend - 1 );
            float event_height = ( event1 - event0 );
            static const ImU32 col_vblank = IM_COL32( 0, 0, 255, 255 );
            static const ImU32 col_viewable = IM_COL32( 128, 128, 128, 128 );
            static const ImU32 col_background = IM_COL32( 255, 255, 255, 50 );
            static const ImU32 col_selected = ImGui::GetColorU32( ImGuiCol_TextSelectedBg );

            // Draw background
            draw_minimap_marker( pos.x, pos.y, map_width, avail.y, col_background );

            // Draw blue marker lines for vblanks
            std::vector< uint32_t > *vblank_locs = trace_events.get_event_locs( "drm_vblank_event" );
            if ( vblank_locs )
            {
                for ( uint32_t id : *vblank_locs )
                {
                    if ( id > event1 )
                        break;
                    if ( id >= event0 )
                    {
                        float pos0 = ( id - event0 ) / event_height;

                        draw_minimap_marker( pos.x + fw, pos.y + avail.y * pos0,
                                             map_width - 2 * fw, 3, col_vblank );
                    }
                }
            }

            // Draw marker for selected event
            if ( m_selected + m_eventstart >= event0 && m_selected + m_eventstart <= event1 )
            {
                float pos0 = ( m_selected + m_eventstart - event0 ) / event_height;

                draw_minimap_marker( pos.x + fw, pos.y + avail.y * pos0,
                                     map_width - 2 * fw, 3, col_selected );
            }

            // Draw rectangle ~ showing visible event location.
            float pos0 = ( start_idx + m_eventstart - event0 ) / event_height;
            float pos1 = ( end_idx + m_eventstart - event0 ) / event_height;

            draw_minimap_marker( pos.x + fw - 1, pos.y + avail.y * pos0,
                                 map_width - 2 * ( fw - 1 ), avail.y * ( pos1 - pos0 ),
                                 col_viewable, 3.0f );

            if ( ImGui::IsMouseClicked( 0 ) && ImGui::IsMouseHoveringWindow() )
            {
                ImVec2 mouse_pos = ImGui::GetMousePos();
                float clickpos = ( mouse_pos.y - pos.y ) / avail.y;
                uint32_t event = event0 + ( event1 - event0 ) * clickpos;

                if ( ( event >= event0 ) && ( event <= event1 ) )
                {
                    m_do_gotoevent = true;
                    m_gotoevent = event;
                }
            }
        }
    }

    ImGui::End();

    m_inited = true;
    return m_open;
}

static int event_cb( TraceEvents *trace_events, const trace_info_t &info,
                     const trace_event_t &event )
{
    size_t id = trace_events->m_trace_events.size();

    if ( trace_events->m_cpucount.empty() )
    {
        trace_events->m_trace_info = info;
        trace_events->m_cpucount.resize( info.cpus, 0 );
    }

    if ( event.ts < trace_events->m_ts_min )
        trace_events->m_ts_min = event.ts;
    if ( event.ts > trace_events->m_ts_max )
        trace_events->m_ts_max = event.ts;

    if ( event.cpu < trace_events->m_cpucount.size() )
        trace_events->m_cpucount[ event.cpu ]++;

    trace_events->m_trace_events.push_back( event );
    trace_events->m_trace_events[ id ].id = id;

    if ( !( id % 100 ) && isatty( 1 ) )
        printf( "\033[1000D  Reading event: %lu", id );

    trace_events->m_event_locations.add_location( event.name, id );
    trace_events->m_comm_locations.add_location( event.comm, id );

#if 0
    //$ TODO mikesart: debug test code
    if ( id > 1000 )
        return 1;
#endif
    return 0;
}

int main( int argc, char **argv )
{
    TraceEvents trace_events;
    SDL_DisplayMode current;
    SDL_Window *window = NULL;
    SDL_GLContext glcontext = NULL;
    TraceEventWin eventwin0;
    TraceEventWin eventwin1;

    const char *file = ( argc > 1 ) ? "trace.dat" : argv[ 1 ];

    printf( "Reading trace file %s...\n", file );

    EventCallback cb = std::bind( event_cb, &trace_events, _1, _2 );
    if ( read_trace_file( argv[ 1 ], trace_events.m_strpool, cb ) < 0 )
    {
        fprintf( stderr, "\nERROR: read_trace_file(%s) failed.\n", file );
        exit( -1 );
    }
    printf( "\n" );
    printf( "Events read: %lu\n", trace_events.m_trace_events.size() );

    // Setup SDL
    if ( SDL_Init( SDL_INIT_VIDEO | SDL_INIT_TIMER ) != 0 )
    {
        printf( "Error: %s\n", SDL_GetError() );
        return -1;
    }

    // Setup window
    SDL_GL_SetAttribute( SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG );
    SDL_GL_SetAttribute( SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE );
    SDL_GL_SetAttribute( SDL_GL_DOUBLEBUFFER, 1 );
    SDL_GL_SetAttribute( SDL_GL_DEPTH_SIZE, 24 );
    SDL_GL_SetAttribute( SDL_GL_STENCIL_SIZE, 8 );
    SDL_GL_SetAttribute( SDL_GL_CONTEXT_MAJOR_VERSION, 3 );
    SDL_GL_SetAttribute( SDL_GL_CONTEXT_MINOR_VERSION, 2 );

    SDL_GetCurrentDisplayMode( 0, &current );

    window = SDL_CreateWindow( "GPUVis", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               1280, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE );
    glcontext = SDL_GL_CreateContext( window );

    gl3wInit();

    // Setup ImGui binding
    ImGui_ImplSdlGL3_Init( window );

    bool done = false;
    bool show_test_window = false;
    bool show_another_window = false;
    ImVec4 clear_color = ImColor( 114, 144, 154 );

    // Main loop
    while ( !done )
    {
        SDL_Event event;

        while ( SDL_PollEvent( &event ) )
        {
            ImGui_ImplSdlGL3_ProcessEvent( &event );
            if ( event.type == SDL_QUIT )
                done = true;
        }
        ImGui_ImplSdlGL3_NewFrame( window );

        // 1. Show a simple window
        // Tip: if we don't call ImGui::Begin()/ImGui::End() the widgets appears in a window automatically called "Debug"
        {
            static float f = 0.0f;

            ImGui::Text( "Hello, world!" );
            ImGui::SliderFloat( "float", &f, 0.0f, 1.0f );
            ImGui::ColorEdit3( "clear color", ( float * )&clear_color );

            if ( ImGui::Button( "Test Window" ) )
                show_test_window ^= 1;
            if ( ImGui::Button( "Another Window" ) )
                show_another_window ^= 1;

            ImGui::Text( "Application average %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate );
        }

        // 2. Show another simple window, this time using an explicit Begin/End pair
        if ( show_another_window )
        {
            ImGui::SetNextWindowSize( ImVec2( 200, 100 ), ImGuiSetCond_FirstUseEver );
            ImGui::Begin( "Another Window", &show_another_window );
            ImGui::Text( "Hello" );
            ImGui::End();
        }

        // 3. Show the ImGui test window. Most of the sample code is in ImGui::ShowTestWindow()
        if ( show_test_window )
        {
            ImGui::SetNextWindowPos( ImVec2( 650, 20 ), ImGuiSetCond_FirstUseEver );
            ImGui::ShowTestWindow( &show_test_window );
        }

        // Render events for our loaded trace file.
        eventwin0.render( "blah0", trace_events );
        eventwin1.render( "blah1", trace_events );

        // Rendering
        glViewport( 0, 0, ( int )ImGui::GetIO().DisplaySize.x, ( int )ImGui::GetIO().DisplaySize.y );
        glClearColor( clear_color.x, clear_color.y, clear_color.z, clear_color.w );
        glClear( GL_COLOR_BUFFER_BIT );

        ImGui::Render();

        SDL_GL_SwapWindow( window );
    }

    // Cleanup
    ImGui_ImplSdlGL3_Shutdown();
    SDL_GL_DeleteContext( glcontext );
    SDL_DestroyWindow( window );
    SDL_Quit();

    return 0;
}
