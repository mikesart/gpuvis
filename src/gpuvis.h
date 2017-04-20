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
#include <limits.h>

#include "imgui/imgui.h"
#include "imgui/imgui_impl_sdl_gl3.h"

#include "gpuvis_macros.h"
#include "gpuvis_utils.h"
#include "stlini.h"

class StrPool
{
public:
    StrPool() {}
    ~StrPool() {}

    const char *getstr( const char *str, size_t len = ( size_t )-1 );
    const char *findstr( uint32_t hashval );

public:
    typedef std::unordered_map< uint32_t, std::string > pool_t;
    pool_t m_pool;
};

struct trace_info_t
{
    uint32_t cpus = 0;
    std::string file;
    std::string uname;
    bool timestamp_in_us;
    std::vector< std::string > cpustats;
};

struct event_field_t
{
    const char *key;
    const char *value;
};

enum trace_flag_type_t {
    // TRACE_FLAG_IRQS_OFF = 0x01, // interrupts were disabled
    // TRACE_FLAG_IRQS_NOSUPPORT = 0x02,
    // TRACE_FLAG_NEED_RESCHED = 0x04,
    // TRACE_FLAG_HARDIRQ = 0x08, // inside an interrupt handler
    // TRACE_FLAG_SOFTIRQ = 0x10, // inside a softirq handler
    TRACE_FLAG_FENCE_SIGNALED = 0x100,
    TRACE_FLAG_FTRACE_PRINT = 0x200,
    TRACE_FLAG_IS_VBLANK = 0x400,
    TRACE_FLAG_IS_TIMELINE = 0x800,
};

#define INVALID_ID ( ( uint32_t )-1 )

inline bool is_valid_id( uint32_t id )
{
    return id != INVALID_ID;
}

struct trace_event_t
{
    bool is_fence_signaled() const
    {
        return !!( flags & TRACE_FLAG_FENCE_SIGNALED );
    }
    bool is_ftrace_print() const
    {
        return !!( flags & TRACE_FLAG_FTRACE_PRINT );
    }
    bool is_vblank() const
    {
        return !!( flags & TRACE_FLAG_IS_VBLANK );
    }

    bool is_filtered_out;
    int pid;                    // event process id
    int crtc;                   // drm_vblank_event crtc (or -1)

    uint32_t id;                // event id
    uint32_t cpu;               // cpu this event was hit on
    uint32_t flags;             // TRACE_FLAGS_IRQS_OFF, TRACE_FLAG_HARDIRQ, TRACE_FLAG_SOFTIRQ
    uint32_t context;           // event context (from fields)
    uint32_t seqno;             // event seqno (from fields)
    uint32_t id_start;          // start event if this is a graph sequence event (ie amdgpu_sched_run_job, fence_signaled)
    uint32_t graph_row_id;
    uint32_t duration;          // how long this timeline event took

    int64_t ts;                 // timestamp
    const char *comm;           // command line
    const char *system;         // event system (ftrace-print, etc.)
    const char *name;           // event name
    const char *timeline;       // event timeline (gfx, sdma0, ...)
    const char *user_comm;      // User space comm (if we can figure this out)

    std::vector< event_field_t > fields;
};

const event_field_t *find_event_field( std::vector< event_field_t > &fields, const char *name );
const char *get_event_field_str( std::vector< event_field_t > &fields, const char *name );

typedef std::function< int ( const trace_info_t &info, const trace_event_t &event ) > EventCallback;
int read_trace_file( const char *file, StrPool &strpool, EventCallback &cb );

class TraceLocations
{
public:
    TraceLocations() {}
    ~TraceLocations() {}

    void add_location_u32( uint32_t val, uint32_t location )
    {
        auto i = m_locations.find( val );
        if ( i == m_locations.end() )
            m_locations.emplace( val, std::vector< uint32_t >() );

        m_locations.at( val ).push_back( location );
    }

