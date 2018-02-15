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

// Opts singleton
class Opts &s_opts();

// Main app singleton
class MainApp &s_app();

class TraceEvents;

enum loc_type_t
{
    LOC_TYPE_Comm,
    LOC_TYPE_Tdopexpr,
    LOC_TYPE_Print,
    LOC_TYPE_Plot,
    LOC_TYPE_AMDTimeline,
    LOC_TYPE_AMDTimeline_hw,
    LOC_TYPE_i915RequestWait,
    LOC_TYPE_i915Request,
    LOC_TYPE_Max
};

enum i915_type_t
{
    i915_req_Queue,
    i915_req_Add,
    i915_req_Submit,
    i915_req_In,
    i915_req_Notify,
    i915_req_Out,

    i915_reqwait_begin,
    i915_reqwait_end,

    i915_req_Max
};
i915_type_t get_i915_reqtype( const trace_event_t &event );

class TraceLocations
{
public:
    TraceLocations() {}
    ~TraceLocations() {}

    void add_location_u32( uint32_t hashval, uint32_t loc )
    {
        std::vector< uint32_t > *plocs = m_locs.get_val_create( hashval );

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

class TraceLocationsRingCtxSeq
{
public:
    TraceLocationsRingCtxSeq() {}
    ~TraceLocationsRingCtxSeq() {}

    bool add_location( const trace_event_t &event );
    std::vector< uint32_t > *get_locations( const trace_event_t &event );
    std::vector< uint32_t > *get_locations( const char *ringstr, uint32_t seqno, const char *ctxstr );

    uint64_t db_key( const trace_event_t &event );
    uint64_t db_key( const char *ringstr, uint32_t seqno, const char *ctxstr );

public:
    // Map of db_key to array of event locations.
    util_umap< uint64_t, std::vector< uint32_t > > m_locs;
};

// Given a sorted array (like from TraceLocations), binary search for eventid
//   and return the vector index, or vec.size() if not found.
inline size_t vec_find_eventid( const std::vector< uint32_t > &vec, uint32_t eventid )
{
    auto i = std::lower_bound( vec.begin(), vec.end(), eventid );

    return i - vec.begin();
}

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

    bool init( const char *scanf_str );
    bool parse( const char *buf );

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

    static const std::string get_plot_str( const trace_event_t &event );

public:
    GraphPlot *m_plot = nullptr;
    std::string m_plot_name;

    std::string m_plot_buf;
    std::string m_plot_err_str;
    char m_plot_name_buf[ 128 ];
    char m_plot_filter_buf[ 512 ];
    char m_plot_scanf_buf[ 512 ];
};

class CreateGraphRowDlg
{
public:
    CreateGraphRowDlg() {}
    ~CreateGraphRowDlg() {}

    void init();
    void shutdown();

    bool show_dlg( TraceEvents &trace_events, uint32_t eventid );
    bool render_dlg( TraceEvents &trace_events );

public:
    std::string m_buf;
    std::string m_err_str;

    char m_name_buf[ 128 ];
    char m_filter_buf[ 512 ];

    std::vector< std::string > m_previous_filters;
};

class CreateRowFilterDlg
{
public:
    CreateRowFilterDlg() {}
    ~CreateRowFilterDlg() {}

    void init();
    void shutdown();

    bool show_dlg( TraceEvents &trace_events );
    bool render_dlg( TraceEvents &trace_events );

public:
    std::string m_buf;
    std::string m_err_str;

    char m_filter_buf[ 512 ];

    std::vector< std::string > m_previous_filters;
};

struct row_filter_t
{
    BitVec *bitvec = nullptr;
    std::vector< std::string > filters;
};

class RowFilters
{
public:
    RowFilters( util_umap< uint32_t, row_filter_t > &graph_row_filters, const std::string &row_name ) :
        m_graph_row_filters( graph_row_filters )
    {
        m_rowname_hash = fnv_hashstr32( row_name.c_str() );
        m_row_filters = m_graph_row_filters.get_val( m_rowname_hash );
    }
    ~RowFilters() {}

