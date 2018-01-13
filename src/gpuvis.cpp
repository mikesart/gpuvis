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
#include <set>
#include <string>
#include <array>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <functional>
#include <sys/stat.h>

#define SDL_MAIN_HANDLED
#include <SDL.h>

#define YA_GETOPT_NO_COMPAT_MACRO
#include "ya_getopt.h"

#include "GL/gl3w.h"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"   // BeginColumns(), EndColumns(), PushColumnClipRect()
#include "imgui/imgui_impl_sdl_gl3.h"

#include "gpuvis_macros.h"

#include "tdopexpr.h"
#include "trace-cmd/trace-read.h"

#include "stlini.h"
#include "gpuvis_utils.h"
#include "gpuvis.h"

#include "miniz.h"

// https://github.com/ocornut/imgui/issues/88
#if defined( __APPLE__ )
  #define NOC_FILE_DIALOG_IMPLEMENTATION
  #define NOC_FILE_DIALOG_OSX
  #include "noc_file_dialog.h"
#elif defined( USE_GTK3 )
  #define NOC_FILE_DIALOG_IMPLEMENTATION
  #define NOC_FILE_DIALOG_GTK
  #include "noc_file_dialog.h"
#elif defined( WIN32 )
  #define NOC_FILE_DIALOG_IMPLEMENTATION
  #define NOC_FILE_DIALOG_WIN32
  #include "noc_file_dialog.h"
#endif

MainApp &s_app()
{
    static MainApp s_app;
    return s_app;
}

CIniFile &s_ini()
{
    static CIniFile s_inifile;
    return s_inifile;
}

Opts &s_opts()
{
    static Opts s_opts;
    return s_opts;
}

Clrs &s_clrs()
{
    static Clrs s_clrs;
    return s_clrs;
}

TextClrs &s_textclrs()
{
    static TextClrs s_textclrs;
    return s_textclrs;
}

Keybd &s_keybd()
{
    static Keybd s_keybd;
    return s_keybd;
}