    std::vector< uint32_t > *get_locations_u32( uint32_t val )
    {
        auto i = m_locations.find( val );
        if ( i == m_locations.end() )
            return NULL;

        return &m_locations.at( val );
    }

    void add_location_str( const char *name, uint32_t location )
    {
        add_location_u32( fnv_hashstr32( name ), location );
    }

    std::vector< uint32_t > *get_locations_str( const char *name )
    {
        return get_locations_u32( fnv_hashstr32( name ) );
    }

public:
    // Map of name hashval to array of event locations.
    typedef std::unordered_map< uint32_t, std::vector< uint32_t > > m_loc_t;
    m_loc_t m_locations;
};

// Given a sorted array (like from TraceLocations), binary search for eventid
//   and return the vector index, or vec.size() if not found.
inline size_t vec_find_eventid( const std::vector< uint32_t > &vec, uint32_t eventid )
{
    auto i = std::lower_bound( vec.begin(), vec.end(), eventid );

    return i - vec.begin();
}

inline std::string get_event_gfxcontext_str( const trace_event_t &event )
{
    if ( event.timeline && event.context && event.seqno )
        return string_format( "%s_%u_%u", event.timeline, event.context, event.seqno );
    return "";
}

class TraceEvents
{
public:
    TraceEvents() {}
    ~TraceEvents() {}

public:
    // Return vec of locations for a tdop expression. Ie: "$name=drm_handle_vblank"
    const std::vector< uint32_t > *get_tdopexpr_locs( const char *name );
    // Return vec of locations for a cmdline. Ie: "SkinningApp-1536"
    const std::vector< uint32_t > *get_comm_locs( const char *name );
    // "gfx", "sdma0", etc.
    const std::vector< uint32_t > *get_timeline_locs( const char *name );
    // Pass a string like "gfx_249_91446"
    const std::vector< uint32_t > *get_gfxcontext_locs( const char *name );

public:
    int64_t m_ts_min = 0;
    std::vector< uint32_t > m_cpucount;

    std::string m_filename;
    size_t m_filesize = 0;
    std::string m_title;

    StrPool m_strpool;
    trace_info_t m_trace_info;
    std::vector< trace_event_t > m_events;

    // Map of vblanks hashval to array of event locations.
    TraceLocations m_tdopexpr_locations;
    std::set< uint32_t > m_failed_commands;

    // Map of comm hashval to array of event locations.
    TraceLocations m_comm_locations;

    // Map of timeline/context/seqno to array of event locations.
    TraceLocations m_gfxcontext_locations;

    // Map of timeline (gfx, sdma0, etc) event locations.
    TraceLocations m_timeline_locations;

    // 0: events loaded, 1+: loading events, -1: error
    SDL_atomic_t m_eventsloaded = { 0 };
};

class TraceLoader;

struct TraceConsole
{
public:
    TraceConsole() {}
    ~TraceConsole() {}

    void init( class CIniFile *inifile );
    void shutdown( class CIniFile *inifile );

    void exec_command( const std::string &command_line );

    void render( TraceLoader &loader );
    void render_options( TraceLoader &loader );
    void render_log( TraceLoader &loader );

protected:
    static int text_edit_cb_stub( ImGuiTextEditCallbackData *data );
    int text_edit_cb_completion( ImGuiTextEditCallbackData *data );
    int text_edit_cb_history( ImGuiTextEditCallbackData *data );

public:
    ImGuiTextFilter m_filter;
    std::string m_inputbuf;
    std::vector< std::string > m_log;

    size_t m_completion_index = 0;
    std::vector< const char * > m_completions;
    std::set< std::string > m_commands;

    // -1: new line, 0..History.size-1 browsing history.
    int m_history_pos = -1;
    std::vector< std::string > m_history;

    char m_trace_file[ PATH_MAX ] = { 0 };

    ImVec4 m_clear_color;

    size_t m_log_size = ( size_t )-1;
    bool m_quit = false;
    bool m_show_imgui_test_window = false;
    bool m_show_imgui_style_editor = false;
    bool m_show_imgui_metrics_editor = false;
};

