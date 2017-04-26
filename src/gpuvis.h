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
    util_umap< uint32_t, std::string > m_pool;
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

    TRACE_FLAG_FTRACE_PRINT = 0x100,
    TRACE_FLAG_IS_VBLANK = 0x200,
    TRACE_FLAG_IS_TIMELINE = 0x400,

    TRACE_FLAG_IS_SW_QUEUE = 0x1000, // amdgpu_cs_ioctl
    TRACE_FLAG_IS_HW_QUEUE = 0x2000, // amdgpu_sched_run_job
    TRACE_FLAG_FENCE_SIGNALED = 0x4000, // *fence_signaled
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
    bool is_timeline() const
    {
        return !!( flags & TRACE_FLAG_IS_TIMELINE );
    }
    const char *get_timeline_name( const char *def = NULL ) const
    {
        if ( flags & TRACE_FLAG_IS_SW_QUEUE )
            return "SW queue";
        else if ( flags & TRACE_FLAG_IS_HW_QUEUE )
            return "HW queue";
        else if ( is_fence_signaled() )
            return "Execution";

        return def;
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

    uint32_t color;

    int64_t ts;                 // timestamp
    const char *comm;           // command line
    const char *system;         // event system (ftrace-print, etc.)
    const char *name;           // event name
    const char *timeline;       // event timeline (gfx, sdma0, ...)
    const char *user_comm;      // User space comm (if we can figure this out)

    std::vector< event_field_t > fields;
};

const event_field_t *find_event_field( const std::vector< event_field_t > &fields, const char *name );
const char *get_event_field_val( const std::vector< event_field_t > &fields, const char *name );

typedef std::function< int ( const trace_info_t &info, const trace_event_t &event ) > EventCallback;
int read_trace_file( const char *file, StrPool &strpool, EventCallback &cb );

class TraceLocations
{
public:
    TraceLocations() {}
    ~TraceLocations() {}

    void add_location_u32( uint32_t hashval, uint32_t loc )
    {
        std::vector< uint32_t > *plocs = m_locs.get_val( hashval, std::vector< uint32_t >() );

        plocs->push_back( loc );
    }

    std::vector< uint32_t > *get_locations_u32( uint32_t hashval )
    {
        return m_locs.get_val( hashval );
    }

    void add_location_str( const char *name, uint32_t loc )
    {
        add_location_u32( fnv_hashstr32( name ), loc );
    }

    std::vector< uint32_t > *get_locations_str( const char *name )
    {
        return get_locations_u32( fnv_hashstr32( name ) );
    }

public:
    // Map of name hashval to array of event locations.
    util_umap< uint32_t, std::vector< uint32_t > > m_locs;
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

    // Rename a comm event
    bool rename_comm( const char *comm_old, const char *comm_new );

    void calculate_event_durations();
    void calculate_event_print_info();

    enum loc_type_t
    {
        LOC_TYPE_Comm,
        LOC_TYPE_Tdopexpr,
        LOC_TYPE_Print,
        LOC_TYPE_Timeline,
        LOC_TYPE_Timeline_hw,
        LOC_TYPE_Max
    };

    const std::vector< uint32_t > *get_locs( const char *name, loc_type_t *type = nullptr )
    {
        const std::vector< uint32_t > *plocs = NULL;

        if ( !strcmp( name, "print" ) )
        {
            // Check for explicit "print" row
            if ( type )
                *type = LOC_TYPE_Print;
            plocs = get_tdopexpr_locs( "$name=print" );
        }
        else
        {
            size_t len = strlen( name );

            if ( ( len > 3 ) && !strcmp( name + len - 3, " hw" ) )
            {
                // Check for "gfx hw", "comp_1.1.1 hw", etc.
                uint32_t hashval = fnv_hashstr32( name, len - 3 );

                if ( type )
                    *type = LOC_TYPE_Timeline_hw;
                plocs = m_timeline_locations.get_locations_u32( hashval );
            }
            else
            {
                // Check for regular comm type rows
                if ( type )
                    *type = LOC_TYPE_Comm;
                plocs = get_comm_locs( name );

                if ( !plocs )
                {
                    // TDOP Expressions. Ie, $name = print, etc.
                    if ( type )
                        *type = LOC_TYPE_Tdopexpr;
                    plocs = get_tdopexpr_locs( name );

                    if ( !plocs )
                    {
                        // Timelines: sdma0, gfx, comp_1.2.1, etc.
                        if ( type )
                            *type = LOC_TYPE_Timeline;
                        plocs = get_timeline_locs( name );
                    }
                }
            }
        }

        if ( !plocs && type )
            *type = LOC_TYPE_Max;
        return plocs;
    }

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