    size_t find_filter( const std::string &filter );
    void toggle_filter( TraceEvents &trace_events, size_t idx, const std::string &filter );

public:
    uint32_t m_rowname_hash = 0;
    row_filter_t *m_row_filters = nullptr;
    util_umap< uint32_t, row_filter_t > &m_graph_row_filters;
};

class FrameMarkers
{
public:
    FrameMarkers()
    {
        dlg.m_left_marker_buf[ 0 ] = 0;
        dlg.m_right_marker_buf[ 0 ] = 0;
    }
    ~FrameMarkers() {}

    void init();
    void shutdown();

    bool show_dlg( TraceEvents &trace_events, uint32_t eventid = INVALID_ID );
    bool render_dlg( TraceEvents &trace_events );

    int64_t get_frame_len( TraceEvents &trace_events, int frame );

protected:
    void clear_dlg();
    void set_tooltip();
    void setup_frames( TraceEvents &trace_events, bool set_frames );

public:
    // Variables use in Frame Marker dialog
    struct
    {
        // Whether we've checked current marker_buf filter text
        bool m_checked = false;

        // Stats after checking frame marker filters
        uint32_t m_count = 0;
        int64_t m_tot_ts = 0;
        int64_t m_min_ts = 0;
        int64_t m_max_ts = 0;

        // Left/Right marker filters
        char m_left_marker_buf[ 512 ];
        char m_right_marker_buf[ 512 ];

        // Left/Right marker filter error strings
        std::string m_left_filter_err_str;
        std::string m_right_filter_err_str;

        // Left/Right event locations
        const std::vector< uint32_t > *m_left_plocs = nullptr;
        const std::vector< uint32_t > *m_right_plocs = nullptr;
    } dlg;

    // Variables used to show & select set frame markers
    std::vector< uint32_t > m_left_frames;
    std::vector< uint32_t > m_right_frames;

    // Which frame is left, right, and selected in graph
    int m_frame_marker_left = -1;
    int m_frame_marker_right = -1;
    int m_frame_marker_selected = -1;

    std::vector< std::pair< std::string, std::string > > m_previous_filters;
};

struct print_info_t
{
    // Event time stamp. Can be shifted from original event ts.
    int64_t ts;
    int tgid;

    uint32_t graph_row_id_pid;
    uint32_t graph_row_id_tgid;

    const char *buf;
    ImVec2 size;
};

struct ftrace_row_info_t
{
    // pid=-1: rows+count for all ftrace print events
    // tgid=# & pid=0: rows+count for tgid
    // tgid=0 & pid=#: rows+count for pid
    int pid = 0;
    int tgid = 0;
    uint32_t rows = 0;
    uint32_t count = 0;
};

class TraceEvents
{
public:
    TraceEvents() {}
    ~TraceEvents();

public:
    enum tracestatus_t
    {
        Trace_Loaded,
        Trace_Loading,
        Trace_Initializing,
        Trace_Error
    };
    tracestatus_t get_load_status( uint32_t *count = NULL );

    // Return vec of locations for a tdop expression. Ie: "$name=drm_handle_vblank"
    const std::vector< uint32_t > *get_tdopexpr_locs( const char *name, std::string *err = nullptr );
    // Return vec of locations for a cmdline. Ie: "SkinningApp-1536"
    const std::vector< uint32_t > *get_comm_locs( const char *name );
    // "gfx", "sdma0", etc.
    const std::vector< uint32_t > *get_timeline_locs( const char *name );
    // Pass a string like "gfx_249_91446"
    const std::vector< uint32_t > *get_gfxcontext_locs( const char *name );
    // Return vec of locations for sched_switch events.
    enum switch_t { SCHED_SWITCH_PREV, SCHED_SWITCH_NEXT };
    const std::vector< uint32_t > *get_sched_switch_locs( int pid, switch_t switch_type );