Actions &s_actions()
{
    static Actions s_actions;
    return s_actions;
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

const char *StrPool::getstrf( const char *fmt, ... )
{
    va_list args;
    char buf[ 512 ];

    va_start( args, fmt );
    vsnprintf_safe( buf, fmt, args );
    va_end( args );

    return getstr( buf );
}

uint32_t StrPool::getu32( const char *str, size_t len )
{
    uint32_t hashval = fnv_hashstr32( str, len );
    const std::string *ret = m_pool.get_val( hashval );

    if ( !ret )
    {
        if ( len == ( size_t )-1 )
            len = strlen( str );
        m_pool.get_val( hashval, std::string( str, len ) );
    }

    return hashval;
}

uint32_t StrPool::getu32f( const char *fmt, ... )
{
    va_list args;
    char buf[ 512 ];

    va_start( args, fmt );
    vsnprintf_safe( buf, fmt, args );
    va_end( args );

    return getu32( buf );
}

const char *StrPool::findstr( uint32_t hashval )
{
    std::string *str = m_pool.get_val( hashval );

    return str ? str->c_str() : NULL;
}

/*
 * TraceLocationsRingCtxSeq
 */
uint64_t TraceLocationsRingCtxSeq::db_key( const char *ringstr, uint32_t seqno, const char *ctxstr )
{
    uint32_t ctx = strtoul( ctxstr, NULL, 10 );
    uint32_t ring = strtoul( ringstr, NULL, 10 );

    // ring | ctx      | seqno
    //  0xe | 1fffffff | ffffffff
    return ( ( uint64_t )ring << 61 ) | ( ( uint64_t )ctx << 32 ) | seqno;
}

uint64_t TraceLocationsRingCtxSeq::db_key( const trace_event_t &event )
{
    if ( event.seqno )
    {
        const char *ringstr = get_event_field_val( event, "ring", NULL );
        const char *ctxstr = get_event_field_val( event, "ctx", NULL );

        // i915:intel_engine_notify has only ring & seqno, so default ctx to "0"
        if ( !ctxstr && !strcmp( event.name, "intel_engine_notify" ) )
            ctxstr = "0";

        if ( ringstr && ctxstr )
            return db_key( ringstr, event.seqno, ctxstr );
    }

    return 0;
}

bool TraceLocationsRingCtxSeq::add_location( const trace_event_t &event )
{
    uint64_t key = db_key( event );

    if ( key )
    {
        std::vector< uint32_t > *plocs = m_locs.get_val_create( key );

        plocs->push_back( event.id );
        return true;
    }

    return false;
}

std::vector< uint32_t > *TraceLocationsRingCtxSeq::get_locations( const trace_event_t &event )
{
    uint64_t key = db_key( event );

    return m_locs.get_val( key );
}

std::vector< uint32_t > *TraceLocationsRingCtxSeq::get_locations( const char *ringstr, uint32_t seqno, const char *ctxstr )
{
    uint64_t key = db_key( ringstr, seqno, ctxstr );

    return m_locs.get_val( key );
}

/*
 * Opts
 */
void Opts::init_opt_bool( option_id_t optid, const char *description, const char *key,
                    bool defval, OPT_Flags flags )
{
    option_t &opt = m_options[ optid ];

    opt.flags = OPT_Bool | flags;
    opt.desc = description;
    opt.inikey = key;
    opt.inisection = "$options$";
    opt.valf = defval;
}

void Opts::init_opt( option_id_t optid, const char *description, const char *key,
               float defval, float minval, float maxval, OPT_Flags flags )
{
    option_t &opt = m_options[ optid ];

    opt.flags = flags;
    opt.desc = description;
    opt.inikey = key;
    opt.inisection = "$options$";
    opt.valf = defval;
    opt.valf_min = minval;
    opt.valf_max = maxval;
}

void Opts::init()
{
    m_options.resize( OPT_PresetMax );

    init_opt_bool( OPT_TimelineLabels, "Show gfx timeline labels", "timeline_gfx_labels", true );
    init_opt_bool( OPT_TimelineEvents, "Show gfx timeline events", "timeline_gfx_events", true );
    init_opt_bool( OPT_TimelineRenderUserSpace, "Show gfx timeline userspace", "timeline_gfx_userspace", false );
    init_opt_bool( OPT_PrintTimelineLabels, "Show print timeline labels", "print_timeline_gfx_labels", true );
    init_opt_bool( OPT_GraphOnlyFiltered, "Graph only filtered events", "graph_only_filtered", true );
    init_opt_bool( OPT_Graph_HideEmptyFilteredRows, "Hide empty filtered comm rows", "hide_empty_filtered_rows", true );
    init_opt_bool( OPT_ShowEventList, "Toggle showing event list", "show_event_list", true );
    init_opt_bool( OPT_SyncEventListToGraph, "Sync event list to graph mouse location", "sync_eventlist_to_graph", true );
    init_opt_bool( OPT_HideSchedSwitchEvents, "Hide sched_switch events", "hide_sched_switch_events", true );
    init_opt_bool( OPT_ShowFps, "Show frame rate", "show_fps", false );
    init_opt_bool( OPT_VerticalSync, "Vertical sync", "vertical_sync", true );

    m_options[ OPT_ShowEventList ].action = action_toggle_show_eventlist;

    init_opt( OPT_GraphHeight, "Graph Size: %.1f", "graph_height", 0, 0, 1, OPT_Float | OPT_Hidden );
    init_opt( OPT_GraphHeightZoomed, "Zoomed Graph Size: %.1f", "graph_height_zoomed", 0, 0, 1, OPT_Float | OPT_Hidden );
    init_opt( OPT_EventListRowCount, "Event List Size: %.0f", "eventlist_rows", 0, 0, 100, OPT_Int | OPT_Hidden );
    init_opt( OPT_Scale, "Font Scale: %.1f", "scale", 2.0f, 0.25f, 6.0f, OPT_Float | OPT_Hidden );
    init_opt_bool( OPT_TrimTrace, "Trim Trace to align CPU buffers", "trim_trace_to_cpu_buffers", true, OPT_Hidden );
    init_opt_bool( OPT_UseFreetype, "Use Freetype", "use_freetype", true, OPT_Hidden );

    for ( uint32_t i = OPT_RenderCrtc0; i <= OPT_RenderCrtc9; i++ )
    {
        const std::string desc = string_format( "Show vblank crtc%d markers", i - OPT_RenderCrtc0 );
        const std::string inikey = string_format( "render_crtc%d", i - OPT_RenderCrtc0 );

        init_opt_bool( i, desc.c_str(), inikey.c_str(), true );
    }
    init_opt_bool( OPT_RenderFrameMarkers, "Show render frame markers", "render_framemarkers", true );

    // Set up action mappings so we can display hotkeys in render_imgui_opt().
    m_options[ OPT_RenderCrtc0 ].action = action_toggle_vblank0;
    m_options[ OPT_RenderCrtc1 ].action = action_toggle_vblank1;
    m_options[ OPT_RenderFrameMarkers ].action = action_toggle_framemarkers;

    add_opt_graph_rowsize( "gfx", 8 );

    // Default sizes for these rows are in get_comm_option_id() in gpuvis_graph.cpp...
    //   add_opt_graph_rowsize( "print", 10 );
    //   add_opt_graph_rowsize( "i915_req ring0", 8 );
    //   add_opt_graph_rowsize( "i915_reqwait ring0", 2, 2 );

    // Create all the entries for the compute shader rows
    // for ( uint32_t val = 0; ; val++ )
    // {
    //     std::string str = comp_str_create_val( val );
    //     if ( str.empty() )
    //         break;
    //
    //     add_opt_graph_rowsize( str.c_str() );
    // }

    // Read option values stored in ini file
    for ( size_t i = 0; i < m_options.size(); i++ )
    {
        option_t &opt = m_options[ i ];

        opt.valf = s_ini().GetFloat( opt.inikey.c_str(), opt.valf, opt.inisection.c_str() );
    }
}

void Opts::shutdown()
{
    for ( size_t i = 0; i < m_options.size(); i++ )
    {
        const option_t &opt = m_options[ i ];

        if ( opt.flags & OPT_Int )
            s_ini().PutInt( opt.inikey.c_str(), ( int )opt.valf, opt.inisection.c_str() );
        else if ( opt.flags & OPT_Bool )
            s_ini().PutInt( opt.inikey.c_str(), opt.valf ? 1 : 0, opt.inisection.c_str() );
        else
            s_ini().PutFloat( opt.inikey.c_str(), opt.valf, opt.inisection.c_str() );
    }
}

option_id_t Opts::add_opt_graph_rowsize( const char *row_name, int defval, int minval )
{
    option_t opt;
    const char *fullname = row_name;

    if ( !strncmp( row_name, "plot:", 5 ) )
        row_name = fullname + 5;

    opt.flags = OPT_Int | OPT_Hidden;
    opt.desc = string_format( "Row height: %%.0f" );
    opt.inikey = row_name;
    opt.inisection = "$row_sizes$";
    opt.valf = s_ini().GetInt( opt.inikey.c_str(), defval, opt.inisection.c_str() );
    opt.valf_min = minval;
    opt.valf_max = MAX_ROW_SIZE;

    // Upper case first letter in description
    opt.desc[ 0 ] = toupper( opt.desc[ 0 ] );

    option_id_t optid = m_options.size();
    m_options.push_back( opt );
    m_graph_rowname_optid_map.m_map[ fullname ] = optid;

    return optid;
}

option_id_t Opts::get_opt_graph_rowsize_id( const std::string &row_name )
{
    option_id_t *optid = m_graph_rowname_optid_map.get_val( row_name );

    return optid ? *optid : OPT_Invalid;
}

int Opts::geti( option_id_t optid )
{
    assert( m_options[ optid ].flags & OPT_Int );

    return ( int )m_options[ optid ].valf;
}

bool Opts::getb( option_id_t optid )
{
    assert( m_options[ optid ].flags & OPT_Bool );

    return ( m_options[ optid ].valf != 0.0f );
}

float Opts::getf( option_id_t optid )
{
    assert( !( m_options[ optid ].flags & ( OPT_Int | OPT_Bool ) ) );

    return m_options[ optid ].valf;
}

bool Opts::getcrtc( int crtc )
{
    uint32_t val = crtc + OPT_RenderCrtc0;

    return ( val <= OPT_RenderCrtc9 ) ? getb( val ) : false;
}

void Opts::setf( option_id_t optid, float valf, float valf_min, float valf_max )
{
    m_options[ optid ].valf = valf;

    if ( valf_min != FLT_MAX )
        m_options[ optid ].valf_min = valf_min;

    if ( valf_max != FLT_MAX )
        m_options[ optid ].valf_max = valf_max;
}

void Opts::setb( option_id_t optid, bool valb )
{
    assert( m_options[ optid ].flags & OPT_Bool );

    m_options[ optid ].valf = valb ? 1.0f : 0.0f;
}

void Opts::setdesc( option_id_t optid, const std::string &desc )
{
    m_options[ optid ].desc = desc;
}

bool Opts::render_imgui_opt( option_id_t optid, float w )
{
    bool changed = false;
    option_t &opt = m_options[ optid ];

    ImGui::PushID( optid );

    if ( opt.flags & OPT_Bool )
    {
        bool val = !!opt.valf;
        std::string desc = opt.desc;

        if ( ( optid == OPT_RenderCrtc0 ) || ( optid == OPT_RenderCrtc1 ) )
        {
            // Quick hack to color the vblank string.
            const char *vblankstr = " vblank ";
            ImU32 color = col_VBlank0 + ( optid - OPT_RenderCrtc0 );
            std::string str = s_textclrs().mstr( vblankstr, s_clrs().get( color ) );

            string_replace_str( desc, vblankstr, str );
        }

        changed = ImGui::Checkbox( desc.c_str(), &val );

        if ( opt.action != action_nil )
        {
            ImGui::SameLine();
            ImGui::TextDisabled( "%s", s_actions().hotkey_str( opt.action ).c_str() );
        }

        if ( changed )
            opt.valf = val;
    }
    else
    {
        ImGui::PushItemWidth( imgui_scale( w ) );
        changed = ImGui::SliderFloat( "##opt_valf", &opt.valf, opt.valf_min, opt.valf_max, opt.desc.c_str() );
        ImGui::PopItemWidth();
    }

    ImGui::PopID();

    return changed;
}

void Opts::render_imgui_options()
{
    for ( size_t i = 0; i < m_options.size(); i++ )
    {
        if ( m_options[ i ].flags & OPT_Hidden )
            continue;

        if ( ( i >= OPT_RenderCrtc0 ) && ( i <= OPT_RenderCrtc9 ) )
        {
            int crtc = ( int )( i - OPT_RenderCrtc0 );

            if ( crtc > m_crtc_max )
                continue;
        }

        render_imgui_opt( i );
    }
}

/*
 * MainApp
 */
MainApp::state_t MainApp::get_state()
{
    return ( state_t )SDL_AtomicGet( &m_loading_info.state );
}

bool MainApp::is_trace_loaded()
{
    return m_trace_win && ( m_trace_win->m_trace_events.get_load_status() == TraceEvents::Trace_Loaded );
}

void MainApp::set_state( state_t state, const char *filename )
{
    if ( state == State_Loading )
        m_loading_info.filename = filename;
    else
        m_loading_info.filename.clear();

    m_loading_info.win = NULL;
    m_loading_info.thread = NULL;

    SDL_AtomicSet( &m_loading_info.state, state );
}

void MainApp::cancel_load_file()
{
    // Switch to cancel loading if we're currently loading
    SDL_AtomicCAS( &m_loading_info.state, State_Loading, State_CancelLoading );
}

static std::string unzip_first_file( const char *zipfile )
{
    std::string ret;
    mz_zip_archive zip_archive;

    memset( &zip_archive, 0, sizeof( zip_archive ) );

    if ( mz_zip_reader_init_file( &zip_archive, zipfile, 0 ) )
    {
        mz_uint fileCount = mz_zip_reader_get_num_files( &zip_archive );

        if ( fileCount )
        {
            mz_zip_archive_file_stat file_stat;

            if ( mz_zip_reader_file_stat( &zip_archive, 0, &file_stat ) )
            {
                for ( mz_uint i = 0; i < fileCount; i++ )
                {
                    if ( !mz_zip_reader_file_stat( &zip_archive, i, &file_stat ) )
                        continue;

                    if ( file_stat.m_is_directory )
                        continue;

                    const char *filename = util_basename( file_stat.m_filename );
                    ret = string_format( "%s_%s", std::tmpnam( NULL ), filename );
                    if ( mz_zip_reader_extract_to_file( &zip_archive, i, ret.c_str(), 0 ) )
                        break;

                    ret.clear();
                }
            }
        }

        mz_zip_reader_end( &zip_archive );
    }

    return ret;
}

bool MainApp::load_file( const char *filename )
{
    std::string tmpfile;
    const char *ext = strrchr( filename, '.' );

    if ( get_state() != State_Idle )
    {
        logf( "[Error] %s failed, currently loading %s.", __func__, m_loading_info.filename.c_str() );
        return false;
    }

    if ( ext && !strcmp( ext, ".zip" ) )
    {
        tmpfile = unzip_first_file( filename );

        if ( !tmpfile.empty() )
            filename = tmpfile.c_str();
    }

    size_t filesize = get_file_size( filename );
    if ( !filesize )
    {
        logf( "[Error] %s (%s) failed: %s", __func__, filename, strerror( errno ) );
        return false;
    }

    set_state( State_Loading, filename );

    delete m_trace_win;
    m_trace_win = new TraceWin( filename, filesize );

    m_loading_info.win = m_trace_win;
    m_loading_info.thread = SDL_CreateThread( thread_func, "eventloader", &m_loading_info );
    if ( !m_loading_info.thread )
    {
        logf( "[Error] %s: SDL_CreateThread failed.", __func__ );

        delete m_trace_win;
        m_trace_win = NULL;

        set_state( State_Idle );
        return false;
    }

    return true;
}

// See notes at top of gpuvis_graph.cpp for explanation of these events.
static bool is_amd_timeline_event( const trace_event_t &event )
{
    if ( !event.seqno )
        return false;

    const char *context = get_event_field_val( event, "context", NULL );
    const char *timeline = get_event_field_val( event, "timeline", NULL );

    if ( !context || !timeline )
        return false;

    return ( event.is_fence_signaled() ||
             !strcmp( event.name, "amdgpu_cs_ioctl" ) ||
             !strcmp( event.name, "amdgpu_sched_run_job" ) );
}

static void add_sched_switch_pid_comm( trace_info_t &trace_info, const trace_event_t &event,
                                       const char *pidstr, const char *commstr )
{
    int pid = atoi( get_event_field_val( event, pidstr ) );

    if ( pid )
    {
        const char *comm = get_event_field_val( event, commstr );

        // If this pid is not in our pid_comm map or it is a sched_switch
        //  pid we already added, then map the pid -> the latest comm value
        if ( !trace_info.pid_comm_map.get_val( pid ) ||
             trace_info.sched_switch_pid_comm_map.get_val( pid ) )
        {
            // Add to pid_comm map
            trace_info.pid_comm_map.set_val( pid, comm );

            // And add to sched_switch pid_comm map
            trace_info.sched_switch_pid_comm_map.set_val( pid, comm );
        }
    }
}

TraceEvents::~TraceEvents()
{
    for ( trace_event_t &event : m_events )
    {
        if ( event.fields )
        {
            delete [] event.fields;
            event.fields = NULL;
            event.numfields = 0;
        }
    }
}

// Callback from trace_read.cpp. We mostly just store the events in our array
//  and then init_new_event() does the real work of initializing them later.
int TraceEvents::new_event_cb( const trace_event_t &event )
{
    // Add event to our m_events array
    m_events.push_back( event );

    // If this is a sched_switch event, see if it has comm info we don't know about.
    // This is the reason we're initializing events in two passes to collect all this data.
    if ( event.is_sched_switch() )
    {
        add_sched_switch_pid_comm( m_trace_info, event, "prev_pid", "prev_comm" );
        add_sched_switch_pid_comm( m_trace_info, event, "next_pid", "next_comm" );
    }
    else if ( event.is_ftrace_print() )
    {
        new_event_ftrace_print( m_events.back() );
    }

    // Record the maximum crtc value we've ever seen
    m_crtc_max = std::max< int >( m_crtc_max, event.crtc );

    // 1+ means loading events
    SDL_AtomicAdd( &m_eventsloaded, 1 );

    // Return 1 to cancel loading
    return ( s_app().get_state() == MainApp::State_CancelLoading );
}

int SDLCALL MainApp::thread_func( void *data )
{
    util_time_t t0 = util_get_time();
    loading_info_t *loading_info = ( loading_info_t *)data;
    TraceEvents &trace_events = loading_info->win->m_trace_events;
    const char *filename = loading_info->filename.c_str();

    logf( "Reading trace file %s...", filename );

    trace_events.m_trace_info.trim_trace = s_opts().getb( OPT_TrimTrace );

    EventCallback trace_cb = std::bind( &TraceEvents::new_event_cb, &trace_events, _1 );
    int ret = read_trace_file( filename, trace_events.m_strpool,
                               trace_events.m_trace_info, trace_cb );
    if ( ret < 0 )
    {
        logf( "[Error] read_trace_file(%s) failed.", filename );

        // -1 means loading error
        SDL_AtomicSet( &trace_events.m_eventsloaded, -1 );
        s_app().set_state( State_Idle );
        return -1;
    }

    float time_load = util_time_to_ms( t0, util_get_time() );

    // Call TraceEvents::init() to initialize all events, etc.
    trace_events.init();

    float time_init = util_time_to_ms( t0, util_get_time() ) - time_load;
    logf( "Events read: %lu (Load:%.2fms Init:%.2fms)", trace_events.m_events.size(), time_load, time_init );

    // 0 means events have all all been loaded
    SDL_AtomicSet( &trace_events.m_eventsloaded, 0 );
    s_app().set_state( State_Idle );

    return 0;
}

void MainApp::init( int argc, char **argv )
{
    parse_cmdline( argc, argv );

    imgui_set_custom_style( s_clrs().getalpha( col_ThemeAlpha ) );

    logf( "Welcome to gpuvis\n" );
    logf( " " );

    imgui_set_scale( s_opts().getf( OPT_Scale ) );
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

SDL_Window *MainApp::create_window( const char *title )
{
    int x, y, w, h;
    SDL_Window *window;

    get_window_pos( x, y, w, h );

    window = SDL_CreateWindow( title, x, y, w, h, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE );
    sdl_setwindow_icon( window );

    return window;
}

void MainApp::shutdown( SDL_Window *window )
{
    if ( window )
    {
        // Write main window position / size to ini file.
        int x, y, w, h;
        int top, left, bottom, right;

        SDL_GetWindowBordersSize( window, &top, &left, &bottom, &right );
        SDL_GetWindowPosition( window, &x, &y );
        SDL_GetWindowSize( window, &w, &h );

        save_window_pos( x - left, y - top, w, h );
    }

    if ( m_loading_info.thread )
    {
        // Cancel any file loading going on.
        cancel_load_file();

        // Wait for our thread to die.
        SDL_WaitThread( m_loading_info.thread, NULL );
        m_loading_info.thread = NULL;
    }

    set_state( State_Idle );

    delete m_trace_win;
    m_trace_win = NULL;
}

void MainApp::render_save_filename()
{
    struct stat st;
    float w = imgui_scale( 300.0f );
    bool window_appearing = ImGui::IsWindowAppearing();
    bool do_save = s_actions().get( action_return );

    // Text label
    ImGui::Text( "%s", m_saving_info.title.c_str() );

    // New filename input text field
    if ( imgui_input_text2( "New Filename:", m_saving_info.filename_buf, w, 0 ) || window_appearing )
    {
        m_saving_info.errstr.clear();
        m_saving_info.filename_new = get_realpath( m_saving_info.filename_buf );

        if ( !m_saving_info.filename_new.empty() &&
             ( m_saving_info.filename_new != m_saving_info.filename_orig ) &&
             !stat( m_saving_info.filename_new.c_str(), &st ) )
        {
            m_saving_info.errstr = string_format( "WARNING: %s exists", m_saving_info.filename_new.c_str() );
        }
    }

    // Set focus to input text on first pass through
    if ( window_appearing )
        ImGui::SetKeyboardFocusHere( -1 );

    // Spew out any error / warning messages
    if ( !m_saving_info.errstr.empty() )
        ImGui::TextColored( ImVec4( 1, 0, 0, 1 ), "%s", m_saving_info.errstr.c_str() );

    bool disabled = m_saving_info.filename_new.empty() ||
            ( m_saving_info.filename_new == m_saving_info.filename_orig );

    // Save button
    {
        ImGuiCol idx = disabled ? ImGuiCol_TextDisabled : ImGuiCol_Text;
        ImGuiButtonFlags flags = disabled ? ImGuiButtonFlags_Disabled : 0;

        ImGui::PushStyleColor( ImGuiCol_Text, ImGui::GetStyleColorVec4( idx ) );
        do_save |= ImGui::ButtonEx( "Save", ImVec2( w / 3.0f, 0 ), flags );
        ImGui::PopStyleColor();
    }

    bool close_popup = false;
    if ( do_save && !disabled )
        close_popup = m_saving_info.save_cb( m_saving_info );

    // Cancel button (or escape key)
    ImGui::SameLine();
    if ( ImGui::Button( "Cancel", ImVec2( w / 3.0f, 0 ) ) ||
         s_actions().get( action_escape ) )
    {
        close_popup = true;
    }

    if ( close_popup )
    {
        ImGui::CloseCurrentPopup();

        m_saving_info.filename_buf[ 0 ] = 0;
        m_saving_info.title.clear();
        m_saving_info.filename_new.clear();
        m_saving_info.filename_orig.clear();
        m_saving_info.errstr.clear();
    }
}

static void imgui_setnextwindowsize( float w, float h, float x = -1.0f, float y = -1.0f )
{
    if ( x >= 0.0f )
    {
        ImGui::SetNextWindowPos( ImVec2( imgui_scale( x ), imgui_scale( y ) ),
                                 ImGuiCond_FirstUseEver );
    }

    ImGui::SetNextWindowSize( ImVec2( imgui_scale( w ), imgui_scale( h ) ),
                              ImGuiSetCond_FirstUseEver );
}

void MainApp::render()
{
    if ( m_trace_win && m_trace_win->m_open )
    {
        float w = ImGui::GetIO().DisplaySize.x;
        float h = ImGui::GetIO().DisplaySize.y;

        ImGui::SetNextWindowPos( ImVec2( 0, 0 ), ImGuiSetCond_Always );
        ImGui::SetNextWindowSizeConstraints( ImVec2( w, h ), ImVec2( w, h ) );

        m_trace_win->render();
    }
    else if ( m_trace_win )
    {
        delete m_trace_win;
        m_trace_win = NULL;
    }
    else if ( !m_show_scale_popup && m_loading_info.inputfiles.empty() )
    {
        // If we have no main window and nothing to load, show the console
        m_show_gpuvis_console = true;
    }

    // Render dialogs only if the scale popup dialog isn't up
    if ( !m_show_scale_popup )
    {
        if ( m_focus_gpuvis_console )
        {
            ImGui::SetWindowFocus( "Gpuvis Console" );
            m_show_gpuvis_console = true;
            m_focus_gpuvis_console = false;
        }
        if ( m_show_gpuvis_console )
        {
            imgui_setnextwindowsize( 600, 600, 4, 4 );

            render_console();
        }

        if ( m_show_imgui_test_window )
        {
            imgui_setnextwindowsize( 800, 600 );

            ImGui::ShowDemoWindow( &m_show_imgui_test_window );
        }

        if ( m_show_imgui_style_editor )
        {
            imgui_setnextwindowsize( 800, 600 );

            ImGui::Begin( "Style Editor", &m_show_imgui_style_editor );
            ImGui::ShowStyleEditor();
            ImGui::End();
        }

        if ( m_show_imgui_metrics_editor )
        {
            ImGui::ShowMetricsWindow( &m_show_imgui_metrics_editor );
        }

        if ( m_show_font_window )
        {
            imgui_setnextwindowsize( 800, 600 );

            ImGui::Begin( "Font Options", &m_show_font_window );
            render_font_options();
            ImGui::End();
        }

        if ( m_show_color_picker )
        {
            imgui_setnextwindowsize( 800, 600 );

            ImGui::Begin( "Color Configuration", &m_show_color_picker );
            render_color_picker();
            ImGui::End();
        }

        if ( !m_show_trace_info.empty() && is_trace_loaded() )
        {
            bool show_trace_info = !!m_trace_win;

            if ( show_trace_info )
            {
                imgui_setnextwindowsize( 800, 600 );

                ImGui::Begin( m_show_trace_info.c_str(), &show_trace_info );
                m_trace_win->trace_render_info();
                ImGui::End();

                if ( s_actions().get( action_escape ) )
                    show_trace_info = false;
            }

            if ( !show_trace_info )
                m_show_trace_info.clear();
        }

        if ( !m_saving_info.title.empty() && !ImGui::IsPopupOpen( "Save Filename" ) )
            ImGui::OpenPopup( "Save Filename" );
        if ( ImGui::BeginPopupModal( "Save Filename", NULL, ImGuiWindowFlags_AlwaysAutoResize ) )
        {
            render_save_filename();
            ImGui::EndPopup();
        }

        if ( m_show_help && !ImGui::IsPopupOpen( "GpuVis Help" ) )
        {
            ImGui::OpenPopup( "GpuVis Help" );

            imgui_setnextwindowsize( 600, 600 );
        }
        if ( ImGui::BeginPopupModal( "GpuVis Help", &m_show_help, 0 ) )
        {
            static const struct
            {
                const char *hotkey;
                const char *desc;
            } s_help[] =
            {
                { "Ctrl+click drag", "Select graph area" },
                { "Shift+click drag", "Zoom selected graph area" },
                { "Mousewheel", "Zoom graph in / out" },
                { "Alt down", "Hide graph labels" },
            };

            if ( imgui_begin_columns( "gpuvis_help", { "Hotkey", "Description" } ) )
                ImGui::SetColumnWidth( 0, imgui_scale( 170.0f ) );

            for ( size_t i = 0; i < ARRAY_SIZE( s_help ); i++ )
            {
                ImGui::Text( "%s", s_textclrs().bright_str( s_help[ i ].hotkey ).c_str() );
                ImGui::NextColumn();

                ImGui::Text( "%s", s_help[ i ].desc );
                ImGui::NextColumn();

                ImGui::Separator();
            }

            for ( const Actions::actionmap_t &map : s_actions().m_actionmap )
            {
                if ( map.desc )
                {
                    std::string hotkey = s_actions().hotkey_str( map.action );

                    ImGui::Text( "%s", s_textclrs().bright_str( hotkey ).c_str() );
                    ImGui::NextColumn();

                    ImGui::Text( "%s", map.desc );
                    ImGui::NextColumn();

                    ImGui::Separator();
                }
            }

            ImGui::EndColumns();

            if ( s_actions().get( action_escape ) )
            {
                m_show_help = false;
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }

    if ( m_show_scale_popup && !ImGui::IsPopupOpen( "Display Scaling" ) )
        ImGui::OpenPopup( "Display Scaling" );
    if ( ImGui::BeginPopupModal( "Display Scaling", NULL, ImGuiWindowFlags_AlwaysAutoResize ) )
    {
        ImGui::Text( "Are you running on a high resolution display?" );
        ImGui::Text( " You can update settings in Font Options dialog." );

        ImGui::Separator();

        if ( ImGui::Button( "Yes", ImVec2( 150, 0 ) ) )
        {
            ImGui::CloseCurrentPopup();
            m_show_scale_popup = false;
        }

        ImGui::SameLine();
        if ( ImGui::Button( "No", ImVec2( 150, 0 ) ) )
        {
            s_opts().setf( OPT_Scale, 1.0f );
            m_font_main.m_changed = true;
            ImGui::CloseCurrentPopup();
            m_show_scale_popup = false;
        }

        ImGui::EndPopup();
    }
}

void MainApp::update()
{
    if ( !m_loading_info.inputfiles.empty() && ( get_state() == State_Idle ) )
    {
        const char *filename = m_loading_info.inputfiles[ 0 ].c_str();

        load_file( filename );

        m_loading_info.inputfiles.erase( m_loading_info.inputfiles.begin() );
    }

    if ( ( m_font_main.m_changed || m_font_small.m_changed ) &&
         !ImGui::IsMouseDown( 0 ) )
    {
        imgui_set_scale( s_opts().getf( OPT_Scale ) );

        ImGui_ImplSdlGL3_InvalidateDeviceObjects();
        load_fonts();
    }
}

void MainApp::load_fonts()
{
    // Clear all font texture data, ttf data, glyphs, etc.
    ImGui::GetIO().Fonts->Clear();

    // Add main font
    m_font_main.load_font( "$imgui_font_main$", "Roboto Regular", 14.0f );

    // Add small font
    m_font_small.load_font( "$imgui_font_small$", "Roboto Condensed", 14.0f );

    static const ImWchar ranges[] =
    {
        0x0020, 0x007F,
        0,
    };
    m_font_big.m_reset = true;
    m_font_big.load_font( "$imgui_font_big$", m_font_main.m_name.c_str(),
                          m_font_main.m_size * 4.0f, &ranges[ 0 ] );

    // Reset max rect size for the print events so they'll redo the CalcTextSize for the
    //  print graph row backgrounds (in graph_render_print_timeline).
    if ( m_trace_win )
        m_trace_win->m_trace_events.invalidate_ftraceprint_colors();

    if ( s_ini().GetFloat( "scale", -1.0f ) == -1.0f )
    {
        s_ini().PutFloat( "scale", s_opts().getf( OPT_Scale ) );

        m_show_scale_popup = true;
    }
}

void MainApp::get_window_pos( int &x, int &y, int &w, int &h )
{
    x = s_ini().GetInt( "win_x", SDL_WINDOWPOS_CENTERED );
    y = s_ini().GetInt( "win_y", SDL_WINDOWPOS_CENTERED );
    w = s_ini().GetInt( "win_w", 1280 );
    h = s_ini().GetInt( "win_h", 1024 );
}

void MainApp::save_window_pos( int x, int y, int w, int h )
{
    s_ini().PutInt( "win_x", x );
    s_ini().PutInt( "win_y", y );
    s_ini().PutInt( "win_w", w );
    s_ini().PutInt( "win_h", h );
}

/*
 * TraceWin
 */
bool TraceWin::graph_marker_valid( int idx0 )
{
    return ( m_graph.ts_markers[ idx0 ] != INT64_MAX );
}

void TraceWin::graph_marker_set( size_t index, int64_t ts, const char *str )
{
    m_graph.ts_markers[ index ] = str ? timestr_to_ts( str ) : ts;

    if ( ts == INT64_MAX )
        m_graph.marker_bufs[ index ][ 0 ] = 0;
    else
        strcpy_safe( m_graph.marker_bufs[ index ],
                     ts_to_timestr( m_graph.ts_markers[ index ], 4 ).c_str() );

    if ( graph_marker_valid( 0 ) && graph_marker_valid( 1 ) )
    {
        strcpy_safe( m_graph.marker_delta_buf,
                     ts_to_timestr( m_graph.ts_markers[ 1 ] - m_graph.ts_markers[ 0 ], 4 ).c_str() );
    }
}

uint32_t TraceWin::ts_to_eventid( int64_t ts )
{
    // When running a debug build w/ ASAN on, the lower_bound function is
    //  horribly slow so we cache the timestamp to event ids.
    uint32_t *peventid = m_ts_to_eventid_cache.get_val( ts );
    if ( peventid )
        return *peventid;

    trace_event_t x;
    const std::vector< trace_event_t > &events = m_trace_events.m_events;

    x.ts = ts;

    auto eventidx = std::lower_bound( events.begin(), events.end(), x,
        []( const trace_event_t &f1, const trace_event_t &f2 ) {
            return f1.ts < f2.ts;
        } );

    uint32_t id = eventidx - events.begin();

    if ( id >= events.size() )
        id = events.size() - 1;

    m_ts_to_eventid_cache.set_val( ts, id );
    return id;
}

uint32_t TraceWin::timestr_to_eventid( const char *buf )
{
    int64_t ts = timestr_to_ts( buf );

    return ts_to_eventid( ts );
}

const char *filter_get_key_func( StrPool *strpool, const char *name, size_t len )
{
    return strpool->getstr( name, len );
}

const char *filter_get_keyval_func( trace_info_t *trace_info, const trace_event_t *event,
                                    const char *name, char ( &buf )[ 64 ] )
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
    else if ( !strcasecmp( name, "tgid" ) )
    {
        int *tgid = trace_info->pid_tgid_map.get_val( event->pid );

        snprintf_safe( buf, "%d", tgid ? *tgid : 0 );
        return buf;
    }
    else if ( !strcasecmp( name, "ts" ) )
    {
        snprintf_safe( buf, "%.6f", event->ts * ( 1.0 / NSECS_PER_MSEC ) );
        return buf;
    }
    else if ( !strcasecmp( name, "cpu" ) )
    {
        snprintf_safe( buf, "%u", event->cpu );
        return buf;
    }
    else if ( !strcasecmp( name, "duration" ) )
    {
        if ( !event->has_duration() )
            buf[ 0 ] = 0;
        else
            snprintf_safe( buf, "%.6f", event->duration * ( 1.0 / NSECS_PER_MSEC ) );
        return buf;
    }

    for ( uint32_t i = 0; i < event->numfields; i++ )
    {
        const event_field_t &field = event->fields[ i ];

        // We can compare pointers since they're from same string pool
        if ( name == field.key )
            return field.value;
    }

    return "";
}

const std::vector< uint32_t > *TraceEvents::get_tdopexpr_locs( const char *name, std::string *err )
{
    std::vector< uint32_t > *plocs;
    uint32_t hashval = fnv_hashstr32( name );

    if ( err )
        err->clear();

    // Try to find whatever our name hashed to. Name should be something like:
    //   $name=drm_vblank_event
    plocs = m_tdopexpr_locs.get_locations_u32( hashval );
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
            if ( err )
                *err = errstr;
            else
                logf( "[Error] compiling '%s': %s", name, errstr.c_str() );
        }
        else
        {
            for ( trace_event_t &event : m_events )
            {
                const char *ret;
                tdop_get_keyval_func get_keyval_func = std::bind( filter_get_keyval_func, &m_trace_info, &event, _1, _2 );

                ret = tdopexpr_exec( tdop_expr, get_keyval_func );
                if ( ret[ 0 ] )
                    m_tdopexpr_locs.add_location_u32( hashval, event.id );
            }

            tdopexpr_delete( tdop_expr );
        }
    }

    // Try to find this name/expression again and add to failed list if we miss again
    plocs = m_tdopexpr_locs.get_locations_u32( hashval );
    if ( !plocs )
        m_failed_commands.insert( hashval );

    return plocs;
}

const std::vector< uint32_t > *TraceEvents::get_comm_locs( const char *name )
{
    return m_comm_locs.get_locations_str( name );
}

const std::vector< uint32_t > *TraceEvents::get_sched_switch_locs( int pid, switch_t switch_type )
{
    return ( switch_type == SCHED_SWITCH_PREV ) ?
                m_sched_switch_prev_locs.get_locations_u32( pid ) :
                m_sched_switch_next_locs.get_locations_u32( pid );
}

const std::vector< uint32_t > *TraceEvents::get_timeline_locs( const char *name )
{
    return m_amd_timeline_locs.get_locations_str( name );
}

// Pass a string like "gfx_249_91446"
const std::vector< uint32_t > *TraceEvents::get_gfxcontext_locs( const char *name )
{
    return m_gfxcontext_locs.get_locations_str( name );
}

uint32_t row_pos_t::get_row( int64_t min_ts, int64_t max_ts )
{
    uint32_t row = 0;

    for ( ; row < m_row_pos.size(); row++ )
    {
        int64_t ts_end_prev = INT64_MIN;
        int64_t ts_start_next = INT64_MAX;

        // Find first element with start time >= min_ts
        auto &rpos = m_row_pos[ row ];
        auto idx = rpos.lower_bound( min_ts );

        if ( idx != rpos.end() )
        {
            // Got item with start time >= our min_ts.
            ts_start_next = idx->first;

            if ( idx != rpos.begin() )
            {
                // Get previous item end time
                --idx;
                ts_end_prev = idx->second;
            }
        }
        else
        {
            // No items start after us - grab last item in map
            auto idx2 = rpos.rbegin();

            // Get end of last item ever
            if ( idx2 != rpos.rend() )
                ts_end_prev = idx2->second;
        }

        // If start of next item is greater than our end
        // And end of previous item is less than our start
        if ( ( ts_start_next >= max_ts ) && ( ts_end_prev <= min_ts ) )
            break;
    }

    if ( row >= m_row_pos.size() )
        row = 0;

    auto &rpos = m_row_pos[ row ];
    rpos[ min_ts ] = max_ts;

    m_rows = std::max< uint32_t >( m_rows, row + 1 );
    return row;
}

void TraceEvents::update_fence_signaled_timeline_colors()
{
    float label_sat = s_clrs().getalpha( col_Graph_TimelineLabelSat );
    float label_alpha = s_clrs().getalpha( col_Graph_TimelineLabelAlpha );

    for ( auto &timeline_locs : m_amd_timeline_locs.m_locs.m_map )
    {
        std::vector< uint32_t > &locs = timeline_locs.second;

        for ( uint32_t index : locs )
        {
            trace_event_t &fence_signaled = m_events[ index ];

            if ( fence_signaled.is_fence_signaled() &&
                 is_valid_id( fence_signaled.id_start ) )
            {
                uint32_t hashval = fnv_hashstr32( fence_signaled.user_comm );

                // Mark this event as autogen'd color so it doesn't get overwritten
                fence_signaled.flags |= TRACE_FLAG_AUTOGEN_COLOR;
                fence_signaled.color = imgui_col_from_hashval( hashval, label_sat, label_alpha );
            }
        }
    }
}

void TraceEvents::update_tgid_colors()
{
    float label_sat = s_clrs().getalpha( col_Graph_PrintLabelSat );
    float label_alpha = s_clrs().getalpha( col_Graph_PrintLabelAlpha );

    for ( auto &it : m_trace_info.tgid_pids.m_map )
    {
        tgid_info_t &tgid_info = it.second;

        tgid_info.color = imgui_col_from_hashval( tgid_info.hashval,
                                                  label_sat, label_alpha );

        TextClr clr( tgid_info.color );
        const char *commstr = comm_from_pid( tgid_info.tgid, "<...>" );

        tgid_info.commstr_clr = m_strpool.getstrf( "%s%s%s", clr.str(),
                                                   commstr, s_textclrs().str( TClr_Def ) );
        tgid_info.commstr = commstr;
    }
}

const char *TraceEvents::comm_from_pid( int pid, const char *def )
{
    const char *const *comm = m_trace_info.pid_comm_map.get_val( pid );

    if ( !comm && !def )
        return NULL;

    return m_strpool.getstrf( "%s-%d", comm ? *comm : def, pid );
}

const char *TraceEvents::tgidcomm_from_pid( int pid )
{
    const char **mapped_comm = m_pid_commstr_map.get_val( pid );

    if ( mapped_comm )
        return *mapped_comm;

    const tgid_info_t *tgid_info = tgid_from_pid( pid );
    const char *comm = comm_from_pid( pid, "<...>" );

    if ( tgid_info )
    {
        comm = m_strpool.getstrf( "%s (%s)", comm, tgid_info->commstr_clr );
    }

    // Add pid / comm mapping
    m_pid_commstr_map.get_val( pid, comm );

    return comm;
}

const char *TraceEvents::tgidcomm_from_commstr( const char *comm )
{
    // Parse comm string to get pid. Ie: mainthread-1324
    const char *pidstr = comm ? strrchr( comm, '-' ) : NULL;

    if ( pidstr )
    {
        int pid = atoi( pidstr + 1 );

        return tgidcomm_from_pid( pid );
    }

    return comm;
}

const tgid_info_t *TraceEvents::tgid_from_pid( int pid )
{
    int *tgid = m_trace_info.pid_tgid_map.get_val( pid );

    return tgid ? m_trace_info.tgid_pids.get_val( *tgid ) : NULL;
}

const tgid_info_t *TraceEvents::tgid_from_commstr( const char *comm )
{
    const char *pidstr = comm ? strrchr( comm, '-' ) : NULL;

    if ( pidstr )
    {
        int pid = atoi( pidstr + 1 );

        return tgid_from_pid( pid );
    }

    return NULL;
}

const char *TraceEvents::get_event_gfxcontext_str( const trace_event_t &event )
{
    if ( event.seqno )
    {
        const char *context = get_event_field_val( event, "context", NULL );
        const char *timeline = get_event_field_val( event, "timeline", NULL );

        if ( timeline && context )
        {
            return m_strpool.getstrf( "%s_%s_%u", timeline, context, event.seqno );
        }
    }

    return "";
}

std::string TraceEvents::get_ftrace_ctx_str( const trace_event_t &event )
{
    if ( event.seqno != UINT32_MAX )
    {
        return string_format( " %s[ctx=%u]%s", s_textclrs().str( TClr_Bright ),
                              event.seqno, s_textclrs().str( TClr_Def ) );
    }

    return "";
}

void TraceEvents::init_sched_switch_event( trace_event_t &event )
{
    const char *prev_pid_str = get_event_field_val( event, "prev_pid" );
    const char *next_pid_str = get_event_field_val( event, "next_pid" );

    if ( *prev_pid_str && *next_pid_str )
    {
        int prev_pid = atoi( prev_pid_str );
        int next_pid = atoi( next_pid_str );
        const std::vector< uint32_t > *plocs;

        // Look in the sched_switch next queue for an event that said we were starting up.
        plocs = get_sched_switch_locs( prev_pid, TraceEvents::SCHED_SWITCH_NEXT );
        if ( plocs )
        {
            const trace_event_t &event_prev = m_events[ plocs->back() ];

            // TASK_RUNNING (0): On the run queue
            // TASK_INTERRUPTABLE (1): Sleeping but can be woken up
            // TASK_UNINTERRUPTABLE (2): Sleeping but can't be woken up by a signal
            // TASK_STOPPED (4): Stopped process by job control signal or ptrace
            // TASK_ZOMBIE (32): Finished but waiting for parent to call wait() to cleanup
            int prev_state = atoi( get_event_field_val( event, "prev_state" ) );
            int task_state = prev_state & ( TASK_STATE_MAX - 1 );

            if ( task_state == 0 )
                event.flags |= TRACE_FLAG_SCHED_SWITCH_TASK_RUNNING;

            event.duration = event.ts - event_prev.ts;
        }

        m_sched_switch_prev_locs.add_location_u32( prev_pid, event.id );
        m_sched_switch_next_locs.add_location_u32( next_pid, event.id );

        //$ TODO mikesart: This is messing up the m_comm_locs event counts
        if ( prev_pid != event.pid )
        {
            const char *comm = comm_from_pid( prev_pid );
            if ( comm )
                m_comm_locs.add_location_str( comm, event.id );
        }
        if ( next_pid != event.pid )
        {
            const char *comm = comm_from_pid( next_pid );
            if ( comm )
                m_comm_locs.add_location_str( comm, event.id );
        }
    }
}

void TraceEvents::init_sched_process_fork( trace_event_t &event )
{
    // parent_comm=glxgears parent_pid=23543 child_comm=glxgears child_pid=23544
    int tgid = atoi( get_event_field_val( event, "parent_pid", "0" ) );
    int pid = atoi( get_event_field_val( event, "child_pid", "0" ) );
    const char *tgid_comm = get_event_field_val( event, "parent_comm", NULL );
    const char *child_comm = get_event_field_val( event, "child_comm", NULL );

    if ( tgid && pid && tgid_comm && child_comm )
    {
        tgid_info_t *tgid_info = m_trace_info.tgid_pids.get_val_create( tgid );

        if ( !tgid_info->tgid )
        {
            tgid_info->tgid = tgid;
            tgid_info->hashval += fnv_hashstr32( tgid_comm );
        }
        tgid_info->add_pid( tgid );
        tgid_info->add_pid( pid );

        // Add to our pid --> comm map
        m_trace_info.pid_comm_map.get_val( tgid, m_strpool.getstr( tgid_comm ) );
        m_trace_info.pid_comm_map.get_val( pid, m_strpool.getstr( child_comm ) );

        // tgid --> tgid, pid --> tgid
        m_trace_info.pid_tgid_map.get_val( tgid, tgid );
        m_trace_info.pid_tgid_map.get_val( pid, tgid );
    }
}

void TraceEvents::init_amd_timeline_event( trace_event_t &event )
{
    const char *gfxcontext = get_event_gfxcontext_str( event );
    const char *timeline = get_event_field_val( event, "timeline" );

    // Add this event under the "gfx", "sdma0", etc timeline map
    m_amd_timeline_locs.add_location_str( timeline, event.id );

    // Add this event under our "gfx_ctx_seq" or "sdma0_ctx_seq", etc. map
    m_gfxcontext_locs.add_location_str( gfxcontext, event.id );

    // Grab the event locations for this event context
    const std::vector< uint32_t > *plocs = get_gfxcontext_locs( gfxcontext );
    if ( plocs->size() > 1 )
    {
        // First event.
        const trace_event_t &event0 = m_events[ plocs->front() ];

        // Event right before the event we just added.
        auto it = plocs->rbegin() + 1;
        const trace_event_t &event_prev = m_events[ *it ];

        // Assume the user comm is the first comm event in this set.
        event.user_comm = event0.comm;

        // Point the event we just added to the previous event in this series
        event.id_start = event_prev.id;

        if ( event.is_fence_signaled() )
        {
            // Mark all the events in this series as timeline events
            for ( uint32_t idx : *plocs )
            {
                m_events[ idx ].flags |= TRACE_FLAG_TIMELINE;
            }
        }
    }
}

void TraceEvents::init_i915_event( trace_event_t &event )
{
    i915_type_t event_type = get_i915_reqtype( event );

    if ( event_type == i915_reqwait_begin )
    {
        m_i915.reqwait_begin_locs.add_location( event );
    }
    else if ( event_type == i915_reqwait_end )
    {
        std::vector< uint32_t > *plocs = m_i915.reqwait_begin_locs.get_locations( event );

        if ( plocs )
        {
            trace_event_t &event_begin = m_events[ plocs->back() ];
            const char *ring = get_event_field_val( event, "ring", NULL );

            event_begin.duration = event.ts - event_begin.ts;
            event.duration = event_begin.duration;

            if ( ring )
            {
                char buf[ 128 ];
                uint32_t ringno = strtoul( ring, NULL, 10 );

                event.graph_row_id = ( uint32_t )-1;
                event.id_start = event_begin.id;

                snprintf_safe( buf, "i915_reqwait ring%u", ringno );
                m_i915.reqwait_end_locs.add_location_str( buf, event.id );
            }
        }
    }
    else if ( event_type <= i915_req_Notify )
    {
        m_i915.gem_req_locs.add_location( event );
    }
}

static int64_t normalize_vblank_diff( int64_t diff )
{
    static const int64_t rates[] =
    {
        66666666, // 15Hz
        33333333, // 30Hz
        16666666, // 60Hz
        11111111, // 90Hz
        10526315, // 95Hz
        8333333,  // 120Hz
        6944444,  // 144Hz
        6060606,  // 165Hz
        4166666,  // 240Hz
    };

    for ( size_t i = 0; i < ARRAY_SIZE( rates ); i++ )
    {
        int64_t pct = 10000 * ( diff - rates[ i ]  ) / rates[ i ];

        // If the diff is < 1.0% off this common refresh rate, use it.
        if ( ( pct > -100 ) && ( pct < 100 ) )
            return rates[ i ];
    }

    return diff;
}

void TraceEvents::init_new_event_vblank( trace_event_t &event )
{
    // See if we have a drm_vblank_event_queued with the same seq number
    uint32_t seqno = strtoul( get_event_field_val( event, "seq" ), NULL, 10 );
    uint32_t *vblank_queued_id = m_drm_vblank_event_queued.get_val( seqno );

    if ( vblank_queued_id )
    {
        trace_event_t &event_vblank_queued = m_events[ *vblank_queued_id ];

        // If so, set the vblank queued time
        event_vblank_queued.duration = event.ts - event_vblank_queued.ts;
    }

    m_tdopexpr_locs.add_location_str( "$name=drm_vblank_event", event.id );

    /*
     * vblank interval calculations
     */
    if ( m_vblank_info[ event.crtc ].last_vblank_ts )
    {
        int64_t diff = event.ts - m_vblank_info[ event.crtc ].last_vblank_ts;

        // Normalize ts diff to known frequencies
        diff = normalize_vblank_diff( diff );

        // Bump count for this diff ts value
        m_vblank_info[ event.crtc ].diff_ts_count[ diff / 1000 ]++;
        m_vblank_info[ event.crtc ].count++;
    }

    m_vblank_info[ event.crtc ].last_vblank_ts = event.ts;
}

// new_event_cb adds all events to array, this function initializes them.
void TraceEvents::init_new_event( trace_event_t &event )
{
    // If our pid is in the sched_switch pid map, update our comm to the sched_switch
    // value that it recorded.
    const char **comm = m_trace_info.sched_switch_pid_comm_map.get_val( event.pid );
    if ( comm )
    {
        event.comm = m_strpool.getstrf( "%s-%d", *comm, event.pid );
    }

    if ( event.is_vblank() )
    {
        init_new_event_vblank( event );
    }
    else if ( !strcmp( event.name, "drm_vblank_event_queued" ) )
    {
        uint32_t seqno = strtoul( get_event_field_val( event, "seq" ), NULL, 10 );

        if ( seqno )
            m_drm_vblank_event_queued.set_val( seqno, event.id );
    }

    // Add this event comm to our comm locations map (ie, 'thread_main-1152')
    m_comm_locs.add_location_str( event.comm, event.id );

    // Add this event name to event name map
    if ( event.is_vblank() )
    {
        // Add vblanks as "drm_vblank_event1", etc
        uint32_t hashval = m_strpool.getu32f( "%s%d", event.name, event.crtc );

        m_eventnames_locs.add_location_u32( hashval, event.id );
    }
    else
    {
        m_eventnames_locs.add_location_str( event.name, event.id );
    }

    if ( !strcmp( event.name, "sched_process_exec" ) )
    {
        // pid, old_pid, filename
        const char *filename = get_event_field_val( event, "filename" );

        filename = strrchr( filename, '/' );
        if ( filename )
        {
            // Add pid --> comm map if it doesn't already exist
            filename = m_strpool.getstr( filename + 1 );
            m_trace_info.pid_comm_map.get_val( event.pid, filename );
        }
    }
    else if ( !strcmp( event.name, "sched_process_exit" ) )
    {
        const char *pid_comm = get_event_field_val( event, "comm", NULL );

        if ( pid_comm )
            m_trace_info.pid_comm_map.set_val( event.pid, m_strpool.getstr( pid_comm ) );
    }
#if 0
    // Disabled for now. Need to figure out how to prevent sudo, bash, etc from becoming the parent. Ie:
    //    <...>-7860  [021]  3726.235512: sched_process_fork:   comm=sudo pid=7860 child_comm=sudo child_pid=7861
    //    <...>-7861  [010]  3726.825033: sched_process_fork:   comm=glxgears pid=7861 child_comm=glxgears child_pid=7862
    //    <...>-7861  [010]  3726.825304: sched_process_fork:   comm=glxgears pid=7861 child_comm=glxgears child_pid=7863
    else if ( !strcmp( event.name, "sched_process_fork" ) )
    {
        init_sched_process_fork( event );
    }
#endif

    if ( event.is_sched_switch() )
    {
        init_sched_switch_event( event );
    }
    else if ( is_amd_timeline_event( event ) )
    {
        init_amd_timeline_event( event );
    }
    else if ( event.seqno && !event.is_ftrace_print() )
    {
        init_i915_event( event );
    }

    if ( !strcmp( event.name, "amdgpu_job_msg" ) )
    {
        const char *msg = get_event_field_val( event, "msg", NULL );
        const char *gfxcontext = get_event_gfxcontext_str( event );

        if ( msg && msg[ 0 ] && gfxcontext[ 0 ] )
            m_gfxcontext_msg_locs.add_location_str( gfxcontext, event.id );
    }

    // 1+ means loading events
    SDL_AtomicAdd( &m_eventsloaded, 1 );
}

TraceEvents::tracestatus_t TraceEvents::get_load_status( uint32_t *count )
{
    int eventsloaded = SDL_AtomicGet( &m_eventsloaded );

    if ( eventsloaded > 0 )
    {
        if ( count )
            *count = eventsloaded & ~0x40000000;

        return ( eventsloaded & 0x40000000 ) ?
                    Trace_Initializing : Trace_Loading;
    }
    else if ( !eventsloaded )
    {
        if ( count )
            *count = m_events.size();
        return Trace_Loaded;
    }

    if ( count )
        *count = 0;
    return Trace_Error;
}

void TraceEvents::calculate_vblank_info()
{
    // Go through all the vblank crtcs
    for ( uint32_t i = 0; i < m_vblank_info.size(); i++ )
    {
        if ( !m_vblank_info[ i ].count )
            continue;

        uint32_t median = m_vblank_info[ i ].count / 2;

        for ( const auto &x : m_vblank_info[ i ].diff_ts_count )
        {
            if ( x.second >= median )
            {
                // This is the median tsdiff
                int64_t diff = x.first * 1000;

                m_vblank_info[ i ].median_diff_ts = diff;

                std::string str = ts_to_timestr( diff, 2 );
                const std::string desc = string_format( "Show vblank crtc%u markers (~%s)", i, str.c_str() );

                s_opts().setdesc( OPT_RenderCrtc0 + i, desc );
                break;
            }

            median -= x.second;
        }
    }
}

void TraceEvents::init()
{
    // Set m_eventsloaded initializing bit
    SDL_AtomicSet( &m_eventsloaded, 0x40000000 );

    m_vblank_info.resize( m_crtc_max + 1 );

    s_opts().set_crtc_max( m_crtc_max );

    // Initialize events...
    for ( trace_event_t &event : m_events )
        init_new_event( event );

    // Figure out median vblank intervals
    calculate_vblank_info();

    // Init amd event durations
    calculate_amd_event_durations();

    // Init intel event durations
    calculate_i915_req_event_durations();
    calculate_i915_reqwait_event_durations();

    // Init print column information
    calculate_event_print_info();

    // Remove tgid groups with single threads
    remove_single_tgids();

    // Update tgid colors
    update_tgid_colors();

    std::vector< INIEntry > entries = s_ini().GetSectionEntries( "$imgui_eventcolors$" );

    // Restore event colors
    for ( const INIEntry &entry : entries )
    {
        const std::string &eventname = entry.first;
        const std::string &val = entry.second;

        if ( !val.empty() )
        {
            uint32_t color = std::stoull( val, NULL, 0 );

            set_event_color( eventname.c_str(), color );
        }
    }
}

void TraceEvents::remove_single_tgids()
{
    std::unordered_map< int, tgid_info_t > &tgid_pids = m_trace_info.tgid_pids.m_map;

    for ( auto it = tgid_pids.begin(); it != tgid_pids.end(); )
    {
        tgid_info_t &tgid_info = it->second;

        // If this tgid has only itself as a thread, remove it
        if ( ( tgid_info.pids.size() == 1 ) && ( tgid_info.pids[ 0 ] == tgid_info.tgid ) )
            it = tgid_pids.erase( it );
        else
            it++;
    }
}

void TraceEvents::set_event_color( const std::string &eventname, ImU32 color )
{
    const std::vector< uint32_t > *plocs =
            m_eventnames_locs.get_locations_str( eventname.c_str() );

    if ( plocs )
    {
        s_ini().PutUint64( eventname.c_str(), color, "$imgui_eventcolors$" );

        for ( uint32_t idx : *plocs )
        {
            trace_event_t &event = m_events[ idx ];

            // If it's not an autogen'd color, set new color
            if ( !( event.flags & TRACE_FLAG_AUTOGEN_COLOR ) )
                event.color = color;
        }
    }
}

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
void TraceEvents::calculate_amd_event_durations()
{
    std::vector< uint32_t > erase_list;
    std::vector< trace_event_t > &events = m_events;
    float label_sat = s_clrs().getalpha( col_Graph_TimelineLabelSat );
    float label_alpha = s_clrs().getalpha( col_Graph_TimelineLabelAlpha );

    // Go through gfx, sdma0, sdma1, etc. timelines and calculate event durations
    for ( auto &timeline_locs : m_amd_timeline_locs.m_locs.m_map )
    {
        uint32_t graph_row_id = 0;
        int64_t last_fence_signaled_ts = 0;
        std::vector< uint32_t > &locs = timeline_locs.second;
        // const char *name = m_strpool.findstr( timeline_locs.first );

        // Erase all timeline events with single entries or no fence_signaled
        locs.erase( std::remove_if( locs.begin(), locs.end(),
                                    [&events]( const uint32_t index )
                                        { return !events[ index ].is_timeline(); }
                                  ),
                    locs.end() );

        if ( locs.empty() )
            erase_list.push_back( timeline_locs.first );

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

                uint32_t hashval = fnv_hashstr32( fence_signaled.user_comm );

                // Mark this event as autogen'd color so it doesn't get overwritten
                fence_signaled.flags |= TRACE_FLAG_AUTOGEN_COLOR;
                fence_signaled.color = imgui_col_from_hashval( hashval, label_sat, label_alpha );
            }
        }
    }

    for ( uint32_t hashval : erase_list )
    {
        // Completely erase timeline rows with zero entries.
        m_amd_timeline_locs.m_locs.m_map.erase( hashval );
    }
}