    struct event_print_info_t
    {
        const char *buf;
        ImVec2 buf_size;
    };
    util_umap< uint32_t, event_print_info_t > m_print_buf_info;
    float m_buf_size_max_x = -1.0f;

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
    void render_console( TraceLoader &loader );

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

class GraphRows
{
public:
    GraphRows() {}
    ~GraphRows() {}

public:
    // Initialize graph rows
    void init( CIniFile &inifile, TraceEvents *trace_events );

    struct graph_rows_info_t
    {
        size_t event_count;
        std::string name;
        bool hidden;
    };
    const std::vector< graph_rows_info_t > get_hidden_rows_list( TraceEvents *trace_events );

    enum graph_rows_show_t
    {
        SHOW_ROW,
        SHOW_ALL_ROWS,
        HIDE_ROW,
        HIDE_ROW_AND_ALL_BELOW
    };
    void show_row( const std::string &name, graph_rows_show_t show );

    void rename_comm( TraceEvents *trace_events, const char *comm_old, const char *comm_new );

public:
    // List of graph rows
    std::vector< graph_rows_info_t > m_graph_rows_list;
    // List of graph rows we need to hide
    std::vector< std::string > m_graph_rows_list_hide;
};

class TraceWin
{
public:
    TraceWin( TraceLoader &loader, TraceEvents *trace_events, std::string &title );
    ~TraceWin();

public:
    // Main TraceWin render
    bool render();

protected:
    // Render trace information header
    void render_trace_info();
    // Render the process graph
    void render_process_graph();
    // Render events list
    void render_events_list( CIniFile &inifile );
    // Render the graph color picker
    void render_color_picker();

protected:
    // Handle events list popup menu
    bool render_events_list_popup( uint32_t eventid );

    // Initialize m_graph_start_ts
    void render_time_offset_button_init( TraceEvents &trace_events );

    // Handle mouse clicking and tooltips for event list
    bool handle_event_list_mouse( const trace_event_t &event, uint32_t i );

    // Render regular graph row
    void render_graph_row( class graph_info_t &gi );

    // Render timeline grah row
    uint32_t render_graph_row_timeline( class graph_info_t &gi );
    // Render hw graph row
    uint32_t render_graph_hw_row_timeline( class graph_info_t &gi );
    // Render ftrace print row
    uint32_t render_graph_print_timeline( class graph_info_t &gi );
    // Render regular trace events
    uint32_t render_graph_row_events( class graph_info_t &gi );

    // Render graph vblanks, tick markers, mouse location, etc.
    void render_graph_vblanks( class graph_info_t &gi );
    // Right clicking on graph
    bool render_graph_popup( class graph_info_t &gi );

    // Mouse wheel, clicking for graph
    void handle_mouse_graph( class graph_info_t &gi );
    // Graph mouse handler when captured
    void handle_mouse_graph_captured( class graph_info_t &gi );
    // Set mouse tooltip for graph
    void set_mouse_graph_tooltip( class graph_info_t &gi, int64_t mouse_ts );

    // Potentially dd a hovered mouse graph event
    bool add_mouse_hovered_event( float x, class graph_info_t &gi, const trace_event_t &event );

    // Handle graph keys
    void handle_graph_hotkeys();
    void handle_graph_keyboard_scroll();


    // Make sure m_graph_start_ts and m_graph_length_ts are legit
    void range_check_graph_location();

    // Rename a comm event
    bool rename_comm_event( const char *comm_old, const char *comm_new );

public:
    // Return an event id for a given time stamp
    int ts_to_eventid( int64_t ts );
    // Return an event id from a time string
    int timestr_to_eventid( const char *buf, int64_t tsoffset );
    // Convert a time string to a time stamp
    int64_t timestr_to_ts( const char *buf, int64_t tsoffset = 0 );
    // Convert a time stamp to a time string
    std::string ts_to_timestr( int64_t event_ts, int64_t tsoffset = 0, int precision = 6 );

    trace_event_t &get_event( uint32_t id )
    {
        return m_trace_events->m_events[ id ];
    }

public:
    TraceLoader &m_loader;

    // Our events
    TraceEvents *m_trace_events = nullptr;

    // Window title
    std::string m_title;

    // false first time through render() call
    bool m_inited = false;

    // Whether our window is open or not
    bool m_open = true;

    GraphRows m_graphrows;

    bool m_do_gotoevent = false;
    int m_goto_eventid = 0;