    void calculate_amd_event_durations();
    void calculate_i915_req_event_durations();
    void calculate_i915_reqwait_event_durations();
    void calculate_event_print_info();
    void calculate_vblank_info();

    void invalidate_ftraceprint_colors();
    void update_ftraceprint_colors();

    void update_fence_signaled_timeline_colors();
    void update_tgid_colors();

    void remove_single_tgids();

    const std::vector< uint32_t > *get_locs( const char *name, loc_type_t *type = nullptr, std::string *errstr = nullptr );

    GraphPlot *get_plot_ptr( const char *plot_name )
    {
        return m_graph_plots.get_val( fnv_hashstr32( plot_name ) );
    }
    GraphPlot &get_plot( const char *plot_name )
    {
        return m_graph_plots.m_map[ fnv_hashstr32( plot_name ) ];
    }

    // Return "foorbarapp-1234" comm string for specified pid
    const char *comm_from_pid( int pid, const char *def = NULL );

    // Return "thread1-1234 (mainthread-1233)" string from "thread1-1234" comm string
    // If no tgid info, return comm
    const char *tgidcomm_from_pid( int pid );
    const char *tgidcomm_from_commstr( const char *comm );

    // Return tgid info for a specified pid (or NULL)
    const tgid_info_t *tgid_from_pid( int pid );
    // Parse a "foorbarapp-1234" comm string and return tgid info (or NULL)
    const tgid_info_t *tgid_from_commstr( const char *comm );

    void set_event_color( const std::string &eventname, ImU32 color );

    const char *get_event_gfxcontext_str( const trace_event_t &event );

    std::string get_ftrace_ctx_str( const trace_event_t &event );

    print_info_t *get_print_info( uint32_t id )
    {
        return m_ftrace.print_info.get_val( id );
    }
    uint32_t ts_to_ftrace_print_info_idx( const std::vector< uint32_t > &locs, int64_t ts );

public:
    // Called once on background thread after all events loaded.
    void init();

    void init_new_event( trace_event_t &event );
    void init_new_event_vblank( trace_event_t &event );
    void init_sched_switch_event( trace_event_t &event );
    void init_sched_process_fork( trace_event_t &event );
    void init_amd_timeline_event( trace_event_t &event );
    void init_i915_event( trace_event_t &event );

    int new_event_cb( const trace_event_t &event );
    void new_event_ftrace_print( trace_event_t &event );

    ftrace_row_info_t *get_ftrace_row_info_pid( int pid, bool add = false );
    ftrace_row_info_t *get_ftrace_row_info_tgid( int tgid, bool add = false );
    // Get info for print row names: "print", "print pid:1234", "print tgid:3456", etc.
    ftrace_row_info_t *get_ftrace_row_info( const char *row_name );

public:
    std::string m_filename;
    size_t m_filesize = 0;

    StrPool m_strpool;
    trace_info_t m_trace_info;
    std::vector< trace_event_t > m_events;

    // Max drm_vblank_event crc value we've seen
    int m_crtc_max = -1;

    // Map of tdop expression string hashval to array of event locations.
    TraceLocations m_tdopexpr_locs;
    std::set< uint32_t > m_failed_commands;

    // Map of comm hashval to array of event locations.
    TraceLocations m_comm_locs;

    // Map of event names to event locations
    TraceLocations m_eventnames_locs;

    // Map of timeline/context/seqno to array of event locations.
    TraceLocations m_gfxcontext_locs;
    TraceLocations m_gfxcontext_msg_locs;

    // Map of timeline (gfx, sdma0, etc) event locations.
    TraceLocations m_amd_timeline_locs;

    // Map of pid to sched_switch event locations.
    TraceLocations m_sched_switch_prev_locs;
    TraceLocations m_sched_switch_next_locs;

    // plot name to GraphPlot
    util_umap< uint32_t, GraphPlot > m_graph_plots;

