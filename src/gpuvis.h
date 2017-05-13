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

class TraceEvents;
class TraceLoader;

/*
   [Compositor] NewFrame idx=2776
   [Compositor Client] WaitGetPoses End ThreadId=5125
   [Compositor] frameTimeout( 27 ms )
   [Compositor Client] Received Idx 100
   [Compositor] NewFrame idx=3769
   [Compositor] Predicting( 33.047485 ms )
   [Compositor] Re-predicting( 25.221056 ms )
   [Compositor] Re-predicting( -28.942781 ms )
   [Compositor] TimeSinceLastVSync: 0.076272(79975)
*/
class GraphPlot
{
public:
    GraphPlot() {}
    ~GraphPlot() {}

    bool init( TraceEvents &trace_events, const std::string &name,
               const std::string &filter_str, const std::string scanf_str );

    uint32_t find_ts_index( int64_t ts0 );

public:
    struct plotdata_t
    {
        int64_t ts;
        uint32_t eventid;
        float valf;
    };
    std::vector< plotdata_t > m_plotdata;

    float m_minval = FLT_MAX;
    float m_maxval = FLT_MIN;

    ImU32 m_color_line;
    ImU32 m_color_point;

    // plot: TimeSyncLastVSync
    std::string m_name;

    // $buf =~ "[Compositor] TimeSyncLastVsync: "
    std::string m_filter_str;

    // "[Compositor] TimeSyncLastVsync: %f("
    std::string m_scanf_str;
};

class ParsePlotStr
{
public:
    ParsePlotStr() {}
    ~ParsePlotStr() {}

    bool init( const char *scanf_str )
    {
        const char *pct_f = strstr( scanf_str, "%f" );

        if ( pct_f )
        {
            m_scanf_str = scanf_str;
            m_scanf_len = pct_f - scanf_str;
            return true;
        }

        return false;
    }

    bool parse( const char *buf )
    {
        if ( buf )
        {
            const char *pat_start = strncasestr( buf, m_scanf_str, m_scanf_len );

            if ( pat_start )
            {
                char *val_end;
                const char *val_start = pat_start + m_scanf_len;

                m_valf = strtof( val_start, &val_end );

                if ( val_start != val_end )
                {
                    m_val_start = val_start;
                    m_val_end = val_end;
                    return true;
                }
            }
        }

        return false;
    }

public:
    float m_valf;
    const char *m_val_start;
    const char *m_val_end;

    const char *m_scanf_str = nullptr;
    size_t m_scanf_len = 0;
};

class CreatePlotDlg
{
public:
    CreatePlotDlg() {}
    ~CreatePlotDlg() {}

    bool init( TraceEvents &trace_events, uint32_t eventid );
    bool render_dlg( TraceEvents &trace_events );
    void add_plot( CIniFile &inifile, class GraphRows &rows );

    static const char *get_plot_str( const trace_event_t &event );

public:
    GraphPlot *m_plot = nullptr;
    std::string m_plot_name;

    std::string m_plot_buf;
    std::string m_plot_err_str;
    char m_plot_name_buf[ 64 ];
    char m_plot_filter_buf[ 256 ];
    char m_plot_scanf_buf[ 256 ];
};

class TraceEvents
{
public:
    TraceEvents() {}
    ~TraceEvents() {}

public:
    // Return vec of locations for a tdop expression. Ie: "$name=drm_handle_vblank"
    const std::vector< uint32_t > *get_tdopexpr_locs( const char *name, std::string *err = nullptr );
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
        LOC_TYPE_Plot,
        LOC_TYPE_Timeline,
        LOC_TYPE_Timeline_hw,
        LOC_TYPE_Max
    };
    const std::vector< uint32_t > *get_locs( const char *name, loc_type_t *type = nullptr );

    GraphPlot *get_plot_ptr( const char *plot_name )
    {
        return m_graph_plots.get_val( fnv_hashstr32( plot_name ) );
    }
    GraphPlot &get_plot( const char *plot_name )
    {
        return m_graph_plots.m_map[ fnv_hashstr32( plot_name ) ];
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

    // plot name to GraphPlot
    util_umap< uint32_t, GraphPlot > m_graph_plots;

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

    void render( TraceLoader &loader );
    void render_options( TraceLoader &loader );
    void render_font_options( TraceLoader &loader );
    void render_log( TraceLoader &loader );
    void render_console( TraceLoader &loader );
    void render_menu( TraceLoader& loader );

public:
    ImGuiTextFilter m_filter;
    std::string m_inputbuf;
    std::vector< std::string > m_log;

    char m_trace_file[ PATH_MAX ] = { 0 };

    ImVec4 m_clear_color;

    size_t m_log_size = ( size_t )-1;
    bool m_quit = false;
    bool m_show_imgui_test_window = false;
    bool m_show_imgui_style_editor = false;
    bool m_show_imgui_metrics_editor = false;
    bool m_show_font_window = false;
};