    // Mouse timestamp location in graph
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

    float m_resize_graph_click_pos = 0.0f;

    bool m_graph_popup = false;
    std::string m_mouse_over_row_name;
    std::vector< GraphRows::graph_rows_info_t > m_graph_rows_hidden_rows;

    char m_rename_comm_buf[ 512 ] = { 0 };

    // Graph Start
    bool m_do_graph_start_timestr = false;
    int64_t m_graph_start_ts = 0;
    char m_graphtime_start_buf[ 32 ] = { 0 };
    // Graph Length
    bool m_do_graph_length_timestr = false;
    int64_t m_graph_length_ts = INT64_MAX;
    char m_graphtime_length_buf[ 32 ] = { 0 };

    // Graph vertical panning
    float m_graph_start_y = 0.0f;

    bool m_do_graph_zoom_in = false;
    bool m_do_graph_zoom_out = false;

    // Currently selected event.
    uint32_t m_selected_eventid = INVALID_ID;
    // Currently hovered event in event list.
    uint32_t m_hovered_eventlist_eventid = INVALID_ID;
    // Graph hovered event
    uint32_t m_hovered_graph_eventid = INVALID_ID;
    // Hovered event ids so we can highlight them in the events list
    std::vector< uint32_t > m_highlight_ids;

    uint32_t m_eventlist_start_eventid = INVALID_ID;
    uint32_t m_eventlist_end_eventid = INVALID_ID;

    std::vector< std::pair< int64_t, int64_t > > m_saved_graph_locs;

    // Mouse currently over our events graph?
    bool m_mouse_over_graph = false;

    enum mouse_captured_t
    {
        MOUSE_NOT_CAPTURED = 0,
        MOUSE_CAPTURED_SELECT_AREA,
        MOUSE_CAPTURED_ZOOM,
        MOUSE_CAPTURED_PAN,
    };
    mouse_captured_t m_mouse_captured = MOUSE_NOT_CAPTURED;
    ImVec2 m_mouse_capture_pos;

    int m_selected_color = 0;
    ColorPicker m_colorpicker;

    std::vector< uint32_t > m_filtered_events;
    std::string m_filtered_events_str;

    util_umap< int64_t, int > m_ts_to_eventid_cache;

    const int64_t g_min_graph_length = 100;
    const int64_t g_max_graph_length = 5000 * MSECS_PER_SEC;

    friend class graph_info_t;
};

// TraceLoader ini options
enum option_id_t
{
    OPT_Invalid = -1,
    OPT_TimelineZoomGfx,
    OPT_TimelineLabels,
    OPT_TimelineEvents,
    OPT_TimelineRenderUserSpace,
    OPT_PrintTimelineLabels,
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
    OPT_GraphHeight,
    OPT_GraphHeightZoomed,
    OPT_EventListRowCount,

    OPT_TimelineGfxSize,
    OPT_TimelinePrint,

    OPT_TimelineSdma0Size,
    OPT_TimelineSdma1Size,

    OPT_PresetMax
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

    enum opt_type { OPT_Bool, OPT_Int, OPT_Float };
    struct option_t
    {
        void opt_bool( const std::string &description, const std::string &key, bool defval )
        {
            type = OPT_Bool;
            desc = description;
            inikey = key;
            val = defval;
        }

        void opt_int( const std::string &description, const std::string &key, int defval, int minval, int maxval )
        {
            type = OPT_Int;
            desc = description;
            inikey = key;
            val = defval;
            val_min = minval;
            val_max = maxval;
        }

        void opt_float( const std::string &description, const std::string &key, float defval, float minval, float maxval )
        {
            type = OPT_Float;
            desc = description;
            inikey = key;
            valf = defval;
            valf_min = minval;
            valf_max = maxval;
        }

        std::string desc;
        std::string inikey;

        bool hidden = false;

        opt_type type;
        int val;
        int val_min;
        int val_max;
        float valf;
        float valf_min;
        float valf_max;
    };
    int get_opt( option_id_t opt )
    {
        return m_options[ opt ].val;
    }
    int get_optf( option_id_t opt )
    {
        return m_options[ opt ].valf;
    }
    int get_opt_crtc( int crtc )
    {
        int val = crtc + OPT_RenderCrtc0;

        return ( val <= OPT_RenderCrtc9 ) ? m_options[ val ].val : 0;
    }

    size_t m_comp_option_index = 0;
    size_t m_comp_option_count = 0;

    std::vector< option_t > m_options;
    std::string m_event_filter_str;
};
