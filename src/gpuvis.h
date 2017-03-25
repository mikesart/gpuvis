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
#include "gpuvis_macros.h"
#include "imgui/imgui.h"
#include "imgui/imgui_impl_sdl_gl3.h"
#include "stlini.h"

using std::placeholders::_1;
using std::placeholders::_2;
using std::placeholders::_3;
using std::placeholders::_4;
using std::placeholders::_5;

#define MSECS_PER_SEC		1000000ULL

extern "C" uint32_t fnv_hashstr32( const char *str );

void logf( const char *fmt, ... ) ATTRIBUTE_PRINTF( 1, 2 );
void logf_clear();

std::string string_format( const char *fmt, ... ) ATTRIBUTE_PRINTF( 1, 2 );
size_t get_file_size( const char* filename );

template < typename T >
T Clamp( const T& val, const T& lower, const T& upper )
{
    return std::max( lower, std::min( val, upper ) );
}

class StrPool
{
public:
    StrPool() {}
    ~StrPool() {}

    const char *getstr( const char *str );
    const char *getstr( uint32_t hashval );

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

struct trace_event_t
{
    uint32_t id;
    int pid;
    uint32_t cpu;
    int missed_events;
    int64_t ts;
    const char *comm;
    const char *system;
    const char *name;
    uint64_t flags; // TRACE_FLAGS_IRQS_OFF, TRACE_FLAG_HARDIRQ, TRACE_FLAG_SOFTIRQ
    int seqno;
    std::vector< event_field_t > fields;
};

typedef std::function< int ( const trace_info_t &info, const trace_event_t &event ) > EventCallback;
int read_trace_file( const char *file, StrPool &strpool, EventCallback &cb );

class TraceLocations
{
public:
    TraceLocations() {}
    ~TraceLocations() {}

    void add_location( const char *name, uint32_t location )
    {
        uint32_t hashval = fnv_hashstr32( name );

        auto i = m_locations.find( hashval );
        if ( i == m_locations.end() )
            m_locations.emplace( hashval, std::vector< uint32_t >() );

        m_locations.at( hashval ).push_back( location );
    }

