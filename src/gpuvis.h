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
    unsigned long long ts;
    const char *comm;
    const char *system;
    const char *name;
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

class TraceEvents
{
public:
    TraceEvents() {}
    ~TraceEvents() {}

public:
    // Return vec of locations for an event name. Ie: "drm_handle_vblank"
    std::vector< uint32_t > *get_event_locs( const char *name )
    {
        return m_event_locations.get_locations( name );
    }

    // Return vec of locations for a cmdline. Ie: "SkinningApp-1536"
    std::vector< uint32_t > *get_comm_locs( const char *name )
    {
        return m_comm_locations.get_locations( name );
    }

public:
    unsigned long long m_ts_min = ( unsigned long long )-1;
    unsigned long long m_ts_max = 0;
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

    void exec_command( const char *command_line );

    void render( class TraceLoader *loader );

protected:
    static int text_edit_cb_stub( ImGuiTextEditCallbackData *data );
    int text_edit_cb_completion( ImGuiTextEditCallbackData *data );
    int text_edit_cb_history( ImGuiTextEditCallbackData *data );

public:
    ImGuiTextFilter m_filter;
    std::array< char, 512 > m_inputbuf = {""};
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
        m_trace_events = trace_events;
        m_title = title;
    }

    ~TraceWin() {}

public:
    bool render( class TraceLoader *loader );
    bool render_events();
    bool render_options();
    void render_events_list();
    void render_process_graphs();

protected:
    void render_time_delta_button_init( TraceEvents &trace_events );
    void render_time_delta_button( TraceEvents &trace_events );
    bool render_time_goto_button( TraceEvents &trace_events );

public:
    bool m_inited = false;
    int m_setfocus = 0;

    std::string m_title;
    TraceEvents *m_trace_events = nullptr;

    bool m_do_gotoevent = false;
    bool m_time_goto = false;
    int m_gotoevent = 0;

    int m_eventstart = 0;
    int m_eventend = INT32_MAX;
    bool m_open = true;
    uint32_t m_selected = ( uint32_t )-1;

    bool m_mouse_over_graph = false;

    char m_timegoto_buf[ 32 ];

    char m_timedelta_buf[ 32 ] = { 0 };
    unsigned long long m_tsdelta = ( unsigned long long )-1;
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
    TraceLoader() {}
    ~TraceLoader() {}

    bool load_file( const char *filename );
    void cancel_load_file();
    bool is_loading();

    void new_event_window( TraceEvents *trace_events );
    void close_event_file( TraceEvents *trace_events, bool close_file  );

    void render();

    void shutdown();

protected:
    state_t get_state();
    void set_state( state_t state );

    static int SDLCALL thread_func( void *data );
    static int new_event_cb( TraceLoader *loader, const trace_info_t &info,
                         const trace_event_t &event );

public:
    std::string m_filename;
    SDL_atomic_t m_state = { 0 };
    SDL_Thread *m_thread = nullptr;
    TraceEvents *m_trace_events = nullptr;

    std::vector< TraceEvents * > m_trace_events_list;
    std::vector< TraceWin * > m_trace_windows_list;
};