i915_type_t get_i915_reqtype( const trace_event_t &event )
{
    if ( !strcmp( event.name, "i915_gem_request_queue" ) )
        return i915_req_Queue;
    else if ( !strcmp( event.name, "i915_gem_request_add" ) )
        return i915_req_Add;
    else if ( !strcmp( event.name, "i915_gem_request_submit" ) )
        return i915_req_Submit;
    else if ( !strcmp( event.name, "i915_gem_request_in" ) )
        return i915_req_In;
    else if ( !strcmp( event.name, "i915_gem_request_out" ) )
        return i915_req_Out;
    else if ( !strcmp( event.name, "intel_engine_notify" ) )
        return i915_req_Notify;
    else if ( !strcmp( event.name, "i915_gem_request_wait_begin" ) )
        return i915_reqwait_begin;
    else if ( !strcmp( event.name, "i915_gem_request_wait_end" ) )
        return i915_reqwait_end;

    return i915_req_Max;
}

static bool intel_set_duration( trace_event_t *event0, trace_event_t *event1, uint32_t color_index )
{
    if ( event0 && event1 && !event1->has_duration() && ( event1->ts >= event0->ts ) )
    {
        event1->duration = event1->ts - event0->ts;
        event1->color_index = color_index;
        event1->id_start = event0->id;
        return true;
    }

    return false;
}

