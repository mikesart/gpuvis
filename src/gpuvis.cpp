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

//$ TODO: Right click on events - popup menu
//    start graph at a specific location
//    find event in graph

//$ TODO: Add ability to show row for an event with a parameter?

//$ TODO: Need to handle lots of graph rows, ie ~100
//$ TODO: Figure out crash when you have too many graph rows and zoom out

//$ TODO: popup graph tooltip shows events around location you're at?

static bool imgui_input_int( int *val, float w, const char *label, const char *label2, ImGuiInputTextFlags flags = 0 )
{
    bool ret = ImGui::Button( label );

    ImGui::SameLine();
    ImGui::PushItemWidth( imgui_scale( w ) );
    ret |= ImGui::InputInt( label2, val, 0, 0, flags );
    ImGui::PopItemWidth();

    return ret;
}

static bool imgui_input_text( const char *button_label, const char *text_label,
                              std::string &str, size_t capacity, float w )
{
    bool ret = ImGui::Button( button_label );

    ImGui::SameLine();
    ImGui::PushItemWidth( w );

    str.reserve( capacity );
    ret |= ImGui::InputText( text_label, &str[ 0 ], str.capacity(), 0, 0 );

    ImGui::PopItemWidth();

    return ret;
}

static int imgui_ini_save_settings_cb( CIniFile *inifile, int index, const ImGuiIniData &data )
{
    std::string section = "imguiwin_";

    section += data.Name;

    inifile->PutStr( "name", data.Name, section.c_str() );
    inifile->PutVec2( "pos", data.Pos, section.c_str() );
    inifile->PutVec2( "size", data.Size, section.c_str() );
    inifile->PutInt( "collapsed", data.Collapsed, section.c_str() );

    inifile->PutStr( std::to_string( index ).c_str(), section.c_str(), "$imguiwindows$" );
    inifile->PutStr( std::to_string( index + 1 ).c_str(), "", "$imguiwindows$" );
    return 0;
}

static int imgui_ini_load_settings_cb( CIniFile *inifile, int index, ImGuiIniData &data )
{
    std::string section = inifile->GetStr( std::to_string( index ).c_str(), "", "$imguiwindows$" );

    if ( !section.empty() )
    {
        std::string name = inifile->GetStr( "name", "", section.c_str() );

        if ( !name.empty() )
        {
            data.Pos = inifile->GetVec2( "pos", ImVec2( 0, 0 ), section.c_str() );
            data.Size = inifile->GetVec2( "size", ImVec2( 0, 0 ), section.c_str() );
            data.Collapsed = inifile->GetInt( "collapsed", 0, section.c_str() );
            data.Name = strdup( name.c_str() );
        }
        return 0;
    }

    return -1;
}

template < typename T >
void imgui_headers( const char *title, const T &headers )
{
    ImGui::Columns( headers.size(), "events" );

    for ( const char *str : headers )
    {
        ImGui::TextColored( ImVec4( 1, 1, 0, 1 ), "%s", str );
        ImGui::NextColumn();
    }
    ImGui::Separator();
}

/*
 * StrPool
 */
const char *StrPool::getstr( const char *str )
{
    uint32_t hashval = fnv_hashstr32( str );

    auto i = m_pool.find( hashval );
    if ( i == m_pool.end() )
        m_pool[ hashval ] = std::string( str );

    return m_pool[ hashval ].c_str();
}

const char *StrPool::getstr( uint32_t hashval )
{
    auto i = m_pool.find( hashval );

    if ( i == m_pool.end() )
        return NULL;
    return m_pool[ hashval ].c_str();
}

/*
 * TraceLoader
 */
TraceLoader::state_t TraceLoader::get_state()
{
    return ( state_t )SDL_AtomicGet( &m_state );
}

bool TraceLoader::is_loading()
{
    return ( get_state() == State_Loading || get_state() == State_CancelLoading );
}

void TraceLoader::set_state( state_t state )
{
    m_filename = "";
    m_trace_events = NULL;
    m_thread = NULL;

    SDL_AtomicSet( &m_state, state );
}

void TraceLoader::cancel_load_file()
{
    // Switch to cancel loading if we're currently loading
    SDL_AtomicCAS( &m_state, State_Loading, State_CancelLoading );
}

bool TraceLoader::load_file( const char *filename )
{
    if ( is_loading() )
    {
        logf( "[Error] %s failed, currently loading %s.", __func__, m_filename.c_str() );
        return false;
    }

    if ( access( filename, R_OK ) == -1 )
    {
        logf( "[Error] %s (%s) failed: %s", __func__, filename, strerror( errno ) );
        return false;
    }

    size_t filesize = get_file_size( filename );
    std::string title = string_format( "%s (%.2f MB)", filename, filesize / ( 1024.0f * 1024.0f ) );

    // Check if we've already loaded this trace file.
    for ( TraceEvents *events : m_trace_events_list )
    {
        if ( events->m_title == title )
        {
            new_event_window( events );
            return true;
        }
    }

    set_state( State_Loading );
    m_filename = filename;

    m_trace_events = new TraceEvents;
    m_trace_events->m_filename = filename;
    m_trace_events->m_filesize = filesize;
    m_trace_events->m_title = title;

    SDL_AtomicSet( &m_trace_events->m_eventsloaded, 1 );

    m_thread = SDL_CreateThread( thread_func, "eventloader", ( void * )this );
    if ( m_thread )
    {
        new_event_window( m_trace_events );
        m_trace_events_list.push_back( m_trace_events );
        return true;
    }

    logf( "[Error] %s: SDL_CreateThread failed.", __func__ );

    delete m_trace_events;
    m_trace_events = NULL;

    set_state( State_Idle );
    return false;
}

void TraceLoader::new_event_window( TraceEvents *trace_events )
{
    size_t refcount = 0;
    std::string title = trace_events->m_title;

    for ( int i = m_trace_windows_list.size() - 1; i >= 0; i-- )
    {
        if ( m_trace_windows_list[ i ]->m_trace_events == trace_events )
            refcount++;
    }

    if ( refcount )
        title += string_format( " #%lu", refcount + 1 );

    TraceWin *win = new TraceWin( *this, trace_events, title );

    m_trace_windows_list.push_back( win );
    win->m_setfocus = 2;
}

void TraceLoader::close_event_file( TraceEvents *trace_events, bool close_file )
{
    for ( int i = m_trace_windows_list.size() - 1; i >= 0; i-- )
    {
        TraceWin *win = m_trace_windows_list[ i ];

        if ( win->m_open && ( win->m_trace_events == trace_events ) )
            win->m_open = false;
    }

    if ( close_file )
    {
        for ( size_t i = 0; i < m_trace_events_list.size(); i++ )
        {
            if ( m_trace_events_list[ i ] == trace_events )
            {
                delete trace_events;
                m_trace_events_list.erase( m_trace_events_list.begin() + i );
                break;
            }
        }
    }
}

