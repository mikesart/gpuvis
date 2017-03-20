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

class StrPool
{
public:
    StrPool() {}
    ~StrPool() {}

    inline const char *getstr( const char *str )
    {
        uint32_t hashval = fnv_hashstr32( str );

        auto i = m_pool.find( hashval );
        if ( i == m_pool.end() )
            m_pool[ hashval ] = std::string( str );

        return m_pool[ hashval ].c_str();
    }

    inline const char *getstr( uint32_t hashval )
    {
        auto i = m_pool.find( hashval );

        if ( i == m_pool.end() )
            return NULL;
        return m_pool[ hashval ].c_str();
    }

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

    StrPool m_strpool;
    trace_info_t m_trace_info;
    std::vector< trace_event_t > m_trace_events;

    // Map of event name hashval to array of event locations.
    TraceLocations m_event_locations;

    // Map of comm hashval to array of event locations.
    TraceLocations m_comm_locations;
};

struct GPUVisCon
{
public:
    GPUVisCon() {}
    ~GPUVisCon() {}

    void init( class CIniFile *inifile );
    void shutdown( class CIniFile *inifile );

    void clear_log();
    void logf( const char *fmt, ... ) ATTRIBUTE_PRINTF( 2, 3 );

    void exec_command( const char *command_line );

    void render( const char *title );

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

    ImVec4 m_clear_color;
    bool m_open = true;
    bool m_quit = false;
    bool m_scroll_to_bottom = true;
    bool m_show_imgui_test_window = false;
    bool m_show_imgui_style_editor = false;
    bool m_show_imgui_metrics_editor = false;
};