class TraceWin
{
public:
    TraceWin( TraceLoader &loader, TraceEvents *trace_events, std::string &title );
    ~TraceWin();

public:
    bool render();

protected:
    void render_trace_info();

    void render_events_list( CIniFile &inifile );
    bool render_events_list_popup( uint32_t eventid );
    void render_color_picker();

    void render_process_graph();
    void render_graph_row( const std::string &comm, const std::vector< uint32_t > &locs,
                           class graph_info_t &gi );
    void render_graph_row_timeline( const std::vector< uint32_t > &locs, graph_info_t &gi );
    void render_graph_hw_row_timeline( graph_info_t &gi );
    void render_graph_vblanks( class graph_info_t &gi );
    bool render_graph_popup( class graph_info_t &gi );

    void handle_graph_hotkeys();
    void handle_graph_keyboard_scroll();

    void handle_mouse_graph( class graph_info_t &gi );
    void handle_mouse_graph_captured( class graph_info_t &gi );
    void set_mouse_graph_tooltip( class graph_info_t &gi, int64_t mouse_ts );

    bool add_mouse_hovered_event( float x, class graph_info_t &gi, const trace_event_t &event );

    void render_time_offset_button_init( TraceEvents &trace_events );

    // Return an event id for a given time stamp
    int ts_to_eventid( int64_t ts );
    // Return an event id from a time string
    int timestr_to_eventid( const char *buf, int64_t tsoffset );
    // Convert a time string to a time stamp
    int64_t timestr_to_ts( const char *buf, int64_t tsoffset = 0 );
    // Convert a time stamp to a time string
    std::string ts_to_timestr( int64_t event_ts, int64_t tsoffset = 0, int precision = 6 );

    void graph_rows_initstr( bool reset = false );
    void graph_rows_updatelist();
    enum graph_rows_show_t
    {
        SHOW_ROW,
        HIDE_ROW,
        HIDE_ROW_AND_ALL_BELOW
    };
    bool graph_rows_show( const std::string &name, graph_rows_show_t show );
    std::vector< std::string > graph_rows_get_hidden_rows();

    void range_check_graph_location();

    std::unordered_map< int64_t, int > m_ts_to_eventid_cache;

    trace_event_t &get_event( uint32_t id )
    {
        return m_trace_events->m_events[ id ];
    }

public:
    TraceLoader &m_loader;

    bool m_inited = false;

    bool m_open = true;
    int m_setfocus = 0;
    std::string m_title;
    TraceEvents *m_trace_events = nullptr;

    bool m_do_gotoevent = false;
    int m_goto_eventid = 0;

    int64_t m_ts_marker = -1;

    // Is Event List Header visible?
    bool m_show_eventlist = false;

    // Whether column has been resized.
    bool m_columns_eventlist_resized = false;

    // Goto Time buffer
    char m_timegoto_buf[ 32 ] = { 0 };

    bool m_do_event_filter = false;
    char m_event_filter_buf[ 512 ] = { 0 };

    // Time Offset
    char m_timeoffset_buf[ 32 ] = { 0 };
    int64_t m_tsoffset = 0;

    // Event id of event list popup menu
    uint32_t m_events_list_popup_eventid = INVALID_ID;

    bool m_graph_popup = false;
    std::string m_mouse_over_row_name;
    std::vector< std::string > m_graph_rows_hidden_rows;

    // Graph Start
    bool m_do_graph_start_timestr = false;
    int64_t m_graph_start_ts = 0;
    char m_graphtime_start_buf[ 32 ] = { 0 };
    // Graph Length
    bool m_do_graph_length_timestr = false;
    int64_t m_graph_length_ts = INT64_MAX;
    char m_graphtime_length_buf[ 32 ] = { 0 };

    float m_graph_start_y = 0.0f;

    bool m_do_graph_zoom_in = false;
    bool m_do_graph_zoom_out = false;

    // Graph rows
    std::string m_graph_rows_str;
    std::vector< std::string > m_graph_rows;