int TraceLoader::new_event_cb( TraceLoader *loader, const trace_info_t &info,
                               const trace_event_t &event )
{
    TraceEvents *trace_events = loader->m_trace_events;
    size_t id = trace_events->m_events.size();

    if ( trace_events->m_cpucount.empty() )
    {
        trace_events->m_trace_info = info;
        trace_events->m_cpucount.resize( info.cpus, 0 );
    }

    if ( event.cpu < trace_events->m_cpucount.size() )
        trace_events->m_cpucount[ event.cpu ]++;

    loader->m_crtc_max = std::max( loader->m_crtc_max, event.crtc );

    if ( id == 0 )
        trace_events->m_ts_min = event.ts;

    trace_events->m_events.push_back( event );
    trace_events->m_events[ id ].id = id;
    trace_events->m_events[ id ].ts -= trace_events->m_ts_min;

    trace_events->m_event_locations.add_location( event.name, id );
    trace_events->m_comm_locations.add_location( event.comm, id );

    SDL_AtomicAdd( &trace_events->m_eventsloaded, 1 );

    if ( loader->get_state() == State_CancelLoading )
        return 1;

    return 0;
}

int SDLCALL TraceLoader::thread_func( void *data )
{
    TraceLoader *loader = ( TraceLoader * )data;
    TraceEvents *trace_events = loader->m_trace_events;
    const char *filename = loader->m_filename.c_str();

    logf( "Reading trace file %s...", filename );

    EventCallback trace_cb = std::bind( new_event_cb, loader, _1, _2 );
    if ( read_trace_file( filename, trace_events->m_strpool, trace_cb ) < 0 )
    {
        logf( "[Error]: read_trace_file(%s) failed.", filename );

        SDL_AtomicSet( &trace_events->m_eventsloaded, -1 );
        loader->set_state( State_Idle );
        return -1;
    }

    logf( "Events read: %lu", trace_events->m_events.size() );

    SDL_AtomicSet( &trace_events->m_eventsloaded, 0 );
    loader->set_state( State_Loaded );
    return 0;
}

void TraceLoader::init()
{
    m_fullscreen = m_inifile.GetInt( "fullscreen", 0 );
    m_show_events_list = m_inifile.GetInt( "show_events_list", 0 );
    m_graph_row_count = m_inifile.GetInt( "graph_row_count", 0 );
    m_show_color_picker = m_inifile.GetInt( "show_color_picker", 0 );
    m_sync_eventlist_to_graph = m_inifile.GetInt( "sync_eventlist_to_graph", 0 );
    m_eventlist_row_count = m_inifile.GetInt( "eventlist_row_count", 0 );

    for ( size_t i = 0; i < m_render_crtc.size(); i++ )
    {
        std::string key = string_format( "render_crtc%lu", i );

        m_render_crtc[ i ] = m_inifile.GetInt( key.c_str(), 1 );
    }
}

void TraceLoader::shutdown()
{
    if ( m_thread )
    {
        // Cancel any file loading going on.
        cancel_load_file();

        // Wait for our thread to die.
        SDL_WaitThread( m_thread, NULL );
        m_thread = NULL;
    }

    set_state( State_Idle );

    for ( TraceWin *win : m_trace_windows_list )
        delete win;
    m_trace_windows_list.clear();

    for ( TraceEvents *events : m_trace_events_list )
        delete events;
    m_trace_events_list.clear();

    m_inifile.PutInt( "fullscreen", m_fullscreen );
    m_inifile.PutInt( "show_events_list", m_show_events_list );
    m_inifile.PutInt( "graph_row_count", m_graph_row_count );
    m_inifile.PutInt( "show_color_picker", m_show_color_picker );
    m_inifile.PutInt( "sync_eventlist_to_graph", m_sync_eventlist_to_graph );
    m_inifile.PutInt( "eventlist_row_count", m_eventlist_row_count );

    for ( size_t i = 0; i < m_render_crtc.size(); i++ )
    {
        std::string key = string_format( "render_crtc%lu", i );

        m_inifile.PutInt( key.c_str(), m_render_crtc[ i ] );
    }
}

void TraceLoader::render()
{
    if ( m_fullscreen && !m_trace_windows_list.empty() )
    {
        ImGuiIO &io = ImGui::GetIO();
        float w = io.DisplaySize.x;
        float h = io.DisplaySize.y;

        ImGui::SetNextWindowPosCenter();
        ImGui::SetNextWindowSizeConstraints( ImVec2( w, h ), ImVec2( w, h ) );
    }

    for ( int i = m_trace_windows_list.size() - 1; i >= 0; i-- )
    {
        TraceWin *win = m_trace_windows_list[ i ];

        if ( win->m_open )
            win->render();
        if ( !win->m_open )
        {
            delete win;
            m_trace_windows_list.erase( m_trace_windows_list.begin() + i );
        }
    }
}

/*
 * TraceWin
 */
void TraceWin::render_time_offset_button_init( TraceEvents &trace_events )
{
    std::vector< trace_event_t > &events = trace_events.m_events;
    const std::vector< uint32_t > &vblank_locs = trace_events.get_event_locs( "drm_vblank_event" );

    if ( !vblank_locs.empty() )
    {
        uint32_t id = vblank_locs.back();

        m_do_gotoevent = true;
        m_goto_eventid = id;

        m_tsoffset = events[ id ].ts;
        m_timeoffset_buf = ts_to_timestr( m_tsoffset );
    }
}

int64_t TraceWin::timestr_to_ts( const char *buf, int64_t tsoffset )
{
    double val;

    if ( sscanf( buf, "%lf", &val ) != 1 )
        val = 0.0;

    return tsoffset + ( int64_t )( val * MSECS_PER_SEC );
}

std::string TraceWin::ts_to_timestr( int64_t event_ts, int64_t tsoffset, int precision )
{
    double val = ( event_ts - tsoffset ) * ( 1.0 / MSECS_PER_SEC );

    return string_format( "%.*lf", precision, val );
}

void TraceWin::init_graph_rows_str()
{
    m_graph_rows_str = "# comm and event names to graph\n\n";
    m_graph_rows_str += "# fence_signaled\n";
    m_graph_rows_str += "# amd_sched_job\n\n";

    for ( auto item : m_trace_events->m_comm_locations.m_locations )
    {
        uint32_t hashval = item.first;
        const char *comm = m_trace_events->m_strpool.getstr( hashval );

        item.second.size();
        m_comm_info.push_back( { item.second.size(), comm } );
    }

    // Sort by count of events
    std::sort( m_comm_info.begin(), m_comm_info.end(),
        [=]( const comm_t &lx, const comm_t &rx )
        {
            return rx.event_count < lx.event_count;
        }
    );

    for ( const comm_t &item : m_comm_info )
    {
        m_graph_rows_str += string_format( "# %lu events:\n%s\n",
                                           item.event_count, item.comm );
    }

    update_graph_rows_list();
}