void TraceEvents::calculate_i915_reqwait_event_durations()
{
    for ( auto &req_locs : m_i915.reqwait_end_locs.m_locs.m_map )
    {
        row_pos_t row_pos;
        std::vector< uint32_t > &locs = req_locs.second;
        // const char *name = m_strpool.findstr( req_locs.first );

        for ( uint32_t idx : locs )
        {
            trace_event_t &event = m_events[ idx ];
            trace_event_t &event_begin = m_events[ event.id_start ];
            uint32_t row = row_pos.get_row( event_begin.ts, event.ts );

            event_begin.graph_row_id = row;
            event.graph_row_id = row;
        }

        m_row_count.m_map[ req_locs.first ] = row_pos.m_rows;
    }
}

void TraceEvents::calculate_i915_req_event_durations()
{
    // Our map should have events with the same ring/ctx/seqno
    for ( auto &req_locs : m_i915.gem_req_locs.m_locs.m_map )
    {
        const char *ring = "";
        trace_event_t *events[ i915_req_Max ] = { NULL };
        std::vector< uint32_t > &locs = req_locs.second;

        for ( uint32_t index : locs )
        {
            trace_event_t &event = m_events[ index ];
            i915_type_t event_type = get_i915_reqtype( event );

            if ( event_type <= i915_req_Out )
            {
                events[ event_type ] = &event;

                if ( !ring[ 0 ] )
                    ring = get_event_field_val( event, "ring" );
            }
        }

        // Notify shouldn't be set yet. It only has a ring and global seqno.
        // If we have request_in, search for the corresponding notify.
        if ( !events[ i915_req_Notify ] && events[ i915_req_In ] )
        {
            // Try to find the global seqno from our request_in event
            const char *globalstr = get_event_field_val( *events[ i915_req_In ], "global_seqno", NULL );

            if ( !globalstr )
                globalstr = get_event_field_val( *events[ i915_req_In ], "global", NULL );
            if ( globalstr )
            {
                uint32_t global_seqno = strtoul( globalstr, NULL, 10 );
                const std::vector< uint32_t > *plocs =
                        m_i915.gem_req_locs.get_locations( ring, global_seqno, "0" );

                // We found event(s) that match our ring and global seqno.
                if ( plocs )
                {
                    // Go through looking for an intel_engine_notify event.
                    for ( uint32_t i : *plocs )
                    {
                        trace_event_t &event_notify = m_events[ i ];

                        if ( !strcmp( event_notify.name, "intel_engine_notify" ) )
                        {
                            // Set id_start to point to the request_in event
                            event_notify.id_start = events[ i915_req_In ]->id;

                            // Add our notify event to the event list for this ring/ctx/seqno
                            locs.push_back( event_notify.id );
                            std::sort( locs.begin(), locs.end() );

                            // Set our notify event
                            events[ i915_req_Notify ] = &event_notify;
                            break;
                        }
                    }
                }
            }
        }

        bool set_duration = intel_set_duration( events[ i915_req_Queue ], events[ i915_req_Add ], col_Graph_Bari915Queue );

        // submit-delay: req_add -> req_submit
        set_duration |= intel_set_duration( events[ i915_req_Add ], events[ i915_req_Submit ], col_Graph_Bari915SubmitDelay );

        // execute-delay: req_submit -> req_in
        set_duration |= intel_set_duration( events[ i915_req_Submit ], events[ i915_req_In ], col_Graph_Bari915ExecuteDelay );

        // execute (start to user interrupt): req_in -> engine_notify
        set_duration |= intel_set_duration( events[ i915_req_In ], events[ i915_req_Notify ], col_Graph_Bari915Execute );

        // context-complete-delay (user interrupt to context complete): engine_notify -> req_out
        set_duration |= intel_set_duration( events[ i915_req_Notify ], events[ i915_req_Out ], col_Graph_Bari915CtxCompleteDelay );

        // If we didn't get an intel_engine_notify event, do req_in -> req_out
        set_duration |= intel_set_duration( events[ i915_req_In ], events[ i915_req_Out ], col_Graph_Bari915Execute );

        if ( set_duration )
        {
            uint32_t ringno = strtoul( ring, NULL, 10 );
            uint32_t hashval = m_strpool.getu32f( "i915_req ring%u", ringno );

            for ( uint32_t i = 0; i < i915_req_Max; i++ )
            {
                if ( events[ i ] )
                {
                    events[ i ]->graph_row_id = ( uint32_t )-1;
                    m_i915.req_locs.add_location_u32( hashval, events[ i ]->id );
                }
            }
        }
    }

    // Sort the events in the ring maps
    for ( auto &req_locs : m_i915.req_locs.m_locs.m_map )
    {
        row_pos_t row_pos;
        std::vector< uint32_t > &locs = req_locs.second;
        // const char *name = m_strpool.findstr( req_locs.first );

        std::sort( locs.begin(), locs.end() );

        for ( uint32_t idx : locs )
        {
            if ( m_events[ idx ].graph_row_id != ( uint32_t )-1 )
                continue;

            trace_event_t &event = m_events[ idx ];
            const std::vector< uint32_t > *plocs;
            const trace_event_t *pevent = !strcmp( event.name, "intel_engine_notify" ) ?
                        &m_events[ event.id_start ] : &event;

            plocs = m_i915.gem_req_locs.get_locations( *pevent );
            if ( plocs )
            {
                int64_t min_ts = m_events[ plocs->front() ].ts;
                int64_t max_ts = m_events[ plocs->back() ].ts;
                uint32_t row = row_pos.get_row( min_ts, max_ts );

                for ( uint32_t i : *plocs )
                    m_events[ i ].graph_row_id = row;
            }
        }

        m_row_count.m_map[ req_locs.first ] = row_pos.m_rows;
    }
}

