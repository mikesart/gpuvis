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

#include <SDL2/SDL.h>

#include "GL/gl3w.h"
#include "gpuvis.h"
#include "imgui/imgui.h"
#include "imgui/imgui_impl_sdl_gl3.h"

class TraceEventWin
{
public:
    TraceEventWin() {}
    ~TraceEventWin() {}

public:
    bool render( TraceEvents &trace_events );

public:
    bool m_open = false;
    uint32_t m_selected = ( uint32_t )-1;
};

bool TraceEventWin::render( TraceEvents &trace_events )
{
    ImGuiWindowFlags winflags = ImGuiWindowFlags_MenuBar;
    std::vector< trace_event_t > &events = trace_events.m_trace_events;
    size_t event_count = events.size();

    ImGui::SetNextWindowSize( ImVec2( 0, 0 ), ImGuiSetCond_FirstUseEver );
    ImGui::Begin( trace_events.m_trace_info.file.c_str(), &m_open, winflags );

    ImGui::Text( "Hello" );

    if ( m_selected != ( uint32_t )-1 )
        ImGui::Text( "Selected: %u\n", m_selected );
    ImGui::Text( "Events: %lu\n", event_count );

    // Events list
    {
        float lineh = ImGui::GetTextLineHeightWithSpacing();

        // Set the child window size to hold count of items + header + separator
        ImGui::SetNextWindowContentSize( { 0.0f, ( event_count + 1 ) * lineh + 1 } );
        ImGui::BeginChild( "eventlistbox" );

        float winh = ImGui::GetWindowHeight();
        float scrolly = ImGui::GetScrollY();
        uint32_t start_idx = ( scrolly >= lineh ) ? ( uint32_t )( scrolly / lineh - 1 ) : 0;
        uint32_t end_idx = std::min< uint32_t >( start_idx + 1 + ( winh + 1 ) / lineh, event_count );

        // Draw columns
        ImGui::Columns( 6, "events" );
        ImGui::Text( "#" ); ImGui::NextColumn();
        ImGui::Text( "CPU" ); ImGui::NextColumn();
        ImGui::Text( "Time Stamp" ); ImGui::NextColumn();
        ImGui::Text( "Task" ); ImGui::NextColumn();
        ImGui::Text( "PID" ); ImGui::NextColumn();
        ImGui::Text( "Event" ); ImGui::NextColumn();
        ImGui::Separator();

        if ( start_idx > 0 )
        {
            // Move cursor position down to where we've scrolled.
            ImGui::SetCursorPosY( ImGui::GetCursorPosY() + lineh * ( start_idx - 1 ) );

            // Scoot to next row (fixes top row occasionally drawing half).
            for ( int i = 0; i < 6; i++ )
                ImGui::NextColumn();
        }

        // Draw events
        for ( uint32_t i = start_idx; i < end_idx; i++ )
        {
            char label[ 32 ];
            unsigned long secs = events[ i ].ts / NSECS_PER_SEC;
            unsigned long nsecs = events[ i ].ts - secs * NSECS_PER_SEC;

            snprintf( label, sizeof( label ), "%u", events[ i ].id );
            if ( ImGui::Selectable( label, m_selected == i, ImGuiSelectableFlags_SpanAllColumns ) )
                m_selected = i;
            ImGui::NextColumn();

            ImGui::Text( "%u", events[ i ].cpu );
            ImGui::NextColumn();

            ImGui::Text( "%5lu.%9lu", secs, nsecs );
            ImGui::NextColumn();

            ImGui::Text( "%s", events[ i ].comm );
            ImGui::NextColumn();

            ImGui::Text( "%u", events[ i ].pid );
            ImGui::NextColumn();

            ImGui::Text( "%s", events[ i ].name );
            ImGui::NextColumn();
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
    trace_events->m_trace_events.back().id = trace_events->m_trace_events.size() - 1;

    size_t count = trace_events->m_trace_events.size();
    if ( !( count % 100 ) && isatty( 1 ) )
        printf( "\033[1000D  Reading event: %lu", count );
    return 1;
}

int main( int argc, char **argv )
{
    TraceEvents trace_events;
    SDL_DisplayMode current;
    SDL_Window *window = NULL;
    SDL_GLContext glcontext = NULL;
    TraceEventWin eventwin;

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
        eventwin.render( trace_events );

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