class GraphRows
{
public:
    GraphRows() {}
    ~GraphRows() {}

public:
    // Initialize graph rows
    void init( CIniFile &inifile, TraceEvents &trace_events );

    struct graph_rows_info_t
    {
        TraceEvents::loc_type_t type;
        size_t event_count;
        std::string row_name;
        bool hidden;
    };
    const std::vector< graph_rows_info_t > get_hidden_rows_list();

    enum graph_rows_show_t
    {
        SHOW_ROW,
        SHOW_ALL_ROWS,
        HIDE_ROW,
        HIDE_ROW_AND_ALL_BELOW
    };
    void show_row( const std::string &name, graph_rows_show_t show );
    void add_row( TraceEvents &trace_events, const std::string &name );
    void move_row( const std::string &name_src, const std::string &name_dest );
    void rename_row( const char *comm_old, const char *comm_new );

    // Search in m_graph_rows_list for name. Returns index or -1 if not found.
    size_t find_row( const std::string &name, size_t not_found_val = ( size_t )-1 );

public:
    // List of graph rows
    std::vector< graph_rows_info_t > m_graph_rows_list;
    // List of graph rows we need to hide
    std::vector< std::string > m_graph_rows_hide;
    // List of filter expression we need to add to graph rows list
    std::vector< std::string > m_graph_rows_add;
    // Map of user row moves: row src --> row dst
    util_umap< std::string, std::string > m_graph_rows_move;
};

class TraceWin
{
public:
    TraceWin( TraceLoader &loader, TraceEvents &trace_events, std::string &title );
    ~TraceWin();

public:
    bool render();

protected:
    // Render events list
    void events_list_render( CIniFile &inifile );
    // Handle events list popup menu
    bool events_list_render_popupmenu( uint32_t eventid );
    // Handle mouse clicking and tooltips for event list
    bool events_list_handle_mouse( const trace_event_t &event, uint32_t i );

protected:
    // Render the process graph
    void graph_render_process();
    // Render regular graph row
    void graph_render_row( class graph_info_t &gi );
    // Render timeline graph row
    uint32_t graph_render_row_timeline( class graph_info_t &gi );
    // Render hw graph row
    uint32_t graph_render_hw_row_timeline( class graph_info_t &gi );
    // Render ftrace print row
    uint32_t graph_render_print_timeline( class graph_info_t &gi );
    // Render plot row
    uint32_t graph_render_plot( class graph_info_t &gi );
    // Render regular trace events
    uint32_t graph_render_row_events( class graph_info_t &gi );
    // Render graph vblanks, tick markers, mouse location, etc.
    void graph_render_vblanks( class graph_info_t &gi );

    // Handle graph popup menu
    bool graph_render_popupmenu( class graph_info_t &gi );
    // Mouse wheel, clicking for graph
    void graph_handle_mouse( class graph_info_t &gi );
    // Graph mouse handler when captured
    void graph_handle_mouse_captured( class graph_info_t &gi );
    // Set mouse tooltip for graph
    void graph_set_mouse_tooltip( class graph_info_t &gi, int64_t mouse_ts );

    // graph keyboard handling
    void graph_handle_hotkeys();
    void graph_handle_keyboard_scroll();

    // Make sure m_graph.start_ts and m_graph.length_ts are legit
    void graph_range_check_times();
    // Zoom in / out graph
    void graph_zoom( int64_t center_ts, int64_t ts0, bool zoomin );

protected:
    // Render trace information header
    void trace_render_info();
    // Render graph color picker
    void color_picker_render();

protected:
    // Rename a comm event
    bool rename_comm_event( const char *comm_old, const char *comm_new );
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
        return m_trace_events.m_events[ id ];
    }