const std::vector< uint32_t > *TraceEvents::get_locs( const char *name,
        loc_type_t *ptype, std::string *errstr )
{
    loc_type_t type = LOC_TYPE_Max;
    const std::vector< uint32_t > *plocs = NULL;

    if ( errstr )
        errstr->clear();

    if ( get_ftrace_row_info( name ) )
    {
        type = LOC_TYPE_Print;
        plocs = &m_ftrace.print_locs;
    }
    else if ( !strncmp( name, "i915_reqwait ring", 17 ) )
    {
        type = LOC_TYPE_i915RequestWait;
        plocs = m_i915.reqwait_end_locs.get_locations_str( name );
    }
    else if ( !strncmp( name, "i915_req ring", 13 ) )
    {
        type = LOC_TYPE_i915Request;
        plocs = m_i915.req_locs.get_locations_str( name );
    }
    else if ( !strncmp( name, "plot:", 5 ) )
    {
        GraphPlot *plot = get_plot_ptr( name );

        if ( plot )
        {
            type = LOC_TYPE_Plot;
            plocs = get_tdopexpr_locs( plot->m_filter_str.c_str() );
        }
    }
    else
    {
        size_t len = strlen( name );

        if ( ( len > 3 ) && !strcmp( name + len - 3, " hw" ) )
        {
            // Check for "gfx hw", "comp_1.1.1 hw", etc.
            uint32_t hashval = fnv_hashstr32( name, len - 3 );

            type = LOC_TYPE_AMDTimeline_hw;
            plocs = m_amd_timeline_locs.get_locations_u32( hashval );
        }

        if ( !plocs )
        {
            // Check for regular comm type rows
            type = LOC_TYPE_Comm;
            plocs = get_comm_locs( name );

            if ( !plocs )
            {
                // TDOP Expressions. Ie, $name = print, etc.
                type = LOC_TYPE_Tdopexpr;
                plocs = get_tdopexpr_locs( name );

                if ( !plocs )
                {
                    // Timelines: sdma0, gfx, comp_1.2.1, etc.
                    type = LOC_TYPE_AMDTimeline;
                    plocs = get_timeline_locs( name );
                }
            }
        }
    }

    if ( ptype )
        *ptype = plocs ? type : LOC_TYPE_Max;
    return plocs;
}

// Check if an expression is surrounded by parens: "( expr )"
// Assumes no leading / trailing whitespace in expr.
static bool is_surrounded_by_parens( const char *expr )
{
    if ( expr[ 0 ] == '(' )
    {
        int level = 1;

        for ( size_t i = 1; expr[ i ]; i++ )
        {
            if ( expr[ i ] == '(' )
            {
                level++;
            }
            else if ( expr[ i ] == ')' )
            {
                if ( --level == 0 )
                    return !expr[ i + 1 ];
            }
        }
    }

    return false;
}

template < size_t T >
static void add_event_filter( char ( &dest )[ T ], const char *fmt, ... ) ATTRIBUTE_PRINTF( 2, 3 );
template < size_t T >
static void add_event_filter( char ( &dest )[ T ], const char *fmt, ... )
{
    va_list args;
    char expr[ T ];

    va_start( args, fmt );
    vsnprintf_safe( expr, fmt, args );
    va_end( args );

    str_strip_whitespace( dest );

    if ( !dest[ 0 ] )
    {
        strcpy_safe( dest, expr );
    }
    else if ( !strstr_ignore_spaces( dest, expr ) )
    {
        char dest2[ T ];
        bool has_parens = is_surrounded_by_parens( dest );

        strcpy_safe( dest2, dest );
        snprintf_safe( dest, "%s%s%s && (%s)",
                       has_parens ? "" : "(", dest2, has_parens ? "" : ")",
                       expr );
    }
}

template < size_t T >
static void remove_event_filter( char ( &dest )[ T ], const char *fmt, ... ) ATTRIBUTE_PRINTF( 2, 3 );
template < size_t T >
static void remove_event_filter( char ( &dest )[ T ], const char *fmt, ... )
{
    va_list args;
    char expr[ T ];

    va_start( args, fmt );
    vsnprintf_safe( expr, fmt, args );
    va_end( args );

    // Remove '&& expr'
    remove_substrings( dest, "&& %s", expr );
    // Remove 'expr &&'
    remove_substrings( dest, "%s &&", expr );

    for ( int i = 6; i >= 1; i-- )
    {
        // Remove '&& (expr)' strings
        remove_substrings( dest, "&& %.*s%s%.*s", i, "((((((", expr, i, "))))))" );
        // Remove '(expr) &&'
        remove_substrings( dest, "%.*s%s%.*s &&", i, "((((((", expr, i, "))))))" );
    }

    // Remove 'expr'
    remove_substrings( dest, "%s", expr );

    // Remove empty parenthesis
    remove_substrings( dest, "%s", "()" );

    // Remove leading / trailing whitespace
    str_strip_whitespace( dest );
}

TraceWin::TraceWin( const char *filename, size_t filesize )
{
    // Note that m_trace_events is possibly being loaded in
    //  a background thread at this moment, so be sure to check
    //  m_eventsloaded before accessing it...

    m_title = string_format( "%s (%.2f MB)", filename, filesize / ( 1024.0f * 1024.0f ) );
    m_trace_events.m_filename = filename;
    m_trace_events.m_filesize = filesize;

    strcpy_safe( m_eventlist.timegoto_buf, "0.0" );

    strcpy_safe( m_filter.buf, s_ini().GetStr( "event_filter_buf", "" ) );
    m_filter.enabled = !!m_filter.buf[ 0 ];

    m_graph.saved_locs.resize( action_graph_save_location5 - action_graph_save_location1 + 1 );

    m_frame_markers.init();
    m_create_graph_row_dlg.init();
}

TraceWin::~TraceWin()
{
    s_ini().PutStr( "event_filter_buf", m_filter.buf );

    m_graph.rows.shutdown();

    m_frame_markers.shutdown();
    m_create_graph_row_dlg.shutdown();

    s_opts().set_crtc_max( -1 );
}

void TraceWin::render()
{
    uint32_t count = 0;
    TraceEvents::tracestatus_t status = m_trace_events.get_load_status( &count );

    ImGui::Begin( m_title.c_str(), &m_open, ImGuiWindowFlags_MenuBar );

    s_app().render_menu( "menu_tracewin" );

    if ( status == TraceEvents::Trace_Loaded )
    {
        if ( count )
        {
            if ( !m_inited )
            {
                int64_t last_ts = m_trace_events.m_events.back().ts;

                // Initialize our graph rows first time through.
                m_graph.rows.init( m_trace_events );

                m_graph.length_ts = std::min< int64_t >( last_ts, 40 * NSECS_PER_MSEC );
                m_graph.start_ts = last_ts - m_graph.length_ts;
                m_graph.recalc_timebufs = true;

                m_eventlist.do_gotoevent = true;
                m_eventlist.goto_eventid = ts_to_eventid( m_graph.start_ts + m_graph.length_ts / 2 );
            }

            // Update pinned tooltips
            m_ttip.tipwins.update( m_graph.mouse_captured != MOUSE_NOT_CAPTURED );

            if ( !s_opts().getb( OPT_ShowEventList ) ||
                 imgui_collapsingheader( "Event Graph", &m_graph.has_focus, ImGuiTreeNodeFlags_DefaultOpen ) )
            {
                graph_render_options();
                graph_render();
            }

            if ( s_opts().getb( OPT_ShowEventList ) &&
                 imgui_collapsingheader( "Event List", &m_eventlist.has_focus, ImGuiTreeNodeFlags_DefaultOpen ) )
            {
                eventlist_render_options();
                eventlist_render();
                eventlist_handle_hotkeys();
            }

            // Render any pinned tooltips
            m_ttip.tipwins.set_tooltip( "Pinned Tooltip", &m_ttip.visible, m_ttip.str.c_str() );

            // graph/eventlist didn't handle this action, so just toggle ttip visibility.
            if ( s_actions().get( action_graph_pin_tooltip ) )
                m_ttip.visible = !m_ttip.visible;

            // Render plot, graph rows, filter dialogs, etc
            graph_dialogs_render();

            m_inited = true;
        }
    }
    else if ( status == TraceEvents::Trace_Loading ||
              status == TraceEvents::Trace_Initializing )
    {
        bool loading = ( status == TraceEvents::Trace_Loading );

        ImGui::Text( "%s events %u...", loading ? "Loading" : "Initializing", count );

        if ( ImGui::Button( "Cancel" ) ||
             ( ImGui::IsWindowFocused() && s_actions().get( action_escape ) ) )
        {
            s_app().cancel_load_file();
        }
    }
    else
    {
        ImGui::Text( "Error loading file %s...\n", m_trace_events.m_filename.c_str() );
    }

    ImGui::End();
}

void TraceWin::trace_render_info()
{
    size_t event_count = m_trace_events.m_events.size();

    ImGui::Text( "Total Events: %lu\n", event_count );
    if ( !event_count )
        return;

    const trace_info_t &trace_info = m_trace_events.m_trace_info;

    ImGui::Text( "Trace time: %s",
                 ts_to_timestr( m_trace_events.m_events.back().ts, 4 ).c_str() );
    ImGui::Text( "Trace time start: %s",
                 ts_to_timestr( m_trace_events.m_trace_info.trimmed_ts, 4 ).c_str() );

    ImGui::Text( "Trace cpus: %u", trace_info.cpus );

    if ( !trace_info.uname.empty() )
        ImGui::Text( "Trace uname: %s", trace_info.uname.c_str() );

    if ( !m_graph.rows.m_graph_rows_list.empty() &&
         ImGui::CollapsingHeader( "Graph Row Info" ) )
    {
        int tree_tgid = -1;
        bool display_event = true;

        if ( imgui_begin_columns( "row_info", { "Row Name", "Events" } ) )
            ImGui::SetColumnWidth( 0, imgui_scale( 250.0f ) );

        for ( const GraphRows::graph_rows_info_t &info : m_graph.rows.m_graph_rows_list )
        {
            const tgid_info_t *tgid_info = NULL;
            const char *row_name = info.row_name.c_str();
            ftrace_row_info_t *ftrace_row_info = m_trace_events.get_ftrace_row_info( row_name );

            if ( info.type == LOC_TYPE_Comm )
                tgid_info = m_trace_events.tgid_from_commstr( info.row_name.c_str() );

            if ( ( tree_tgid >= 0 ) &&
                 ( !tgid_info || ( tgid_info->tgid != tree_tgid ) ) )
            {
                // Close the tree node
                if ( display_event )
                    ImGui::TreePop();

                tree_tgid = -1;
                display_event = true;
            }

            // If we have tgid_info and it isn't a current tree, create new treenode
            if ( tgid_info && ( tgid_info->tgid != tree_tgid ) )
            {
                size_t count = tgid_info->pids.size();

                // Store current tree tgid
                tree_tgid = tgid_info->tgid;

                display_event = ImGui::TreeNode( &info, "%s (%lu thread%s)",
                                                 tgid_info->commstr_clr, count,
                                                 ( count > 1 ) ? "s" : "" );
                ImGui::NextColumn();
                ImGui::NextColumn();
            }

            if ( display_event )
            {
                // Indent if we're in a tgid tree node
                if ( tree_tgid >= 0 )
                    ImGui::Indent();

                ImGui::Text( "%s", row_name );

                ImGui::NextColumn();
                ImGui::Text( "%lu", ftrace_row_info ? ftrace_row_info->count : info.event_count );

                if ( info.type == LOC_TYPE_Plot )
                {
                    GraphPlot *plot = m_trace_events.get_plot_ptr( info.row_name.c_str() );

                    if ( plot )
                    {
                        ImGui::SameLine();
                        ImGui::Text( "(minval:%.2f maxval:%.2f)", plot->m_minval, plot->m_maxval );
                    }
                }
                ImGui::NextColumn();

                if ( tree_tgid >= 0 )
                    ImGui::Unindent();
            }
        }

        if ( ( tree_tgid >= 0 ) && display_event )
            ImGui::TreePop();

        ImGui::EndColumns();
    }

    if ( ImGui::CollapsingHeader( "Event info" ) )
    {
        if ( imgui_begin_columns( "event_info", { "Event Name", "Count", "Pct" } ) )
        {
            ImGui::SetColumnWidth( 0, imgui_scale( 200.0f ) );
            ImGui::SetColumnWidth( 1, imgui_scale( 75.0f ) );
        }

        for ( auto item : m_trace_events.m_eventnames_locs.m_locs.m_map )
        {
            const char *eventname = m_trace_events.m_strpool.findstr( item.first );
            const std::vector< uint32_t > &locs = item.second;

            ImGui::Text( "%s", eventname );
            ImGui::NextColumn();
            ImGui::Text( "%lu", locs.size() );
            ImGui::NextColumn();
            ImGui::Text( "%.2f%%", 100.0f * locs.size() / event_count );
            ImGui::NextColumn();
        }

        ImGui::EndColumns();
    }

    if ( !trace_info.cpu_info.empty() &&
         ImGui::CollapsingHeader( "CPU Info" ) )
    {
        if ( imgui_begin_columns( "cpu_stats", { "CPU", "Stats", "Events", "Min ts", "Max ts", "File Size" } ) )
            ImGui::SetColumnWidth( 0, imgui_scale( 75.0f ) );

        for ( uint32_t cpu = 0; cpu < trace_info.cpu_info.size(); cpu++ )
        {
            const cpu_info_t &cpu_info = trace_info.cpu_info[ cpu ];

            // CPU: 0, CPU: 1, etc.
            ImGui::Text( "CPU: %u", cpu );
            ImGui::NextColumn();

            // Stats
            ImGui::BeginGroup();
            if ( cpu_info.entries )
                ImGui::Text( "Entries: %" PRIu64, cpu_info.entries );
            if ( cpu_info.overrun )
                ImGui::Text( "Overrun: %" PRIu64, cpu_info.overrun );
            if ( cpu_info.commit_overrun )
                ImGui::Text( "Commit overrun: %" PRIu64, cpu_info.commit_overrun );
            ImGui::Text( "Bytes: %" PRIu64, cpu_info.bytes );
            ImGui::Text( "Oldest event ts: %s", ts_to_timestr( cpu_info.oldest_event_ts, 6 ).c_str() );
            ImGui::Text( "Now ts: %s", ts_to_timestr( cpu_info.now_ts, 6 ).c_str() );
            if ( cpu_info.dropped_events )
                ImGui::Text( "Dropped events: %" PRIu64, cpu_info.dropped_events );
            ImGui::Text( "Read events: %" PRIu64, cpu_info.read_events );
            //$ ImGui::Text( "file offset: %" PRIu64 "\n", cpu_info.file_offset );
            ImGui::EndGroup();

            if ( ImGui::IsItemHovered() )
            {
                const char *text[] =
                {
                    "Ring buffer stats:",
                    "  Entries: The number of events that are still in the buffer.",
                    "  Overrun: The number of lost events due to overwriting when the buffer was full.",
                    "  Commit overrun: Should always be zero.",
                    "    This gets set if so many events happened within a nested event (ring buffer is re-entrant),",
                    "    that it fills the buffer and starts dropping events.",
                    "  Bytes: Bytes actually read (not overwritten).",
                    "  Oldest event ts: The oldest timestamp in the buffer.",
                    "  Now ts: The current timestamp.",
                    "  Dropped events: Events lost due to overwrite option being off.",
                    "  Read events: The number of events read."
                };
                const char *clr_bright = s_textclrs().str( TClr_Bright );
                const char *clr_def = s_textclrs().str( TClr_Def );

                ImGui::BeginTooltip();
                for ( size_t i = 0; i < ARRAY_SIZE( text ); i++ )
                {
                    const char *str = text[ i ];
                    const char *colon = strchr( str, ':' );

                    if ( colon )
                        ImGui::Text( "%s%.*s%s%s", clr_bright, ( int )( colon - str ), str, clr_def, colon );
                    else
                        ImGui::Text( "%s", str );
                }
                ImGui::EndTooltip();
            }
            ImGui::NextColumn();

            // Events
            ImGui::Text( "%" PRIu64 " / %" PRIu64, cpu_info.events, cpu_info.tot_events );
            ImGui::NextColumn();

            // Min ts
            if ( cpu_info.min_ts != INT64_MAX )
                ImGui::Text( "%s", ts_to_timestr( cpu_info.min_ts, 6 ).c_str() );
            ImGui::NextColumn();

            // Max ts
            if ( cpu_info.max_ts != INT64_MAX )
                ImGui::Text( "%s", ts_to_timestr( cpu_info.max_ts, 6 ).c_str() );
            ImGui::NextColumn();

            // File Size
            if  ( cpu_info.tot_events )
                ImGui::Text( "%" PRIu64 "\n%.2f b/event\n", cpu_info.file_size, ( float )cpu_info.file_size / cpu_info.tot_events );
            else
                ImGui::Text( "%" PRIu64 "\n", cpu_info.file_size );
            ImGui::NextColumn();

            ImGui::Separator();
        }

        ImGui::EndColumns();
    }
}

void TraceWin::graph_center_event( uint32_t eventid )
{
    trace_event_t &event = get_event( eventid );

    m_eventlist.selected_eventid = event.id;
    m_graph.start_ts = event.ts - m_graph.length_ts / 2;
    m_graph.recalc_timebufs = true;
    m_graph.show_row_name = event.comm;
}

