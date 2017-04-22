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
#include "ya_getopt.h"
#include "gpuvis.h"
#include "tdopexpr.h"

// String we prefix graph rows with to hide them
static const char g_hide_row_str[] = "#hide# ";

multi_text_color multi_text_color::yellow = { ImVec4( 1, 1, 0, 1 ) };
multi_text_color multi_text_color::def = { ImVec4( 0.90f, 0.90f, 0.90f, 1.00f ) };

static bool imgui_input_int( int *val, float w, const char *label, const char *label2, ImGuiInputTextFlags flags = 0 )
{
    bool ret = ImGui::Button( label );

    ImGui::SameLine();
    ImGui::PushItemWidth( imgui_scale( w ) );
    ret |= ImGui::InputInt( label2, val, 0, 0, flags );
    ImGui::PopItemWidth();

    return ret;
}

template < size_t T >
static bool imgui_input_text( const char *button_label, const char *text_label,
                              char ( &buf )[ T ], float w, int flags = 0)
{
    bool ret = ImGui::Button( button_label );

    ImGui::SameLine();
    ImGui::PushItemWidth( imgui_scale( w ) );

    ret |= ImGui::InputText( text_label, buf, T, flags, 0 );

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
            data.Collapsed = !!inifile->GetInt( "collapsed", 0, section.c_str() );
            data.Name = strdup( name.c_str() );
        }
        return 0;
    }

    return -1;
}

static bool imgui_begin_columns( const char *title,
                                 const std::initializer_list< const char * > &headers,
                                 CIniFile *inifile = NULL, bool *resized = NULL )
{
    bool inited = ImGui::BeginColumns( title, headers.size() );

    for ( const char *str : headers )
    {
        ImGui::TextColored( ImVec4( 1, 1, 0, 1 ), "%s", str );
        ImGui::NextColumn();
    }
    ImGui::Separator();

    // If we were just initialized or resized...
    if ( inifile && ( inited || ( *resized && ImGui::IsMouseReleased( 0 ) ) ) )
    {
        // Go through the columns...
        for ( size_t i = 0; i < headers.size(); i++ )
        {
            std::string key = string_format( "column_width_%s%lu", title, i );

            if ( inited )
            {
                // Try to restore the column widths
                float val = inifile->GetFloat( key.c_str(), -1.0f );
                if ( val <= 0.0f )
                    break;

                ImGui::SetColumnWidth( i, val );
            }
            else
            {
                // Save the column widths
                inifile->PutFloat( key.c_str(), ImGui::GetColumnWidth( i ) );
            }
        }

        // Clear the resized flag
        *resized = false;
    }

    return inited;
}

/*
 * StrPool
 */
const char *StrPool::getstr( const char *str, size_t len )
{
    uint32_t hashval = fnv_hashstr32( str, len );
    const std::string *ret = m_pool.get_val( hashval );

    if ( !ret )
    {
        if ( len == ( size_t )-1 )
            len = strlen( str );
        ret = m_pool.get_val( hashval, std::string( str, len ) );
    }
    return ret->c_str();
}