void TraceWin::update_graph_rows_list()
{
    const char *begin = m_graph_rows_str.c_str();

    m_graph_rows.clear();

    for ( ;; )
    {
        const char *end = strchr( begin, '\n' );
        std::string val = end ? std::string( begin, end - begin ) : begin;

        string_trim( val );

        if ( !val.empty() && val[ 0 ] != '#' )
            m_graph_rows.push_back( val );

        if ( !end )
            break;

        begin = end + 1;
    }
}

int TraceWin::ts_to_eventid( int64_t ts )
{
    // When running a debug build w/ ASAN on, the lower_bound function is
    //  horribly slow so we cache the timestamp to event ids.
    auto i = m_ts_to_eventid_cache.find( ts );
    if ( i != m_ts_to_eventid_cache.end() )
        return m_ts_to_eventid_cache.at( ts );

    trace_event_t x;
    std::vector< trace_event_t > &events = m_trace_events->m_events;

    x.ts = ts;

    auto eventidx = std::lower_bound( events.begin(), events.end(), x,
        []( const trace_event_t &f1, const trace_event_t &f2 ) {
            return f1.ts < f2.ts;
        } );

    int id = eventidx - events.begin();

    if ( ( size_t )id >= events.size() )
        id = events.size() - 1;

    m_ts_to_eventid_cache[ ts ] = id;
    return id;
}

int TraceWin::timestr_to_eventid( const char *buf, int64_t tsoffset )
{
    int64_t ts = timestr_to_ts( buf, tsoffset );

    return ts_to_eventid( ts );
}

void TraceWin::render_color_picker()
{
    if ( !m_loader.m_show_color_picker )
        return;

    if ( !ImGui::CollapsingHeader( "Color Picker", ImGuiTreeNodeFlags_DefaultOpen ) )
        return;

    ImGui::Columns( 2 );
    ImGui::SetColumnOffset( 1, imgui_scale( 200.0f ) );

    /*
     * Column 1: draw our graph items and their colors
     */
    float w = imgui_scale( 32.0f );
    float text_h = ImGui::GetTextLineHeight();

    for ( int i = col_1Event; i < col_Max; i++ )
    {
        bool selected = i == m_selected_color;
        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImU32 col = col_get( ( colors_t )i );
        const char *name = col_get_name( ( colors_t )i );

        ImGui::GetWindowDrawList()->AddRectFilled( pos, ImVec2( pos.x + w, pos.y + text_h ), col );

        ImGui::Indent( imgui_scale( 40.0f ) );
        if ( ImGui::Selectable( name, selected, 0 ) )
            m_selected_color = i;
        ImGui::Unindent( imgui_scale( 40.0f ) );
    }

    ImGui::NextColumn();

    /*
     * Column 2: Draw our color picker
     */
    ImU32 color;
    if ( m_colorpicker.render( &color ) )
    {
        col_set( ( colors_t )m_selected_color, color );
    }

    ImGui::Columns( 1 );
}