    // map of pid to 'thread1-1234 (mainthread-1233)'
    util_umap< int, const char * > m_pid_commstr_map;

    // Map hashed row name to count of rows calculated by row_pos_t
    util_umap< uint32_t, uint32_t > m_row_count;

    struct
    {
        // Intel request_wait_begin events key'd on ring/ctx/seqno
        TraceLocationsRingCtxSeq reqwait_begin_locs;
        // Intel request_wait_end events key'd on: "i915_reqwait ring%u",ring
        TraceLocations reqwait_end_locs;

        // i915_gem_request_[add|submit|in|out], intel_engine_notify events
        TraceLocationsRingCtxSeq gem_req_locs;
        // Intel request events key'd on: "i915_req ring%u",ring
        TraceLocations req_locs;
    } m_i915;

    struct ftrace_pair_t
    {
        uint32_t lefthashval;   // hashval of leftstr
        uint32_t righthashval;  // hashval of rightstr;
        std::string leftstr;    // "[Compositor] Before wait query", etc
        std::string rightstr;   // "[Compositor] After wait query", etc
    };

    struct
    {
        // ftrace print event IDs sorted by timestamp
        std::vector< uint32_t > print_locs;

        // event id to ftrace print event info map
        util_umap< uint32_t, print_info_t > print_info;

        // Max ftrace print rendering x size value
        float text_size_max = -1.0f;

        // Max ftrace print ts duration value
        int64_t print_ts_max = 0;

        // Row info for each pid / tgid row
        util_umap< uint32_t, ftrace_row_info_t > row_info;

        // Map of ftrace print begin/end ctx to event ids
        util_umap< uint64_t, uint32_t > begin_ctx;
        util_umap< uint64_t, uint32_t > end_ctx;

        // map of ftrace buf '( lefthashval << 32 ) + event.pid' to ftrace start event id
        util_umap< uint64_t, uint32_t > pairs_ctx;

        // Array of ftrace pairs sorted on lefthashval
        std::vector< ftrace_pair_t > ftrace_pairs;
    } m_ftrace;

    struct vblank_info_t
    {
        // Last vblank ts we hit for this crtc
        int64_t last_vblank_ts = 0;
        // Median diff ts
        int64_t median_diff_ts = 0;

        // map tsdiff to count
        std::map< int64_t, uint32_t > diff_ts_count;
        // Total count of vblank events in diff_ts_count map
        uint32_t count = 0;
    };
    // vblank information for specific crtc
    std::vector< vblank_info_t > m_vblank_info;
    // Map vblank seq to m_drm_vblank_event_queued event id
    util_umap< uint32_t, uint32_t > m_drm_vblank_event_queued;

    // 0: events loaded, 1+: loading events, -1: error
    SDL_atomic_t m_eventsloaded = { 1 };
};

class GraphRows
{
public:
    GraphRows() {}
    ~GraphRows() {}

public:
    struct graph_rows_info_t
    {
        bool hidden;
        loc_type_t type;

        std::string row_name;
        std::string row_filter_expr;
        size_t event_count;
    };

public:
    // Initialize graph rows
    void init( TraceEvents &trace_events );
    void shutdown();

    void add_row( const std::string &name, const std::string &filter_expr, float scale = 1.0f );
    void move_row( const std::string &name_src, const std::string &name_dest );

    // Search in m_graph_rows_list for name. Returns index or -1 if not found.
    size_t find_row( const std::string &name, size_t not_found_val = ( size_t )-1 );
    graph_rows_info_t *get_row( const std::string &name );

    const std::vector< graph_rows_info_t > get_hidden_rows_list();

    float get_row_scale_ts( const std::string &name );