    // Currently selected event.
    uint32_t m_selected_eventid = INVALID_ID;
    uint32_t m_hovered_eventlist_eventid = INVALID_ID;

    uint32_t m_eventlist_start_eventid = INVALID_ID;
    uint32_t m_eventlist_end_eventid = INVALID_ID;

    std::vector< std::pair< int64_t, int64_t > > m_locations;

    // Mouse currently over our events graph?
    bool m_mouse_over_graph = false;

    // 0:mouse not captured
    // 1:shift+click+drag: zoom graph
    // 2:ctrl+click+drag: pan graph
    int m_mouse_captured = 0;
    ImVec2 m_mouse_capture_pos;

    int m_selected_color = 0;
    ColorPicker m_colorpicker;

    std::vector< uint32_t > m_filtered_events;
    std::string m_filtered_events_str;

    struct comm_t
    {
        size_t event_count;
        const char *comm;
    };
    std::vector< comm_t > m_comm_info;

    const int64_t g_min_graph_length = 100;
    const int64_t g_max_graph_length = 5000 * MSECS_PER_SEC;

    multi_text_color m_col_yellow{ ImVec4( 1, 1, 0, 1 ) };
};

// TraceLoader ini options
enum option_id_t
{
    OPT_TimelineZoomGfx,
    OPT_TimelineLabels,
    OPT_TimelineEvents,
    OPT_TimelineRenderUserSpace,
    OPT_GraphOnlyFiltered,
    OPT_Fullscreen,
    OPT_ShowEventList,
    OPT_SyncEventListToGraph,
    OPT_ShowColorPicker,
    OPT_RenderCrtc0,
    OPT_RenderCrtc1,
    OPT_RenderCrtc2,
    OPT_RenderCrtc3,
    OPT_RenderCrtc4,
    OPT_RenderCrtc5,
    OPT_RenderCrtc6,
    OPT_RenderCrtc7,
    OPT_RenderCrtc8,
    OPT_RenderCrtc9,
    OPT_GraphRowCount,
    OPT_EventListRowCount,
    OPT_TimelineGfxRowCount,
    OPT_TimelineSdma0RowCount,
    OPT_TimelineSdma1RowCount,
    OPT_Max
};

class TraceLoader
{
public:
    enum state_t
    {
        State_Idle,
        State_Loading,
        State_Loaded,
        State_CancelLoading
    };

public:
    TraceLoader( CIniFile &inifile ) : m_inifile( inifile ) {}
    ~TraceLoader() {}

    bool load_file( const char *filename );
    void cancel_load_file();
    bool is_loading();

    void new_event_window( TraceEvents *trace_events );
    void close_event_file( TraceEvents *trace_events, bool close_file  );

    void render();

    void init();
    void shutdown();

protected:
    state_t get_state();
    void set_state( state_t state );

    static int SDLCALL thread_func( void *data );
    static int new_event_cb( TraceLoader *loader, const trace_info_t &info,
                         const trace_event_t &event );
    int init_new_event( trace_event_t &event, const trace_info_t &info );

public:
    CIniFile &m_inifile;
    std::string m_filename;
    SDL_atomic_t m_state = { 0 };
    SDL_Thread *m_thread = nullptr;
    TraceEvents *m_trace_events = nullptr;

    std::vector< TraceEvents * > m_trace_events_list;
    std::vector< TraceWin * > m_trace_windows_list;

    uint32_t m_crtc_max = 0;
    std::vector< std::string > m_inputfiles;

    struct option_t
    {
        std::string desc;
        std::string inikey;
        int val;
        int val_min;
        int val_max;
    };
    int get_opt( option_id_t opt )
    {
        return m_options[ opt ].val;
    }
    int get_opt_crtc( int crtc )
    {
        int val = crtc + OPT_RenderCrtc0;

        return ( val <= OPT_RenderCrtc9 ) ? m_options[ val ].val : 0;
    }

    std::vector< option_t > m_options;
    std::string m_event_filter_str;
};