bool TraceWin::render()
{
    ImGui::SetNextWindowSize( ImVec2( 800, 600 ), ImGuiSetCond_FirstUseEver );

    // If we're told to set focus, wait until the mouse isn't down as they
    //  could have clicked on a button to set focus. Also, hack to do it
    //  twice as the button code still steals our focus if we do it once.
    if ( m_setfocus && !ImGui::IsMouseDown( 0 ) )
    {
        ImGui::SetNextWindowFocus();
        m_setfocus--;
    }

    int eventsloaded = SDL_AtomicGet( &m_trace_events->m_eventsloaded );
    if ( eventsloaded > 0 )
    {
        ImGui::Begin( m_title.c_str(), &m_open );

        ImGui::Text( "Loading events %u...", eventsloaded );
        if ( ImGui::Button( "Cancel" ) )
            m_loader.cancel_load_file();

        ImGui::End();
        return true;
    }
    else if ( eventsloaded == -1 )
    {
        ImGui::Begin( m_title.c_str(), &m_open );

        ImGui::Text( "Error loading filed %s...\n", m_trace_events->m_filename.c_str() );

        ImGui::End();
        return true;
    }

    ImGui::Begin( m_title.c_str(), &m_open );

    if ( ImGui::CollapsingHeader( "Trace Info" ) )
        render_trace_info();

    if ( m_trace_events->m_events.empty() )
    {
        ImGui::End();
        return true;
    }

    if ( !m_inited )
    {
        // Initialize our graph rows first time through.
        init_graph_rows_str();

        render_time_offset_button_init( *m_trace_events );

        int64_t last_ts = m_trace_events->m_events.back().ts;

        m_do_graph_start_timestr = true;
        m_do_graph_length_timestr = true;
        m_graph_length_ts = ( last_ts > 40 * MSECS_PER_SEC ) ?
                    40 * MSECS_PER_SEC : last_ts;
        m_graph_start_ts = last_ts - m_tsoffset - m_graph_length_ts;
    }

    // Sanity check these aren't out of range.
    size_t event_count = m_trace_events->m_events.size();
    m_start_eventid = Clamp< int >( m_start_eventid, 0, event_count - 1 );
    m_end_eventid = Clamp< int >( m_end_eventid, m_start_eventid, event_count - 1 );

    if ( ImGui::CollapsingHeader( "Events Graph", ImGuiTreeNodeFlags_DefaultOpen ) )
    {
        ImGui::Indent();
        if ( ImGui::CollapsingHeader( "Graph Rows" ) )
        {
            if ( ImGui::Button( "Update Graph Rows" ) )
                update_graph_rows_list();

            ImGui::SameLine();
            if ( ImGui::Button( "Reset Graph Rows" ) )
                init_graph_rows_str();

            m_graph_rows_str.reserve( 8192 );
            ImGui::InputTextMultiline( "##GraphRows", &m_graph_rows_str[ 0 ], m_graph_rows_str.capacity(),
                                       ImVec2( -1.0f, ImGui::GetTextLineHeight() * 16 ) );
        }
        ImGui::Unindent();

        if ( imgui_input_text( "Start:", "##GraphStart", m_graphtime_start_buf, 32, 150 ) )
            m_graph_start_ts = timestr_to_ts( m_graphtime_start_buf.c_str() );

        ImGui::SameLine();
        if ( imgui_input_text( "Length:", "##GraphLength", m_graphtime_length_buf, 32, 150 ) )
            m_graph_length_ts = timestr_to_ts( m_graphtime_length_buf.c_str() );

        ImGui::SameLine();
        m_do_graph_zoom_in |= ImGui::SmallButton( "Zoom In" );
        ImGui::SameLine();
        m_do_graph_zoom_out |= ImGui::SmallButton( "Zoom Out" );

        if ( m_graph_length_ts >= g_max_graph_length )
            m_do_graph_zoom_out = false;

        if ( m_do_graph_zoom_in || m_do_graph_zoom_out )
        {
            int64_t sign = m_do_graph_zoom_in ? -1 : +1;
            int64_t amt = sign * m_graph_length_ts / 2;
            int64_t newlen = m_graph_length_ts + amt;

            if ( ( newlen > g_min_graph_length ) && ( newlen < g_max_graph_length ) )
            {
                m_graph_start_ts -= amt / 2;
                m_graph_length_ts = newlen;
                m_do_graph_start_timestr = true;
                m_do_graph_length_timestr = true;
            }

            m_do_graph_zoom_in = false;
            m_do_graph_zoom_out = false;
        }

        if ( m_do_graph_start_timestr )
            m_graphtime_start_buf = ts_to_timestr( m_graph_start_ts, 0, 4 );
        if ( m_do_graph_length_timestr )
            m_graphtime_length_buf = ts_to_timestr( m_graph_length_ts, 0, 4 );

        render_process_graph();

        ImGui::Indent();
        render_color_picker();
        ImGui::Unindent();
    }

    ImGuiTreeNodeFlags eventslist_flags = m_loader.m_show_events_list ?
        ImGuiTreeNodeFlags_DefaultOpen : 0;
    m_show_eventlist = ImGui::CollapsingHeader( "Events List", eventslist_flags );
    if ( m_show_eventlist )
    {
        bool update_eventids = imgui_input_int( &m_start_eventid, 75.0f,
                "Event Start:", "##EventStart", ImGuiInputTextFlags_EnterReturnsTrue );

        ImGui::SameLine();
        update_eventids |= imgui_input_int( &m_end_eventid, 75.0f,
                "Event End:", "##EventEnd", ImGuiInputTextFlags_EnterReturnsTrue );

        if ( update_eventids )
        {
            m_start_eventid = Clamp< int >( m_start_eventid, 0, event_count - 1 );
            m_end_eventid = Clamp< int >( m_end_eventid, m_start_eventid, event_count - 1 );
        }

        ImGui::SameLine();
        if ( imgui_input_text( "Time Offset:", "##TimeOffset", m_timeoffset_buf, 32, 150 ) )
            m_tsoffset = timestr_to_ts( m_timeoffset_buf.c_str() );

        ImGui::SameLine();
        m_do_gotoevent |= imgui_input_int( &m_goto_eventid, 75.0f, "Goto Event:", "##GotoEvent" );

        ImGui::SameLine();
        if ( imgui_input_text( "Goto Time:", "##GotoTime", m_timegoto_buf, 32, 150 ) )
        {
            m_do_gotoevent = true;
            m_goto_eventid = timestr_to_eventid( m_timegoto_buf.c_str(), m_tsoffset );
        }

        render_events_list( m_loader.m_inifile );
    }

    ImGui::End();

    m_inited = true;
    return m_open;
}

void TraceWin::render_trace_info()
{
    size_t event_count = m_trace_events->m_events.size();

    ImGui::Text( "Total Events: %lu\n", event_count );

    if ( event_count )
    {
        trace_info_t &trace_info = m_trace_events->m_trace_info;
        ImGui::Text( "Trace cpus: %u", trace_info.cpus );

        if ( !trace_info.uname.empty() )
            ImGui::Text( "Trace uname: %s", trace_info.uname.c_str() );

        ImGui::Indent();

        if ( ImGui::CollapsingHeader( "Comm Info", ImGuiTreeNodeFlags_DefaultOpen ) )
        {
            if ( !m_comm_info.empty() )
            {
                static const std::array< const char *, 2 > columns =
                    { "Comm", "Events" };

                imgui_headers( "comm_info", columns );

                for ( const comm_t &info : m_comm_info )
                {
                    ImGui::Text( "%s", info.comm );
                    ImGui::NextColumn();
                    ImGui::Text( "%lu", info.event_count );
                    ImGui::NextColumn();
                }

                ImGui::Columns( 1 );
            }
        }

        if ( ImGui::CollapsingHeader( "CPU Info" ) )
        {
            if ( !trace_info.cpustats.empty() )
            {
                static const std::array< const char *, 2 > columns =
                    { "CPU", "Stats" };

                imgui_headers( "cpu_stats", columns );

                for ( const std::string &str : trace_info.cpustats )
                {
                    const char *lf = strchr( str.c_str(), '\n' );

                    if ( lf )
                    {
                        ImGui::Text( "%.*s", ( int )( lf - str.c_str() ), str.c_str() );
                        ImGui::NextColumn();
                        ImGui::Text( "%s", lf + 1 );
                        ImGui::NextColumn();
                        ImGui::Separator();
                    }
                }

                ImGui::Columns( 1 );
            }
        }

        ImGui::Unindent();
    }
}

//$ TODO mikesart: Temporary popup menu
bool TraceWin::render_events_list_popup()
{
    if ( !ImGui::BeginPopup( "EventsListPopup" ) )
        return false;

    const char *names[] = { "Bream", "Haddock", "Mackerel", "Pollock", "Tilefish" };
    static bool toggles[] = { true, false, false, false, false };

    for ( int i = 0; i < 5; i++ )
        ImGui::MenuItem( names[ i ], "", &toggles[ i ] );

    if ( ImGui::BeginMenu( "Sub-menu" ) )
    {
        ImGui::MenuItem( "Click me" );
        ImGui::EndMenu();
    }

    ImGui::Separator();
    ImGui::Text( "Tooltip here" );

    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "I am a tooltip over a popup" );

    if ( ImGui::Button( "Stacked Popup" ) )
        ImGui::OpenPopup( "another popup" );

    if ( ImGui::BeginPopup( "another popup" ) )
    {
        for ( int i = 0; i < 5; i++ )
            ImGui::MenuItem( names[ i ], "", &toggles[ i ] );

        if ( ImGui::BeginMenu( "Sub-menu" ) )
        {
            ImGui::MenuItem( "Click me" );
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }
    ImGui::EndPopup();

    return true;
}

std::string get_event_field_str( std::vector< event_field_t > &fields, const char *eqstr, char sep )
{
    std::string fieldstr;

    for ( const event_field_t &field : fields )
    {
        fieldstr += string_format( "%s%s%s%c", field.key, eqstr, field.value, sep );
    }

    return fieldstr;
}