const char *StrPool::findstr( uint32_t hashval )
{
    std::string *str = m_pool.get_val( hashval );

    return str ? str->c_str() : NULL;
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

    size_t filesize = get_file_size( filename );
    if ( !filesize )
    {
        logf( "[Error] %s (%s) failed: %s", __func__, filename, strerror( errno ) );
        return false;
    }

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

    for ( int i = ( int )m_trace_windows_list.size() - 1; i >= 0; i-- )
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
    for ( int i = ( int )m_trace_windows_list.size() - 1; i >= 0; i-- )
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

// See notes at top of gpuvis_graph.cpp for explanation of these events.
static bool is_timeline_event( const trace_event_t &event )
{
    if ( !event.timeline || !event.seqno || !event.context )
        return false;

    return ( event.is_fence_signaled() ||
             !strcmp( event.name, "amdgpu_cs_ioctl" ) ||
             !strcmp( event.name, "amdgpu_sched_run_job" ) );
}

int TraceLoader::init_new_event( trace_event_t &event, const trace_info_t &info )
{
    if ( event.id == 0 )
    {
        m_trace_events->m_ts_min = event.ts;
    }

    if ( m_trace_events->m_cpucount.empty() )
    {
        m_trace_events->m_trace_info = info;
        m_trace_events->m_cpucount.resize( info.cpus, 0 );
    }

    if ( event.cpu < m_trace_events->m_cpucount.size() )
        m_trace_events->m_cpucount[ event.cpu ]++;

    // Record the maximum crtc we've ever seen.
    m_crtc_max = std::max< int >( m_crtc_max, event.crtc );

    event.ts -= m_trace_events->m_ts_min;

    // Make sure our events are cleared
    event.flags &= ~( TRACE_FLAG_FENCE_SIGNALED |
                      TRACE_FLAG_FTRACE_PRINT |
                      TRACE_FLAG_IS_VBLANK |
                      TRACE_FLAG_IS_TIMELINE );

    // fence_signaled was renamed to dma_fence_signaled post v4.9
    if ( strstr( event.name, "fence_signaled" ) )
        event.flags |= TRACE_FLAG_FENCE_SIGNALED;
    else if ( !strcmp( event.system, "ftrace-print" ) )
        event.flags |= TRACE_FLAG_FTRACE_PRINT;
    else if ( !strcmp( event.name, "drm_vblank_event" ) )
        event.flags |= TRACE_FLAG_IS_VBLANK;

    // Add this event name to our event locations map
    if ( event.is_vblank() )
        m_trace_events->m_tdopexpr_locations.add_location_str( "$name=drm_vblank_event", event.id );

    // Add this event comm to our comm locations map
    m_trace_events->m_comm_locations.add_location_str( event.comm, event.id );

    if ( is_timeline_event( event ) )
    {
        const std::string gfxcontext = get_event_gfxcontext_str( event );

        // Add this event under the "gfx", "sdma0", etc timeline map
        m_trace_events->m_timeline_locations.add_location_str( event.timeline, event.id );

        // Add this event under our "gfx_ctx_seq" or "sdma0_ctx_seq", etc. map
        m_trace_events->m_gfxcontext_locations.add_location_str( gfxcontext.c_str(), event.id );

        // Grab the event locations for this event context
        const std::vector< uint32_t > *plocs = m_trace_events->get_gfxcontext_locs( gfxcontext.c_str() );
        if ( plocs->size() > 1 )
        {
            // First event.
            trace_event_t &event0 = m_trace_events->m_events[ plocs->front() ];

            // Event right before the event we just added.
            auto it = plocs->rbegin() + 1;
            const trace_event_t &event_prev = m_trace_events->m_events[ *it ];

            event0.flags |= TRACE_FLAG_IS_TIMELINE;
            event.flags |= TRACE_FLAG_IS_TIMELINE;

            // Assume the user comm is the first comm event in this set.
            event.user_comm = event0.comm;

            // Point the event we just added to the previous event in this series
            event.id_start = event_prev.id;
        }
    }

    SDL_AtomicAdd( &m_trace_events->m_eventsloaded, 1 );

    if ( get_state() == State_CancelLoading )
        return 1;

    return 0;
}

int TraceLoader::new_event_cb( TraceLoader *loader, const trace_info_t &info,
                               const trace_event_t &event )
{
    TraceEvents *trace_events = loader->m_trace_events;
    size_t id = trace_events->m_events.size();

    trace_events->m_events.push_back( event );
    trace_events->m_events.back().id = id;

    return loader->init_new_event( trace_events->m_events.back(), info );
}

// Go through gfx, sdma0, sdma1, etc. timelines and calculate event durations
static void calculate_event_durations( TraceEvents *trace_events )
{
    std::vector< trace_event_t > &events = trace_events->m_events;

    for ( const auto &timeline_locs : trace_events->m_timeline_locations.m_locs.m_map )
    {
        uint32_t graph_row_id = 0;
        int64_t last_fence_signaled_ts = 0;
        const std::vector< uint32_t > &locs = timeline_locs.second;
        // const char *name = trace_events->m_strpool.findstr( timeline_locs.first );

        for ( uint32_t index : locs )
        {
            trace_event_t &fence_signaled = events[ index ];

            if ( fence_signaled.is_fence_signaled() &&
                 is_valid_id( fence_signaled.id_start ) )
            {
                trace_event_t &amdgpu_sched_run_job = events[ fence_signaled.id_start ];
                int64_t start_ts = amdgpu_sched_run_job.ts;

                // amdgpu_cs_ioctl   amdgpu_sched_run_job   fence_signaled
                //       |-----------------|---------------------|
                //       |user-->          |hw-->                |
                //                                               |
                //          amdgpu_cs_ioctl  amdgpu_sched_run_job|   fence_signaled
                //                |-----------------|------------|--------|
                //                |user-->          |hwqueue-->  |hw->    |
                //                                                        |

                // Our starting location will be the last fence signaled timestamp or
                //  our amdgpu_sched_run_job timestamp, whichever is larger.
                int64_t hw_start_ts = std::max< int64_t >( last_fence_signaled_ts, amdgpu_sched_run_job.ts );

                // Set duration times
                fence_signaled.duration = fence_signaled.ts - hw_start_ts;
                amdgpu_sched_run_job.duration = hw_start_ts - amdgpu_sched_run_job.ts;

                if ( is_valid_id( amdgpu_sched_run_job.id_start ) )
                {
                    trace_event_t &amdgpu_cs_ioctl = events[ amdgpu_sched_run_job.id_start ];

                    amdgpu_cs_ioctl.duration = amdgpu_sched_run_job.ts - amdgpu_cs_ioctl.ts;

                    start_ts = amdgpu_cs_ioctl.ts;
                }

                // If our start time stamp is greater than the last fence time stamp then
                //  reset our graph row back to the top.
                if ( start_ts > last_fence_signaled_ts )
                    graph_row_id = 0;
                fence_signaled.graph_row_id = graph_row_id++;

                last_fence_signaled_ts = fence_signaled.ts;
            }
        }
    }
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

    // Init event durations
    calculate_event_durations( trace_events );

    logf( "Events read: %lu", trace_events->m_events.size() );

    SDL_AtomicSet( &trace_events->m_eventsloaded, 0 );
    loader->set_state( State_Loaded );
    return 0;
}

void TraceLoader::init()
{
    m_options.resize( OPT_Max );

    m_options[ OPT_Fullscreen ].opt_bool( "Fullscreen Trace Window", "fullscreen", false );

    m_options[ OPT_TimelineZoomGfx ].opt_bool( "Zoom gfx timeline (Ctrl+Shift+Z)", "zoom_gfx_timeline", false );
    m_options[ OPT_TimelineLabels ].opt_bool( "Show timeline labels", "timeline_labels", true );
    m_options[ OPT_TimelineEvents ].opt_bool( "Show timeline events", "timeline_events", true );
    m_options[ OPT_TimelineRenderUserSpace ].opt_bool( "Show timeline userspace", "timeline_userspace", false );

    m_options[ OPT_GraphOnlyFiltered ].opt_bool( "Graph only filtered events", "graph_only_filtered", false );

    m_options[ OPT_ShowEventList ].opt_bool( "Show Event List", "show_event_list", true );
    m_options[ OPT_SyncEventListToGraph ].opt_bool( "Sync Event List to graph mouse location", "sync_eventlist_to_graph", true );
    m_options[ OPT_ShowColorPicker ].opt_bool( "Show graph color picker", "show_color_picker", false );

    m_options[ OPT_GraphHeight ].opt_float( "Graph Size: %.1f", "graph_height", 0, 0, 1 );
    m_options[ OPT_GraphHeightZoomed ].opt_float( "Zoomed Graph Size: %.1f", "graph_height_zoomed", 0, 0, 1 );

    m_options[ OPT_EventListRowCount ].opt_int( "Event List Size: %.0f", "eventlist_row_count", 0, 0, 100 );

    for ( int i = OPT_RenderCrtc0; i <= OPT_RenderCrtc9; i++ )
    {
        const std::string desc = string_format( "Show drm_vblank_event crtc%d markers", i - OPT_RenderCrtc0 );
        const std::string inikey = string_format( "render_crtc%d", i - OPT_RenderCrtc0 );

        m_options[ i ].opt_bool( desc, inikey, true );
    }

    // Read option settings from ini file
    for ( size_t i = 0; i < m_options.size(); i++ )
    {
        option_t &opt = m_options[ i ];

        if ( opt.type == OPT_Float )
            opt.valf = m_inifile.GetFloat( opt.inikey.c_str(), opt.valf );
        else
            opt.val = m_inifile.GetInt( opt.inikey.c_str(), opt.val );
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

    // Write option settings to ini file
    for ( size_t i = 0; i < m_options.size(); i++ )
    {
        const option_t &opt = m_options[ i ];

        if ( opt.type == OPT_Float )
            m_inifile.PutFloat( opt.inikey.c_str(), opt.valf );
        else
            m_inifile.PutInt( opt.inikey.c_str(), opt.val );
    }
}

void TraceLoader::render()
{
    if ( get_opt( OPT_Fullscreen ) && !m_trace_windows_list.empty() )
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
    const std::vector< uint32_t > *vblank_locs = trace_events.get_tdopexpr_locs( "$name=drm_vblank_event" );

    if ( vblank_locs )
    {
        uint32_t id = vblank_locs->back();

        m_do_gotoevent = true;
        m_goto_eventid = id;

        // Don't set offset time. If user cancels when loading it'll be at different locations
        //  so let the user explicitly control this.
        m_tsoffset = 0; // events[ id ].ts;
        strcpy_safe( m_timeoffset_buf, ts_to_timestr( m_tsoffset ) );
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

std::vector< std::string > TraceWin::graph_rows_get_hidden_rows()
{
    std::vector< std::string > ret;
    size_t len = strlen( g_hide_row_str );
    std::vector< std::string > lines = string_explode( m_graph_rows_str, '\n' );

    for ( std::string &entry : lines )
    {
        if ( !strncasecmp( entry.c_str(), g_hide_row_str, len ) )
            ret.push_back( entry.c_str() + len );
    }

    return ret;
}

bool TraceWin::graph_rows_show( const std::string &name, graph_rows_show_t show )
{
    bool modified = false;
    const std::string hide_name = g_hide_row_str + name;
    const std::string searchstr = ( show == SHOW_ROW ) ? hide_name : name;
    std::vector< std::string > lines = string_explode( m_graph_rows_str, '\n' );

    for ( std::string &entry : lines )
    {
        std::string entry_trimmed = string_trimmed( entry );

        if ( entry_trimmed.empty() )
            continue;

        if ( modified && ( show == HIDE_ROW_AND_ALL_BELOW ) )
        {
            if ( entry_trimmed[ 0 ] != '#' )
                entry = g_hide_row_str + entry_trimmed;
        }
        else if ( entry_trimmed == searchstr )
        {
            entry = ( show == SHOW_ROW ) ? name : hide_name;
            modified = true;
        }
    }

    if ( modified )
    {
        m_graph_rows_str = string_implode( lines, "\n" );
        graph_rows_updatelist();
    }

    return modified;
}

void TraceWin::graph_rows_initstr( bool reset )
{
    if ( m_comm_info.empty() )
    {
        for ( auto item : m_trace_events->m_comm_locations.m_locs.m_map )
        {
            uint32_t hashval = item.first;
            const char *comm = m_trace_events->m_strpool.findstr( hashval );

            item.second.size();
            m_comm_info.push_back( { item.second.size(), comm } );
        }

        // Sort by count of events
        std::sort( m_comm_info.begin(), m_comm_info.end(),
                   [=]( const comm_t &lx, const comm_t &rx )
        {
            return rx.event_count < lx.event_count;
        } );

    }

    if ( reset )
        m_graph_rows_str.clear();
    else
        m_graph_rows_str = m_loader.m_inifile.GetStr( "graph_rows_str", "" );

    if ( m_graph_rows_str.empty() )
    {
        m_graph_rows_str = "# comm and filter expressions to graph\n";
        m_graph_rows_str += "# Show items in gfx/sdma timelines\n";
        m_graph_rows_str += "gfx\n";
        m_graph_rows_str += "sdma0\n";
        m_graph_rows_str += "sdma1\n";
        m_graph_rows_str += "gfx hw\n";
#if 0
        m_graph_rows_str += "print\n\n";
#endif

        m_graph_rows_str += "# $name=fence_signaled\n";
        m_graph_rows_str += "# $id > 100 && $id < 200\n\n";

        m_graph_rows_str += "# comms\n#\n";

        for ( const comm_t &item : m_comm_info )
        {
            m_graph_rows_str += string_format( "#   %lu events:\n%s\n",
                                               item.event_count, item.comm );
        }
    }

    graph_rows_updatelist();
}

void TraceWin::graph_rows_updatelist()
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
    int *pid = m_ts_to_eventid_cache.get_val( ts );
    if ( pid )
        return *pid;

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

    m_ts_to_eventid_cache.set_val( ts, id );
    return id;
}

int TraceWin::timestr_to_eventid( const char *buf, int64_t tsoffset )
{
    int64_t ts = timestr_to_ts( buf, tsoffset );

    return ts_to_eventid( ts );
}

void TraceWin::render_color_picker()
{
    if ( !m_loader.get_opt( OPT_ShowColorPicker ) )
        return;

    if ( !ImGui::CollapsingHeader( "Color Picker", ImGuiTreeNodeFlags_DefaultOpen ) )
        return;

    if ( ImGui::BeginColumns( "color_picker", 2, 0 ) )
        ImGui::SetColumnWidth( 0, imgui_scale( 200.0f ) );

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
    ImGui::NextColumn();

    ImGui::EndColumns();
}

const char *filter_get_key_func( StrPool *strpool, const char *name, size_t len )
{
    return strpool->getstr( name, len );
}

const char *filter_get_keyval_func( const trace_event_t *event, const char *name, char ( &buf )[ 64 ] )
{
    if ( !strcasecmp( name, "name" ) )
    {
        return event->name;
    }
    else if ( !strcasecmp( name, "comm" ) )
    {
        return event->comm;
    }
    else if ( !strcasecmp( name, "user_comm" ) )
    {
        return event->user_comm;
    }
    else if ( !strcasecmp( name, "id" ) )
    {
        snprintf_safe( buf, "%u", event->id );
        return buf;
    }
    else if ( !strcasecmp( name, "pid" ) )
    {
        snprintf_safe( buf, "%d", event->pid );
        return buf;
    }
    else if ( !strcasecmp( name, "ts" ) )
    {
        snprintf_safe( buf, "%.6f", event->ts * ( 1.0 / MSECS_PER_SEC ) );
        return buf;
    }

    for ( const event_field_t &field : event->fields )
    {
        // We can compare pointers since they're from same string pool
        if ( name == field.key )
            return field.value;
    }

    return "";
}

const std::vector< uint32_t > *TraceEvents::get_tdopexpr_locs( const char *name )
{
    std::vector< uint32_t > *plocs;
    uint32_t hashval = fnv_hashstr32( name );

    // Try to find whatever our name hashed to. Name should be something like:
    //   $name=drm_vblank_event
    plocs = m_tdopexpr_locations.get_locations_u32( hashval );
    if ( plocs )
        return plocs;

    // Not found - check if we've tried and failed with this name before.
    if ( m_failed_commands.find( hashval ) != m_failed_commands.end() )
        return NULL;

    // If the name has a tdop expression variable prefix, try compiling it
    if ( strchr( name, '$' ) )
    {
        std::string errstr;
        tdop_get_key_func get_key_func = std::bind( filter_get_key_func, &m_strpool, _1, _2 );
        class TdopExpr *tdop_expr = tdopexpr_compile( name, get_key_func, errstr );

        if ( !tdop_expr )
        {
            logf( "[Error] compiling '%s': %s", name, errstr.c_str() );
        }
        else
        {
            for ( trace_event_t &event : m_events )
            {
                const char *ret;
                tdop_get_keyval_func get_keyval_func = std::bind( filter_get_keyval_func, &event, _1, _2 );

                ret = tdopexpr_exec( tdop_expr, get_keyval_func );
                if ( ret[ 0 ] )
                    m_tdopexpr_locations.add_location_u32( hashval, event.id );
            }

            tdopexpr_delete( tdop_expr );
        }
    }

    // Try to find this name/expression again and add to failed list if we miss again
    plocs = m_tdopexpr_locations.get_locations_u32( hashval );
    if ( !plocs )
        m_failed_commands.insert( hashval );

    return plocs;
}

const std::vector< uint32_t > *TraceEvents::get_comm_locs( const char *name )
{
    return m_comm_locations.get_locations_str( name );
}

const std::vector< uint32_t > *TraceEvents::get_timeline_locs( const char *name )
{
    return m_timeline_locations.get_locations_str( name );
}

// Pass a string like "gfx_249_91446"
const std::vector< uint32_t > *TraceEvents::get_gfxcontext_locs( const char *name )
{
    return m_gfxcontext_locations.get_locations_str( name );
}

TraceWin::TraceWin( TraceLoader &loader, TraceEvents *trace_events, std::string &title ) : m_loader( loader )
{
    // Note that m_trace_events is possibly being loaded in
    //  a background thread at this moment, so be sure to check
    //  m_eventsloaded before accessing it...
    m_trace_events = trace_events;
    m_title = title;

    strcpy_safe( m_timegoto_buf, "0.0" );
    strcpy_safe( m_timeoffset_buf, "0.0" );

    std::string event_filter = m_loader.m_inifile.GetStr( "event_filter_buf", "" );
    if ( !event_filter.empty() )
    {
        strcpy_safe( m_event_filter_buf, event_filter.c_str() );
        m_do_event_filter = true;
    }
}

TraceWin::~TraceWin()
{
    m_loader.m_inifile.PutStr( "event_filter_buf", m_event_filter_buf );
    m_loader.m_inifile.PutStr( "graph_rows_str", m_graph_rows_str.c_str() );
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
        graph_rows_initstr();

        render_time_offset_button_init( *m_trace_events );

        int64_t last_ts = m_trace_events->m_events.back().ts;

        m_do_graph_start_timestr = true;
        m_do_graph_length_timestr = true;
        m_graph_length_ts = std::min< int64_t >( last_ts, 40 * MSECS_PER_SEC );
        m_graph_start_ts = last_ts - m_tsoffset - m_graph_length_ts;
    }

    if ( ImGui::CollapsingHeader( "Events Graph", ImGuiTreeNodeFlags_DefaultOpen ) )
    {
        ImGui::Indent();
        if ( ImGui::CollapsingHeader( "Edit Graph Rows" ) )
        {
            if ( ImGui::Button( "Update Graph Rows" ) )
                graph_rows_updatelist();

            ImGui::SameLine();
            if ( ImGui::Button( "Reset Graph Rows" ) )
                graph_rows_initstr( true );

            m_graph_rows_str.reserve( 8192 );
            ImGui::InputTextMultiline( "##GraphRowsText", &m_graph_rows_str[ 0 ], m_graph_rows_str.capacity(),
                                       ImVec2( -1.0f, ImGui::GetTextLineHeight() * 16 ) );
        }
        ImGui::Unindent();

        if ( imgui_input_text( "Start:", "##GraphStart", m_graphtime_start_buf, 150 ) )
            m_graph_start_ts = timestr_to_ts( m_graphtime_start_buf );

        ImGui::SameLine();
        if ( imgui_input_text( "Length:", "##GraphLength", m_graphtime_length_buf, 150 ) )
            m_graph_length_ts = timestr_to_ts( m_graphtime_length_buf );

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
            strcpy_safe( m_graphtime_start_buf, ts_to_timestr( m_graph_start_ts, 0, 4 ) );
        if ( m_do_graph_length_timestr )
            strcpy_safe( m_graphtime_length_buf, ts_to_timestr( m_graph_length_ts, 0, 4 ) );

        render_process_graph();

        ImGui::Indent();
        render_color_picker();
        ImGui::Unindent();
    }

    ImGuiTreeNodeFlags eventslist_flags = m_loader.get_opt( OPT_ShowEventList) ?
        ImGuiTreeNodeFlags_DefaultOpen : 0;
    m_show_eventlist = ImGui::CollapsingHeader( "Events List", eventslist_flags );
    if ( m_show_eventlist )
    {
        m_do_gotoevent |= imgui_input_int( &m_goto_eventid, 75.0f, "Goto Event:", "##GotoEvent" );

        ImGui::SameLine();
        if ( imgui_input_text( "Goto Time:", "##GotoTime", m_timegoto_buf, 150 ) )
        {
            m_do_gotoevent = true;
            m_goto_eventid = timestr_to_eventid( m_timegoto_buf, m_tsoffset );
        }

        ImGui::SameLine();
        if ( imgui_input_text( "Time Offset:", "##TimeOffset", m_timeoffset_buf, 150 ) )
            m_tsoffset = timestr_to_ts( m_timeoffset_buf );

        if ( m_do_event_filter ||
             imgui_input_text( "Event Filter:", "##Event Filter", m_event_filter_buf, 500,
                               ImGuiInputTextFlags_EnterReturnsTrue ) )
        {
            m_filtered_events.clear();
            m_filtered_events_str.clear();
            m_do_event_filter = false;

            if ( m_event_filter_buf[ 0 ] )
            {
                tdop_get_key_func get_key_func = std::bind( filter_get_key_func, &m_trace_events->m_strpool, _1, _2 );
                class TdopExpr *tdop_expr = tdopexpr_compile( m_event_filter_buf, get_key_func, m_filtered_events_str );

                util_time_t t0 = util_get_time();

                if ( tdop_expr )
                {
                    for ( trace_event_t &event : m_trace_events->m_events )
                    {
                        tdop_get_keyval_func get_keyval_func = std::bind( filter_get_keyval_func, &event, _1, _2 );

                        const char *ret = tdopexpr_exec( tdop_expr, get_keyval_func );

                        event.is_filtered_out = !ret[ 0 ];
                        if ( !event.is_filtered_out )
                            m_filtered_events.push_back( event.id );
                    }

                    if ( m_filtered_events.empty() )
                        m_filtered_events_str = "WARNING: No events found.";

                    tdopexpr_delete( tdop_expr );
                    tdop_expr = NULL;
                }

                float time = util_time_to_ms( t0, util_get_time() );
                if ( time > 1000.0f )
                    logf( "tdopexpr_compile(\"%s\"): %.2fms\n", m_event_filter_buf, time );
            }
        }

        if ( ImGui::IsItemHovered() )
        {
            std::string tooltip;

            tooltip += multi_text_color::yellow.m_str( "Event Filter\n\n" );
            tooltip += "Vars: Any field in Info column plus:\n";
            tooltip += "      $name, $comm, $user_comm, $id, $pid, $ts\n";
            tooltip += "Operators: &&, ||, !=, =, >, >=, <, <=, =~\n\n";

            tooltip += "Examples:\n";
            tooltip += "  $pid = 4615\n";
            tooltip += "  $ts >= 11.1 && $ts < 12.5\n";
            tooltip += "  $ring_name = 0xffff971e9aa6bdd0\n";
            tooltip += "  $buf =~ \"[Compositor] Warp\"\n";
            tooltip += "  ( $timeline = gfx ) && ( $id < 10 || $id > 100 )";

            imgui_set_tooltip( tooltip );
        }

        ImGui::SameLine();
        if ( ImGui::SmallButton( "Clear Filter" ) )
        {
            m_filtered_events.clear();
            m_filtered_events_str.clear();
            m_event_filter_buf[ 0 ] = 0;
        }

        if ( !m_filtered_events_str.empty() )
        {
            ImGui::SameLine();
            ImGui::TextColored( ImVec4( 1, 0, 0, 1 ), "%s", m_filtered_events_str.c_str() );
        }
        else if ( !m_filtered_events.empty() )
        {
            std::string label = string_format( "Graph only filtered (%lu events)", m_filtered_events.size() );

            ImGui::SameLine();
            bool val = !!m_loader.get_opt( OPT_GraphOnlyFiltered );
            if ( ImGui::Checkbox( label.c_str(), &val ) )
                m_loader.m_options[ OPT_GraphOnlyFiltered ].val = val;
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

        if ( !m_comm_info.empty() )
        {
            if ( ImGui::CollapsingHeader( "Comm Info", ImGuiTreeNodeFlags_DefaultOpen ) )
            {
                if ( imgui_begin_columns( "comm_info", { "Comm", "Events" } ) )
                    ImGui::SetColumnWidth( 0, imgui_scale( 200.0f ) );

                for ( const comm_t &info : m_comm_info )
                {
                    ImGui::Text( "%s", info.comm );
                    ImGui::NextColumn();
                    ImGui::Text( "%lu", info.event_count );
                    ImGui::NextColumn();
                }

                ImGui::EndColumns();
            }
        }

        if ( !trace_info.cpustats.empty() )
        {
            if ( ImGui::CollapsingHeader( "CPU Info" ) )
            {
                if ( imgui_begin_columns( "cpu_stats", { "CPU", "Stats" } ) )
                    ImGui::SetColumnWidth( 0, imgui_scale( 200.0f ) );

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

                ImGui::EndColumns();
            }
        }

        ImGui::Unindent();
    }
}

bool TraceWin::render_events_list_popup( uint32_t eventid )
{
    if ( !ImGui::BeginPopup( "EventsListPopup" ) )
        return false;

    imgui_text_bg( "Options", ImGui::GetColorVec4( ImGuiCol_Header ) );
    ImGui::Separator();

    trace_event_t &event = get_event( eventid );

    std::string label = string_format( "Center event %u on graph", event.id );
    if ( ImGui::MenuItem( label.c_str() ) )
    {
        m_selected_eventid = event.id;
        m_graph_start_ts = event.ts - m_tsoffset - m_graph_length_ts / 2;
        m_do_graph_start_timestr = true;
    }

    label = string_format( "Set Time Offset to %s", ts_to_timestr( event.ts ).c_str() );
    if ( ImGui::MenuItem( label.c_str() ) )
    {
        m_tsoffset = event.ts;
        strcpy_safe( m_timeoffset_buf, ts_to_timestr( m_tsoffset ) );
    }

    ImGui::Separator();

    label = string_format( "Filter pid %d events", event.pid );
    if ( ImGui::MenuItem( label.c_str() ) )
    {
        snprintf_safe( m_event_filter_buf, "$pid == %d", event.pid );
        m_do_event_filter = true;
    }

    label = string_format( "Filter '%s' events", event.name );
    if ( ImGui::MenuItem( label.c_str() ) )
    {
        snprintf_safe( m_event_filter_buf, "$name == %s", event.name );
        m_do_event_filter = true;
    }

    if ( !m_filtered_events.empty() && ImGui::MenuItem( "Clear Filter" ) )
    {
        m_event_filter_buf[ 0 ] = 0;
        m_do_event_filter = true;
    }

    ImGui::EndPopup();
    return true;
}

std::string get_event_fields_str( const trace_event_t &event, const char *eqstr, char sep )
{
    std::string fieldstr;
    const std::vector< event_field_t > &fields = event.fields;

    if ( event.user_comm != event.comm )
        fieldstr += string_format( "%s%s%s%c", "user_comm", eqstr, event.user_comm, sep );

    for ( const event_field_t &field : fields )
    {
        std::string str = string_format( "%s%s%s%c", field.key, eqstr, field.value, sep );

        if ( event.is_ftrace_print() && !strcmp( field.key, "buf" ) )
            fieldstr += multi_text_color::yellow.m_str( str.c_str() );
        else
            fieldstr += str;
    }

    fieldstr += string_format( "%s%s%s", "system", eqstr, event.system );

    return fieldstr;
}

static float get_keyboard_scroll_lines( float visible_rows )
{
    float scroll_lines = 0.0f;

    if ( ImGui::IsWindowFocused() )
    {
        if ( imgui_key_pressed( ImGuiKey_PageDown ) )
            scroll_lines = std::max< float>( visible_rows - 5, 1 );
        else if ( imgui_key_pressed( ImGuiKey_PageUp ) )
            scroll_lines = std::min< float >( -visible_rows, -1 );
        else if ( imgui_key_pressed( ImGuiKey_DownArrow ) )
            scroll_lines = 1;
        else if ( imgui_key_pressed( ImGuiKey_UpArrow ) )
            scroll_lines = -1;
        else if ( imgui_key_pressed( ImGuiKey_Home ) )
            scroll_lines = -ImGui::GetScrollMaxY();
        else if ( imgui_key_pressed( ImGuiKey_End ) )
            scroll_lines = ImGui::GetScrollMaxY();
    }

    return scroll_lines;
}


bool TraceWin::handle_event_list_mouse( const trace_event_t &event, uint32_t i )
{
    bool popup_shown = false;

    // Check if item is hovered and we don't have a popup menu going already.
    if ( !is_valid_id( m_events_list_popup_eventid ) &&
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
            std::string ts_str = ts_to_timestr( event.ts, m_tsoffset );
            std::string fieldstr = get_event_fields_str( event, ": ", '\n' );

            imgui_set_tooltip( string_format( "Id: %u\nTime: %s\nComm: %s\n%s",
                               event.id, ts_str.c_str(), event.comm, fieldstr.c_str() ) );
        }
    }

    // If we've got an active popup menu, render it.
    if ( m_events_list_popup_eventid == i )
    {
        uint32_t eventid = !m_filtered_events.empty() ?
                    m_filtered_events[ m_events_list_popup_eventid ] :
                    m_events_list_popup_eventid;

        imgui_pop_smallfont();

        if ( !TraceWin::render_events_list_popup( eventid ) )
            m_events_list_popup_eventid = INVALID_ID;

        imgui_push_smallfont();
        popup_shown = true;
    }

    return popup_shown;
}

static void draw_ts_line( const ImVec2 &pos )
{
    ImGui::PopClipRect();

    float max_x = ImGui::GetWindowClipRectMax().x;
    float spacing_U = ( float )( int )( ImGui::GetStyle().ItemSpacing.y * 0.5f );
    float pos_y = pos.y - spacing_U;

    ImGui::GetWindowDrawList()->AddLine(
                ImVec2( pos.x, pos_y ), ImVec2( max_x, pos_y ),
                col_get( col_MousePos ), imgui_scale( 2.0f ) );

    ImGui::PushColumnClipRect();
}

void TraceWin::render_events_list( CIniFile &inifile )
{
    std::vector< trace_event_t > &events = m_trace_events->m_events;
    size_t event_count = m_filtered_events.empty() ?
                events.size() : m_filtered_events.size();

    // Set focus on event list first time we open.
    if ( !m_inited && ImGui::IsWindowFocused() )
        ImGui::SetNextWindowFocus();

    // Events list
    imgui_push_smallfont();

    {
        float lineh = ImGui::GetTextLineHeightWithSpacing();
        const ImVec2 content_avail = ImGui::GetContentRegionAvail();

        int eventlist_row_count = m_loader.get_opt( OPT_EventListRowCount );

        // If the user has set the event list row count to 0 (auto size), make
        //  sure we always have at least 20 rows.
        if ( !eventlist_row_count && ( content_avail.y / lineh < 20 ) )
            eventlist_row_count = 20;

        // Set the child window size to hold count of items + header + separator
        float sizey = eventlist_row_count * lineh;

        ImGui::SetNextWindowContentSize( { 0.0f, ( event_count + 1 ) * lineh + 1 } );
        ImGui::BeginChild( "eventlistbox", ImVec2( 0.0f, sizey ) );

        float winh = ImGui::GetWindowHeight();
        uint32_t visible_rows = ( winh + 1 ) / lineh;

        float scroll_lines = get_keyboard_scroll_lines( visible_rows );
        if ( scroll_lines )
            ImGui::SetScrollY( ImGui::GetScrollY() + scroll_lines * lineh );

        bool filtered_events = !m_filtered_events.empty();

        if ( m_do_gotoevent )
        {
            uint32_t pos;

            if ( filtered_events )
            {
                auto i = std::lower_bound( m_filtered_events.begin(), m_filtered_events.end(), m_goto_eventid );

                pos = i - m_filtered_events.begin();
            }
            else
            {
                pos = m_goto_eventid;
            }

            pos = std::min< uint32_t >( pos, event_count - 1 );

            // Only scroll if our goto event isn't visible.
            if ( ( uint32_t )m_goto_eventid <= m_eventlist_start_eventid )
            {
                float scrolly = std::max< int >( 0, pos ) * lineh;

                ImGui::SetScrollY( scrolly );
            }
            else if ( ( uint32_t )m_goto_eventid + 1 >= m_eventlist_end_eventid )
            {
                float scrolly = std::max< int >( 0, pos - visible_rows + 1 ) * lineh;

                ImGui::SetScrollY( scrolly );
            }

            // Select the event also
            m_selected_eventid = m_goto_eventid;

            m_do_gotoevent = false;
        }

        float scrolly = ImGui::GetScrollY();
        uint32_t start_idx = std::max< float >( scrolly / lineh, 1.0f ) - 1;
        uint32_t end_idx = std::min< uint32_t >( start_idx + 2 + visible_rows, event_count );

        // Draw columns
        imgui_begin_columns( "event_list", { "Id", "Time Stamp", "Task", "Event", "context", "Info" },
                             &inifile, &m_columns_eventlist_resized );
        {
            bool popup_shown = false;
            int64_t ts_marker_diff = 0;

            // Reset our hovered event id
            m_hovered_eventlist_eventid = INVALID_ID;

            // Move cursor position down to where we've scrolled.
            if ( start_idx > 0 )
                ImGui::SetCursorPosY( ImGui::GetCursorPosY() + lineh * ( start_idx - 1 ) );

            if ( filtered_events )
            {
                m_eventlist_start_eventid = m_filtered_events[ start_idx ];
                m_eventlist_end_eventid = m_filtered_events[ end_idx - 1 ];
            }
            else
            {
                m_eventlist_start_eventid = start_idx;
                m_eventlist_end_eventid = end_idx;
            }

            // Loop through and draw events
            for ( uint32_t i = start_idx; i < end_idx; i++ )
            {
                trace_event_t &event = filtered_events ?
                            m_trace_events->m_events[ m_filtered_events[ i ] ] :
                            m_trace_events->m_events[ i ];
                bool selected = ( m_selected_eventid == event.id );
                ImVec2 cursorpos = ImGui::GetCursorScreenPos();

                ImGui::PushID( i );

                if ( event.is_vblank() )
                {
                    // If this is a vblank event, draw the text in blue or red vblank colors
                    ImColor col = col_get( ( event.crtc > 0 ) ? col_VBlank1 : col_VBlank0 );

                    ImGui::PushStyleColor( ImGuiCol_Text, col );
                }

                // If this event is in the highlighted list, give it a bit of a colored background
                bool highlight = !selected && std::binary_search(
                            m_highlight_ids.begin(), m_highlight_ids.end(), event.id );
                if ( highlight )
                {
                    const ImVec4 &col = ImGui::GetColorVec4( ImGuiCol_PlotLinesHovered );

                    ImGui::PushStyleColor( ImGuiCol_Header, ImVec4( col.x, col.y, col.z, 0.4f ) );
                }

                // column 0: event id
                {
                    if ( ImGui::Selectable( std::to_string( event.id ).c_str(),
                                            highlight || selected, ImGuiSelectableFlags_SpanAllColumns ) )
                    {
                        m_selected_eventid = event.id;
                    }

                    // Columns bug workaround: selectable with SpanAllColumns & overlaid button does not work.
                    // https://github.com/ocornut/imgui/issues/684
                    ImGui::SetItemAllowOverlap();

                    // Handle popup menu / tooltip
                    popup_shown |= handle_event_list_mouse( event, i );

                    ImGui::NextColumn();
                }

                // column 1: time stamp
                {
                    std::string ts_str = ts_to_timestr( event.ts, m_tsoffset ) + "ms";

                    if ( event.id > 0 )
                    {
                        // Add time delta from previous event
                        const trace_event_t &event0 = get_event( event.id - 1 );
                        ts_str += " (+" + ts_to_timestr( event.ts - event0.ts, 0, 4 ) + ")";
                    }

                    ImGui::Text( "%s", ts_str.c_str() );
                    ImGui::NextColumn();
                }

                // column 2: comm
                {
                    ImGui::Text( "%s (%u)", event.comm, event.cpu );
                    ImGui::NextColumn();
                }

                // column 3: event name
                {
                    ImGui::Text( "%s", event.name );
                    ImGui::NextColumn();
                }

                // column 4: gfx context
                {
                    ImGui::Text( "%s", get_event_gfxcontext_str( event ).c_str() );
                    ImGui::NextColumn();
                }

                // column 5: event fields
                {
                    if ( event.is_ftrace_print() )
                    {
                        ImGui::TextColored( ImVec4( 1, 1, 0, 1 ), "%s", get_event_field_val( event.fields, "buf" ) );
                    }
                    else
                    {
                        std::string fieldstr = get_event_fields_str( event, "=", ' ' );

                        ImGui::Text( "%s", fieldstr.c_str() );
                    }
                    ImGui::NextColumn();
                }

                // Draw time stamp marker diff line if we're right below m_ts_marker
                {
                    int64_t ts_diff = event.ts - m_ts_marker;
                    bool do_draw_ts_line = ( ts_marker_diff < 0 ) && ( ts_diff > 0 );

                    if ( do_draw_ts_line )
                        draw_ts_line( cursorpos );

                    ts_marker_diff = ts_diff;
                }

                ImGui::PopStyleColor( event.is_vblank() + highlight );
                ImGui::PopID();
            }

            if ( !popup_shown )
            {
                // When we modify a filter via the context menu, it can hide the item
                //  we right clicked on which means render_events_list_popup() won't get
                //  called. Check for that case here.
                m_events_list_popup_eventid = INVALID_ID;
            }
        }
        if ( ImGui::EndColumns() )
            m_columns_eventlist_resized = true;

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

    strcpy_safe( m_trace_file, "trace.dat" );
}

void TraceConsole::shutdown( CIniFile *inifile )
{
    inifile->PutVec4( "clearcolor", m_clear_color );

    m_history.clear();
}

void TraceConsole::render_options( TraceLoader &loader )
{
    // Align text to upcoming widgets
    ImGui::AlignFirstTextHeightToWidgets();
    ImGui::Text( "Clear Color:" );
    ImGui::SameLine();
    ImGui::ColorEdit3( "", ( float * )&m_clear_color );

    ImGui::Separator();

    // Align text to upcoming widgets
    ImGui::AlignFirstTextHeightToWidgets();
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

    for ( size_t i = 0; i < loader.m_options.size(); i++ )
    {
        TraceLoader::option_t &opt = loader.m_options[ i ];

        if ( ( i >= OPT_RenderCrtc0 ) &&
             ( i <= OPT_RenderCrtc9 ) )
        {
            if ( i - OPT_RenderCrtc0 > loader.m_crtc_max )
                continue;
        }

        ImGui::PushID( i );

        if ( opt.type == TraceLoader::OPT_Bool )
        {
            ImGui::CheckboxInt( opt.desc.c_str(), &opt.val );
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

            ImVec4 col = ImVec4( 1, 1, 1, 1 );

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
    ImGui::SetNextWindowSize( ImVec2( 720, 800 ), ImGuiSetCond_FirstUseEver );

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

        // Align text to upcoming widgets
        ImGui::AlignFirstTextHeightToWidgets();
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
        ImGui::BeginColumns( "event_files", 2 );

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

        ImGui::EndColumns();
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
                loader.m_options[ OPT_Fullscreen ].val = true;
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
    strcpy_safe( console.m_trace_file, filename );

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

    parse_cmdline( loader, argc, argv );

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

    // Setup imgui default text color
    multi_text_color::def.set( ImGui::GetColorVec4( ImGuiCol_Text ) );

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

        imgui_render_tooltip();

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