    enum graph_rows_show_t
    {
        SHOW_ROW,
        SHOW_ALL_ROWS,
        HIDE_ROW,
        HIDE_ROW_AND_ALL_BELOW
    };
    void show_row( const std::string &name, graph_rows_show_t show );
    void show_tgid_rows( const tgid_info_t *tgid_info, graph_rows_show_t show );

protected:
    void push_row( const std::string &name, loc_type_t type, size_t event_count, bool hidden = false )
        { m_graph_rows_list.push_back( { hidden, type, name, name, event_count } ); }

public:
    TraceEvents *m_trace_events = nullptr;

    // List of graph rows
    std::vector< graph_rows_info_t > m_graph_rows_list;

    // List of graph rows we need to hide
    std::vector< std::string > m_graph_rows_hide;

    // List of name / filter expressions we need to add to graph rows list
    util_umap< std::string, std::string > m_graph_rows_add;

    // Map of user row moves: row src --> row dst
    util_umap< std::string, std::string > m_graph_rows_move;

    // Map of user row name to timestamp scaling
    util_umap< std::string, std::string > m_graph_row_scale_ts;
};

class graph_info_t;

class TraceWin
{
public:
    TraceWin( const char *filename, size_t filesize );
    ~TraceWin();

public:
    void render();
    void trace_render_info();

    trace_event_t &get_event( uint32_t id )
    {
        return m_trace_events.m_events[ id ];
    }

protected:
    // Render events list
    void eventlist_render_options();
    void eventlist_render();

    // Handle events list popup menu
    bool eventlist_render_popupmenu( uint32_t eventid );

    // Handle mouse clicking and tooltips for event list
    bool eventlist_handle_mouse( const trace_event_t &event, uint32_t i );
    void eventlist_handle_hotkeys();

protected:
    // Render graph
    void graph_render_options();
    void graph_render();

protected:
    // Internal render graph functions
    void graph_render_resizer( graph_info_t &gi );
    void graph_render_hscrollbar( graph_info_t &gi );

    // Render graph rows
    void graph_render_rows( graph_info_t &gi );
    void graph_render_zoomed_rows( graph_info_t &gi );
    // Render a graph row
    void graph_render_single_row( graph_info_t &gi );

    // Render amd timeline graph row
    uint32_t graph_render_amd_timeline( graph_info_t &gi );
    // Render amd hw graph row
    uint32_t graph_render_amdhw_timeline( graph_info_t &gi );
    // Render ftrace print row
    uint32_t graph_render_print_timeline( graph_info_t &gi );
    // Render plot row
    uint32_t graph_render_plot( graph_info_t &gi );
    // Render regular trace events
    uint32_t graph_render_row_events( graph_info_t &gi );
    // Render intel i915 request_wait events
    uint32_t graph_render_i915_reqwait_events( graph_info_t &gi );
    // Render intel i915 request_add, request_submit, request_in, request_out, intel_engine_notify
    uint32_t graph_render_i915_req_events( graph_info_t &gi );

    // Render graph decorations
    void graph_render_time_ticks( graph_info_t &gi, float h0, float h1 );
    void graph_render_vblanks( graph_info_t &gi );
    void graph_render_mouse_pos( graph_info_t &gi );
    void graph_render_eventids( graph_info_t &gi );
    void graph_render_mouse_selection( graph_info_t &gi );
    void graph_render_eventlist_selection( graph_info_t &gi );
    void graph_render_row_labels( graph_info_t &gi );
    void graph_render_framemarker_frames( graph_info_t &gi );

    bool frame_markers_enabled();
    void frame_markers_goto( int target, bool fit_frame );

    // Render plot, graph rows, filter dialogs, etc
    void graph_dialogs_render();
    // Handle graph popup menu
    bool graph_render_popupmenu( graph_info_t &gi );
    // Mouse wheel, clicking for graph
    void graph_handle_mouse( graph_info_t &gi );
    // Graph mouse handler when captured
    void graph_handle_mouse_captured( graph_info_t &gi );
    void graph_handle_mouse_over( graph_info_t &gi );