void TraceWin::render_events_list( CIniFile &inifile )
{
    size_t event_count = m_end_eventid - m_start_eventid + 1;
    std::vector< trace_event_t > &events = m_trace_events->m_events;

    // Set focus on event list first time we open.
    if ( !m_inited && ImGui::IsWindowFocused() )
        ImGui::SetNextWindowFocus();

    // Events list
    imgui_push_smallfont();

    {
        // Set the child window size to hold count of items + header + separator
        float lineh = ImGui::GetTextLineHeightWithSpacing();
        float y = m_loader.m_eventlist_row_count * lineh;

        ImGui::SetNextWindowContentSize( { 0.0f, ( event_count + 1 ) * lineh + 1 } );
        ImGui::BeginChild( "eventlistbox", ImVec2( 0.0f, y ) );

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
            m_goto_eventid = std::min< uint32_t >( m_goto_eventid, event_count - 1 );
            ImGui::SetScrollY( std::max< int >( 0, m_goto_eventid - m_start_eventid ) * lineh );

            m_do_gotoevent = false;
        }

        float scrolly = ImGui::GetScrollY();
        uint32_t start_idx = ( scrolly >= lineh ) ? ( uint32_t )( scrolly / lineh - 1 ) : 0;
        uint32_t rows = ( winh + 1 ) / lineh;
        uint32_t end_idx = std::min< uint32_t >( start_idx + 2 + rows, event_count );

        // Draw columns
        static const std::array< const char *, 6 > columns =
            { "Id", "Time Stamp", "Task", "Event", "seqno", "Info" };
        imgui_headers( "events", columns );

        if ( !m_columns_inited )
        {
            // Try to restore the column sizes from our ini file.
            for ( size_t i = 1; i < columns.size(); i++ )
            {
                float val = inifile.GetFloat( string_format( "column_offset%lu", i ).c_str(), -1.0f );
                if ( val <= 0.0f )
                    break;

                ImGui::SetColumnOffset( i, val );
            }

            m_columns_inited = true;
        }
        else if ( ImGui::IsWindowHovered() && ImGui::IsMouseReleased( 0 ) )
        {
            // Someone release the mouse - save column sizes in case they were changed.
            for ( size_t i = 1; i < columns.size(); i++ )
            {
                inifile.PutFloat( string_format( "column_offset%lu", i ).c_str(),
                                  ImGui::GetColumnOffset( i ) );
            }
        }

        if ( start_idx > 0 )
        {
            // Move cursor position down to where we've scrolled.
            ImGui::SetCursorPosY( ImGui::GetCursorPosY() + lineh * ( start_idx - 1 ) );

            // Scoot to next row (fixes top row occasionally drawing half).
            for ( size_t i = 0; i < columns.size(); i++ )
                ImGui::NextColumn();
        }

        // Reset our hovered event id
        m_hovered_eventlist_eventid = ( uint32_t )-1;

        // Draw events
        m_eventlist_start_eventid = m_start_eventid + start_idx;
        m_eventlist_end_eventid = m_start_eventid + end_idx;
        for ( uint32_t i = start_idx; i < end_idx; i++ )
        {
            char label[ 32 ];
            int colors_pushed = 0;
            trace_event_t &event = events[ m_start_eventid + i ];
            bool selected = ( m_selected_eventid == event.id );
            bool is_vblank = !strcmp( event.name, "drm_vblank_event" );
            std::string ts_str = ts_to_timestr( event.ts, m_tsoffset );

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
                m_selected_eventid = event.id;

            // Check if item is hovered and we don't have a popup menu going already.
            if ( ( m_events_list_popup_eventid == ( uint32_t )-1 ) &&
                 ImGui::IsItemHovered() &&
                 ImGui::IsRootWindowOrAnyChildFocused() )
            {
                // Store the hovered event id.
                m_hovered_eventlist_eventid = event.id;

                if ( ImGui::IsMouseClicked( 1 ) )
                {
                    // If they right clicked, show the context menu.
                    m_events_list_popup_eventid = i;

                    // Open the popup for render_events_list_popup().
                    ImGui::OpenPopup( "EventsListPopup" );
                }
                else
                {
                    // Otherwise show a tooltip.
                    std::string fieldstr = get_event_field_str( event.fields, ": ", '\n' );

                    ImGui::SetTooltip( "Id: %u\nTime: %s\nComm: %s\n%s",
                                       event.id, ts_str.c_str(), event.comm, fieldstr.c_str() );
                }
            }

            // If we've got an active popup menu, render it.
            if ( m_events_list_popup_eventid == i )
            {
                if ( !TraceWin::render_events_list_popup() )
                    m_events_list_popup_eventid = ( uint32_t )-1;
            }

            ImGui::NextColumn();

            ImGui::Text( "%s", ts_str.c_str() );
            ImGui::NextColumn();
            ImGui::Text( "%s (%u)", event.comm, event.cpu );
            ImGui::NextColumn();
            ImGui::Text( "%s", event.name );
            ImGui::NextColumn();

            if ( event.seqno )
                ImGui::Text( "%u", event.seqno );
            ImGui::NextColumn();

            std::string fieldstr = get_event_field_str( event.fields, "=", ' ' );
            ImGui::Text( "%s", fieldstr.c_str() );
            ImGui::NextColumn();

            ImGui::PopStyleColor( colors_pushed );
        }

        ImGui::Columns( 1 );
        ImGui::EndChild();
    }

    imgui_pop_smallfont();
}

/*
 * TraceConsole
 */
void TraceConsole::init( CIniFile *inifile )
{
    m_clear_color = inifile->GetVec4( "clearcolor", ImColor( 114, 144, 154 ) );

    logf( "Welcome to gpuvis\n" );

    logf( "graph shortcuts:" );
    logf( "  shift+click+drag: zoom to selection" );
    logf( "  click+drag: pan graph" );

    //$ TODO mikesart: use https://github.com/SirCmpwn/libccmd.git
    //$ TODO mikesart: add "load" command

    m_commands.insert( "clear" );
    m_commands.insert( "help" );
    m_commands.insert( "history" );
    m_commands.insert( "quit" );
    m_commands.insert( "q" );

    SDL_strlcpy( m_trace_file, "trace.dat", sizeof( m_trace_file ) );
}

void TraceConsole::shutdown( CIniFile *inifile )
{
    inifile->PutVec4( "clearcolor", m_clear_color );

    m_history.clear();
}