bool TraceWin::eventlist_render_popupmenu( uint32_t eventid )
{
    if ( !ImGui::BeginPopup( "EventsListPopup" ) )
        return false;

    imgui_text_bg( ImGui::GetStyleColorVec4( ImGuiCol_Header ), "%s", "Options" );
    ImGui::Separator();

    trace_event_t &event = get_event( eventid );

    std::string label = string_format( "Center event %u on graph", event.id );
    if ( ImGui::MenuItem( label.c_str() ) )
        graph_center_event( eventid );

    // Set / Goto / Clear Markers
    {
        int idx = graph_marker_menuitem( "Set Marker", false, action_graph_set_markerA );
        if ( idx >= 0 )
            graph_marker_set( idx, event.ts );

        idx = graph_marker_menuitem( "Goto Marker", true, action_graph_goto_markerA );
        if ( idx >= 0 )
        {
            m_graph.start_ts = m_graph.ts_markers[ idx ] - m_graph.length_ts / 2;
            m_graph.recalc_timebufs = true;
        }

        idx = graph_marker_menuitem( "Clear Marker", true, action_nil );
        if ( idx >= 0 )
            graph_marker_set( idx, INT64_MAX );
    }

    ImGui::Separator();

    label = string_format( "Add '$name == %s' filter", event.name );
    if ( ImGui::MenuItem( label.c_str() ) )
    {
        remove_event_filter( m_filter.buf, "$name != \"%s\"", event.name );
        add_event_filter( m_filter.buf, "$name == \"%s\"", event.name );
        m_filter.enabled = true;
    }
    label = string_format( "Add '$name != %s' filter", event.name );
    if ( ImGui::MenuItem( label.c_str() ) )
    {
        remove_event_filter( m_filter.buf, "$name == \"%s\"", event.name );
        add_event_filter( m_filter.buf, "$name != \"%s\"", event.name );
        m_filter.enabled = true;
    }

    label = string_format( "Add '$pid == %d' filter", event.pid );
    if ( ImGui::MenuItem( label.c_str() ) )
    {
        remove_event_filter( m_filter.buf, "$pid != %d", event.pid );
        add_event_filter( m_filter.buf, "$pid == %d", event.pid );
        m_filter.enabled = true;
    }
    label = string_format( "Add '$pid != %d' filter", event.pid );
    if ( ImGui::MenuItem( label.c_str() ) )
    {
        remove_event_filter( m_filter.buf, "$pid == %d", event.pid );
        add_event_filter( m_filter.buf, "$pid != %d", event.pid );
        m_filter.enabled = true;
    }

    const tgid_info_t *tgid_info = m_trace_events.tgid_from_pid( event.pid );
    if ( tgid_info )
    {
        ImGui::Separator();

        label = string_format( "Filter process '%s' events", tgid_info->commstr_clr );
        if ( ImGui::MenuItem( label.c_str() ) )
        {
            remove_event_filter( m_filter.buf, "$tgid != %d", tgid_info->tgid );
            add_event_filter( m_filter.buf, "$tgid == %d", tgid_info->tgid );
            m_filter.enabled = true;
        }
        label = string_format( "Hide process '%s' events", tgid_info->commstr_clr );
        if ( ImGui::MenuItem( label.c_str() ) )
        {
            remove_event_filter( m_filter.buf, "$tgid == %d", tgid_info->tgid );
            add_event_filter( m_filter.buf, "$tgid != %d", tgid_info->tgid );
            m_filter.enabled = true;
        }
    }

    if ( !m_filter.events.empty() )
    {
        ImGui::Separator();

        if ( ImGui::MenuItem( "Clear Filter" ) )
        {
            m_filter.buf[ 0 ] = 0;
            m_filter.enabled = true;
        }
    }

    const std::string plot_str = CreatePlotDlg::get_plot_str( event );
    if ( !plot_str.empty() )
    {
        std::string plot_label = std::string( "Create Plot for " ) + plot_str;

        ImGui::Separator();
        if ( ImGui::MenuItem( plot_label.c_str() ) )
            m_create_plot_eventid = event.id;
    }

    ImGui::Separator();

    if ( ImGui::MenuItem( "Set Frame Markers..." ) )
        m_create_filter_eventid = event.id;

    if ( ImGui::MenuItem( "Edit Frame Markers..." ) )
        m_create_filter_eventid = m_trace_events.m_events.size();

    if ( m_frame_markers.m_left_frames.size() &&
         ImGui::MenuItem( "Clear Frame Markers" ) )
    {
        m_frame_markers.m_left_frames.clear();
        m_frame_markers.m_right_frames.clear();
    }

    if ( s_actions().get( action_escape ) )
        ImGui::CloseCurrentPopup();

    ImGui::EndPopup();
    return true;
}

static std::string get_event_fields_str( const trace_event_t &event, const char *eqstr, char sep )
{
    std::string fieldstr;

    if ( event.user_comm != event.comm )
        fieldstr += string_format( "%s%s%s%c", "user_comm", eqstr, event.user_comm, sep );

    for ( uint32_t i = 0; i < event.numfields; i++ )
    {
        std::string buf;
        const char *key = event.fields[ i ].key;
        const char *value = event.fields[ i ].value;

        if ( event.is_ftrace_print() && !strcmp( key, "buf" ) )
        {
            buf = s_textclrs().mstr( value, event.color );
            value = buf.c_str();
        }

        fieldstr += string_format( "%s%s%s%c", key, eqstr, value, sep );
    }

    fieldstr += string_format( "%s%s%s", "system", eqstr, event.system );

    return fieldstr;
}

static float get_keyboard_scroll_lines( float visible_rows )
{
    float scroll_lines = 0.0f;

    if ( ImGui::IsWindowFocused() && s_actions().count() )
    {
        if ( s_actions().get( action_scroll_pagedown ) )
            scroll_lines = std::max< float>( visible_rows - 5, 1 );
        else if ( s_actions().get( action_scroll_pageup ) )
            scroll_lines = std::min< float >( -( visible_rows - 5), -1 );
        else if ( s_actions().get( action_scroll_down ) )
            scroll_lines = 1;
        else if ( s_actions().get( action_scroll_up ) )
            scroll_lines = -1;
        else if ( s_actions().get( action_scroll_home ) )
            scroll_lines = -ImGui::GetScrollMaxY();
        else if ( s_actions().get( action_scroll_end ) )
            scroll_lines = ImGui::GetScrollMaxY();
    }

    return scroll_lines;
}

bool TraceWin::eventlist_handle_mouse( const trace_event_t &event, uint32_t i )
{
    bool popup_shown = false;

    // Check if item is hovered and we don't have a popup menu going already.
    if ( !is_valid_id( m_eventlist.popup_eventid ) &&
         ImGui::IsItemHovered() &&
         ImGui::IsRootWindowOrAnyChildFocused() )
    {
        // Store the hovered event id.
        m_eventlist.hovered_eventid = event.id;
        m_graph.last_hovered_eventid = event.id;

        if ( ImGui::IsMouseClicked( 1 ) )
        {
            // If they right clicked, show the context menu.
            m_eventlist.popup_eventid = i;

            // Open the popup for eventlist_render_popupmenu().
            ImGui::OpenPopup( "EventsListPopup" );
        }
        else
        {
            // Otherwise show a tooltip.
            std::string ttip = s_textclrs().str( TClr_Def );
            std::string ts_str = ts_to_timestr( event.ts, 6 );
            const char *commstr = m_trace_events.tgidcomm_from_pid( event.pid );

            if ( graph_marker_valid( 0 ) || graph_marker_valid( 1 ) )
            {
                if ( graph_marker_valid( 0 ) )
                    ttip += "Marker A: " + ts_to_timestr( m_graph.ts_markers[ 0 ] - event.ts, 2, " ms\n" );
                if ( graph_marker_valid( 1 ) )
                    ttip += "Marker B: " + ts_to_timestr( m_graph.ts_markers[ 1 ] - event.ts, 2, " ms\n" );
                ttip += "\n";
            }

            ttip += string_format( "Id: %u\nTime: %s\nComm: %s\nCpu: %u\nEvent: %s\n",
                                   event.id,
                                   ts_str.c_str(),
                                   commstr,
                                   event.cpu,
                                   event.name );

            if ( event.has_duration() )
                ttip += "Duration: " + ts_to_timestr( event.duration, 4, " ms\n" );

            ttip += "\n";
            ttip += get_event_fields_str( event, ": ", '\n' );

            ImGui::SetTooltip( "%s", ttip.c_str() );

            if ( s_actions().get( action_graph_pin_tooltip ) )
            {
                m_ttip.str = ttip;
                m_ttip.visible = true;
            }
        }
    }

    // If we've got an active popup menu, render it.
    if ( m_eventlist.popup_eventid == i )
    {
        ImGui::PushStyleColor( ImGuiCol_Text, s_clrs().getv4( col_ImGui_Text ) );

        uint32_t eventid = !m_filter.events.empty() ?
                    m_filter.events[ m_eventlist.popup_eventid ] :
                    m_eventlist.popup_eventid;

        if ( !TraceWin::eventlist_render_popupmenu( eventid ) )
            m_eventlist.popup_eventid = INVALID_ID;

        popup_shown = true;

        ImGui::PopStyleColor();
    }

    return popup_shown;
}

static void draw_ts_line( const ImVec2 &pos, ImU32 color )
{
    ImGui::PopClipRect();

    float max_x = ImGui::GetWindowDrawList()->GetClipRectMax().x;
    float spacing_U = ( float )( int )( ImGui::GetStyle().ItemSpacing.y * 0.5f );
    float pos_y = pos.y - spacing_U;

    ImGui::GetWindowDrawList()->AddLine(
                ImVec2( pos.x, pos_y ), ImVec2( max_x, pos_y ),
                color, imgui_scale( 2.0f ) );

    ImGui::PushColumnClipRect();
}

static bool imgui_input_uint32( uint32_t *pval, float w, const char *label, const char *label2, ImGuiInputTextFlags flags = 0 )
{
    int val = *pval;
    bool ret = ImGui::Button( label );

    ImGui::SameLine();
    ImGui::PushItemWidth( imgui_scale( w ) );
    ret |= ImGui::InputInt( label2, &val, 0, 0, flags );
    ImGui::PopItemWidth();

    if ( ret )
        *pval = val;

    return ret;
}

void TraceWin::eventlist_render_options()
{
    // Goto event
    m_eventlist.do_gotoevent |= imgui_input_uint32( &m_eventlist.goto_eventid, 75.0f, "Goto Event:", "##GotoEvent" );
    if ( ImGui::IsItemActive() )
        m_eventlist.ts_marker_mouse_sync = m_graph.ts_marker_mouse;

    ImGui::SameLine();
    if ( imgui_input_text2( "Goto Time:", m_eventlist.timegoto_buf, 120.0f, ImGuiInputText2FlagsLeft_LabelIsButton ) )
    {
        m_eventlist.do_gotoevent = true;
        m_eventlist.goto_eventid = timestr_to_eventid( m_eventlist.timegoto_buf );
    }

    if ( !m_inited ||
         m_eventlist.hide_sched_switch_events_val != s_opts().getb( OPT_HideSchedSwitchEvents ) )
    {
        bool hide_sched_switch = s_opts().getb( OPT_HideSchedSwitchEvents );
        static const char filter_str[] = "$name != \"sched_switch\"";

        remove_event_filter( m_filter.buf, "$name == \"sched_switch\"" );
        remove_event_filter( m_filter.buf, filter_str );

        if ( hide_sched_switch )
            add_event_filter( m_filter.buf, filter_str );

        m_filter.enabled = true;
        m_eventlist.hide_sched_switch_events_val = hide_sched_switch;
    }

    if ( m_filter.enabled ||
         imgui_input_text2( "Event Filter:", m_filter.buf, 500.0f,
                            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputText2FlagsLeft_LabelIsButton ) )
    {
        m_filter.events.clear();
        m_filter.pid_eventcount.m_map.clear();
        m_filter.errstr.clear();
        m_filter.enabled = false;

        if ( m_filter.buf[ 0 ] )
        {
            tdop_get_key_func get_key_func = std::bind( filter_get_key_func, &m_trace_events.m_strpool, _1, _2 );
            class TdopExpr *tdop_expr = tdopexpr_compile( m_filter.buf, get_key_func, m_filter.errstr );

            util_time_t t0 = util_get_time();

            if ( tdop_expr )
            {
                for ( trace_event_t &event : m_trace_events.m_events )
                {
                    tdop_get_keyval_func get_keyval_func = std::bind( filter_get_keyval_func,
                                                                      &m_trace_events.m_trace_info, &event, _1, _2 );

                    const char *ret = tdopexpr_exec( tdop_expr, get_keyval_func );

                    event.is_filtered_out = !ret[ 0 ];
                    if ( !event.is_filtered_out )
                    {
                        m_filter.events.push_back( event.id );

                        // Bump up count of !filtered events for this pid
                        uint32_t *count = m_filter.pid_eventcount.get_val( event.pid, 0 );
                        (*count)++;
                    }
                }

                if ( m_filter.events.empty() )
                    m_filter.errstr = "WARNING: No events found.";

                tdopexpr_delete( tdop_expr );
                tdop_expr = NULL;
            }

            float time = util_time_to_ms( t0, util_get_time() );
            if ( time > 1000.0f )
                logf( "tdopexpr_compile(\"%s\"): %.2fms\n", m_filter.buf, time );
        }
    }

    if ( ImGui::IsItemHovered() )
    {
        std::string ttip;

        ttip += s_textclrs().bright_str( "Event Filter\n\n" );
        ttip += "Vars: Any field in Info column plus:\n";
        ttip += "    $name, $comm, $user_comm, $id, $pid, $tgid, $ts, $cpu, $duration\n";
        ttip += "Operators: &&, ||, !=, =, >, >=, <, <=, =~\n\n";

        ttip += "Examples:\n";
        ttip += "  $pid = 4615\n";
        ttip += "  $ts >= 11.1 && $ts < 12.5\n";
        ttip += "  $ring_name = 0xffff971e9aa6bdd0\n";
        ttip += "  $buf =~ \"[Compositor] Warp\"\n";
        ttip += "  ( $timeline = gfx ) && ( $id < 10 || $id > 100 )";

        ImGui::SetTooltip( "%s", ttip.c_str() );

        if ( s_actions().get( action_graph_pin_tooltip ) )
        {
            m_ttip.str = ttip;
            m_ttip.visible = true;
        }
    }

    ImGui::SameLine();
    if ( ImGui::Button( "Clear Filter" ) )
    {
        m_filter.events.clear();
        m_filter.pid_eventcount.m_map.clear();
        m_filter.errstr.clear();
        m_filter.buf[ 0 ] = 0;
    }

    if ( !m_filter.errstr.empty() )
    {
        ImGui::SameLine();
        ImGui::TextColored( ImVec4( 1, 0, 0, 1 ), "%s", m_filter.errstr.c_str() );
    }
    else if ( !m_filter.events.empty() )
    {
        std::string label = string_format( "Graph only filtered (%lu events)", m_filter.events.size() );

        ImGui::SameLine();
        s_opts().render_imgui_opt( OPT_GraphOnlyFiltered );

        if ( s_opts().getb( OPT_GraphOnlyFiltered ) )
        {
            ImGui::SameLine();
            s_opts().render_imgui_opt( OPT_Graph_HideEmptyFilteredRows );
        }
    }
}