    // Graph mouse tooltip functions
    void graph_mouse_tooltip( graph_info_t &gi, int64_t mouse_ts );
    void graph_mouse_tooltip_rowinfo( std::string &ttip, graph_info_t &gi, int64_t mouse_ts );
    void graph_mouse_tooltip_vblanks( std::string &ttip, graph_info_t &gi, int64_t mouse_ts );
    void graph_mouse_tooltip_markers( std::string &ttip, graph_info_t &gi, int64_t mouse_ts );
    void graph_mouse_tooltip_sched_switch( std::string &ttip, graph_info_t &gi, int64_t mouse_ts );
    void graph_mouse_tooltip_hovered_items( std::string &ttip, graph_info_t &gi, int64_t mouse_ts );
    void graph_mouse_tooltip_hovered_amd_fence_signaled( std::string &ttip, graph_info_t &gi, int64_t mouse_ts );

    // Graph keyboard handling
    void graph_handle_hotkeys( graph_info_t &gi );
    void graph_handle_keyboard_scroll( graph_info_t &gi );

protected:
    // Make sure m_graph.start_ts and m_graph.length_ts are legit
    void graph_range_check_times();

    // Zoom in / out graph
    void graph_zoom( int64_t center_ts, int64_t ts0, bool zoomin, int64_t newlen = INT64_MAX );

    bool is_graph_row_zoomable();
    void zoom_graph_row();

    void graph_center_event( uint32_t eventid );

    int graph_marker_menuitem( const char *label, bool check_valid, action_t action );

    bool graph_has_saved_locs();

protected:
    // Return an event id for a given time stamp
    uint32_t ts_to_eventid( int64_t ts );
    // Return an event id from a time string
    uint32_t timestr_to_eventid( const char *buf );

    bool graph_marker_valid( int idx0 );
    void graph_marker_set( size_t index, int64_t ts, const char *str = NULL );

public:
    // Window title
    std::string m_title;

    // Whether our window is open or not
    bool m_open = true;

    // false first time through render() call
    bool m_inited = false;

    // trace events
    TraceEvents m_trace_events;

    // Ftrace print event id to display create plot dialog
    uint32_t m_create_plot_eventid = INVALID_ID;
    CreatePlotDlg m_create_plot_dlg;

    uint32_t m_create_graph_row_eventid = INVALID_ID;
    CreateGraphRowDlg m_create_graph_row_dlg;

    bool m_row_filters_enabled = true;
    bool m_create_row_filter_dlg_show = false;
    std::string m_create_row_filter_dlg_rowname;
    CreateRowFilterDlg m_create_row_filter_dlg;

    // Map graph row name hash to array of row_filters
    util_umap< uint32_t, row_filter_t > m_graph_row_filters;

    uint32_t m_create_filter_eventid = INVALID_ID;
    FrameMarkers m_frame_markers;

    util_umap< int64_t, uint32_t > m_ts_to_eventid_cache;

    // Filter data
    struct
    {
        bool enabled = false;
        // Filter string
        char buf[ 512 ] = { 0 };
        std::string errstr;
        // List of filtered event ids
        std::vector< uint32_t > events;
        // pid -> count of !filtered events for that pid
        util_umap< int, uint32_t > pid_eventcount;
    } m_filter;

    struct
    {
        bool do_gotoevent = false;
        uint32_t goto_eventid = 0;

        int64_t ts_marker_mouse_sync = -1;

        bool hide_sched_switch_events_val = true;

        // Goto Time buffer
        char timegoto_buf[ 32 ] = { 0 };

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
        bool has_focus = false;
    } m_eventlist;

    enum mouse_captured_t
    {
        MOUSE_NOT_CAPTURED = 0,
        MOUSE_CAPTURED_SELECT_AREA,
        MOUSE_CAPTURED_ZOOM,
        MOUSE_CAPTURED_PAN,
        MOUSE_CAPTURED_RESIZE_GRAPH
    };
    struct
    {
        // Our graph row handling and info
        GraphRows rows;