void TraceConsole::render_options( TraceLoader &loader )
{
    ImGui::Text( "Clear Color:" );
    ImGui::SameLine();
    ImGui::ColorEdit3( "", ( float * )&m_clear_color );

    ImGui::Separator();

    ImGui::Text( "Imgui debug: " );

    ImGui::SameLine();
    if ( ImGui::Button( "Style Editor" ) )
        m_show_imgui_style_editor ^= 1;

    ImGui::SameLine();
    if ( ImGui::Button( "Metrics" ) )
        m_show_imgui_metrics_editor ^= 1;

    ImGui::SameLine();
    if ( ImGui::Button( "Test Window" ) )
        m_show_imgui_test_window ^= 1;

    ImGui::Separator();

    ImGui::Checkbox( "Show Events List when opening new Trace Windows",
                     &loader.m_show_events_list );

    ImGui::Checkbox( "Fullscreen Trace Window",
                     &loader.m_fullscreen );

    ImGui::Checkbox( "Sync event list to graph mouse location",
                     &loader.m_sync_eventlist_to_graph );

    for ( int i = 0; i <= loader.m_crtc_max; i++ )
    {
        std::string label = string_format(
                    "Show drm_vblank_event crtc%d markers", i );

        ImGui::Checkbox( label.c_str(), &loader.m_render_crtc[ i ] );
    }

    ImGui::Checkbox( "Show graph color picker",
                     &loader.m_show_color_picker );

    ImGui::Text( "Event List Row Count:" );

    ImGui::SameLine();
    ImGui::PushItemWidth( imgui_scale( 150.0f ) );
    ImGui::SliderInt( "##EventListRowCount", &loader.m_eventlist_row_count, 0, 100 );
    ImGui::PopItemWidth();
}

void TraceConsole::render_log( TraceLoader &loader )
{
    ImGui::Text( "Log Filter:" );
    ImGui::SameLine();
    ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 0, 0 ) );
    m_filter.Draw( "##log-filter", 180 );
    ImGui::PopStyleVar();

    ImGui::SameLine();
    if ( ImGui::SmallButton( "Clear" ) )
        logf_clear();

    ImGui::SameLine();
    if ( ImGui::SmallButton( "Scroll to bottom" ) )
        m_log_size = ( size_t )-1;

    ImGui::Separator();

    {
        ImGui::BeginChild( "ScrollingRegion",
                           ImVec2( 0, -ImGui::GetItemsLineHeightWithSpacing() ),
                           false, ImGuiWindowFlags_HorizontalScrollbar );

        // Log popup menu
        if ( ImGui::BeginPopupContextWindow() )
        {
            if ( ImGui::Selectable( "Clear" ) )
                logf_clear();
            ImGui::EndPopup();
        }

        // Display every line as a separate entry so we can change their color or add custom widgets. If you only want raw text you can use ImGui::TextUnformatted(log.begin(), log.end());
        // NB- if you have thousands of entries this approach may be too inefficient and may require user-side clipping to only process visible items.
        // You can seek and display only the lines that are visible using the ImGuiListClipper helper, if your elements are evenly spaced and you have cheap random access to the elements.
        // To use the clipper we could replace the 'for (int i = 0; i < Items.Size; i++)' loop with:
        //     ImGuiListClipper clipper(Items.Size);
        //     while (clipper.Step())
        //         for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++)
        // However take note that you can not use this code as is if a filter is active because it breaks the 'cheap random-access' property. We would need random-access on the post-filtered list.
        // A typical application wanting coarse clipping and filtering may want to pre-compute an array of indices that passed the filtering test, recomputing this array when user changes the filter,
        // and appending newly elements as they are inserted. This is left as a task to the user until we can manage to improve this example code!
        // If your items are of variable size you may want to implement code similar to what ImGuiListClipper does. Or split your data into fixed height items to allow random-seeking into your list.

        // Tighten spacing
        ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 4, 1 ) );

        const std::vector< char * > &log = logf_get();
        for ( const char *item : log )
        {
            if ( !m_filter.PassFilter( item ) )
                continue;

            ImVec4 col = ImVec4( 1.0f, 1.0f, 1.0f, 1.0f );

            if ( !strncasecmp( item, "[error]", 7 ) )
                col = ImColor( 1.0f, 0.4f, 0.4f, 1.0f );
            else if ( strncmp( item, "# ", 2 ) == 0 )
                col = ImColor( 1.0f, 0.78f, 0.58f, 1.0f );

            ImGui::PushStyleColor( ImGuiCol_Text, col );
            ImGui::TextUnformatted( item );
            ImGui::PopStyleColor();
        }

        if ( m_log_size != log.size() )
        {
            ImGui::SetScrollHere();

            m_log_size = log.size();
        }

        ImGui::PopStyleVar();
        ImGui::EndChild();
    }

    ImGui::Separator();

    // Command-line
    ImGui::Text( "Command:" );

    ImGui::SameLine();
    m_inputbuf.reserve( 512 );
    if ( ImGui::InputText( "##log-command", &m_inputbuf[ 0 ], m_inputbuf.capacity(),
                           ImGuiInputTextFlags_EnterReturnsTrue |
                               ImGuiInputTextFlags_CallbackCompletion |
                               ImGuiInputTextFlags_CallbackHistory |
                               ImGuiInputTextFlags_CallbackCharFilter,
                           &text_edit_cb_stub, ( void * )this ) )
    {
        exec_command( m_inputbuf.c_str() );

        m_inputbuf = "";
    }

    // Keep auto focus on the input box
    if ( ImGui::IsItemHovered() ||
         ( ImGui::IsRootWindowOrAnyChildFocused() && !ImGui::IsAnyItemActive() && !ImGui::IsMouseClicked( 0 ) ) )
    {
        // Auto focus previous widget
        ImGui::SetKeyboardFocusHere( -1 );
    }
}