    std::vector< uint32_t > *get_locations( const char *name )
    {
        uint32_t hashval = fnv_hashstr32( name );

        auto i = m_locations.find( hashval );
        if ( i == m_locations.end() )
            return NULL;

        return &m_locations.at( hashval );
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

class TraceEvents
{
public:
    TraceEvents() {}
    ~TraceEvents() {}

public:
    // Return vec of locations for an event name. Ie: "drm_handle_vblank"
    std::vector< uint32_t > &get_event_locs( const char *name )
    {
        std::vector< uint32_t > *pvec = m_event_locations.get_locations( name );

        return pvec ? *pvec : m_emptyvec;
    }

    // Return vec of locations for a cmdline. Ie: "SkinningApp-1536"
    std::vector< uint32_t > &get_comm_locs( const char *name )
    {
        std::vector< uint32_t > *pvec = m_comm_locations.get_locations( name );

        return pvec ? *pvec : m_emptyvec;
    }

public:
    std::vector< uint32_t > m_emptyvec;

    int64_t m_ts_min = 0;
    std::vector< uint32_t > m_cpucount;

    std::string m_filename;
    size_t m_filesize = 0;
    std::string m_title;

    StrPool m_strpool;
    trace_info_t m_trace_info;
    std::vector< trace_event_t > m_events;

    // Map of event name hashval to array of event locations.
    TraceLocations m_event_locations;

    // Map of comm hashval to array of event locations.
    TraceLocations m_comm_locations;

    // 0: events loaded, 1+: loading events, -1: error
    SDL_atomic_t m_eventsloaded = { 0 };
};

struct TraceConsole
{
public:
    TraceConsole() {}
    ~TraceConsole() {}

    void init( class CIniFile *inifile );
    void shutdown( class CIniFile *inifile );

    void exec_command( const std::string &command_line );

    void render( class TraceLoader *loader );

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
    TraceWin( TraceEvents *trace_events, std::string &title )
    {
        // Note that m_trace_events is possibly being loaded in
        //  a background thread at this moment, so be sure to check
        //  m_eventsloaded before accessing it...
        m_trace_events = trace_events;
        m_title = title;

        m_timegoto_buf = "0.0";

        m_do_graph_start_ts = true;
        m_graph_start_ts = -2 * MSECS_PER_SEC;

        m_do_graph_length_ts = true;
        m_graph_length_ts = 30 * MSECS_PER_SEC;
    }

    ~TraceWin() {}

public:
    bool render( class TraceLoader *loader );
    bool render_info();
    void render_events_list( CIniFile &inifile );
    bool render_events_list_popup();
    void render_process_graphs();
    void render_graph_row( const std::string &comm, std::vector< uint32_t > &locs, class graph_info_t *pgi );
    void render_graph_vblanks( class graph_info_t *pgi );
    void render_mouse_graph( class graph_info_t *pgi );

protected:
    void render_time_offset_button_init( TraceEvents &trace_events );

    // Return an event id for a given time stamp
    int ts_to_eventid( int64_t ts );
    // Return an event id from a time string
    int timestr_to_eventid( const char *buf, int64_t tsoffset );
    // Convert a time string to a time stamp
    int64_t timestr_to_ts( const char *buf, int64_t tsoffset = 0 );
    // Convert a time stamp to a time string
    std::string ts_to_timestr( int64_t event_ts, int64_t tsoffset = 0, int precision = 6 );

    void init_graph_rows_str();
    void update_graph_rows_list();

    std::unordered_map< int64_t, int > m_ts_to_eventid_cache;

public:
    bool m_inited = false;
    bool m_open = true;
    int m_setfocus = 0;
    std::string m_title;
    TraceEvents *m_trace_events = nullptr;

    // Goto Event
    bool m_do_gotoevent = false;
    int m_goto_eventid = 0;

    // Goto Time buffer
    std::string m_timegoto_buf;

    // Time Offset
    std::string m_timeoffset_buf;
    int64_t m_tsoffset = 0;

    // Event Start
    int m_start_eventid = 0;
    // Event End
    int m_end_eventid = INT32_MAX;

    // Event id of event list popup menu
    uint32_t m_events_list_popup_eventid = ( uint32_t )-1;

    bool m_graph_popup = false;

    // Graph Start
    bool m_do_graph_start_ts = false;
    int64_t m_graph_start_ts = 0;
    std::string m_graphtime_start_buf;
    // Graph Length
    bool m_do_graph_length_ts = false;
    int64_t m_graph_length_ts = INT64_MAX;
    std::string m_graphtime_length_buf;

    // Graph rows
    std::string m_graph_rows_str;
    std::vector< std::string > m_graph_rows;

    // Currently selected event.
    uint32_t m_selected_eventid = ( uint32_t )-1;
    uint32_t m_hovered_eventid = ( uint32_t )-1;

    uint32_t m_eventlist_start_eventid = ( uint32_t )-1;
    uint32_t m_eventlist_end_eventid = ( uint32_t )-1;

    // Stack of last graph locations (right click to pop).
    std::vector< std::pair< int64_t, int64_t > > m_graph_location_stack;

    // Mouse currently over our events graph?
    bool m_mouse_over_graph = false;

    // 0:mouse not captured
    // 1:shift+click+drag: zoom graph
    // 2:ctrl+click+drag: pan graph
    int m_mouse_captured = 0;
    ImVec2 m_mouse_capture_pos;

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

    void render( bool fullscreen );

    void shutdown();

protected:
    state_t get_state();
    void set_state( state_t state );

    static int SDLCALL thread_func( void *data );
    static int new_event_cb( TraceLoader *loader, const trace_info_t &info,
                         const trace_event_t &event );

public:
    CIniFile &m_inifile;
    std::string m_filename;
    SDL_atomic_t m_state = { 0 };
    SDL_Thread *m_thread = nullptr;
    TraceEvents *m_trace_events = nullptr;

    std::vector< TraceEvents * > m_trace_events_list;
    std::vector< TraceWin * > m_trace_windows_list;
};