        // Mouse timestamp location in graph
        int64_t ts_marker_mouse = -1;

        // Marker A and B
        int64_t ts_markers[ 2 ] = { INT64_MAX, INT64_MAX };
        char marker_bufs[ 2 ][ 32 ] = { { 0 } };
        char marker_delta_buf[ 32 ] = { 0 };

        bool popupmenu = false;
        bool has_focus = false;

        std::string zoom_row_name;
        std::string mouse_over_row_name;
        std::string mouse_over_row_filter_expr;
        loc_type_t mouse_over_row_type;

        std::vector< GraphRows::graph_rows_info_t > hidden_rows;

        // Graph start and length
        int64_t start_ts = 0;
        int64_t length_ts = INT64_MAX;
        bool recalc_timebufs = false;
        char time_start_buf[ 32 ] = { 0 };
        char time_end_buf[ 32 ] = { 0 };
        char time_length_buf[ 32 ] = { 0 };

        // Name of row to make visible in graph
        const char *show_row_name = nullptr;

        // Graph vertical panning
        float start_y = 0.0f;

        // Graph hovered event
        uint32_t last_hovered_eventid = INVALID_ID;

        std::vector< std::pair< int64_t, int64_t > > saved_locs;
        std::pair< int64_t, int64_t > zoom_loc = { INT64_MAX, INT64_MAX };

        ImVec2 mouse_capture_pos;
        ImVec2 mouse_capture_last;
        mouse_captured_t mouse_captured = MOUSE_NOT_CAPTURED;

        const int64_t s_min_length = 100;
        const int64_t s_max_length = 7500 * NSECS_PER_MSEC;

        float scroll_pos = -1.0f;
        float scroll_x = -1.0f;
    } m_graph;

    // Pinned graph tooltip windows
    struct
    {
        TipWindows tipwins;

        std::string str;
        bool visible = false;
    } m_ttip;

    friend class graph_info_t;
};

// MainApp ini options
typedef uint32_t option_id_t;
// Preset ini options
enum : uint32_t
{
    OPT_Invalid = ( uint32_t )-1,
    OPT_TimelineLabels = 0,
    OPT_TimelineEvents,
    OPT_TimelineRenderUserSpace,
    OPT_PrintTimelineLabels,
    OPT_PrintRenderPrefixes,
    OPT_GraphOnlyFiltered,
    OPT_Graph_HideEmptyFilteredRows,
    OPT_ShowEventList,
    OPT_SyncEventListToGraph,
    OPT_HideSchedSwitchEvents,
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
    OPT_RenderFrameMarkers,
    OPT_GraphHeight,
    OPT_GraphHeightZoomed,
    OPT_EventListRowCount,
    OPT_Scale,
    OPT_UseFreetype,
    OPT_TrimTrace,
    OPT_ShowFps,
    OPT_VerticalSync,
    OPT_PresetMax
};

struct option_t
{
    uint32_t flags;
    std::string desc;
    std::string inikey;
    std::string inisection;

    float valf;
    float valf_min = 0.0f;
    float valf_max = 1.0f;

    action_t action = action_nil;
};

class Opts
{
public:
    Opts() {}
    ~Opts() {}

    void init();
    void shutdown();

    int geti( option_id_t optid );
    bool getb( option_id_t optid );
    float getf( option_id_t optid );
    bool getcrtc( int crtc );

    void setb( option_id_t optid, bool valb );
    void setf( option_id_t optid, float valf, float valf_min = FLT_MAX, float valf_max = FLT_MAX );

    void setdesc( option_id_t optid, const std::string &desc );

    bool render_imgui_opt( option_id_t optid, float w = 200.0f );
    void render_imgui_options();

    option_id_t add_opt_graph_rowsize( const char *row_name, int defval = 4, int minval = 4 );
    option_id_t get_opt_graph_rowsize_id( const std::string &row_name );