void TraceConsole::render( TraceLoader &loader )
{
    ImGui::SetNextWindowSize( ImVec2( 720, 600 ), ImGuiSetCond_FirstUseEver );

    if ( !ImGui::Begin( "gpuvis console" ) )
    {
        ImGui::End();
        return;
    }

    ImGui::Text( "%.2f ms/frame (%.1f FPS)",
                 1000.0f / ImGui::GetIO().Framerate,
                 ImGui::GetIO().Framerate );

    if ( ImGui::CollapsingHeader( "Trace File", ImGuiTreeNodeFlags_DefaultOpen ) )
    {
        bool is_loading = loader.is_loading();

        ImGui::Text( "File:" );
        ImGui::SameLine();

        ImVec4 &text_disabled = ImGui::GetStyle().Colors[ ImGuiCol_TextDisabled ];
        ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue;

        const char *button_text;
        if ( is_loading )
        {
            ImGui::PushStyleColor( ImGuiCol_Text, text_disabled );

            flags |= ImGuiInputTextFlags_ReadOnly;
            button_text = "Loading...";
        }
        else
        {
            button_text = "Load";
        }

        bool do_load = ImGui::InputText( "##load-trace-file", m_trace_file, sizeof( m_trace_file ), flags );

        ImGui::SameLine();
        do_load |= ImGui::Button( button_text, ImVec2( 0, 0 ) );

        if ( is_loading )
        {
            ImGui::PopStyleColor();
        }
        else if ( do_load )
        {
            loader.load_file( m_trace_file );
        }
    }

    if ( ImGui::CollapsingHeader( "Opened Event Files", ImGuiTreeNodeFlags_DefaultOpen ) )
    {
        ImGui::Columns( 2, "files" );

        ImGui::Separator();

        for ( size_t i = 0; i < loader.m_trace_events_list.size(); i++ )
        {
            TraceEvents *events = loader.m_trace_events_list[ i ];
            int eventsloaded = SDL_AtomicGet( &events->m_eventsloaded );

            if ( !eventsloaded )
            {
                ImGui::Text( "%s", events->m_title.c_str() );
                ImGui::NextColumn();

                if ( ImGui::SmallButton( string_format( "Events##%lu", i ).c_str() ) )
                    loader.new_event_window( events );

                ImGui::SameLine();
                if ( ImGui::SmallButton( string_format( "Close Windows##%lu", i ).c_str() ) )
                    loader.close_event_file( events, false );

                ImGui::SameLine();
                if ( ImGui::SmallButton( string_format( "Free##%lu", i ).c_str() ) )
                    loader.close_event_file( events, true );

                ImGui::NextColumn();
            }
        }

        ImGui::Columns( 1 );
        ImGui::Separator();
    }

    if ( ImGui::CollapsingHeader( "Options", ImGuiTreeNodeFlags_DefaultOpen ) )
        render_options( loader );

    if ( ImGui::CollapsingHeader( "Log", ImGuiTreeNodeFlags_DefaultOpen ) )
        render_log( loader );

    if ( m_show_imgui_test_window )
        ImGui::ShowTestWindow( &m_show_imgui_test_window );

    if ( m_show_imgui_style_editor )
    {
        ImGui::Begin( "Style Editor", &m_show_imgui_style_editor );
        ImGui::ShowStyleEditor();
        ImGui::End();
    }

    if ( m_show_imgui_metrics_editor )
        ImGui::ShowMetricsWindow( &m_show_imgui_metrics_editor );

    ImGui::End();
}

void TraceConsole::exec_command( const std::string &cmdlinein )
{
    std::string cmdline = string_trimmed( cmdlinein );

    if ( cmdline.empty() )
        return;

    logf( "# %s\n", cmdline.c_str() );

    // Insert into history. First find match and delete it so it can be pushed to the back. This isn't trying to be smart or optimal.
    m_history_pos = -1;

    for ( size_t i = 0; i < m_history.size(); i++ )
    {
        if ( cmdline == m_history[ i ] )
        {
            m_history.erase( m_history.begin() + i );
            break;
        }
    }
    m_history.push_back( cmdline );

    // Process command
    if ( cmdline == "clear" )
    {
        logf_clear();
    }
    else if ( ( cmdline == "quit" ) || ( cmdline == "q" ) )
    {
        m_quit = true;
    }
    else if ( cmdline == "help" )
    {
        logf( "Commands:" );

        for ( const std::string &cmd : m_commands )
            logf( "- %s", cmd.c_str() );
    }
    else if ( cmdline == "history" )
    {
        for ( size_t i = m_history.size() >= 20 ? m_history.size() - 20 : 0; i < m_history.size(); i++ )
            logf( "%3lu: %s\n", i, m_history[ i ].c_str() );
    }
    else
    {
        logf( "Unknown command: '%s'\n", cmdline.c_str() );
    }
}

int TraceConsole::text_edit_cb_completion( ImGuiTextEditCallbackData *data )
{
    if ( m_completions.empty() )
    {
        const char *word_end = data->Buf + data->CursorPos;
        const char *word_start = word_end;

        // Locate beginning of current word
        while ( word_start > data->Buf )
        {
            const char c = word_start[ -1 ];

            if ( c == ' ' || c == '\t' || c == ',' || c == ';' )
                break;
            word_start--;
        }

        const char *comp_str = word_start;
        size_t comp_len = word_end - word_start;

        if ( comp_len )
        {
            for ( const std::string &str : m_commands )
            {
                if ( !strncasecmp( str.c_str(), comp_str, comp_len ) )
                {
                    m_completions.push_back( str.c_str() );
                    printf( "%s\n", str.c_str() );
                }
            }
            for ( int i = m_history.size() - 1; i >= 0; i-- )
            {
                std::string str( comp_str, comp_len );

                if ( !strncasecmp( m_history[ i ].c_str(), comp_str, comp_len ) &&
                     m_commands.find( m_history[ i ].c_str() ) == m_commands.end() )
                {
                    m_completions.push_back( m_history[ i ].c_str() );
                }
            }
        }

        m_completion_index = 0;
    }

    if ( m_completion_index < m_completions.size() )
    {
        const char *str = m_completions[ m_completion_index ];
        size_t len = strlen( str );

        // Delete line and replace it
        data->DeleteChars( 0, data->BufTextLen );
        data->InsertChars( 0, str );
        data->InsertChars( len, " " );
        data->CursorPos = len + 1;

        if ( ++m_completion_index >= m_completions.size() )
            m_completion_index = 0;
    }

    return 0;
}

int TraceConsole::text_edit_cb_history( ImGuiTextEditCallbackData *data )
{
    const int prev_history_pos = m_history_pos;

    if ( data->EventKey == ImGuiKey_UpArrow )
    {
        if ( m_history_pos == -1 )
            m_history_pos = m_history.size() - 1;
        else if ( m_history_pos > 0 )
            m_history_pos--;
    }
    else if ( data->EventKey == ImGuiKey_DownArrow )
    {
        if ( m_history_pos != -1 )
        {
            m_history_pos++;
            if ( m_history_pos >= ( int )m_history.size() )
                m_history_pos = -1;
        }
    }

    if ( prev_history_pos != m_history_pos )
    {
        const char *str = ( m_history_pos >= 0 ) ? m_history[ m_history_pos ].c_str() : "";

        SDL_strlcpy( data->Buf, str, data->BufSize );

        data->CursorPos = data->SelectionStart = data->SelectionEnd = data->BufTextLen = strlen( data->Buf );
        data->BufDirty = true;
    }

    return 0;
}