public:
    // Window title
    std::string m_title;

    // Whether our window is open or not
    bool m_open = true;

    // false first time through render() call
    bool m_inited = false;

    // trace events
    TraceEvents &m_trace_events;
    // trace loader
    TraceLoader &m_loader;

    int m_selected_color = 0;
    ColorPicker m_colorpicker;

    // Ftrace print event id to display create plot dialog
    uint32_t m_create_plot_eventid = INVALID_ID;
    CreatePlotDlg m_create_plot_dlg;

    util_umap< int64_t, int > m_ts_to_eventid_cache;

    struct
    {
        // Is event list visible?
        bool show = false;

        bool do_gotoevent = false;
        int goto_eventid = 0;

        bool do_filter = false;
        char filter_buf[ 512 ] = { 0 };
        std::string filtered_events_str;
        std::vector< uint32_t > filtered_events;

        // Goto Time buffer
        char timegoto_buf[ 32 ] = { 0 };

        // Time Offset
        char timeoffset_buf[ 32 ] = { 0 };
        int64_t tsoffset = 0;

        uint32_t start_eventid = INVALID_ID;
        uint32_t end_eventid = INVALID_ID;

        // Event id of event list popup menu
        uint32_t popup_eventid = INVALID_ID;
        // Currently selected event.
        uint32_t selected_eventid = INVALID_ID;
        // Currently hovered event in event list.
        uint32_t hovered_eventid = INVALID_ID;

        // Hovered event ids to highlight in events list
        std::vector< uint32_t > highlight_ids;

        // Whether event list columns have been resized.
        bool columns_resized = false;
    } m_eventlist;

    enum mouse_captured_t
    {
        MOUSE_NOT_CAPTURED = 0,
        MOUSE_CAPTURED_SELECT_AREA,
        MOUSE_CAPTURED_ZOOM,
        MOUSE_CAPTURED_PAN,
    };
    struct
    {
        // Our graph row handling and info
        GraphRows rows;

        // Mouse timestamp location in graph
        int64_t ts_marker_mouse = -1;
        int64_t ts_marker_a = INT64_MAX;

        float resize_graph_click_pos = 0.0f;

        bool popupmenu = false;
        std::string mouse_over_row_name;
        std::vector< GraphRows::graph_rows_info_t > rows_hidden_rows;

        char rename_comm_buf[ 512 ] = { 0 };
        char new_row_buf[ 512 ] = { 0 };
        std::string new_row_errstr;

        // Graph Start
        bool do_start_timestr = false;
        int64_t start_ts = 0;
        char time_start_buf[ 32 ] = { 0 };

        // Graph Length
        bool do_length_timestr = false;
        int64_t length_ts = INT64_MAX;
        char time_length_buf[ 32 ] = { 0 };

        // Graph vertical panning
        float start_y = 0.0f;

        // Graph hovered event
        uint32_t hovered_eventid = INVALID_ID;

        // Mouse currently over our events graph?
        bool is_mouse_over = false;

        std::vector< std::pair< int64_t, int64_t > > saved_locs;

        mouse_captured_t mouse_captured = MOUSE_NOT_CAPTURED;
        ImVec2 mouse_capture_pos;

        const int64_t s_min_length = 100;
        const int64_t s_max_length = 7500 * MSECS_PER_SEC;
    } m_graph;

    friend class graph_info_t;
};

// TraceLoader ini options
typedef uint32_t option_id_t;
// Preset ini options
enum : uint32_t
{
    OPT_Invalid = ( uint32_t )-1,
    OPT_TimelineZoomGfx = 0,
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
    OPT_Scale,
    OPT_UseFreetype,
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

    void load_fonts();

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
    float get_optf( option_id_t opt )
    {
        return m_options[ opt ].valf;
    }
    int get_opt_crtc( int crtc )
    {
        uint32_t val = crtc + OPT_RenderCrtc0;

        return ( val <= OPT_RenderCrtc9 ) ? m_options[ val ].val : 0;
    }
    option_id_t add_option_graph_rowsize( const char *name, int defval = 4 );

    // Map row names to option IDs. Ie, "gfx", "print", "sdma0", etc.
    util_umap< std::string, option_id_t > m_name_optid_map;

    std::vector< option_t > m_options;
    std::string m_event_filter_str;

    FontInfo m_font_main;
    FontInfo m_font_small;
};