void TraceWin::eventlist_render()
{
    size_t event_count;
    std::vector< uint32_t > *filtered_events = NULL;

    if ( ImGui::GetIO().KeyShift && !m_eventlist.highlight_ids.empty() )
    {
        // If shift is down and we have a tooltip with highlighted events,
        // show only those
        filtered_events = &m_eventlist.highlight_ids;
        event_count = filtered_events->size();
    }
    else if ( !m_filter.events.empty() )
    {
        // Otherwise display filtered events
        filtered_events = &m_filter.events;
        event_count = filtered_events->size();
    }
    else
    {
        // Display all events
        event_count = m_trace_events.m_events.size();
    }

    // Set focus on event list first time we open.
    if ( s_actions().get( action_focus_eventlist ) ||
         ( !m_inited && ImGui::IsWindowFocused() ) )
    {
        ImGui::SetNextWindowFocus();
    }

    // Events list
    {
        float lineh = ImGui::GetTextLineHeightWithSpacing();
        const ImVec2 content_avail = ImGui::GetContentRegionAvail();

        int eventlist_row_count = s_opts().geti( OPT_EventListRowCount );

        // If the user has set the event list row count to 0 (auto size), make
        //  sure we always have at least 20 rows.
        if ( !eventlist_row_count && ( content_avail.y / lineh < 20 ) )
            eventlist_row_count = 20;

        // Set the child window size to hold count of items + header + separator
        float sizey = eventlist_row_count * lineh;

        ImGui::SetNextWindowContentSize( { 0.0f, ( event_count + 1 ) * lineh + 1 } );
        ImGui::BeginChild( "eventlistbox", ImVec2( 0.0f, sizey ) );

        m_eventlist.has_focus = ImGui::IsWindowFocused();

        float winh = ImGui::GetWindowHeight();
        uint32_t visible_rows = ( winh + 1 ) / lineh;

        float scroll_lines = get_keyboard_scroll_lines( visible_rows );
        if ( scroll_lines )
            ImGui::SetScrollY( ImGui::GetScrollY() + scroll_lines * lineh );

        if ( s_opts().getb( OPT_SyncEventListToGraph ) &&
             !m_eventlist.do_gotoevent &&
             ( m_graph.ts_marker_mouse != -1 ) &&
             ( m_graph.ts_marker_mouse != m_eventlist.ts_marker_mouse_sync ) )
        {
            m_eventlist.do_gotoevent = true;
            m_eventlist.goto_eventid = ts_to_eventid( m_graph.ts_marker_mouse );
        }

        if ( m_eventlist.do_gotoevent )
        {
            uint32_t pos;

            if ( filtered_events )
            {
                auto i = std::lower_bound( filtered_events->begin(), filtered_events->end(),
                                           m_eventlist.goto_eventid );

                pos = i - filtered_events->begin();
            }
            else
            {
                pos = m_eventlist.goto_eventid;
            }
            pos = std::min< uint32_t >( pos, event_count - 1 );

            ImGui::SetScrollY( ( ( float )pos - visible_rows / 2 + 1 ) * lineh );

            // Select the event also
            m_eventlist.selected_eventid = std::min< uint32_t >( m_eventlist.goto_eventid,
                                                                 event_count - 1 );

            m_eventlist.do_gotoevent = false;
            m_eventlist.ts_marker_mouse_sync = m_graph.ts_marker_mouse;
        }

        float scrolly = ImGui::GetScrollY();
        uint32_t start_idx = Clamp< uint32_t >( scrolly / lineh, 1, event_count ) - 1;
        uint32_t end_idx = std::min< uint32_t >( start_idx + 2 + visible_rows, event_count );

        // Draw columns
        imgui_begin_columns( "event_list", { "Id", "Time Stamp", "Comm", "Cpu", "Event", "Duration", "Info" },
                             &m_eventlist.columns_resized );
        {
            bool popup_shown = false;

            // Reset our hovered event id
            m_eventlist.hovered_eventid = INVALID_ID;

            // Move cursor position down to where we've scrolled.
            if ( start_idx > 0 )
                ImGui::SetCursorPosY( ImGui::GetCursorPosY() + lineh * ( start_idx - 1 ) );

            if ( filtered_events )
            {
                m_eventlist.start_eventid = filtered_events->at( start_idx );
                m_eventlist.end_eventid = filtered_events->at( end_idx - 1 );
            }
            else
            {
                m_eventlist.start_eventid = start_idx;
                m_eventlist.end_eventid = end_idx;
            }

            int64_t prev_ts = INT64_MIN;

            // Loop through and draw events
            for ( uint32_t i = start_idx; i < end_idx; i++ )
            {
                std::string markerbuf;
                trace_event_t &event = filtered_events ?
                        m_trace_events.m_events[ filtered_events->at( i ) ] :
                        m_trace_events.m_events[ i ];
                bool selected = ( m_eventlist.selected_eventid == event.id );
                ImVec2 cursorpos = ImGui::GetCursorScreenPos();
                ImVec4 color = s_clrs().getv4( col_EventList_Text );

                ImGui::PushID( i );

                if ( event.ts == m_graph.ts_markers[ 1 ] )
                {
                    color = s_clrs().getv4( col_Graph_MarkerB );
                    markerbuf = s_textclrs().mstr( "(B)", ( ImColor )color );
                }
                if ( event.ts == m_graph.ts_markers[ 0 ] )
                {
                    color = s_clrs().getv4( col_Graph_MarkerA );
                    markerbuf = s_textclrs().mstr( "(A)", ( ImColor )color ) + markerbuf;
                }
                if ( event.is_vblank() )
                    color = s_clrs().getv4( ( event.crtc > 0 ) ? col_VBlank1 : col_VBlank0 );

                ImGui::PushStyleColor( ImGuiCol_Text, color );

                if ( selected )
                {
                    ImGui::PushStyleColor( ImGuiCol_Header, s_clrs().getv4( col_EventList_Sel ) );
                }
                else
                {
                    // If this event is in the highlighted list, give it a bit of a colored background
                    selected = std::binary_search(
                                m_eventlist.highlight_ids.begin(), m_eventlist.highlight_ids.end(), event.id );

                    if ( selected )
                        ImGui::PushStyleColor( ImGuiCol_Header, s_clrs().getv4( col_EventList_Hov ) );
                }

                // column 0: event id
                {
                    std::string label = std::to_string( event.id ) + markerbuf;
                    ImGuiSelectableFlags flags = ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick;

                    if ( ImGui::Selectable( label.c_str(), selected, flags ) )
                    {
                        if ( ImGui::IsMouseDoubleClicked( 0 ) )
                            graph_center_event( event.id );

                        m_eventlist.selected_eventid = event.id;
                    }

                    // Columns bug workaround: selectable with SpanAllColumns & overlaid button does not work.
                    // https://github.com/ocornut/imgui/issues/684
                    ImGui::SetItemAllowOverlap();

                    // Handle popup menu / tooltip
                    popup_shown |= eventlist_handle_mouse( event, i );

                    ImGui::NextColumn();
                }

                // column 1: time stamp
                {
                    std::string ts_str = ts_to_timestr( event.ts, 6 );

                    // Show time delta from previous event
                    if ( prev_ts != INT64_MIN )
                        ts_str += " (+" + ts_to_timestr( event.ts - prev_ts, 4, "" ) + ")";

                    ImGui::Text( "%s", ts_str.c_str() );
                    ImGui::NextColumn();
                }

                // column 2: comm
                {
                    const tgid_info_t *tgid_info = m_trace_events.tgid_from_pid( event.pid );

                    if ( tgid_info )
                        ImGui::Text( "%s (%s)", event.comm, tgid_info->commstr_clr );
                    else
                        ImGui::Text( "%s", event.comm );
                    ImGui::NextColumn();
                }

                // column 3: cpu
                {
                    ImGui::Text( "%u", event.cpu );
                    ImGui::NextColumn();
                }

                // column 4: event name
                {
                    ImGui::Text( "%s", event.name );
                    ImGui::NextColumn();
                }

                // column 5: duration
                {
                    if ( event.has_duration() )
                        ImGui::Text( "%s", ts_to_timestr( event.duration, 4 ).c_str() );
                    ImGui::NextColumn();
                }

                // column 6: event fields
                {
                    if ( event.is_ftrace_print() )
                    {
                        const char *buf = get_event_field_val( event, "buf" );
                        std::string seqno = m_trace_events.get_ftrace_ctx_str( event );

                        ImGui::TextColored( ImColor( event.color ), "%s%s", buf, seqno.c_str() );
                    }
                    else
                    {
                        std::string fieldstr = get_event_fields_str( event, "=", ' ' );

                        ImGui::Text( "%s", fieldstr.c_str() );
                    }
                    ImGui::NextColumn();
                }

                if ( ( prev_ts < m_graph.ts_marker_mouse ) &&
                     ( event.ts > m_graph.ts_marker_mouse ) )
                {
                    // Draw time stamp marker diff line if we're right below ts_marker_mouse
                    draw_ts_line( cursorpos, s_clrs().get( col_Graph_MousePos ) );
                }
                else
                {
                    for ( size_t idx = 0; idx < ARRAY_SIZE( m_graph.ts_markers ); idx++ )
                    {
                        if ( ( prev_ts < m_graph.ts_markers[ idx ] ) &&
                             ( event.ts > m_graph.ts_markers[ idx ] ) )
                        {
                            draw_ts_line( cursorpos, s_clrs().get( col_Graph_MarkerA + idx ) );
                            break;
                        }
                    }
                }

                ImGui::PopStyleColor( 1 + selected );
                ImGui::PopID();

                prev_ts = event.ts;
            }

            if ( !popup_shown )
            {
                // When we modify a filter via the context menu, it can hide the item
                //  we right clicked on which means eventlist_render_popupmenu() won't get
                //  called. Check for that case here.
                m_eventlist.popup_eventid = INVALID_ID;
            }
        }
        if ( imgui_end_columns() )
            m_eventlist.columns_resized = true;

        ImGui::EndChild();
    }

    // If we are displaying highlighted events only, reset the mouse marker so the
    // next render frame we'll recalc our event list location
    if ( filtered_events == &m_eventlist.highlight_ids )
        m_eventlist.ts_marker_mouse_sync = -1;
}

void TraceWin::eventlist_handle_hotkeys()
{
    if ( m_eventlist.has_focus )
    {
        if ( is_valid_id( m_eventlist.hovered_eventid ) )
        {
            int marker = -1;

            if ( s_actions().get( action_graph_set_markerA ) )
                marker = 0;
            else if ( s_actions().get( action_graph_set_markerB ) )
                marker = 1;

            if ( marker != -1 )
            {
                const trace_event_t &event = get_event( m_eventlist.hovered_eventid );
                graph_marker_set( marker, event.ts );
            }
        }
    }
}

void TraceWin::graph_dialogs_render()
{
    // Plots
    if ( is_valid_id( m_create_plot_eventid ) )
    {
        m_create_plot_dlg.init( m_trace_events, m_create_plot_eventid );
        m_create_plot_eventid = INVALID_ID;
    }
    if ( m_create_plot_dlg.render_dlg( m_trace_events ) )
        m_graph.rows.add_row( m_create_plot_dlg.m_plot_name, m_create_plot_dlg.m_plot_name );

    // Graph rows
    if ( is_valid_id( m_create_graph_row_eventid ) )
    {
        m_create_graph_row_dlg.show_dlg( m_trace_events, m_create_graph_row_eventid );
        m_create_graph_row_eventid = INVALID_ID;
    }
    if ( m_create_graph_row_dlg.render_dlg( m_trace_events ) )
    {
        m_graph.rows.add_row( m_create_graph_row_dlg.m_name_buf,
                              m_create_graph_row_dlg.m_filter_buf );
    }

    // Filter events
    if ( is_valid_id( m_create_filter_eventid ) )
    {
        m_frame_markers.show_dlg( m_trace_events, m_create_filter_eventid );
        m_create_filter_eventid = INVALID_ID;
    }
    m_frame_markers.render_dlg( m_trace_events );
}

static const std::string trace_info_label( TraceEvents &trace_events )
{
    const char *basename = get_path_filename( trace_events.m_filename.c_str() );

    return string_format( "Info for '%s'", s_textclrs().bright_str( basename ).c_str() );
}

void MainApp::render_menu_options()
{
    if ( s_actions().get( action_escape ) )
        ImGui::CloseCurrentPopup();

    {
        ImGui::TextColored( s_clrs().getv4( col_BrightText ), "%s", "Windows" );
        ImGui::Indent();

        if ( ImGui::MenuItem( "GpuVis Help", s_actions().hotkey_str( action_help ).c_str() ) )
        {
            ImGui::SetWindowFocus( "GpuVis Help" );
            m_show_help = true;
        }

        if ( ImGui::MenuItem( "Gpuvis Console" ) )
            m_focus_gpuvis_console = true;

        if ( ImGui::MenuItem( "Font Options" ) )
        {
            ImGui::SetWindowFocus( "Font Options" );
            m_show_font_window = true;
        }

        if ( ImGui::MenuItem( "Color Configuration" ) )
        {
            ImGui::SetWindowFocus( "Color Configuration" );
            m_show_color_picker = true;
        }

        // If we have a trace window and the events are loaded, show Trace Info menu item
        if ( is_trace_loaded() )
        {
            const std::string label = trace_info_label( m_trace_win->m_trace_events );

            ImGui::Separator();

            if ( ImGui::MenuItem( label.c_str(), s_actions().hotkey_str( action_trace_info ).c_str() ) )
            {
                ImGui::SetWindowFocus( label.c_str() );
                m_show_trace_info = label;
            }
        }

        ImGui::Separator();

        if ( ImGui::MenuItem( "ImGui Style Editor" ) )
        {
            ImGui::SetWindowFocus( "Style Editor" );
            m_show_imgui_style_editor = true;
        }

        if ( ImGui::MenuItem( "ImGui Metrics" ) )
        {
            ImGui::SetWindowFocus( "ImGui Metrics" );
            m_show_imgui_metrics_editor = true;
        }

        if ( ImGui::MenuItem( "ImGui Test Window" ) )
        {
            ImGui::SetWindowFocus( "ImGui Demo" );
            m_show_imgui_test_window = true;
        }

        ImGui::Unindent();
    }

    ImGui::Separator();

    ImGui::TextColored( s_clrs().getv4( col_BrightText ), "%s", "Gpuvis Settings" );
    ImGui::Indent();

    s_opts().render_imgui_options();

    ImGui::Unindent();
}

void MainApp::render_font_options()
{
    static const char lorem_str[] =
        "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do"
        "eiusmod tempor incididunt ut labore et dolore magna aliqua. Ut enim ad minim"
        "veniam, quis nostrud exercitation ullamco laboris nisi ut aliquip ex ea commodo"
        "consequat. Duis aute irure dolor in reprehenderit in voluptate velit esse"
        "cillum dolore eu fugiat nulla pariatur. Excepteur sint occaecat cupidatat non"
        "proident, sunt in culpa qui officia deserunt mollit anim id est laborum.";

    ImGui::Indent();
    ImGui::PushID( "font_options" );

    {
        bool changed = false;

#ifdef USE_FREETYPE
        changed |= s_opts().render_imgui_opt( OPT_UseFreetype );
#endif
        changed |= s_opts().render_imgui_opt( OPT_Scale );

        if ( ImGui::Button( "Reset to Defaults" ) )
        {
            m_font_main.m_reset = true;
            m_font_small.m_reset = true;
            m_font_big.m_reset = true;
            changed = true;
        }

        if ( changed )
        {
            // Ping font change so this stuff will reload in main loop.
            m_font_main.m_changed = true;
        }
    }

    if ( ImGui::TreeNodeEx( "Main Font", ImGuiTreeNodeFlags_DefaultOpen ) )
    {
        std::string font_name = s_textclrs().bright_str( m_font_main.m_name );

        ImGui::TextWrapped( "%s: %s", font_name.c_str(), lorem_str );

        m_font_main.render_font_options( s_opts().getb( OPT_UseFreetype ) );
        ImGui::TreePop();
    }

    if ( ImGui::TreeNodeEx( "Small Font", ImGuiTreeNodeFlags_DefaultOpen ) )
    {
        std::string font_name = s_textclrs().bright_str( m_font_small.m_name );

        ImGui::BeginChild( "small_font", ImVec2( 0, ImGui::GetTextLineHeightWithSpacing() * 4 ) );

        imgui_push_smallfont();
        ImGui::TextWrapped( "%s: %s", font_name.c_str(), lorem_str );
        imgui_pop_font();

        ImGui::EndChild();

        m_font_small.render_font_options( s_opts().getb( OPT_UseFreetype ) );

        ImGui::TreePop();
    }

    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    if (ImGui::TreeNode( "Font atlas texture", "Atlas texture (%dx%d pixels)", atlas->TexWidth, atlas->TexHeight ) )
    {
        ImGui::Image( atlas->TexID,
                      ImVec2( (float )atlas->TexWidth, ( float )atlas->TexHeight),
                      ImVec2( 0, 0 ), ImVec2( 1, 1 ),
                      ImVec4( 1, 1, 1, 1 ), ImVec4( 1, 1, 1, 0.5f ) );
        ImGui::TreePop();
    }

    ImGui::PopID();
    ImGui::Unindent();
}

static void render_color_items( colors_t i0, colors_t i1,
        colors_t *selected_color, std::string *selected_color_event )
{
    const float w = imgui_scale( 32.0f );
    const float text_h = ImGui::GetTextLineHeight();

    for ( colors_t i = i0; i < i1; i++ )
    {
        ImGui::BeginGroup();

        ImU32 color = s_clrs().get( i );
        const char *name = s_clrs().name( i );
        bool selected = ( i == *selected_color );
        ImVec2 pos = ImGui::GetCursorScreenPos();

        // Draw colored rectangle
        ImGui::GetWindowDrawList()->AddRectFilled( pos, ImVec2( pos.x + w, pos.y + text_h ), color );

        // Draw color name
        ImGui::Indent( imgui_scale( 40.0f ) );
        if ( ImGui::Selectable( name, selected, 0 ) )
        {
            *selected_color = i;
            selected_color_event->clear();
        }
        ImGui::Unindent( imgui_scale( 40.0f ) );

        ImGui::EndGroup();

        // Tooltip with description
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "%s", s_clrs().desc( i ) );
    }
}

static trace_event_t *get_first_colorable_event(
        TraceEvents &trace_events, const char *eventname )
{
    const std::vector< uint32_t > *plocs =
            trace_events.m_eventnames_locs.get_locations_str( eventname );

    if ( plocs )
    {
        for ( uint32_t idx : *plocs )
        {
            trace_event_t &event = trace_events.m_events[ idx ];

            if ( event.is_ftrace_print() )
                break;
            if ( !( event.flags & TRACE_FLAG_AUTOGEN_COLOR ) )
                return &event;
        }
    }

    return NULL;
}

static void render_color_event_items( TraceEvents &trace_events,
        colors_t *selected_color, std::string *selected_color_event )
{
    const float w = imgui_scale( 32.0f );
    const float text_h = ImGui::GetTextLineHeight();

    // Iterate through all the event names
    for ( auto item : trace_events.m_eventnames_locs.m_locs.m_map )
    {
        const char *eventname = trace_events.m_strpool.findstr( item.first );
        const trace_event_t *event = get_first_colorable_event( trace_events, eventname );

        if ( event )
        {
            ImU32 color = event->color ? event->color : s_clrs().get( col_Graph_1Event );

            ImGui::BeginGroup();

            bool selected = ( eventname == *selected_color_event );
            ImVec2 pos = ImGui::GetCursorScreenPos();

            // Draw colored rectangle
            ImGui::GetWindowDrawList()->AddRectFilled( pos, ImVec2( pos.x + w, pos.y + text_h ), color );

            // Draw event name
            ImGui::Indent( imgui_scale( 40.0f ) );
            if ( ImGui::Selectable( eventname, selected, 0 ) )
            {
                *selected_color = col_Max;
                *selected_color_event = eventname;
            }
            ImGui::Unindent( imgui_scale( 40.0f ) );

            ImGui::EndGroup();
        }
    }
}

static bool render_color_picker_colors( ColorPicker &colorpicker, colors_t selected_color )
{
    bool changed = false;
    ImU32 color = s_clrs().get( selected_color );
    const char *name = s_clrs().name( selected_color );
    const char *desc = s_clrs().desc( selected_color );
    std::string brightname = s_textclrs().bright_str( name );
    bool is_alpha = s_clrs().is_alpha_color( selected_color );
    ImU32 def_color = s_clrs().getdef( selected_color );

    // Color name and description
    imgui_text_bg( ImGui::GetStyleColorVec4( ImGuiCol_Header ),
                   "%s: %s", brightname.c_str(), desc );

    ImGui::NewLine();
    if ( colorpicker.render( color, is_alpha, def_color ) )
    {
        s_clrs().set( selected_color, colorpicker.m_color );
        changed = true;
    }

    return changed;
}