int TraceConsole::text_edit_cb_stub( ImGuiTextEditCallbackData *data )
{
    int ret = 0;
    TraceConsole *console = ( TraceConsole * )data->UserData;

    if ( data->EventFlag == ImGuiInputTextFlags_CallbackCompletion )
    {
        ret = console->text_edit_cb_completion( data );
    }
    else
    {
        console->m_completions.clear();

        if ( data->EventFlag == ImGuiInputTextFlags_CallbackHistory )
            ret = console->text_edit_cb_history( data );
    }

    return ret;
}

static void parse_cmdline( TraceLoader &loader, int argc, char **argv )
{
    static struct option long_opts[] =
    {
        { "fullscreen", no_argument, 0, 0 },
        { 0, 0, 0, 0 }
    };

    int c;
    int opt_ind = 0;
    while ( ( c = getopt_long( argc, argv, "i:",
                               long_opts, &opt_ind ) ) != -1 )
    {
        switch ( c )
        {
        case 0:
            if ( !strcasecmp( "fullscreen", long_opts[ opt_ind ].name ) )
                loader.m_fullscreen = true;
            break;
        case 'i':
            loader.m_inputfiles.push_back( optarg );
            break;

        default:
            break;
        }
    }

    for ( ; optind < argc; optind++ )
    {
        loader.m_inputfiles.push_back( argv[ optind ] );
    }
}

static bool load_trace_file( TraceLoader &loader, TraceConsole &console, const char *filename )
{
    SDL_strlcpy( console.m_trace_file, filename, sizeof( console.m_trace_file ) );

    return loader.load_file( filename );
}

#if SDL_VERSIONNUM( SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_PATCHLEVEL ) < SDL_VERSIONNUM( 2, 0, 5 )
int SDL_GetWindowBordersSize( SDL_Window *window, int *top, int *left, int *bottom, int *right )
{
    *top = 0;
    *left = 0;
    *bottom = 0;
    *right = 0;

    return -1;
}
#endif

static void sdl_setwindow_icon( SDL_Window *window )
{
#include "gpuvis_icon.c"

    SDL_Surface *surface = SDL_CreateRGBSurfaceFrom(
                s_icon.pixel_data,
                s_icon.width,
                s_icon.height,
                s_icon.bytes_per_pixel * 8,
                s_icon.width * s_icon.bytes_per_pixel,
                IM_COL32( 0xff, 0, 0, 0 ),
                IM_COL32( 0, 0xff, 0, 0 ),
                IM_COL32( 0, 0, 0xff, 0 ),
                IM_COL32( 0, 0, 0, 0xff ) );

    SDL_SetWindowIcon( window, surface );
}

int main( int argc, char **argv )
{
    CIniFile inifile;
    TraceConsole console;
    TraceLoader loader( inifile );
    SDL_Window *window = NULL;

    parse_cmdline( loader, argc, argv );

    // Setup SDL
    if ( SDL_Init( SDL_INIT_VIDEO | SDL_INIT_TIMER ) != 0 )
    {
        fprintf( stderr, "Error. SDL_Init failed: %s\n", SDL_GetError() );
        return -1;
    }

    logf_init();

    inifile.Open( "gpuvis", "gpuvis.ini" );

    col_init( inifile );
    loader.init();

    ImGuiIO &io = ImGui::GetIO();
    io.IniLoadSettingCB = std::bind( imgui_ini_load_settings_cb, &inifile, _1, _2 );
    io.IniSaveSettingCB = std::bind( imgui_ini_save_settings_cb, &inifile, _1, _2 );

    int x = inifile.GetInt( "win_x", SDL_WINDOWPOS_CENTERED );
    int y = inifile.GetInt( "win_y", SDL_WINDOWPOS_CENTERED );
    int w = inifile.GetInt( "win_w", 1280 );
    int h = inifile.GetInt( "win_h", 1024 );

    imgui_ini_settings( inifile );

    console.init( &inifile );

    // Setup window
    SDL_GL_SetAttribute( SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG );
    SDL_GL_SetAttribute( SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE );
    SDL_GL_SetAttribute( SDL_GL_DOUBLEBUFFER, 1 );
    SDL_GL_SetAttribute( SDL_GL_DEPTH_SIZE, 24 );
    SDL_GL_SetAttribute( SDL_GL_STENCIL_SIZE, 8 );
    SDL_GL_SetAttribute( SDL_GL_CONTEXT_MAJOR_VERSION, 3 );
    SDL_GL_SetAttribute( SDL_GL_CONTEXT_MINOR_VERSION, 2 );

    SDL_DisplayMode current;
    SDL_GetCurrentDisplayMode( 0, &current );

    window = SDL_CreateWindow( "GPUVis", x, y, w, h,
                               SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE );
    sdl_setwindow_icon( window );

    SDL_GLContext glcontext = SDL_GL_CreateContext( window );

    gl3wInit();

    // Setup ImGui binding
    ImGui_ImplSdlGL3_Init( window );

    imgui_load_fonts();

    // Main loop
    bool done = false;
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

        // Check for logf() calls from background threads.
        logf_update();

        // Render console / options window
        console.render( loader );

        // Render trace windows
        loader.render();

        // Rendering
        const ImVec4 &color = console.m_clear_color;
        const ImVec2 &size = ImGui::GetIO().DisplaySize;

        glViewport( 0, 0, ( int )size.x, ( int )size.y );
        glClearColor( color.x, color.y, color.z, color.w );
        glClear( GL_COLOR_BUFFER_BIT );

        ImGui::Render();

        SDL_GL_SwapWindow( window );

        if ( console.m_quit )
            break;

        if ( !loader.m_inputfiles.empty() && !loader.is_loading() )
        {
            const char *filename = loader.m_inputfiles[ 0 ].c_str();

            load_trace_file( loader, console, filename );

            loader.m_inputfiles.erase( loader.m_inputfiles.begin() );
        }
    }

    // Write main window position / size to ini file.
    int top, left, bottom, right;

    SDL_GetWindowBordersSize( window, &top, &left, &bottom, &right );
    SDL_GetWindowPosition( window, &x, &y );
    SDL_GetWindowSize( window, &w, &h );
    inifile.PutInt( "win_x", x - left );
    inifile.PutInt( "win_y", y - top );
    inifile.PutInt( "win_w", w );
    inifile.PutInt( "win_h", h );

    imgui_ini_settings( inifile, true );

    // Shut down our trace loader
    loader.shutdown();

    // Shut down our console / option window
    console.shutdown( &inifile );

    col_shutdown( inifile );
    logf_clear();

    // Cleanup
    logf_shutdown();

    ImGui_ImplSdlGL3_Shutdown();
    SDL_GL_DeleteContext( glcontext );
    SDL_DestroyWindow( window );
    SDL_Quit();

    inifile.Close();
    return 0;
}