    void set_crtc_max( int crtc_max ) { m_crtc_max = crtc_max; }

    static const uint32_t MAX_ROW_SIZE = 128;

private:
    typedef uint32_t OPT_Flags;
    enum : uint32_t
    {
        OPT_Float = 0x0000,
        OPT_Bool = 0x0001,
        OPT_Int = 0x0002,
        OPT_Hidden = 0x004
    };
    void init_opt_bool( option_id_t optid, const char *description, const char *key,
                        bool defval, OPT_Flags flags = 0 );
    void init_opt( option_id_t optid, const char *description, const char *key,
                   float defval, float minval, float maxval, OPT_Flags flags );

private:
    int m_crtc_max = -1;
    std::vector< option_t > m_options;

    // Map row names to option IDs to store graph row sizes. Ie, "gfx", "print", "sdma0", etc.
    util_umap< std::string, option_id_t > m_graph_rowname_optid_map;
};

class row_pos_t
{
public:
    row_pos_t() {}
    ~row_pos_t() {}

    // Given a start and end time, return a row index with an open spot
    uint32_t get_row( int64_t min_ts, int64_t max_ts );

public:
    // Count of total rows used
    uint32_t m_rows = 0;
    // Map of min_ts -> max_ts blocks used for for each row
    std::array< std::map< int64_t, int64_t >, Opts::MAX_ROW_SIZE > m_row_pos = {};
};

class MainApp
{
public:
    MainApp() {}
    ~MainApp() {}

    SDL_Window *create_window( const char *title );

    void init( int argc, char **argv );
    void shutdown( SDL_Window *window );

    bool load_file( const char *filename );
    void cancel_load_file();

    // Trace file loaded and viewing?
    bool is_trace_loaded();

    void render();
    void render_log();
    void render_console();
    void render_save_filename();
    void render_menu( const char *str_id );
    void render_menu_options();
    void render_font_options();
    void render_color_picker();

    void update();
    void load_fonts();

    void parse_cmdline( int argc, char **argv );

    void handle_hotkeys();

    void get_window_pos( int &x, int &y, int &w, int &h );
    void save_window_pos( int x, int y, int w, int h );

    void open_trace_dialog();

    enum state_t
    {
        State_Idle,
        State_Loading,
        State_CancelLoading
    };
    state_t get_state();
    void set_state( state_t state, const char *filename = nullptr );

    static int SDLCALL thread_func( void *data );

public:
    struct loading_info_t
    {
        // State_Idle, Loading, Loaded, CancelLoading
        SDL_atomic_t state = { 0 };

        std::string filename;
        TraceWin *win = nullptr;
        SDL_Thread *thread = nullptr;
        std::vector< std::string > inputfiles;
    };
    loading_info_t m_loading_info;

    struct save_info_t
    {
        char filename_buf[ PATH_MAX ] = { 0 };

        std::string title;
        std::string filename_new;
        std::string filename_orig;
        std::string errstr;

        std::function< bool ( save_info_t &save_info ) > save_cb;
    };
    save_info_t m_saving_info;

    TraceWin *m_trace_win = nullptr;

    FontInfo m_font_main;
    FontInfo m_font_small;

    ImGuiTextFilter m_filter;
    size_t m_log_size = ( size_t )-1;
    std::vector< std::string > m_log;

    std::string m_colorpicker_event;
    colors_t m_colorpicker_color = 0;
    ColorPicker m_colorpicker;

    ImageBuf m_imagebuf;

    bool m_quit = false;
    bool m_focus_gpuvis_console = false;
    bool m_show_gpuvis_console = false;
    bool m_show_imgui_test_window = false;
    bool m_show_imgui_style_editor = false;
    bool m_show_imgui_metrics_editor = false;
    bool m_show_font_window = false;
    bool m_show_color_picker = false;
    bool m_show_scale_popup = false;
    bool m_show_help = false;

    std::string m_show_trace_info;
};