static bool render_color_picker_event_colors( ColorPicker &colorpicker,
        TraceWin *win, const std::string &selected_color_event )
{
    bool changed = false;
    TraceEvents &trace_events = win->m_trace_events;
    const trace_event_t *event = get_first_colorable_event( trace_events, selected_color_event.c_str() );

    if ( event )
    {
        std::string brightname = s_textclrs().bright_str( selected_color_event.c_str() );
        ImU32 color = event->color ? event->color : s_clrs().get( col_Graph_1Event );
        ImU32 def_color = s_clrs().get( col_Graph_1Event );

        // Color name and description
        imgui_text_bg( ImGui::GetStyleColorVec4( ImGuiCol_Header ), "%s", brightname.c_str() );

        ImGui::NewLine();
        changed = colorpicker.render( color, false, def_color );

        if ( changed && ( colorpicker.m_color == def_color ) )
            colorpicker.m_color = 0;
    }

    return changed;
}

static void update_changed_colors( TraceEvents &trace_events, colors_t color )
{
    switch( color )
    {
    case col_FtracePrintText:
        trace_events.invalidate_ftraceprint_colors();
        break;

    case col_Graph_PrintLabelSat:
    case col_Graph_PrintLabelAlpha:
        // ftrace print label color changes - invalidate current colors
        trace_events.invalidate_ftraceprint_colors();
        trace_events.update_tgid_colors();
        break;

    case col_Graph_TimelineLabelSat:
    case col_Graph_TimelineLabelAlpha:
        // fence_signaled event color change - update event fence_signaled colors
        trace_events.update_fence_signaled_timeline_colors();
        break;
    }
}

static void reset_colors_to_default( TraceWin *win )
{
    for ( colors_t i = 0; i < col_Max; i++ )
        s_clrs().reset( i );

    if ( win )
    {
        win->m_trace_events.invalidate_ftraceprint_colors();
        win->m_trace_events.update_tgid_colors();
        win->m_trace_events.update_fence_signaled_timeline_colors();
    }

    imgui_set_custom_style( s_clrs().getalpha( col_ThemeAlpha ) );

    s_textclrs().update_colors();
}

static void reset_event_colors_to_default( TraceWin *win )
{
    std::vector< INIEntry > entries = s_ini().GetSectionEntries( "$imgui_eventcolors$" );

    for ( const INIEntry &entry : entries )
    {
        s_ini().PutStr( entry.first.c_str(), "", "$imgui_eventcolors$" );
    }

    if ( win )
    {
        for ( trace_event_t &event : win->m_trace_events.m_events )
        {
            // If it's not an autogen'd color, reset color back to 0
            if ( !( event.flags & TRACE_FLAG_AUTOGEN_COLOR ) )
                event.color = 0;
        }
    }
}

void MainApp::render_color_picker()
{
    bool changed = false;
    TraceWin *win = is_trace_loaded() ? m_trace_win : NULL;

    if ( ImGui::Button( "Reset All to Defaults" ) )
    {
        reset_colors_to_default( win );
        reset_event_colors_to_default( win );
    }

    ImGui::Separator();

    if ( imgui_begin_columns( "color_picker", 2, 0 ) )
        ImGui::SetColumnWidth( 0, imgui_scale( 250.0f ) );

    /*
     * Column 1: draw our graph items and their colors
     */
    {
        ImGui::BeginChild( "color_list" );

        if ( ImGui::CollapsingHeader( "GpuVis Colors" ) )
        {
            render_color_items( 0, col_ImGui_Text,
                                &m_colorpicker_color, &m_colorpicker_event );
        }

        if ( ImGui::CollapsingHeader( "ImGui Colors" ) )
        {
            render_color_items( col_ImGui_Text, col_Max,
                                &m_colorpicker_color, &m_colorpicker_event );
        }

        if ( !win )
        {
            m_colorpicker_event.clear();
        }
        else if ( ImGui::CollapsingHeader( "Event Colors" ) )
        {
            render_color_event_items( win->m_trace_events,
                    &m_colorpicker_color, &m_colorpicker_event );
        }

        ImGui::EndChild();
    }
    ImGui::NextColumn();

    /*
     * Column 2: Draw our color picker
     */
    if ( m_colorpicker_color < col_Max )
    {
        changed |= render_color_picker_colors( m_colorpicker, m_colorpicker_color );
    }
    else if ( !m_colorpicker_event.empty() )
    {
        changed |= render_color_picker_event_colors( m_colorpicker,
                win, m_colorpicker_event );
    }

    ImGui::NextColumn();
    ImGui::EndColumns();

    if ( changed )
    {
        if ( m_colorpicker_color < col_Max )
        {
            if ( win )
                update_changed_colors( win->m_trace_events, m_colorpicker_color );

            // imgui color change - set new imgui colors
            if ( s_clrs().is_imgui_color( m_colorpicker_color ) )
                imgui_set_custom_style( s_clrs().getalpha( col_ThemeAlpha ) );

            s_textclrs().update_colors();
        }
        else if ( !m_colorpicker_event.empty() )
        {
            win->m_trace_events.set_event_color( m_colorpicker_event, m_colorpicker.m_color );
        }
    }
}

void MainApp::render_log()
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
                           ImVec2( 0, -ImGui::GetTextLineHeightWithSpacing() ),
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
                col = ImVec4( 1.0f, 0.4f, 0.4f, 1.0f );
            else if ( strncmp( item, "# ", 2 ) == 0 )
                col = ImVec4( 1.0f, 0.78f, 0.58f, 1.0f );

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
}

void MainApp::render_console()
{
    if ( !ImGui::Begin( "Gpuvis Console", &m_show_gpuvis_console, ImGuiWindowFlags_MenuBar ) )
    {
        ImGui::End();
        return;
    }

    render_menu( "menu_console" );

    render_log();

    ImGui::End();
}

#if !defined( NOC_FILE_DIALOG_IMPLEMENTATION )
static const char *noc_file_init()
{
    return "File open dialog NYI";
}

#define NOC_FILE_DIALOG_OPEN 0
static const char *noc_file_dialog_open(int flags, const char *filters,
        const char *default_path, const char *default_name)
{
    return NULL;
}
#endif

void MainApp::open_trace_dialog()
{
    const char *errstr = noc_file_init();

    if ( errstr )
    {
        logf( "[Error] Open Trace: %s\n", errstr );
    }
    else
    {
        const char *file = noc_file_dialog_open( NOC_FILE_DIALOG_OPEN,
                                 "trace-cmd files (*.dat;*.trace)\0*.dat;*.trace\0",
                                 NULL, "trace.dat" );

        if ( file && file[ 0 ] )
            m_loading_info.inputfiles.push_back( file );
    }
}

void MainApp::render_menu( const char *str_id )
{
    ImGui::PushID( str_id );

    if ( !ImGui::BeginMenuBar() )
    {
        ImGui::PopID();
        return;
    }

    if ( ImGui::IsRootWindowOrAnyChildFocused() )
    {
        if ( s_actions().get( action_menu_file ) )
            ImGui::OpenPopup( "File" );
        else if ( s_actions().get( action_menu_options ) )
            ImGui::OpenPopup( "Options" );
    }

    if ( ImGui::BeginMenu( "File" ) )
    {
        if ( s_actions().get( action_escape ) )
            ImGui::CloseCurrentPopup();

#if defined( NOC_FILE_DIALOG_IMPLEMENTATION )
        if ( ImGui::MenuItem( "Open Trace File...", s_actions().hotkey_str( action_open ).c_str() ) )
            open_trace_dialog();
#endif

        if ( m_saving_info.title.empty() && is_trace_loaded() )
        {
            std::string &filename = m_trace_win->m_trace_events.m_filename;
            const char *basename = get_path_filename( filename.c_str() );
            std::string label = string_format( "Save '%s' as...", basename );

            if ( ImGui::MenuItem( label.c_str() ) )
            {
                m_saving_info.filename_orig = get_realpath( filename.c_str() );
                m_saving_info.title = string_format( "Save '%s' as:", m_saving_info.filename_orig.c_str() );
                strcpy_safe( m_saving_info.filename_buf, "blah.trace" );

                // Lambda for copying filename_orig to filename_new
                m_saving_info.save_cb = []( save_info_t &save_info )
                {
                    bool close_popup = copy_file( save_info.filename_orig.c_str(), save_info.filename_new.c_str() );

                    if ( !close_popup )
                    {
                        save_info.errstr = string_format( "ERROR: copy_file to %s failed",
                                                              save_info.filename_new.c_str() );
                    }
                    return close_popup;
                };
            }
        }

        if ( ImGui::MenuItem( "Quit", s_actions().hotkey_str( action_quit ).c_str() ) )
        {
            SDL_Event event;

            event.type = SDL_QUIT;
            SDL_PushEvent( &event );
        }

        ImGui::EndMenu();
    }

    if ( ImGui::BeginMenu( "Options") )
    {
        render_menu_options();
        ImGui::EndMenu();
    }

    if ( s_opts().getb( OPT_ShowFps ) )
    {
        ImGui::Text( "%s%.2f ms/frame (%.1f FPS)%s",
                     s_textclrs().str( TClr_Bright ),
                     1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate,
                     s_textclrs().str( TClr_Def ) );
    }

    ImGui::EndMenuBar();

    ImGui::PopID();
}

void MainApp::handle_hotkeys()
{
    if ( s_actions().get( action_help ) )
    {
        ImGui::SetWindowFocus( "GpuVis Help" );
        m_show_help = true;
    }

    if ( s_actions().get( action_open ) )
        open_trace_dialog();

    if ( s_actions().get( action_quit ) )
    {
        SDL_Event event;

        event.type = SDL_QUIT;
        SDL_PushEvent( &event );
    }

    if ( s_actions().get( action_trace_info ) && is_trace_loaded() )
    {
        const std::string label = trace_info_label( m_trace_win->m_trace_events );

        ImGui::SetWindowFocus( label.c_str() );
        m_show_trace_info = label;
    }

    if ( s_actions().get( action_toggle_vblank0 ) )
        s_opts().setb( OPT_RenderCrtc0, !s_opts().getb( OPT_RenderCrtc0 ) );
    if ( s_actions().get( action_toggle_vblank1 ) )
        s_opts().setb( OPT_RenderCrtc1, !s_opts().getb( OPT_RenderCrtc1 ) );
    if ( s_actions().get( action_toggle_framemarkers ) )
        s_opts().setb( OPT_RenderFrameMarkers, !s_opts().getb( OPT_RenderFrameMarkers ) );

    if ( s_actions().get( action_toggle_show_eventlist ) )
        s_opts().setb( OPT_ShowEventList, !s_opts().getb( OPT_ShowEventList ) );

    if ( s_actions().get( action_save_screenshot ) )
    {
        ImGuiIO& io = ImGui::GetIO();
        int w = ( int )io.DisplaySize.x;
        int h = ( int )io.DisplaySize.y;

        // Capture image
        m_imagebuf.CreateFromCaptureGL( 0, 0, w, h );
        m_imagebuf.FlipVertical();

        m_saving_info.filename_orig.clear();
        m_saving_info.title = string_format( "Save gpuvis screenshot (%dx%d) as:", w, h );
        strcpy_safe( m_saving_info.filename_buf, "gpuvis.png" );

        // Lambda for copying filename_orig to filename_new
        m_saving_info.save_cb = [&]( save_info_t &save_info )
        {
            bool close_popup = !!m_imagebuf.SaveFile( save_info.filename_new.c_str() );

            if ( !close_popup )
            {
                save_info.errstr = string_format( "ERROR: save_file to %s failed",
                                                  save_info.filename_new.c_str() );
            }
            return close_popup;
        };
    }
}

void MainApp::parse_cmdline( int argc, char **argv )
{
    static struct option long_opts[] =
    {
        { "scale", ya_required_argument, 0, 0 },
        { 0, 0, 0, 0 }
    };

    int c;
    int opt_ind = 0;
    while ( ( c = ya_getopt_long( argc, argv, "i:",
                               long_opts, &opt_ind ) ) != -1 )
    {
        switch ( c )
        {
        case 0:
            if ( !strcasecmp( "scale", long_opts[ opt_ind ].name ) )
                s_opts().setf( OPT_Scale, atof( ya_optarg ) );
            break;
        case 'i':
            m_loading_info.inputfiles.clear();
            m_loading_info.inputfiles.push_back( ya_optarg );
            break;

        default:
            break;
        }
    }

    for ( ; ya_optind < argc; ya_optind++ )
    {
        m_loading_info.inputfiles.clear();
        m_loading_info.inputfiles.push_back( argv[ ya_optind ] );
    }
}

static void imgui_render( SDL_Window *window )
{
    const ImVec4 color = s_clrs().getv4( col_ClearColor );
    const ImVec2 &size = ImGui::GetIO().DisplaySize;

    glViewport( 0, 0, ( int )size.x, ( int )size.y );
    glClearColor( color.x, color.y, color.z, color.w );
    glClear( GL_COLOR_BUFFER_BIT );

    ImGui::Render();

    SDL_GL_SwapWindow( window );
}

int main( int argc, char **argv )
{
    // Initialize SDL
    if ( SDL_Init( SDL_INIT_VIDEO | SDL_INIT_TIMER ) )
    {
        fprintf( stderr, "Error. SDL_Init failed: %s\n", SDL_GetError() );
        return -1;
    }

    SDL_Window *window = NULL;
    SDL_GLContext glcontext = NULL;
    SDL_Cursor *cursor_sizens = SDL_CreateSystemCursor( SDL_SYSTEM_CURSOR_SIZENS );
    SDL_Cursor *cursor_default = SDL_GetDefaultCursor();

    // Initialize logging system
    logf_init();

    std::string imguiini = util_get_config_dir( "gpuvis" ) + "/imgui.ini";
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = imguiini.c_str();

    // Init ini singleton
    s_ini().Open( "gpuvis", "gpuvis.ini" );
    // Initialize colors
    s_clrs().init();
    // Init opts singleton
    s_opts().init();
    // Init actions singleton
    s_actions().init();

    // Init app
    MainApp &app = s_app();
    app.init( argc, argv );

    // Setup imgui default text color
    s_textclrs().update_colors();

    // Setup window
    SDL_GL_SetAttribute( SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG );
    SDL_GL_SetAttribute( SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE );
    SDL_GL_SetAttribute( SDL_GL_DOUBLEBUFFER, 1 );
    SDL_GL_SetAttribute( SDL_GL_DEPTH_SIZE, 24 );
    SDL_GL_SetAttribute( SDL_GL_STENCIL_SIZE, 8 );
    SDL_GL_SetAttribute( SDL_GL_CONTEXT_MAJOR_VERSION, 3 );
    SDL_GL_SetAttribute( SDL_GL_CONTEXT_MINOR_VERSION, 2 );

    window = app.create_window( "GPUVis");
    glcontext = SDL_GL_CreateContext( window );

    gl3wInit();

    // Setup ImGui binding
    ImGui_ImplSdlGL3_Init( window );

    // 1 for updates synchronized with the vertical retrace
    bool vsync = true;
    SDL_GL_SetSwapInterval( vsync );

    // Load our fonts
    app.load_fonts();

    // Main loop
    bool done = false;
    ImGuiMouseCursor mouse_cursor = ImGuiMouseCursor_Arrow;
    while ( !done )
    {
        SDL_Event event;

        // Clear keyboard actions.
        s_actions().clear();

        if ( mouse_cursor != ImGui::GetMouseCursor() )
        {
            mouse_cursor = ImGui::GetMouseCursor();

            SDL_SetCursor( ( mouse_cursor == ImGuiMouseCursor_ResizeNS ) ?
                               cursor_sizens : cursor_default );
        }

        while ( SDL_PollEvent( &event ) )
        {
            ImGui_ImplSdlGL3_ProcessEvent( &event );

            if ( ( event.type == SDL_KEYDOWN ) || ( event.type == SDL_KEYUP ) )
                s_keybd().update( event.key );
            else if ( ( event.type == SDL_WINDOWEVENT ) && ( event.window.event == SDL_WINDOWEVENT_FOCUS_LOST ) )
                s_keybd().clear();
            else if ( event.type == SDL_QUIT )
                done = true;
        }

        bool use_freetype = s_opts().getb( OPT_UseFreetype );
        ImGui_ImplSdlGL3_NewFrame( window, &use_freetype );
        s_opts().setb( OPT_UseFreetype, use_freetype );

        if ( s_opts().getb( OPT_VerticalSync ) != vsync )
        {
            vsync = !vsync;
            SDL_GL_SetSwapInterval( vsync );
        }

        // Check for logf() calls from background threads.
        if ( logf_update() )
        {
            // One of the log items was an error - show the console
            app.m_focus_gpuvis_console = true;
        }

        // Handle global hotkeys
        app.handle_hotkeys();

        // Render trace windows
        app.render();

        // ImGui Rendering
        imgui_render( window );

        // Update app font settings, scale, etc
        app.update();

        done |= app.m_quit;
    }

    // Shut down app
    app.shutdown( window );

    // Write option settings to ini file
    s_opts().shutdown();
    // Save color entries
    s_clrs().shutdown();
    // Close ini file
    s_ini().Close();

    logf_clear();

    // Cleanup
    logf_shutdown();

    ImGui_ImplSdlGL3_Shutdown();

    SDL_FreeCursor( cursor_sizens );

    SDL_GL_DeleteContext( glcontext );
    SDL_DestroyWindow( window );
    SDL_Quit();
    return 0;
}
