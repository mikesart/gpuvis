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
#include <unordered_map>
#include <vector>
#include <array>

#include <SDL2/SDL.h>

#include "GL/gl3w.h"
#include "imgui/imgui.h"
#include "imgui/imgui_impl_sdl_gl3.h"
#include "gpuvis.h"

class TraceEventWin
{
public:
    TraceEventWin() {}
    ~TraceEventWin() {}

public:
    bool render( const char *name, TraceEvents &trace_events );

public:
    int m_gotoline = 0;
    int m_eventstart = 0;
    int m_eventend = INT32_MAX;
    bool m_open = false;
    uint32_t m_selected = ( uint32_t )-1;
};

bool TraceEventWin::render( const char *name, TraceEvents &trace_events )
{
    std::vector< trace_event_t > &events = trace_events.m_trace_events;
    size_t event_count = events.size();

    ImGuiWindowFlags winflags = ImGuiWindowFlags_MenuBar;
    ImGui::SetNextWindowSize( ImVec2( 0, 0 ), ImGuiSetCond_FirstUseEver );
    ImGui::Begin( name, &m_open, winflags );

    ImGui::Text( "Hello" );

    if ( m_selected != ( uint32_t )-1 )
        ImGui::Text( "Selected: %u\n", m_selected );
    ImGui::Text( "Events: %lu\n", event_count );

    bool goto_line = ImGui::Button( "Goto Event: " );
    ImGui::SameLine();
    ImGui::PushItemWidth( 75 );
    goto_line |= ImGui::InputInt( "##GotoLine", &m_gotoline, 0, 0 );
    ImGui::PopItemWidth();

    ImGui::SameLine();
    bool event_start = ImGui::Button( "Event Start:" );
    ImGui::SameLine();
    ImGui::PushItemWidth( 75 );
    event_start |= ImGui::InputInt( "##EventStart", &m_eventstart, 0, 0 );
    ImGui::PopItemWidth();

    ImGui::SameLine();
    bool event_end = ImGui::Button( "Event End:" );
    ImGui::SameLine();
    ImGui::PushItemWidth( 75 );
    event_end |= ImGui::InputInt( "##EventEnd", &m_eventend, 0, 0 );
    ImGui::PopItemWidth();

    m_gotoline = std::min< uint32_t >( m_gotoline, event_count - 1 );
    m_eventstart = std::min< uint32_t >( m_eventstart, event_count - 1 );
    m_eventend = std::min< uint32_t >( std::max< uint32_t >( m_eventend, m_eventstart ), event_count - 1 );

    event_count = m_eventend - m_eventstart + 1;

    // Events list
    {
        float lineh = ImGui::GetTextLineHeightWithSpacing();

        // Set the child window size to hold count of items + header + separator
        ImGui::SetNextWindowContentSize( { 0.0f, ( event_count + 1 ) * lineh + 1 } );
        ImGui::BeginChild( "eventlistbox" );

        if ( goto_line )
        {
            m_gotoline = std::min< int >( std::max< int >( 0, m_gotoline ), event_count );
            ImGui::SetScrollY( m_gotoline * lineh);
        }

        float winh = ImGui::GetWindowHeight();
        float scrolly = ImGui::GetScrollY();
        uint32_t start_idx = ( scrolly >= lineh ) ? ( uint32_t )( scrolly / lineh - 1 ) : 0;
        uint32_t end_idx = std::min< uint32_t >( start_idx + 2 + ( winh + 1 ) / lineh, event_count );

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
            unsigned long secs = event.ts / NSECS_PER_SEC;
            unsigned long nsecs = event.ts - secs * NSECS_PER_SEC;
            bool is_vblank = !strcmp( event.name, "drm_handle_vblank" );

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

            ImGui::Text( "%u", event.cpu );
            ImGui::NextColumn();

            ImGui::Text( "%5lu.%9lu", secs, nsecs );
            ImGui::NextColumn();

            ImGui::Text( "%s", event.comm );
            ImGui::NextColumn();

            ImGui::Text( "%s", event.name );
            ImGui::NextColumn();

            ImGui::PopStyleColor( colors_pushed );
        }

        ImGui::Columns( 1 );
        ImGui::EndChild();
    }

    ImGui::End();

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
