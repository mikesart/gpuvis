/*
 * Copyright 2019 Valve Software
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

#include <array>
#include <vector>
#include <algorithm>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <string>

#include <SDL.h>

#include "imgui/imgui.h"

#include "gpuvis_macros.h"
#include "stlini.h"
#include "trace-cmd/trace-read.h"
#include "gpuvis_utils.h"
#include "gpuvis.h"

// Add code to show CPU graphs with sched_switch events shown...

/*
  - add code to show both events when there is a really short duration?

  - For the sched_switch row, it works great, but it doesn't seem to link
  the switch pairs together (both on hover and visually) the same way that
  sched_switch events are shown in other rows, so regaining that custom
  visualization would be good so that you could see where the CPU went
  without having to find the second event. It would be cool if
  context-switching was visualized like the gpu HW row, where different
  tasks are color-coded with a unique color.
    So I guess a special "CPU HW" row that showed you a timeline for every
  core with the same color codes as GPU HW would be what I'm talking
  about. This way having both CPU and GPU hw rows at the top would give
  you a quick at-a-glance view of the whole system.
 */

class event_renderer_t
{
public:
    event_renderer_t( graph_info_t &gi, float y_in, float w_in, float h_in );

    void add_event( uint32_t eventid, float x, ImU32 color );
    void done();

    void draw_event_markers();

    void set_y( float y_in, float h_in );

    bool is_event_filtered( const trace_event_t &event );

protected:
    void start( float x, ImU32 color );
    void draw();

public:
    uint32_t m_count;
    ImU32 m_event_color;
    float m_x0, m_x1;
    float m_y, m_w, m_h;

    uint32_t m_num_events = 0;

    float m_width = 1.0f;
    float m_maxwidth = imgui_scale( 4.0f );

    struct markers_t
    {
        ImVec2 pos;
        ImU32 color;
    };
    std::vector< markers_t > m_markers;

    row_filter_t *m_row_filters = nullptr;
    std::unordered_set< int > *m_cpu_timeline_pids = nullptr;

    graph_info_t &m_gi;
};

typedef std::function< uint32_t ( graph_info_t &gi ) > RenderGraphRowCallback;

struct row_info_t
{
    uint32_t id;

    loc_type_t row_type;
    std::string row_name;
    std::string row_filter_expr;
    const std::vector< uint32_t > *plocs;

    float scale_ts = 1.0f;

    uint32_t num_events = 0;
    float minval = FLT_MAX;
    float maxval = FLT_MIN;

    float row_y;
    float row_h;

    // Only set for LOC_TYPE_Comm rows
    int pid = -1;
    const tgid_info_t *tgid_info = NULL;

    RenderGraphRowCallback render_cb = nullptr;
};

class graph_info_t
{
public:
    graph_info_t( TraceWin &winin ) : win( winin ) {}
    ~graph_info_t() {}

    void init_rows( const std::vector< GraphRows::graph_rows_info_t > &graph_rows );

    void init();
    void set_ts( int64_t start_ts, int64_t length_ts );
    void set_pos_y( float y, float h, row_info_t *ri );

    float ts_to_x( int64_t ts );
    float ts_to_dx( int64_t ts );
    float ts_to_screenx( int64_t ts );

    int64_t screenx_to_ts( float x_in );
    int64_t dx_to_ts( float x_in );

    bool mouse_pos_in_rect( const rect_t &rc );

    row_info_t *find_row( const char *name );

    bool add_mouse_hovered_event( float x, const trace_event_t &event, bool force = false );

    void set_selected_i915_ringctxseq( const trace_event_t &event );
    bool is_i915_ringctxseq_selected( const trace_event_t &event );

    RenderGraphRowCallback get_render_cb( loc_type_t row_type );

    void calc_process_graph_height();

public:
    rect_t rc;           // current drawing rect
    rect_t rcwin;        // window dimensions

    float start_y;

    int64_t ts0;         // visible start time of graph
    int64_t ts1;         // visible end time of graph
    int64_t tsdx;        // ts1 - ts0
    double tsdxrcp;      // 1 / tsdx

    uint32_t eventstart; // visible start event of graph
    uint32_t eventend;   // visible end event of graph

    bool mouse_over;
    ImVec2 mouse_pos;

    // Time of mouse pos if mouse is over a scaled graph row
    int64_t mouse_pos_scaled_ts = INT64_MIN;

    struct hovered_t
    {
        bool neg;
        int64_t dist_ts;
        uint32_t eventid;
    };
    const size_t hovered_max = 10;
    std::vector< hovered_t > hovered_items;

    // Selected i915 ring/seq/ctx info
    struct
    {
        uint32_t selected_ringno = 0;
        uint32_t selected_seqno = 0;
        uint32_t selected_ctx = 0;
    } i915;

    std::vector< uint32_t > sched_switch_bars;

    // Id of hovered / selected fence signaled event
    uint32_t hovered_fence_signaled = INVALID_ID;

    int hovered_framemarker_frame = -1;

    bool timeline_render_user;
    bool graph_only_filtered;

    std::vector< row_info_t > row_info;
    row_info_t *prinfo_cur = nullptr;
    row_info_t *prinfo_zoom = nullptr;
    row_info_t *prinfo_zoom_hw = nullptr;

    float text_h; // ImGui::GetTextLineHeightWithSpacing() for small font
    float visible_graph_height;
    float total_graph_height;

    // row_info id we need to show in visible area
    size_t show_row_id = ( size_t )-1;

    const char *clr_bright = nullptr;
    const char *clr_brightcomp = nullptr;
    const char *clr_def = nullptr;

    uint32_t selected_eventid = INVALID_ID;
    uint32_t hovered_eventid = INVALID_ID;

    TraceWin &win;
};

static bool imgui_is_rect_clipped( const rect_t &rc )
{
    ImDrawList *DrawList = ImGui::GetWindowDrawList();
    const ImVec4 &cr = DrawList->_ClipRectStack.back();

    if ( ( rc.x > cr.z ) || ( rc.x + rc.w < cr.x ) )
        return true;
    if ( ( rc.y > cr.w ) || ( rc.y + rc.h < cr.y ) )
        return true;

    return false;
}

static void imgui_push_cliprect( const rect_t &rc )
{
    ImGui::PushClipRect( ImVec2( rc.x, rc.y ), ImVec2( rc.x + rc.w, rc.y + rc.h ), true );
}

static void imgui_pop_cliprect()
{
    ImGui::PopClipRect();
}

static void imgui_drawrect_filled( float x, float y, float w, float h, ImU32 color )
{
    ImDrawList *DrawList = ImGui::GetWindowDrawList();

    if ( w < 0.0f )
    {
        x += w;
        w = -w;
    }

    if ( !imgui_is_rect_clipped( { x, y, w, h } ) )
    {
        if ( w <= 1.0f )
            DrawList->AddLine( ImVec2( x, y - 0.5f ), ImVec2( x, y + h - 0.5f ), color );
        else
            DrawList->AddRectFilled( ImVec2( x, y ), ImVec2( x + w, y + h ), color );
    }
}

static void imgui_drawrect_filled( const rect_t &rect, ImU32 color )
{
    imgui_drawrect_filled( rect.x, rect.y, rect.w, rect.h, color );
}

static void imgui_drawrect( float x, float y, float w, float h, ImU32 color )
{
    if ( !imgui_is_rect_clipped( { x, y, w, h } ) )
    {
        ImVec2 a, b;
        ImDrawList *DrawList = ImGui::GetWindowDrawList();
        const ImVec4 &cr = DrawList->_ClipRectStack.back();

        // Clip on x axis as imgui is not drawing selection rects when
        // a.x is a large negative number and b.x is a large positive number.
        a.x = std::max< float >( x, cr.x - 1.0f );
        a.y = y;
        b.x = std::min< float >( x + w, cr.z + 1.0f );
        b.y = y + h;

        DrawList->AddRect( a, b, color );
    }
}

static void imgui_drawrect( const rect_t &rect, ImU32 color )
{
    imgui_drawrect( rect.x, rect.y, rect.w, rect.h, color );
}

static void imgui_draw_text( float x, float y, ImU32 color, const char *text, bool background = false )
{
    ImDrawList *DrawList = ImGui::GetWindowDrawList();

    if ( background )
    {
        ImVec2 textsize = ImGui::CalcTextSize( text );

        imgui_drawrect_filled( x - 1, y - 1,
                               textsize.x + 2, textsize.y + 2,
                               s_clrs().get( col_Graph_RowLabelTextBk ) );
    }

    DrawList->AddText( ImVec2( x, y ), color, text );
}

static void imgui_draw_textf( float x, float y, ImU32 color, const char *fmt, ... ) ATTRIBUTE_PRINTF( 4, 0 );
static void imgui_draw_textf( float x, float y, ImU32 color, const char *fmt, ... )
{
    va_list ap;
    char buf[ 512 ];

    va_start( ap, fmt );
    vsnprintf_safe( buf, fmt, ap );
    va_end( ap );

    imgui_draw_text( x, y, color, buf );
}

const char *get_event_field_val( const trace_event_t &event, const char *name, const char *defval )
{
    for ( uint32_t i = 0; i < event.numfields; i++ )
    {
        const event_field_t &field = event.fields[ i ];

        if ( !strcmp( field.key, name ) )
            return field.value;
    }

    return defval;
}

event_field_t *get_event_field( trace_event_t &event, const char *name )
{
    for ( uint32_t i = 0; i < event.numfields; i++ )
    {
        event_field_t &field = event.fields[ i ];

        if ( !strcmp( field.key, name ) )
            return &field;
    }

    return NULL;
}

/*
 * event_renderer_t
 */
event_renderer_t::event_renderer_t( graph_info_t &gi, float y_in, float w_in, float h_in ) : m_gi( gi )
{
    // Calculate how many pixels .0001ms takes
    const float dx = ( .0001f * NSECS_PER_MSEC ) * gi.rc.w * gi.tsdxrcp;

    // Scale width of drawn event from 0..4 when .0001ms takes .1 - 1.5 pixels
    const float minx = 0.1f;
    const float maxx = 1.5f;

    m_width = std::max< float >( 1.0f, m_maxwidth * ( dx - minx ) / ( maxx - minx ) );

    m_y = y_in;
    m_w = w_in;
    m_h = h_in;

    start( -1.0f, 0 );

    if ( gi.win.m_row_filters_enabled )
    {
        uint32_t hashval = hashstr32( gi.prinfo_cur->row_name );

        m_row_filters = gi.win.m_graph_row_filters.get_val( hashval );
        if ( m_row_filters && m_row_filters->filters.empty() )
            m_row_filters = NULL;
    }

    // Check if we're filtering specific pids
    if ( !gi.win.m_graph.cpu_timeline_pids.empty() )
        m_cpu_timeline_pids = &gi.win.m_graph.cpu_timeline_pids;
}

void event_renderer_t::set_y( float y_in, float h_in )
{
    if ( m_y != y_in || m_h != h_in )
    {
        done();

        m_y = y_in;
        m_h = h_in;
    }
}

void event_renderer_t::add_event( uint32_t eventid, float x, ImU32 color )
{
    m_num_events++;

    if ( ( eventid == m_gi.selected_eventid ) ||
         ( eventid == m_gi.hovered_eventid ) )
    {
        colors_t colidx = ( eventid == m_gi.selected_eventid ) ?
                    col_Graph_SelEvent : col_Graph_HovEvent;

        float width = std::min< float >( m_width, m_maxwidth );

        m_markers.push_back( { ImVec2( x + width / 2, m_y + m_h / 2.0f ),
                               s_clrs().get( colidx ) } );
    }

    if ( m_x0 < 0.0f )
    {
        // First event
        start( x, color );
    }
    else if ( ( x - m_x1 > 1.0f ) || ( m_event_color != color ) )
    {
        // New event is away from current group or new color
        draw();

        // Start a new group
        start( x, color );
    }
    else
    {
        // New event real close to last event with same color
        m_x1 = x;
        m_count++;
    }
}

void event_renderer_t::done()
{
    if ( m_x0 != -1.0f )
    {
        draw();
        start( -1.0f, 0 );
    }
}

void event_renderer_t::draw_event_markers()
{
    ImDrawList *DrawList = ImGui::GetWindowDrawList();

    for ( const markers_t &marker : m_markers )
    {
        DrawList->AddCircleFilled( marker.pos, imgui_scale( 5.0f ), marker.color );
    }
}

void event_renderer_t::start( float x, ImU32 color )
{
    m_count = 0;
    m_event_color = color;

    m_x0 = x;
    m_x1 = x + .0001f;
}

void event_renderer_t::draw()
{
    uint32_t index = std::min< uint32_t >( col_Graph_1Event + m_count, col_Graph_6Event );
    ImU32 color = m_event_color ? m_event_color : s_clrs().get( index );
    float min_width = std::min< float >( m_count + m_width, m_maxwidth );
    float width = std::max< float >( m_x1 - m_x0, min_width );

    imgui_drawrect_filled( m_x0, m_y, width, m_h, color );
}

bool event_renderer_t::is_event_filtered( const trace_event_t &event )
{
    bool filtered = false;

    if ( m_cpu_timeline_pids &&
         ( m_cpu_timeline_pids->find( event.pid ) == m_cpu_timeline_pids->end() ) )
    {
        // Check for globally filtered pids first...
        filtered = true;
    }
    else if ( m_row_filters && m_row_filters->bitvec )
    {
        uint32_t event_id = event.id;

        if ( event_id >= m_row_filters->bitvec->size() )
            filtered = true;
        else
            filtered = !m_row_filters->bitvec->get( event_id );
    }

    return filtered;
}

static option_id_t get_comm_option_id( const std::string &row_name, loc_type_t row_type )
{
    option_id_t optid = s_opts().get_opt_graph_rowsize_id( row_name );

    if ( optid != OPT_Invalid )
        return optid;

    if ( row_type == LOC_TYPE_Print ||
         row_type == LOC_TYPE_CpuGraph ||
         row_type == LOC_TYPE_Plot ||
         row_type == LOC_TYPE_AMDTimeline ||
         row_type == LOC_TYPE_i915RequestWait ||
         row_type == LOC_TYPE_i915Request )
    {
        int defval = 4;
        int minval = 4;

        if ( row_type == LOC_TYPE_CpuGraph )
        {
            defval = 6;
        }
        else if ( row_type == LOC_TYPE_Print )
        {
            defval = 10;
        }
        else if ( row_type == LOC_TYPE_i915Request )
        {
            defval = 8;
        }
        else if ( row_type == LOC_TYPE_i915RequestWait )
        {
            defval = 2;
            minval = 2;
        }

        return s_opts().add_opt_graph_rowsize( row_name.c_str(), defval, minval );
    }

    return OPT_Invalid;
}

/*
 * graph_info_t
 */
RenderGraphRowCallback graph_info_t::get_render_cb( loc_type_t row_type )
{
    switch ( row_type )
    {
    case LOC_TYPE_CpuGraph:        return std::bind( &TraceWin::graph_render_cpus_timeline, &win, _1 );
    case LOC_TYPE_Print:           return std::bind( &TraceWin::graph_render_print_timeline, &win, _1 );
    case LOC_TYPE_Plot:            return std::bind( &TraceWin::graph_render_plot, &win, _1 );
    case LOC_TYPE_AMDTimeline:     return std::bind( &TraceWin::graph_render_amd_timeline, &win, _1 );
    case LOC_TYPE_AMDTimeline_hw:  return std::bind( &TraceWin::graph_render_amdhw_timeline, &win, _1 );
    case LOC_TYPE_i915Request:     return std::bind( &TraceWin::graph_render_i915_req_events, &win, _1 );
    case LOC_TYPE_i915RequestWait: return std::bind( &TraceWin::graph_render_i915_reqwait_events, &win, _1 );
    // LOC_TYPE_Comm or LOC_TYPE_Tdopexpr hopefully...
    default:                       return std::bind( &TraceWin::graph_render_row_events, &win, _1 );
    }
}

void graph_info_t::init_rows( const std::vector< GraphRows::graph_rows_info_t > &graph_rows )
{
    GPUVIS_TRACE_BLOCK( __func__ );

    uint32_t id = 0;

    imgui_push_smallfont();

    float graph_row_padding = ImGui::GetStyle().FramePadding.y;

    text_h = ImGui::GetTextLineHeightWithSpacing();

    total_graph_height = graph_row_padding;

    imgui_pop_font();

    for ( const GraphRows::graph_rows_info_t &grow : graph_rows )
    {
        row_info_t rinfo;
        const std::vector< uint32_t > *plocs;
        const std::string &row_name = grow.row_name;

        if ( grow.hidden )
            continue;

        plocs = win.m_trace_events.get_locs( grow.row_filter_expr.c_str(), &rinfo.row_type );

        rinfo.row_y = total_graph_height;
        rinfo.row_h = text_h * 2;
        rinfo.row_name = row_name;
        rinfo.row_filter_expr = grow.row_filter_expr;
        rinfo.scale_ts = win.m_graph.rows.get_row_scale_ts( row_name );

        if ( plocs )
            rinfo.render_cb = get_render_cb( rinfo.row_type );

        if ( rinfo.row_type == LOC_TYPE_Comm )
        {
            const char *pidstr = strrchr( row_name.c_str(), '-' );

            if ( pidstr )
            {
                rinfo.pid = atoi( pidstr + 1 );
                rinfo.tgid_info = win.m_trace_events.tgid_from_pid( rinfo.pid );
            }

            if ( win.m_graph.show_row_name && ( row_name == win.m_graph.show_row_name ) )
            {
                show_row_id = id;
                win.m_graph.show_row_name = NULL;
            }

            // If we're graphing only filtered events, check if this comm has any events
            if ( s_opts().getb( OPT_GraphOnlyFiltered ) &&
                 s_opts().getb( OPT_Graph_HideEmptyFilteredRows ) &&
                 !win.m_filter.events.empty() )
            {
                // Get count of !filtered events for this pid
                uint32_t *count = win.m_filter.pid_eventcount.get_val( rinfo.pid );

                // Bail if no events
                if ( !count )
                    continue;
            }
        }

        option_id_t optid = get_comm_option_id( rinfo.row_name, rinfo.row_type );
        if ( optid != OPT_Invalid )
        {
            int rows = s_opts().geti( optid );

            rinfo.row_h = Clamp< int >( rows, 2, s_opts().MAX_ROW_SIZE ) * text_h;
        }

        rinfo.id = id++;
        rinfo.plocs = plocs;
        row_info.push_back( rinfo );

        total_graph_height += rinfo.row_h + graph_row_padding;
    }

    if ( !win.m_graph.zoom_row_name.empty() )
    {
        const std::string &row_name = win.m_graph.zoom_row_name;

        prinfo_zoom = find_row( row_name.c_str() );
        if ( prinfo_zoom )
        {
            prinfo_zoom_hw = find_row( ( row_name + " hw" ).c_str() );

            if ( !prinfo_zoom_hw && !strncmp( row_name.c_str(), "i915_req ", 9 ) )
            {
                char buf[ 128 ];

                // We are zooming i915_req row, show the i915_reqwait row as well
                snprintf_safe( buf, "i915_reqwait %s", row_name.c_str() + 9 );
                prinfo_zoom_hw = find_row( buf );
            }
        }
    }

    total_graph_height += imgui_scale( 2.0f );
    total_graph_height = std::max< float >( total_graph_height, 8.0f * text_h );

    // Figure out visible_graph_height
    calc_process_graph_height();

    win.m_graph.show_row_name = NULL;
}

void graph_info_t::calc_process_graph_height()
{
    // Zoom mode if we have a gfx row and zoom option is set
    option_id_t optid;
    float max_graph_size;
    const float valf_min = 8.0f * text_h;

    // Check if user hit F11 and only the graph is showing (no event list).
    if ( !s_opts().getb( OPT_ShowEventList ) )
    {
        // If we have a zoomed row, use up all the available window space,
        // otherwise just use the total graph height
        float valf = prinfo_zoom ?
                    ImGui::GetContentRegionAvail().y : total_graph_height;

        visible_graph_height = Clamp< float >( valf, valf_min, ImGui::GetContentRegionAvail().y - ImGui::GetStyle().ScrollbarSize );
        return;
    }

    if ( prinfo_zoom )
    {
        optid = OPT_GraphHeightZoomed;
        max_graph_size = ImGui::GetWindowHeight() * 10.0f;
    }
    else
    {
        optid = OPT_GraphHeight;
        max_graph_size = total_graph_height;
    }

    // Set up min / max sizes and clamp value in that range
    float valf = s_opts().getf( optid );

    // First time initialization - start with 1/2 the screen
    if ( !valf )
        valf = ImGui::GetContentRegionAvail().y / 2.0f;

    valf = Clamp< float >( valf, valf_min, max_graph_size );
    s_opts().setf( optid, valf, valf_min, max_graph_size );

    visible_graph_height = valf;
}

void graph_info_t::set_ts( int64_t start_ts, int64_t length_ts )
{
    ts0 = start_ts;
    ts1 = ts0 + length_ts;

    eventstart = win.ts_to_eventid( ts0 );
    eventend = win.ts_to_eventid( ts1 );

    tsdx = ts1 - ts0 + 1;
    tsdxrcp = 1.0 / tsdx;
}

void graph_info_t::init()
{
    const ImVec2 windowpos = ImGui::GetCursorScreenPos();
    const ImVec2 windowsize = ImGui::GetContentRegionAvail();
    const std::vector< trace_event_t > &events = win.m_trace_events.m_events;

    rcwin = { windowpos.x, windowpos.y, windowsize.x, windowsize.y };

    rc.x = windowpos.x;
    rc.w = windowsize.x;

    // Set whether graph has focus
    win.m_graph.has_focus = ImGui::IsWindowFocused();

    // If we don't have a popup menu, clear the mouse over row name
    if ( !win.m_graph.popupmenu )
    {
        win.m_graph.mouse_over_row_name.clear();
        win.m_graph.mouse_over_row_filter_expr.clear();
        win.m_graph.mouse_over_row_type = LOC_TYPE_Max;
    }

    clr_bright = s_textclrs().str( TClr_Bright );
    clr_brightcomp = s_textclrs().str( TClr_BrightComp );
    clr_def = s_textclrs().str( TClr_Def );

    // Get mouse position
    mouse_pos = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) ?
                ImGui::GetMousePos() : ImVec2( -FLT_MAX, -FLT_MAX );

    // Render timeline user space bars?
    timeline_render_user = s_opts().getb( OPT_TimelineRenderUserSpace );

    // Render filtered events only?
    graph_only_filtered = s_opts().getb( OPT_GraphOnlyFiltered ) &&
            !win.m_filter.events.empty();

    // Grab last hovered graph event
    hovered_eventid = win.m_graph.last_hovered_eventid;

    // If the event list is visible, grab the selected event
    if ( s_opts().getb( OPT_ShowEventList ) )
        selected_eventid = win.m_eventlist.selected_eventid;

    // If our hovered event is an amd timeline event, get the id
    if ( is_valid_id( hovered_eventid ) && events[ hovered_eventid ].is_timeline() )
    {
        // Find the fence signaled event for this timeline
        uint32_t gfxcontext_hash = win.m_trace_events.get_event_gfxcontext_hash( events[ hovered_eventid ] );
        const std::vector< uint32_t > *plocs = win.m_trace_events.get_gfxcontext_locs( gfxcontext_hash );

        // Mark it as hovered so it'll have a selection rectangle
        hovered_fence_signaled = plocs->back();
    }

    // Set start_y
    start_y = win.m_graph.start_y;

    // If we have a show row id, make sure it's visible
    if ( show_row_id != ( size_t )-1 )
    {
        const row_info_t &rinfo = row_info[ show_row_id ];

        if ( ( rinfo.row_y < -start_y ) ||
             ( rinfo.row_y + rinfo.row_h > visible_graph_height - start_y ) )
        {
            start_y = -rinfo.row_y + visible_graph_height / 3;
        }
    }

    // Range check mouse pan values
    start_y = Clamp< float >( start_y, visible_graph_height - total_graph_height, 0.0f );
    win.m_graph.start_y = start_y;
}

void graph_info_t::set_pos_y( float y_in, float h_in, row_info_t *ri )
{
    rc.y = y_in;
    rc.h = h_in;

    prinfo_cur = ri;

    mouse_over = rc.point_in_rect( mouse_pos );
}

float graph_info_t::ts_to_x( int64_t ts )
{
    return rc.w * ( ts - ts0 ) * tsdxrcp;
}

float graph_info_t::ts_to_dx( int64_t ts )
{
    return ts * rc.w * tsdxrcp;
}

float graph_info_t::ts_to_screenx( int64_t ts )
{
    return rc.x + ts_to_x( ts );
}

int64_t graph_info_t::screenx_to_ts( float x_in )
{
    double val = ( x_in - rc.x ) / rc.w;

    return ts0 + val * tsdx;
}
int64_t graph_info_t::dx_to_ts( float x_in )
{
    return ( x_in / rc.w ) * tsdx;
}

bool graph_info_t::mouse_pos_in_rect( const rect_t &rcin )
{
    return rcin.point_in_rect( mouse_pos );
}

row_info_t *graph_info_t::find_row( const char *name )
{
    if ( name && name[ 0 ] )
    {
        for ( row_info_t &ri : row_info )
        {
            if ( ri.row_name == name )
                return &ri;
        }
    }
    return NULL;
}

bool graph_info_t::add_mouse_hovered_event( float xin, const trace_event_t &event, bool force )
{
    bool inserted = false;
    float xdist_mouse = xin - mouse_pos.x;
    bool neg = xdist_mouse < 0.0f;

    // Check if we've already added this event
    for ( const auto &it : hovered_items )
    {
        if ( it.eventid == event.id )
            return true;
    }

    if ( neg )
        xdist_mouse = -xdist_mouse;

    if ( ( xdist_mouse < imgui_scale( 8.0f ) ) || force )
    {
        int64_t dist_ts = dx_to_ts( xdist_mouse );

        for ( auto it = hovered_items.begin(); it != hovered_items.end(); it++ )
        {
            if ( dist_ts < it->dist_ts )
            {
                hovered_items.insert( it, { neg, dist_ts, event.id } );
                inserted = true;
                break;
            }
        }

        if ( !inserted && ( hovered_items.size() < hovered_max ) )
        {
            hovered_items.push_back( { neg, dist_ts, event.id } );
            inserted = true;
        }
        else if ( hovered_items.size() > hovered_max )
        {
            hovered_items.pop_back();
        }
    }

    return inserted;
}

void graph_info_t::set_selected_i915_ringctxseq( const trace_event_t &event )
{
    if ( !i915.selected_seqno )
    {
        const char *ctxstr = get_event_field_val( event, "ctx", "0" );
        uint32_t ringno = TraceLocationsRingCtxSeq::get_i915_ringno( event );

        i915.selected_seqno = event.seqno;
        i915.selected_ringno = ringno;
        i915.selected_ctx = strtoul( ctxstr, NULL, 10 );
    }
}

bool graph_info_t::is_i915_ringctxseq_selected( const trace_event_t &event )
{
    if ( i915.selected_seqno == event.seqno )
    {
        const char *ctxstr = get_event_field_val( event, "ctx", "0" );
        uint32_t ctx = strtoul( ctxstr, NULL, 10 );
        uint32_t ringno = TraceLocationsRingCtxSeq::get_i915_ringno( event );

        return ( ( i915.selected_ringno == ringno ) && ( i915.selected_ctx == ctx ) );
    }

    return false;
}

void CreateGraphRowDlg::init()
{
    std::vector< INIEntry > entries = s_ini().GetSectionEntries( "$graphrow_filters$" );

    for ( const INIEntry &entry : entries )
        m_previous_filters.push_back( entry.second );

    if ( m_previous_filters.empty() )
    {
        // Add a default filter
        m_previous_filters.push_back( "$name = drm_vblank_event && $crtc = 0" );
        m_previous_filters.push_back( "$name = drm_vblank_event && $crtc = 1" );
    }
}

void CreateGraphRowDlg::shutdown()
{
    for ( size_t i = 0; i < m_previous_filters.size(); i++ )
    {
        char key[ 32 ];
        const std::string &value = m_previous_filters[ i ];

        snprintf_safe( key, "%02lu", i );

        s_ini().PutStr( key, value.c_str(), "$graphrow_filters$" );
    }
}

bool CreateGraphRowDlg::show_dlg( TraceEvents &trace_events, uint32_t eventid )
{
    if ( is_valid_id( eventid ) && ( eventid < trace_events.m_events.size() ) )
    {
        const trace_event_t &event = trace_events.m_events[ eventid ];

        snprintf_safe( m_name_buf, "%s", event.comm );
        snprintf_safe( m_filter_buf, "$comm = \"%s\"", event.comm );
    }
    else
    {
        strcpy_safe( m_name_buf, "<New Graph Row Name>" );
        strcpy_safe( m_filter_buf, m_previous_filters[ 0 ].c_str() );
    }

    ImGui::OpenPopup( "Add New Graph Row" );
    return false;
}

bool CreateGraphRowDlg::render_dlg( TraceEvents &trace_events )
{
    if ( !ImGui::BeginPopupModal( "Add New Graph Row", NULL, ImGuiWindowFlags_AlwaysAutoResize ) )
        return false;

    bool ret = false;
    float w = imgui_scale( 350.0f );
    const char row_name[] = "Row Name:  ";
    const char row_filter[] = "Row Filter:  ";
    const ImVec2 button_size = { imgui_scale( 120.0f ), 0.0f };
    const ImVec2 text_size = ImGui::CalcTextSize( row_filter );
    float x = ImGui::GetCursorPos().x + text_size.x;

    imgui_input_text( row_name, m_name_buf, x, w );

    if ( ImGui::IsWindowAppearing() )
        ImGui::SetKeyboardFocusHere( -1 );

    imgui_input_text( row_filter, m_filter_buf, x, w );
    if ( ImGui::IsItemHovered() )
    {
        std::string tooltip;

        tooltip += s_textclrs().bright_str( "Add a new row with filtered events\n\n" );

        tooltip += "Examples:\n";
        tooltip += "  $pid = 4615\n";
        tooltip += "  $duration >= 5.5\n";
        tooltip += "  $buf =~ \"[Compositor] Warp\"\n";
        tooltip += "  ( $timeline = gfx ) && ( $id < 10 || $id > 100 )\n";
        tooltip += "  gfx, gfx hw, sdma0, print, etc.";

        ImGui::SetTooltip( "%s", tooltip.c_str() );
    }

    if ( !m_err_str.empty() )
        ImGui::TextColored( ImVec4( 1, 0, 0, 1), "%s", m_err_str.c_str() );

    if ( ImGui::CollapsingHeader( "Previous Filters", ImGuiTreeNodeFlags_DefaultOpen ) )
    {
        ImGui::BeginChild( "previous_filters", ImVec2( 0.0f, imgui_scale( 150.0f ) ) );
        ImGui::Indent();

        ImGuiSelectableFlags flags = ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_DontClosePopups;

        for ( auto i : m_previous_filters )
        {
            const char *str0 = i.c_str();

            ImGui::PushID( str0 );

            if ( ImGui::Selectable( str0, false, flags ) )
                strcpy_safe( m_filter_buf, str0 );

            ImGui::PopID();
        }

        ImGui::Unindent();
        ImGui::EndChild();
    }

    bool disabled = !m_name_buf[ 0 ] || !m_filter_buf[ 0 ];

    ImGui::PushStyleColor( ImGuiCol_Text,
        ImGui::GetStyleColorVec4( disabled ? ImGuiCol_TextDisabled : ImGuiCol_Text ) );

    bool do_create = ImGui::Button( "Create", button_size ) ||
            s_actions().get( action_return );

    ImGui::PopStyleColor();

    if ( do_create && !disabled )
    {
        const std::vector< uint32_t > *plocs = trace_events.get_locs(
                    m_filter_buf, NULL, &m_err_str );

        ret = !!plocs;

        if ( ret )
        {
            // Try to find this filter pair in our previous filters array
            auto idx = std::find( m_previous_filters.begin(), m_previous_filters.end(), m_filter_buf );

            // Erase the one we found
            if ( idx != m_previous_filters.end() )
                m_previous_filters.erase( idx );

            // Insert it at the beginning
            m_previous_filters.insert( m_previous_filters.begin(), m_filter_buf );

            // Make sure we don't go over ~ 20 filters
            if ( m_previous_filters.size() > 20 )
                m_previous_filters.resize( 20 );
        }
        else if ( m_err_str.empty() )
        {
            m_err_str = "ERROR: No events found.";
        }
    }

    ImGui::SameLine();
    if ( ImGui::Button( "Cancel", button_size ) || s_actions().get( action_escape ) || ret )
        ImGui::CloseCurrentPopup();

    ImGui::EndPopup();
    return ret;
}

void CreateRowFilterDlg::init()
{
    std::vector< INIEntry > entries = s_ini().GetSectionEntries( "$graphrow_row_filters$" );

    for ( const INIEntry &entry : entries )
        m_previous_filters.push_back( entry.second );

    if ( m_previous_filters.empty() )
    {
        // Add some default filters
        m_previous_filters.push_back( "$name = drm_vblank_event" );
    }
}

void CreateRowFilterDlg::shutdown()
{
    for ( size_t i = 0; i < m_previous_filters.size(); i++ )
    {
        char key[ 32 ];
        const std::string &value = m_previous_filters[ i ];

        snprintf_safe( key, "%02lu", i );

        s_ini().PutStr( key, value.c_str(), "$graphrow_row_filters$" );
    }
}

bool CreateRowFilterDlg::show_dlg( TraceEvents &trace_events )
{
    strcpy_safe( m_filter_buf, m_previous_filters[ 0 ].c_str() );

    ImGui::OpenPopup( "Create New Row Filter" );
    return false;
}

bool CreateRowFilterDlg::render_dlg( TraceEvents &trace_events )
{
    if ( !ImGui::BeginPopupModal( "Create New Row Filter", NULL, ImGuiWindowFlags_AlwaysAutoResize ) )
        return false;

    bool ret = false;
    float w = imgui_scale( 350.0f );
    const char row_filter[] = "Row Filter:  ";
    const ImVec2 button_size = { imgui_scale( 120.0f ), 0.0f };
    const ImVec2 text_size = ImGui::CalcTextSize( row_filter );
    float x = ImGui::GetCursorPos().x + text_size.x;

    if ( ImGui::IsWindowAppearing() )
        ImGui::SetKeyboardFocusHere( -1 );

    imgui_input_text( row_filter, m_filter_buf, x, w );
    if ( ImGui::IsItemHovered() )
    {
        std::string tooltip;

        tooltip += s_textclrs().bright_str( "Row filter expression\n\n" );

        tooltip += "Examples:\n";
        tooltip += "  $pid = 4615\n";
        tooltip += "  $duration >= 5.5\n";
        tooltip += "  $buf =~ \"[Compositor] Warp\"\n";
        tooltip += "  ( $timeline = gfx ) && ( $id < 10 || $id > 100 )\n";
        tooltip += "  gfx, gfx hw, sdma0, print, etc.";

        ImGui::SetTooltip( "%s", tooltip.c_str() );
    }

    if ( !m_err_str.empty() )
        ImGui::TextColored( ImVec4( 1, 0, 0, 1), "%s", m_err_str.c_str() );

    if ( ImGui::CollapsingHeader( "Previous Filters", ImGuiTreeNodeFlags_DefaultOpen ) )
    {
        ImGui::BeginChild( "previous_filters", ImVec2( 0.0f, imgui_scale( 150.0f ) ) );
        ImGui::Indent();

        ImGuiSelectableFlags flags = ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_DontClosePopups;

        for ( auto i : m_previous_filters )
        {
            const char *str0 = i.c_str();

            ImGui::PushID( str0 );

            if ( ImGui::Selectable( str0, false, flags ) )
                strcpy_safe( m_filter_buf, str0 );

            ImGui::PopID();
        }

        ImGui::Unindent();
        ImGui::EndChild();
    }

    bool disabled = !m_filter_buf[ 0 ];

    ImGui::PushStyleColor( ImGuiCol_Text,
        ImGui::GetStyleColorVec4( disabled ? ImGuiCol_TextDisabled : ImGuiCol_Text ) );

    bool do_create = ImGui::Button( "Create", button_size ) ||
            s_actions().get( action_return );

    ImGui::PopStyleColor();

    if ( do_create && !disabled )
    {
        const std::vector< uint32_t > *plocs = trace_events.get_locs(
                    m_filter_buf, NULL, &m_err_str );

        ret = !!plocs;

        if ( ret )
        {
            // Try to find this filter pair in our previous filters array
            auto idx = std::find( m_previous_filters.begin(), m_previous_filters.end(), m_filter_buf );

            // Erase the one we found
            if ( idx != m_previous_filters.end() )
                m_previous_filters.erase( idx );

            // Insert it at the beginning
            m_previous_filters.insert( m_previous_filters.begin(), m_filter_buf );

            // Make sure we don't go over ~ 20 filters
            if ( m_previous_filters.size() > 20 )
                m_previous_filters.resize( 20 );
        }
        else if ( m_err_str.empty() )
        {
            m_err_str = "ERROR: No events found.";
        }
    }

    ImGui::SameLine();
    if ( ImGui::Button( "Cancel", button_size ) || s_actions().get( action_escape ) || ret )
        ImGui::CloseCurrentPopup();

    ImGui::EndPopup();
    return ret;
}

size_t RowFilters::find_filter( const std::string &filter )
{
    size_t idx = ( size_t )-1;

    if ( m_row_filters )
    {
        // This row has some applied filters, see if this is one of them
        auto i = std::find( m_row_filters->filters.begin(), m_row_filters->filters.end(), filter );

        if ( i != m_row_filters->filters.end() )
            idx = i - m_row_filters->filters.begin();
    }

    return idx;
}

void RowFilters::toggle_filter( TraceEvents &trace_events, size_t idx, const std::string &filter )
{
    if ( idx == ( size_t )-1 )
    {
        // New Filter
        if ( !m_row_filters )
            m_row_filters = m_graph_row_filters.get_val_create( m_rowname_hash );

        m_row_filters->filters.push_back( filter );
        std::sort( m_row_filters->filters.begin(), m_row_filters->filters.end() );
    }
    else
    {
        // Remove this filter
        m_row_filters->filters.erase( m_row_filters->filters.begin() + idx );
    }

    // Free old bitmask
    delete m_row_filters->bitvec;
    m_row_filters->bitvec = NULL;

    // Create new bitmask of valid eventids
    const std::vector< uint32_t > *plocs_smallest = NULL;
    std::vector< const std::vector< uint32_t > * > locs;

    // Go through all the filters
    for ( const std::string &filterstr : m_row_filters->filters )
    {
        // Get events for this filter
        const std::vector< uint32_t > *plocs = trace_events.get_tdopexpr_locs( filterstr.c_str() );

        if ( plocs )
        {
            if ( !plocs_smallest || ( plocs->size() < plocs_smallest->size() ) )
                plocs_smallest = plocs;

            locs.push_back( plocs );
        }
    }

    if ( plocs_smallest )
    {
        // Remove plocs_smallest from array of filter locs
        auto idx0 = std::find( locs.begin(), locs.end(), plocs_smallest );
        locs.erase( idx0 );

        for ( auto it = plocs_smallest->rbegin(); it != plocs_smallest->rend(); it++ )
        {
            uint32_t eventid = *it;
            bool set_in_all_filters = true;

            // Try to find this eventid in all the filters
            for ( const std::vector< uint32_t > *plocs : locs )
            {
                if ( !std::binary_search( plocs->begin(), plocs->end(), eventid ) )
                {
                    set_in_all_filters = false;
                    break;
                }
            }

            if ( set_in_all_filters )
            {
                if ( !m_row_filters->bitvec )
                    m_row_filters->bitvec = new BitVec( eventid + 1 );

                m_row_filters->bitvec->set( eventid );
            }
        }

        // No events found - just make a small empty bitvec
        if ( !m_row_filters->bitvec )
            m_row_filters->bitvec = new BitVec( 1 );
    }
}

uint32_t TraceWin::graph_render_plot( graph_info_t &gi )
{
    float minval = FLT_MAX;
    float maxval = FLT_MIN;
    std::vector< ImVec2 > points;
    const char *row_name = gi.prinfo_cur->row_name.c_str();
    GraphPlot &plot = m_trace_events.get_plot( row_name );
    uint32_t index0 = plot.find_ts_index( gi.ts0 );
    uint32_t index1 = plot.find_ts_index( gi.ts1 );

    if ( index1 == ( uint32_t)-1 )
        index1 = plot.m_plotdata.size();

    points.reserve( index1 - index0 + 10 );

    uint32_t idx0 = gi.prinfo_cur->plocs->front();
    ImU32 color_line = m_trace_events.m_events[ idx0 ].color ?
                m_trace_events.m_events[ idx0 ].color : 0xffffffff;
    ImU32 color_point = imgui_col_complement( color_line );

    for ( size_t idx = index0; idx < plot.m_plotdata.size(); idx++ )
    {
        GraphPlot::plotdata_t &data = plot.m_plotdata[ idx ];
        float x = gi.ts_to_screenx( data.ts );
        float y = data.valf;

        if ( x <= 0.0f )
        {
            minval = y;
            maxval = y;
        }

        points.push_back( ImVec2( x, y ) );

        minval = std::min< float >( minval, y );
        maxval = std::max< float >( maxval, y );

        // Check if we're mouse hovering this event
        if ( gi.mouse_over )
            gi.add_mouse_hovered_event( x, get_event( data.eventid ) );

        if ( x >= gi.rc.x + gi.rc.w )
            break;
    }

    if ( points.size() )
    {
        bool closed = false;
        float thickness = 2.0f;

        gi.prinfo_cur->minval = minval;
        gi.prinfo_cur->maxval = maxval;

        float pad = 0.15f * ( maxval - minval );
        if ( !pad )
            pad = 1.0f;
        minval -= pad;
        maxval += pad;

        float rcpdenom = gi.rc.h / ( maxval - minval );
        for ( ImVec2 &pt : points )
            pt.y = gi.rc.y + ( maxval - pt.y ) * rcpdenom;

        ImGui::GetWindowDrawList()->AddPolyline( points.data(), points.size(),
                                                 color_line, closed, thickness );

        for ( const ImVec2 &pt : points )
        {
            imgui_drawrect_filled( pt.x - imgui_scale( 1.5f ), pt.y - imgui_scale( 1.5f ),
                                   imgui_scale( 3.0f ), imgui_scale( 3.0f ),
                                   color_point );
        }
    }

    return points.size();
}

class row_draw_info_t
{
public:
    void set_event( graph_info_t &gi, float h,
                    float x2 = FLT_MAX, float y2 = FLT_MAX,
                    const trace_event_t *event = NULL,
                    const print_info_t *print_info = NULL );
    void render_text( graph_info_t &gi, float w, float h );

public:
    float m_x = 0.0f;
    float m_y = 0.0f;
    const trace_event_t *m_event = nullptr;
    const print_info_t *m_print_info = nullptr;
};

void row_draw_info_t::render_text( graph_info_t &gi, float w, float h )
{
    // Text size
    const ImVec2 &tsize = m_print_info->size;

    if ( m_event->has_duration() )
    {
        // Get width of duration, capped at available width
        float wduration = std::min< float >( w, gi.ts_to_dx( m_event->duration ) );

        wduration -= imgui_scale( 4.0f );

        // Text size less than duration width?
        if ( tsize.x < wduration )
        {
            // Save duration x right position
            float xr = m_x + wduration;

            // Center text in duration width
            m_x = m_x + ( wduration - tsize.x ) / 2.0f;

            if ( m_x < gi.rc.x )
            {
                // Centered text off left edge, move it right as far as possible
                m_x = std::min< float >( gi.rc.x, xr - tsize.x );
            }
        }
    }

    // Center text vertically in middle of rectangle
    m_y = m_y + ( h / 2.0f ) - ( tsize.y / 2.0f ) - imgui_scale( 2.0f );

    const char *buf = m_print_info->buf;

    if ( !s_opts().getb( OPT_PrintRenderPrefixes ) )
    {
        if ( !strncasecmp( buf, "[Compositor] ", 13 ) )
            buf += 13;
        else if ( !strncasecmp( buf, "[Compositor Client] ", 20 ) )
            buf += 20;
    }

    imgui_push_cliprect( { m_x, m_y, w, tsize.y + imgui_scale( 1.0f ) } );
    imgui_draw_text( m_x, m_y, m_event->color, buf );
    imgui_pop_cliprect();
}

void row_draw_info_t::set_event( graph_info_t &gi,
                                 float h, float x2, float y2,
                                 const trace_event_t *event,
                                 const print_info_t *print_info )
{
    // Adding a new event at x2,y2. If we had a previous event and
    //   there is room for the label, draw it.

    // Available width
    float wavail = x2 - m_x;

    if ( m_print_info && ( wavail >= imgui_scale( 16.0f ) ) )
    {
        // Right x is x2 or right edge of drawing rect
        float xright = std::min< float >( x2, gi.rc.x + gi.rc.w );
        // Available width is visible right edge to our left edge
        float w = xright - m_x;

        if ( w > 0.0f )
            render_text( gi, w, h );
    }

    m_x = x2 + imgui_scale( 3.0f );
    m_y = y2;
    m_print_info = print_info;
    m_event = event;
}

// Given an array of ftrace print event IDs, do a lower_bound binary search on
//  m_ftrace.print_info ts values.
// Note: locs is sorted by ts values in m_ftrace.print_info.
uint32_t TraceEvents::ts_to_ftrace_print_info_idx( const std::vector< uint32_t > &locs, int64_t ts )
{
    size_t first = 0;
    size_t count = locs.size();

    while ( count > 0 )
    {
        size_t step = count / 2;
        size_t it = first + step;
        const trace_event_t &event = m_events[ locs[ it ] ];
        const print_info_t *print_info = get_print_info( event.id );

        if ( print_info->ts < ts )
        {
            first = ++it;
            count -= step + 1;
        }
        else
        {
            count = step;
        }
    }

    return first;
}

static uint32_t get_graph_row_id( const trace_event_t &event,
                                  ftrace_row_info_t *ftrace_row_info,
                                  const print_info_t *print_info  )
{
    if ( ftrace_row_info->pid < 0 )
        return event.graph_row_id;

    if ( ftrace_row_info->tgid )
    {
        if ( ftrace_row_info->tgid != print_info->tgid )
            return ( uint32_t )-1;

        return print_info->graph_row_id_tgid;
    }

    if ( ftrace_row_info->pid != event.pid )
        return ( uint32_t )-1;

    return print_info->graph_row_id_pid;
}

uint32_t TraceWin::graph_render_cpus_timeline( graph_info_t &gi )
{
    GPUVIS_TRACE_BLOCK( __func__ );

    imgui_push_smallfont();

    uint32_t count = 0;

    uint32_t cpus = m_trace_events.m_trace_info.cpus;
    float row_h = floor( gi.rc.h / cpus );
    ImU32 color_text = s_clrs().get( col_Graph_BarText );
    bool sched_switch_bars_empty = gi.sched_switch_bars.empty();
    bool hide_system_events = m_graph.cpu_hide_system_events;
    bool alt_down = ImGui::GetIO().KeyAlt;

    // TASK_COMM_LEN is 16 in Linux, but try to show if there is
    // room for ~12 characters.
    const ImVec2 text_size = ImGui::CalcTextSize( "0123456789ab" );

    for ( const auto &cpu_locs : m_trace_events.m_sched_switch_cpu_locs.m_locs.m_map )
    {
        const std::vector< uint32_t > &locs = cpu_locs.second;
        uint32_t cpu = get_event( locs[ 0 ] ).cpu;
        float y = gi.rc.y + cpu * row_h;

        // Skip row if it's above or below visible window
        if ( y > gi.rcwin.y + gi.rcwin.h )
            continue;
        if ( y + row_h < gi.rcwin.y )
            continue;

        event_renderer_t event_renderer( gi, y + imgui_scale( 2.0f ), gi.rc.w, row_h - imgui_scale( 3.0f ) );

        for ( size_t idx = vec_find_eventid( locs, gi.eventstart );
              idx < locs.size();
              idx++ )
        {
            const trace_event_t &sched_switch = get_event( locs[ idx ] );
            float x0 = gi.ts_to_screenx( sched_switch.ts - sched_switch.duration );
            float x1 = gi.ts_to_screenx( sched_switch.ts );

            // Bail if we're off the right side of our graph
            if ( x0 > gi.rc.x + gi.rc.w )
                break;

            if ( hide_system_events && ( sched_switch.flags & TRACE_FLAG_SCHED_SWITCH_SYSTEM_EVENT ) )
                continue;

            if ( event_renderer.is_event_filtered( sched_switch ) )
                continue;

            count++;
            if ( ( x1 - x0 ) < imgui_scale( 3.0f ) )
            {
                event_renderer.add_event( sched_switch.id, x0, sched_switch.color );
            }
            else
            {
                bool drawrect = false;

                event_renderer.done();

                imgui_drawrect_filled( x0, y + imgui_scale( 2.0f ), x1 - x0, row_h - imgui_scale( 3.0f ), sched_switch.color );

                // If alt key isn't down and there is room for ~12 characters, render comm name
                if ( !alt_down && ( x1 - x0 > text_size.x ) )
                {
                    float y_text = y + ( row_h - text_size.y ) / 2 - imgui_scale( 1.0f );
                    const char *prev_comm = get_event_field_val( sched_switch, "prev_comm" );

                    imgui_push_cliprect( { x0, y_text, x1 - x0, text_size.y } );
                    imgui_draw_text( x0 + imgui_scale( 1.0f ), y_text, color_text, prev_comm );
                    imgui_pop_cliprect();
                }

                if ( gi.mouse_pos_in_rect( { x0, y, x1 - x0, row_h } ) )
                {
                    drawrect = true;
                    gi.sched_switch_bars.push_back( sched_switch.id );
                }
                else if ( !sched_switch_bars_empty && ( gi.sched_switch_bars[ 0 ] == sched_switch.id ) )
                {
                    drawrect = true;
                }

                if ( drawrect )
                {
                    imgui_drawrect( x0, y + imgui_scale( 1.0f ),
                                    x1 - x0, row_h - imgui_scale( 1.0f ),
                                    s_clrs().get( col_Graph_BarSelRect ) );
                }
            }
        }

        event_renderer.done();
    }

    ImU32 color = s_clrs().get( col_Graph_BarText, 0x30 );
    ImDrawList *DrawList = ImGui::GetWindowDrawList();

    // Draw border around the cpu graph rows
    DrawList->AddLine( ImVec2( gi.rc.x, gi.rc.y ), ImVec2( gi.rc.x + gi.rc.w, gi.rc.y ), color );

    for ( uint32_t i = 0; i < cpus; i++ )
    {
        char label[ 16 ];
        ImVec2 textsize;
        float x = gi.rc.x;
        float y = gi.rc.y + i * row_h;

        snprintf( label, sizeof( label ), "%d", i );
        textsize = ImGui::CalcTextSize( label );

        // Draw labels on the left and right of each row
        imgui_draw_text( x + imgui_scale( 2.0f ), y + imgui_scale( 1.0f ), color, label, true );
        imgui_draw_text( x + gi.rc.w - textsize.x - imgui_scale( 2.0f ), y + imgui_scale( 1.0f ), color, label, true );

        DrawList->AddLine( ImVec2( x, y + row_h ), ImVec2( x + gi.rc.w, y + row_h ), color );
    }

    imgui_pop_font();
    return count;
}

uint32_t TraceWin::graph_render_print_timeline( graph_info_t &gi )
{
    GPUVIS_TRACE_BLOCK( __func__ );

    imgui_push_smallfont();

    // Recalc colors and sizes if font/colors have changed
    if ( m_trace_events.m_ftrace.text_size_max == -1.0f )
        m_trace_events.update_ftraceprint_colors();

    struct
    {
        // Hovered duration timeline bar info
        float dist = FLT_MAX;
        uint32_t eventid = INVALID_ID;
        rect_t rc;
        float x0;
        float x1;
    } hovinfo;

    const char *row_name = gi.prinfo_cur->row_name.c_str();
    ftrace_row_info_t *ftrace_row_info = m_trace_events.get_ftrace_row_info( row_name );
    if ( !ftrace_row_info )
        ftrace_row_info = m_trace_events.get_ftrace_row_info_pid( -1 );

#if 0
    // If height is less than half text height, turn off labels.
    if ( h < ( gi.text_h / 2.0f ) )
        timeline_labels = false;
#endif

    // Check if we're drawing timeline labels
    bool timeline_labels = s_opts().getb( OPT_PrintTimelineLabels ) &&
            !ImGui::GetIO().KeyAlt;

    int64_t ts_duration_max = m_trace_events.m_ftrace.print_ts_max;
    int64_t ts_text_max = timeline_labels ? gi.dx_to_ts( m_trace_events.m_ftrace.text_size_max ) : 0;
    int64_t ts_offset = std::max< int64_t >( ts_duration_max, ts_text_max );
    const std::vector< uint32_t > &locs = *gi.prinfo_cur->plocs;

    uint32_t max_row_id = 1;
    for ( size_t idx = m_trace_events.ts_to_ftrace_print_info_idx( locs, gi.ts0 - ts_offset );
          idx < locs.size();
          idx++ )
    {
        uint32_t row_id;
        const trace_event_t &event = get_event( locs[ idx ] );
        const print_info_t *print_info = m_trace_events.get_print_info( event.id );

        if ( print_info->ts > gi.ts1 )
            break;
        else if ( gi.graph_only_filtered && event.is_filtered_out )
            continue;

        row_id = get_graph_row_id( event, ftrace_row_info, print_info );
        if ( row_id != ( uint32_t )-1 )
            max_row_id = std::max< uint32_t >( max_row_id, row_id );
    }

    uint32_t row_count = std::min< uint32_t >( max_row_id + 1, ftrace_row_info->rows );
    std::vector< row_draw_info_t > row_draw_info( row_count );
    ImU32 baralpha = s_clrs().get( col_Graph_PrintBarAlpha ) & IM_COL32_A_MASK;

    // Get height of each bar.
    float h = Clamp< float>( gi.rc.h / row_count, 2.0f, gi.text_h * 1.2f );
    float dy = gi.rc.h - row_count * h;

    event_renderer_t event_renderer( gi, gi.rc.y, gi.rc.w, gi.rc.h );

    for ( size_t idx = m_trace_events.ts_to_ftrace_print_info_idx( locs, gi.ts0 - ts_offset );
          idx < locs.size();
          idx++ )
    {
        uint32_t row_id;
        const trace_event_t &event = get_event( locs[ idx ] );
        const print_info_t *print_info = m_trace_events.get_print_info( event.id );
        int64_t event_start_ts = print_info->ts;

        if ( event_start_ts > gi.ts1 )
            break;
        else if ( gi.graph_only_filtered && event.is_filtered_out )
            continue;

        if ( event_renderer.is_event_filtered( event ) )
            continue;

        row_id = get_graph_row_id( event, ftrace_row_info, print_info );
        if ( row_id == ( uint32_t )-1 )
            continue;

        float x = gi.ts_to_screenx( event_start_ts );
        float y = gi.rc.y + ( row_count - row_id - 1 ) * h + dy;

        if ( timeline_labels )
        {
            row_draw_info[ row_id ].set_event( gi, h, x, y, &event, print_info );
        }

        if ( event.has_duration() )
        {
            float offy = h * .10f;
            float x1 = gi.ts_to_screenx( event_start_ts + event.duration );
            rect_t rc = { x, y + offy, x1 - x, h - offy * 2 };
            ImU32 color = baralpha | ( event.color & ~IM_COL32_A_MASK );

            if ( rc.w < 0 )
            {
                rc.x += rc.w;
                rc.w = -rc.w;
            }

            imgui_drawrect_filled( rc, color );

            if ( gi.mouse_pos_in_rect( rc ) )
            {
                float dist = fabs( x - gi.mouse_pos.x );

                if ( dist < hovinfo.dist )
                {
                    hovinfo.eventid = event.id;
                    hovinfo.dist = dist;
                    hovinfo.rc = rc;
                    hovinfo.x0 = x;
                    hovinfo.x1 = x1;
                }
            }
        }

        // Draw a tick for this event
        event_renderer.set_y( y, h );
        event_renderer.add_event( event.id, x, event.color );

        // Check if we're mouse hovering this event
        if ( gi.mouse_over && ( gi.mouse_pos.y >= y ) && ( gi.mouse_pos.y <= y + h ) )
        {
            if ( gi.add_mouse_hovered_event( x, event ) &&
                 is_valid_id( event.id_start ) )
            {
                const trace_event_t &event1 = get_event( event.id_start );
                float x1 = gi.ts_to_screenx( event_start_ts + event.duration );

                gi.add_mouse_hovered_event( x1, event1, true );
            }
        }
    }

    if ( is_valid_id( hovinfo.eventid ) )
    {
        const trace_event_t &event = get_event( hovinfo.eventid );

        // Draw hovered selection rectangle
        imgui_drawrect( hovinfo.rc, imgui_col_complement( event.color ) );

        // Add this event to mouse hovered list
        gi.add_mouse_hovered_event( hovinfo.x0, event, true );

        // If this is a begin_ctx event, add the end_ctx event to the hovered list
        if ( is_valid_id( event.id_start ) )
        {
            const trace_event_t &event1 = get_event( event.id_start );

            gi.add_mouse_hovered_event( hovinfo.x1, event1, true );
        }
    }

    event_renderer.done();
    event_renderer.draw_event_markers();

    // Flush print labels
    for ( uint32_t row_id = 0; row_id < row_draw_info.size(); row_id++ )
        row_draw_info[ row_id ].set_event( gi, h );

    imgui_pop_font();

    return event_renderer.m_num_events;
}

uint32_t TraceWin::graph_render_amdhw_timeline( graph_info_t &gi )
{
    imgui_push_smallfont();

    float row_h = gi.rc.h;
    uint32_t num_events = 0;
    ImU32 col_event = s_clrs().get( col_Graph_1Event );

    rect_t hov_rect;
    float y = gi.rc.y;
    ImU32 last_color = 0;
    bool draw_label = !ImGui::GetIO().KeyAlt;
    const std::vector< uint32_t > &locs = *gi.prinfo_cur->plocs;

    for ( size_t idx = vec_find_eventid( locs, gi.eventstart );
          idx < locs.size();
          idx++ )
    {
        const trace_event_t &fence_signaled = get_event( locs.at( idx ) );

        if ( fence_signaled.is_fence_signaled() &&
             is_valid_id( fence_signaled.id_start ) &&
             ( fence_signaled.ts - fence_signaled.duration < gi.ts1 ) )
        {
            float x0 = gi.ts_to_screenx( fence_signaled.ts - fence_signaled.duration );
            float x1 = gi.ts_to_screenx( fence_signaled.ts );

            imgui_drawrect_filled( x0, y, x1 - x0, row_h, fence_signaled.color );

            // Draw a label if we have room.
            if ( draw_label )
            {
                const char *label = fence_signaled.user_comm;
                ImVec2 size = ImGui::CalcTextSize( label );

                if ( size.x + imgui_scale( 4 ) >= x1 - x0 )
                {
                    // No room for the comm, try just the pid.
                    label = strrchr( label, '-' );
                    if ( label )
                        size = ImGui::CalcTextSize( ++label );
                }

                if ( size.x + imgui_scale( 4 ) < x1 - x0 )
                {
                    ImU32 color = s_clrs().get( col_Graph_BarText );
                    const tgid_info_t *tgid_info = m_trace_events.tgid_from_commstr( fence_signaled.user_comm );

                    imgui_draw_text( x0 + imgui_scale( 2.0f ), y + imgui_scale( 2.0f ),
                                     color, label );

                    if ( tgid_info )
                    {
                        imgui_push_cliprect( { x0, y, x1 - x0, row_h } );

                        imgui_draw_textf( x0 + imgui_scale( 2.0f ), y + size.y + imgui_scale( 2.0f ),
                                     color, "(%s)", tgid_info->commstr );

                        imgui_pop_cliprect();
                    }
                }
            }

            // If we drew the same color last time, draw a separator.
            if ( last_color == fence_signaled.color )
                imgui_drawrect_filled( x0, y, 1.0, row_h, col_event );
            else
                last_color = fence_signaled.color;

            // Check if this fence_signaled is selected / hovered
            if ( ( gi.hovered_fence_signaled == fence_signaled.id ) ||
                 gi.mouse_pos_in_rect( { x0, y, x1 - x0, row_h } ) )
            {
                hov_rect = { x0, y, x1 - x0, row_h };

                if ( !is_valid_id( gi.hovered_fence_signaled ) )
                    gi.hovered_fence_signaled = fence_signaled.id;
            }

            num_events++;
        }
    }

    imgui_drawrect( hov_rect, s_clrs().get( col_Graph_BarSelRect ) );

    imgui_pop_font();

    return num_events;
}

uint32_t TraceWin::graph_render_amd_timeline( graph_info_t &gi )
{
    imgui_push_smallfont();

    rect_t hov_rect;
    uint32_t num_events = 0;
    uint32_t timeline_row_count = gi.rc.h / gi.text_h;
    ImU32 col_hwrunning = s_clrs().get( col_Graph_BarHwRunning );
    ImU32 col_userspace = s_clrs().get( col_Graph_BarUserspace );
    ImU32 col_hwqueue = s_clrs().get( col_Graph_BarHwQueue );
    const std::vector< uint32_t > &locs = *gi.prinfo_cur->plocs;
    bool render_timeline_events = s_opts().getb( OPT_TimelineEvents );
    bool render_timeline_labels = s_opts().getb( OPT_TimelineLabels ) &&
            !ImGui::GetIO().KeyAlt;

    event_renderer_t event_renderer( gi, gi.rc.y, gi.rc.w, gi.rc.h );

    event_renderer.m_maxwidth = 1.0f;

    for ( size_t idx = vec_find_eventid( locs, gi.eventstart );
          idx < locs.size();
          idx++ )
    {
        const trace_event_t &fence_signaled = get_event( locs[ idx ] );

        if ( !fence_signaled.is_fence_signaled() || !is_valid_id( fence_signaled.id_start ) )
            continue;

        const trace_event_t &sched_run_job = get_event( fence_signaled.id_start );
        const trace_event_t &cs_ioctl = is_valid_id( sched_run_job.id_start ) ?
                    get_event( sched_run_job.id_start ) : sched_run_job;

        //$ TODO mikesart: can we bail out of this loop at some point if
        //  our start times for all the graphs are > gi.ts1?
        if ( cs_ioctl.ts >= gi.ts1 )
            continue;

        bool hovered = false;
        float y = gi.rc.y + ( fence_signaled.graph_row_id % timeline_row_count ) * gi.text_h;

        // amdgpu_cs_ioctl  amdgpu_sched_run_job   |   fence_signaled
        //       |-----------------|---------------|--------|
        //       |user-->          |hwqueue-->     |hw->    |
        float x_user_start = gi.ts_to_screenx( cs_ioctl.ts );
        float x_hwqueue_start = gi.ts_to_screenx( sched_run_job.ts );
        float x_hwqueue_end = gi.ts_to_screenx( fence_signaled.ts - fence_signaled.duration );
        float x_hw_end = gi.ts_to_screenx( fence_signaled.ts );
        float xleft = gi.timeline_render_user ? x_user_start : x_hwqueue_start;

        // Check if this fence_signaled is selected / hovered
        if ( ( gi.hovered_fence_signaled == fence_signaled.id ) ||
            gi.mouse_pos_in_rect( { xleft, y, x_hw_end - xleft, gi.text_h } ) )
        {
            // Mouse is hovering over this fence_signaled.
            hovered = true;
            hov_rect = { x_user_start, y, x_hw_end - x_user_start, gi.text_h };

            if ( !is_valid_id( gi.hovered_fence_signaled ) )
                gi.hovered_fence_signaled = fence_signaled.id;
        }

        // Draw user bar
        if ( hovered || gi.timeline_render_user )
        {
            imgui_drawrect_filled( x_user_start, y,
                                   x_hwqueue_start - x_user_start, gi.text_h,
                                   col_userspace );
        }

        // Draw hw queue bar
        if ( x_hwqueue_end != x_hwqueue_start )
        {
            imgui_drawrect_filled( x_hwqueue_start, y,
                                   x_hwqueue_end - x_hwqueue_start, gi.text_h,
                                   col_hwqueue );
        }

        // Draw hw running bar
        imgui_drawrect_filled( x_hwqueue_end, y,
                               x_hw_end - x_hwqueue_end, gi.text_h,
                               col_hwrunning );

        if ( render_timeline_labels )
        {
            const ImVec2 size = ImGui::CalcTextSize( cs_ioctl.user_comm );
            float x_text = std::max< float >( x_hwqueue_start, gi.rc.x ) + imgui_scale( 2.0f );

            if ( x_hw_end - x_text >= size.x )
            {
                ImU32 color = s_clrs().get( col_Graph_BarText );
                const tgid_info_t *tgid_info = m_trace_events.tgid_from_pid( cs_ioctl.pid );

                imgui_draw_text( x_text, y + imgui_scale( 1.0f ),
                                 color, cs_ioctl.user_comm );

                if ( tgid_info )
                {
                    imgui_push_cliprect( { x_text, y, x_hw_end - x_text, size.y } );

                    imgui_draw_textf( x_text + size.x, y + imgui_scale( 1.0f ),
                                     color, "  (%s)", tgid_info->commstr );

                    imgui_pop_cliprect();
                }
            }
        }

        if ( render_timeline_events )
        {
            event_renderer.set_y( y, gi.text_h );

            if ( cs_ioctl.id != sched_run_job.id )
            {
                // Draw event line for start of user
                event_renderer.add_event( cs_ioctl.id, x_user_start, cs_ioctl.color );

                // Check if we're mouse hovering starting event
                if ( gi.mouse_over && ( gi.mouse_pos.y >= y ) && ( gi.mouse_pos.y <= y + gi.text_h ) )
                {
                    // If we are hovering, and no selection bar is set, do it.
                    if ( gi.add_mouse_hovered_event( x_user_start, cs_ioctl ) &&
                         ( hov_rect.x == FLT_MAX ) )
                    {
                        hov_rect = { x_user_start, y, x_hw_end - x_user_start, gi.text_h };

                        // Draw user bar for hovered events if they weren't already drawn
                        if ( !hovered && !gi.timeline_render_user )
                        {
                            imgui_drawrect_filled( x_user_start, y,
                                                   x_hwqueue_start - x_user_start, gi.text_h,
                                                   col_userspace );
                        }
                    }
                }
            }

            // Draw event line for hwqueue start and hw end
            event_renderer.add_event( sched_run_job.id, x_hwqueue_start, sched_run_job.color );
            event_renderer.add_event( fence_signaled.id, x_hw_end, fence_signaled.color );
        }

        num_events++;
    }

    event_renderer.done();
    event_renderer.draw_event_markers();

    imgui_drawrect( hov_rect, s_clrs().get( col_Graph_BarSelRect ) );

    imgui_pop_font();

    return num_events;
}

uint32_t TraceWin::graph_render_row_events( graph_info_t &gi )
{
    const std::vector< uint32_t > &locs = *gi.prinfo_cur->plocs;
    event_renderer_t event_renderer( gi, gi.rc.y + 4, gi.rc.w, gi.rc.h - 8 );
    bool hide_sched_switch = s_opts().getb( OPT_HideSchedSwitchEvents );

    for ( size_t idx = vec_find_eventid( locs, gi.eventstart );
          idx < locs.size();
          idx++ )
    {
        uint32_t eventid = locs[ idx ];
        const trace_event_t &event = get_event( eventid );

        if ( eventid > gi.eventend )
            break;
        else if ( gi.graph_only_filtered && event.is_filtered_out )
            continue;
        else if ( hide_sched_switch && event.is_sched_switch() )
            continue;

        if ( event_renderer.is_event_filtered( event ) )
            continue;

        float x = gi.ts_to_screenx( event.ts );

        // Check if we're mouse hovering this event
        if ( gi.mouse_over )
            gi.add_mouse_hovered_event( x, event );

        event_renderer.add_event( event.id, x, event.color );
    }

    event_renderer.done();
    event_renderer.draw_event_markers();

    if ( gi.prinfo_cur->pid >= 0 )
    {
        // Grab all the sched_switch events that have our comm listed as prev_comm
        const std::vector< uint32_t > *plocs = m_trace_events.get_sched_switch_locs(
                    gi.prinfo_cur->pid, TraceEvents::SCHED_SWITCH_PREV );

        if ( plocs )
        {
            ImU32 colors[ 2 ] =
            {
                s_clrs().get( col_Graph_TaskRunning ),
                s_clrs().get( col_Graph_TaskSleeping )
            };
            bool sched_switch_bars_empty = gi.sched_switch_bars.empty();

            for ( size_t idx = vec_find_eventid( *plocs, gi.eventstart );
                  idx < plocs->size();
                  idx++ )
            {
                const trace_event_t &sched_switch = get_event( plocs->at( idx ) );

                if ( sched_switch.has_duration() )
                {
                    float row_h = gi.text_h;
                    float y = gi.rc.y + ( gi.rc.h - row_h ) / 2;
                    bool drawrect = false;
                    float x0 = gi.ts_to_screenx( sched_switch.ts - sched_switch.duration );
                    float x1 = gi.ts_to_screenx( sched_switch.ts );
                    int running = !!( sched_switch.flags & TRACE_FLAG_SCHED_SWITCH_TASK_RUNNING );

                    // Bail if we're off the right side of our graph
                    if ( x0 > gi.rc.x + gi.rc.w )
                        break;

                    imgui_drawrect_filled( x0, y, x1 - x0, row_h, colors[ running ] );

                    if ( gi.mouse_pos_in_rect( { x0, y, x1 - x0, row_h } ) )
                    {
                        drawrect = true;
                        gi.sched_switch_bars.push_back( sched_switch.id );
                    }
                    else if ( !sched_switch_bars_empty && ( gi.sched_switch_bars[ 0 ] == sched_switch.id ) )
                    {
                        drawrect = true;
                    }

                    if ( drawrect )
                        imgui_drawrect( x0, y, x1 - x0, row_h, s_clrs().get( col_Graph_BarSelRect ) );
                }
            }
        }
    }

    return event_renderer.m_num_events;
}

uint32_t TraceWin::graph_render_i915_reqwait_events( graph_info_t &gi )
{
    const trace_event_t *pevent_sel = NULL;
    const std::vector< uint32_t > &locs = *gi.prinfo_cur->plocs;
    event_renderer_t event_renderer( gi, gi.rc.y + 4, gi.rc.w, gi.rc.h - 8 );
    ImU32 barcolor = s_clrs().get( col_Graph_Bari915ReqWait );
    ImU32 textcolor = s_clrs().get( col_Graph_BarText );

    uint32_t hashval = hashstr32( gi.prinfo_cur->row_name );
    uint32_t row_count = m_trace_events.m_row_count.m_map[ hashval ];
    float row_h = std::max< float >( 2.0f, gi.rc.h / row_count );

    // Check if we're drawing timeline labels
    bool timeline_labels = s_opts().getb( OPT_PrintTimelineLabels ) &&
            !ImGui::GetIO().KeyAlt;

#if 0
    // If height is less than half text height, turn off labels.
    if ( row_h < ( gi.text_h / 2.0f ) )
        timeline_labels = false;
#endif

    for ( size_t idx = vec_find_eventid( locs, gi.eventstart );
          idx < locs.size();
          idx++ )
    {
        float y;
        bool do_selrect = false;
        const trace_event_t &event = get_event( locs[ idx ] );
        const trace_event_t &event_begin = get_event( event.id_start );
        float x0 = gi.ts_to_screenx( event_begin.ts );
        float x1 = gi.ts_to_screenx( event.ts );

        if ( ( x0 > gi.rc.x + gi.rc.w ) || ( x1 < gi.rc.x ) )
            continue;

        if ( event_renderer.is_event_filtered( event ) )
            continue;

        y = gi.rc.y + ( event.graph_row_id % row_count ) * row_h;

        event_renderer.set_y( y, row_h );
        event_renderer.add_event( event_begin.id, x0, event_begin.color );
        event_renderer.add_event( event.id, x1, event.color );

        // Draw bar
        imgui_drawrect_filled( x0, y, x1 - x0, row_h, barcolor );

        if ( timeline_labels && ( x1 - x0 >= imgui_scale( 16.0f ) ) )
        {
            const char *label = "";
            const char *ctxstr = get_event_field_val( event, "ctx", "0" );
            float ty = y + ( row_h / 2.0f ) - ( gi.text_h / 2.0f ) - imgui_scale( 2.0f );

            // Find the i915_request_queue event for this ring/ctx/seqno
            const std::vector< uint32_t > *plocs = m_trace_events.m_i915.req_queue_locs.get_locations( event );
            if ( plocs )
            {
                const trace_event_t &e = get_event( plocs->back() );
                const tgid_info_t *tgid_info = m_trace_events.tgid_from_commstr( e.user_comm );

                // Use commstr from i915_request_queue since it's from interrupt handler and
                //   should have tid of user-space thread
                label = tgid_info ? tgid_info->commstr : event.user_comm;
            }

            imgui_push_cliprect( { x0, ty, x1 - x0, gi.text_h } );
            imgui_draw_textf( x0 + imgui_scale( 1.0f ), ty,
                             textcolor, "%s-%u %s", ctxstr, event.seqno, label );
            imgui_pop_cliprect();
        }

        if ( gi.mouse_over && ( gi.mouse_pos.y >= y ) && ( gi.mouse_pos.y <= y + row_h ) )
        {
            bool add_hovered;

            if ( gi.mouse_pos_in_rect( { x0, y, x1 - x0, row_h } ) )
            {
                do_selrect = true;
                add_hovered = true;
            }
            else
            {
                add_hovered = gi.add_mouse_hovered_event( x0, event_begin );
                add_hovered |= gi.add_mouse_hovered_event( x1, event );
            }

            if ( add_hovered )
            {
                gi.add_mouse_hovered_event( x0, event_begin, true );
                gi.add_mouse_hovered_event( x1, event, true );
            }
        }

        if ( do_selrect || gi.is_i915_ringctxseq_selected( event ) )
        {
            pevent_sel = &event;

            imgui_drawrect( x0, y, x1 - x0, row_h, s_clrs().get( col_Graph_BarSelRect ) );
        }
    }

    event_renderer.done();
    event_renderer.draw_event_markers();

    if ( pevent_sel )
        gi.set_selected_i915_ringctxseq( *pevent_sel );

    return event_renderer.m_num_events / 2;
}

uint32_t TraceWin::graph_render_i915_req_events( graph_info_t &gi )
{
    ImU32 textcolor = s_clrs().get( col_Graph_BarText );
    const std::vector< uint32_t > &locs = *gi.prinfo_cur->plocs;
    event_renderer_t event_renderer( gi, gi.rc.y, gi.rc.w, gi.rc.h );

    uint32_t hashval = hashstr32( gi.prinfo_cur->row_name );
    uint32_t row_count = m_trace_events.m_row_count.m_map[ hashval ];
    float row_h = std::max< float >( 2.0f, gi.rc.h / row_count );

    // Check if we're drawing timeline labels
    bool render_timeline_labels = s_opts().getb( OPT_PrintTimelineLabels ) &&
            !ImGui::GetIO().KeyAlt;

#if 0
    // If height is less than half text height, turn off labels.
    if ( row_h < ( gi.text_h / 2.0f ) )
        render_timeline_labels = false;
#endif

    struct barinfo_t
    {
        float x0;
        float x1;
        float y;
        uint64_t ctx;
        uint32_t seqno;
        uint32_t first_event_id;
    };
    util_umap< uint64_t, barinfo_t > rendered_bars;

    for ( size_t idx = vec_find_eventid( locs, gi.eventstart );
          idx < locs.size();
          idx++ )
    {
        float y;
        const trace_event_t &event = get_event( locs[ idx ] );
        bool has_duration = event.has_duration();
        float x1 = gi.ts_to_screenx( event.ts );
        float x0 = has_duration ? gi.ts_to_screenx( event.ts - event.duration ) : x1;

        if ( ( x0 > gi.rc.x + gi.rc.w ) || ( x1 < gi.rc.x ) )
            continue;

        if ( event_renderer.is_event_filtered( event ) )
            continue;

        y = gi.rc.y + event.graph_row_id * row_h;

        event_renderer.set_y( y, row_h );
        event_renderer.add_event( event.id, x1, event.color );

        if ( gi.mouse_over && ( gi.mouse_pos.y >= y ) && ( gi.mouse_pos.y <= y + row_h ) )
            gi.add_mouse_hovered_event( x1, event );

        if ( has_duration )
        {
            const trace_event_t &event0 = get_event( event.id_start );
            const trace_event_t *pevent = !strcmp( event.name, "intel_engine_notify" ) ?
                        &event0 : &event;

            // Draw bar
            imgui_drawrect_filled( x0, y, x1 - x0, row_h, s_clrs().get( event.color_index ) );

            if ( gi.mouse_pos_in_rect( { x0, y, x1 - x0, row_h } ) )
                gi.set_selected_i915_ringctxseq( *pevent );

            // Add bar information: ctx, seqno, and size
            const char *ctxstr = get_event_field_val( *pevent, "ctx", "0" );
            uint64_t ctx = strtoull( ctxstr, NULL, 10 );
            uint64_t key = ( ctx << 32 ) | pevent->seqno;
            barinfo_t *barinfo = rendered_bars.get_val( key );

            if ( !barinfo )
                barinfo = rendered_bars.get_val( key, { x0, x1, y, ctx, pevent->seqno, pevent->id } );
            else
                barinfo->x1 = x1;
        }
    }

    for ( const auto &bar : rendered_bars.m_map )
    {
        const barinfo_t &barinfo = bar.second;
        float y = barinfo.y;
        float x0 = barinfo.x0;
        float x1 = barinfo.x1;
        const char *label = "";

        // We added info for the first event we saw... get all events associated with this one
        const trace_event_t &event1 = get_event( barinfo.first_event_id );
        const std::vector< uint32_t > *plocs = m_trace_events.m_i915.gem_req_locs.get_locations( event1 );

        // Check if mouse is in this rect
        bool do_selrect = !!gi.mouse_pos_in_rect( { x0, y, x1 - x0, row_h } );

        if ( plocs )
        {
            // Go through all events with this ctx + seqno
            for ( uint32_t idx : *plocs )
            {
                const trace_event_t &event = get_event( idx );
                i915_type_t event_type = get_i915_reqtype( event );

                if ( event_type == i915_req_Queue )
                {
                    const tgid_info_t *tgid_info = m_trace_events.tgid_from_commstr( event.user_comm );

                    // Use commstr from i915_request_queue since it's from interrupt handler and
                    //   should have tid of user-space thread
                    label = tgid_info ? tgid_info->commstr : event.user_comm;
                }

                if ( do_selrect || gi.is_i915_ringctxseq_selected( event ) )
                {
                    gi.add_mouse_hovered_event( gi.ts_to_screenx( event.ts ), event, true );
                    do_selrect = true;
                }
            }

            if ( do_selrect )
            {
                plocs = m_trace_events.m_i915.reqwait_begin_locs.get_locations( event1 );

                if ( plocs )
                {
                    for ( uint32_t idx : *plocs )
                    {
                        const trace_event_t &event = get_event( idx );

                        // Add i915_request_wait_begin
                        gi.add_mouse_hovered_event( gi.ts_to_screenx( event.ts ), event, true );
                        // Add i915_request_wait_end
                        gi.add_mouse_hovered_event( gi.ts_to_screenx( event.ts ), get_event( event.id_start ), true );
                    }
                }
            }
        }

        if ( render_timeline_labels && ( x1 - x0 >= imgui_scale( 16.0f ) ) )
        {
            float ty = y + ( row_h / 2.0f ) - ( gi.text_h / 2.0f ) - imgui_scale( 2.0f );

            imgui_push_cliprect( { x0, ty, x1 - x0, gi.text_h } );
            imgui_draw_textf( x0 + imgui_scale( 1.0f ), ty, textcolor,
                              "%lu-%u %s", barinfo.ctx, barinfo.seqno, label );
            imgui_pop_cliprect();
        }

        if ( do_selrect )
        {
            imgui_drawrect( x0, y, x1 - x0, row_h, s_clrs().get( col_Graph_BarSelRect ) );
        }
    }

    event_renderer.done();
    event_renderer.draw_event_markers();

    return event_renderer.m_num_events;
}

void TraceWin::graph_render_single_row( graph_info_t &gi )
{
    if ( gi.mouse_over )
    {
        m_graph.mouse_over_row_name = gi.prinfo_cur->row_name;
        m_graph.mouse_over_row_filter_expr = gi.prinfo_cur->row_filter_expr;
        m_graph.mouse_over_row_type = gi.prinfo_cur->row_type;
    }

    // Draw background
    imgui_drawrect_filled( gi.rc, s_clrs().get( col_Graph_RowBk ) );

    if ( gi.prinfo_cur->tgid_info )
    {
        ImGui::GetWindowDrawList()->AddLine(
                    ImVec2( gi.rc.x, gi.rc.y + gi.rc.h + 1 ),
                    ImVec2( gi.rc.x + gi.rc.w, gi.rc.y + gi.rc.h + 1 ),
                    gi.prinfo_cur->tgid_info->color );
    }

    uint32_t num_events = 0;

    if ( gi.prinfo_cur->render_cb )
    {
        float scale_ts = gi.prinfo_cur->scale_ts - 1.0f;

        if ( scale_ts > 0.0f )
        {
            int64_t start_ts = m_graph.start_ts;
            int64_t length_ts = m_graph.length_ts;

            scale_ts = Clamp< float >( scale_ts, 0.001f, 100.0f );

            start_ts -= length_ts * scale_ts;
            length_ts += length_ts * 2 * scale_ts;
            gi.set_ts( start_ts, length_ts );

            if ( gi.mouse_over )
                gi.mouse_pos_scaled_ts = gi.screenx_to_ts( gi.mouse_pos.x );

            graph_render_time_ticks( gi, imgui_scale( 6.0f ), imgui_scale( 2.0f ) );
        }

        // Call the render callback function
        num_events = gi.prinfo_cur->render_cb( gi );

        if ( scale_ts > 0.0f )
        {
            float x0 = gi.ts_to_screenx( m_graph.start_ts );
            float x1 = gi.ts_to_screenx( m_graph.start_ts + m_graph.length_ts );

            ImGui::GetWindowDrawList()->AddRectFilled(
                        ImVec2( x0, gi.rc.y ), ImVec2( x1, gi.rc.y + gi.rc.h ),
                        0x5fffffff, 9.0f, 0x0f );
            gi.set_ts( m_graph.start_ts, m_graph.length_ts );
        }
    }

    gi.prinfo_cur->num_events = num_events;
}

void TraceWin::graph_render_time_ticks( graph_info_t &gi, float h0, float h1 )
{
    // Draw time ticks every millisecond
    int64_t tsstart = std::max< int64_t >( gi.ts0 / NSECS_PER_MSEC - 1, 0 ) * NSECS_PER_MSEC;
    float dx = gi.rc.w * NSECS_PER_MSEC * gi.tsdxrcp;
    const float w = imgui_scale( 1.0f );
    const float wmin = imgui_scale( 4.0f );

    if ( dx <= wmin )
    {
        tsstart = std::max< int64_t >( gi.ts0 / NSECS_PER_SEC - 1, 0 ) * NSECS_PER_SEC;
        dx = gi.rc.w * NSECS_PER_SEC * gi.tsdxrcp;
    }

    if ( dx > wmin )
    {
        float x0 = gi.ts_to_x( tsstart );
        const float wmin2 = imgui_scale( 35.0f );

        for ( ; x0 <= gi.rc.w; x0 += dx )
        {
            imgui_drawrect_filled( gi.rc.x + x0, gi.rc.y, w, h0,
                                   s_clrs().get( col_Graph_TimeTick ) );

            if ( dx >= wmin2 )
            {
                for ( int i = 1; i < 4; i++ )
                {
                    imgui_drawrect_filled( gi.rc.x + x0 + i * dx / 4, gi.rc.y, w, h1,
                                           s_clrs().get( col_Graph_TimeTick ) );
                }
            }
        }
    }
}

static float get_vblank_xdiffs( TraceWin &win, graph_info_t &gi, const std::vector< uint32_t > *vblank_locs )
{
    float xdiff = 0.0f;
    float xlast = 0.0f;
    uint32_t count = 0;

    for ( size_t idx = vec_find_eventid( *vblank_locs, gi.eventstart );
          idx < vblank_locs->size();
          idx++ )
    {
        uint32_t id = vblank_locs->at( idx );
        trace_event_t &event = win.get_event( id );

        if ( s_opts().getcrtc( event.crtc ) )
        {
            float x = gi.ts_to_screenx( event.ts );

            if ( xlast )
                xdiff = std::max< float >( xdiff, x - xlast );
            xlast = x;

            if ( count++ >= 10 )
                break;
        }
    }

    return xdiff;
}

void TraceWin::graph_render_vblanks( graph_info_t &gi )
{
    // Draw vblank events on every graph.
    const std::vector< uint32_t > *vblank_locs = m_trace_events.get_tdopexpr_locs( "$name=drm_vblank_event" );

    if ( vblank_locs )
    {
        /*
         * From Pierre-Loup: One thing I notice when zooming out is that things become
         * very noisy because of the vblank bars. I'm changing their colors so they're not
         * fullbright, which helps, but can they be changed to be in the background of
         * other rendering past a certain zoom threshold? You want them in the foreground
         * when pretty close, but in the background if there's more than ~50 on screen
         * probably?
         */
        float xdiff = get_vblank_xdiffs( *this, gi, vblank_locs ) / imgui_scale( 1.0f );
        uint32_t alpha = std::min< uint32_t >( 255, 50 + 2 * xdiff );

        for ( size_t idx = vec_find_eventid( *vblank_locs, gi.eventstart );
              idx < vblank_locs->size();
              idx++ )
        {
            uint32_t id = vblank_locs->at( idx );

            if ( id > gi.eventend )
                break;

            trace_event_t &event = get_event( id );

            if ( s_opts().getcrtc( event.crtc ) )
            {
                // Handle drm_vblank_event0 .. drm_vblank_event2
                uint32_t col = Clamp< uint32_t >( col_VBlank0 + event.crtc, col_VBlank0, col_VBlank2 );
                float x = gi.ts_to_screenx( event.ts );

                imgui_drawrect_filled( x, gi.rc.y, imgui_scale( 1.0f ), gi.rc.h,
                                       s_clrs().get( col, alpha ) );
            }
        }
    }
}

void TraceWin::graph_render_framemarker_frames( graph_info_t &gi )
{
    if ( m_frame_markers.m_right_frames.empty() )
        return;

    if ( !s_opts().getb( OPT_RenderFrameMarkers ) )
        return;

    // Clear frame markers
    m_frame_markers.m_frame_marker_selected = -1;
    m_frame_markers.m_frame_marker_left = -1;
    m_frame_markers.m_frame_marker_right = -2;

    bool markers_set = false;
    float midx = gi.rc.x + gi.rc.w / 2.0f;

    for ( size_t idx = vec_find_eventid( m_frame_markers.m_right_frames, gi.eventstart );
          idx < m_frame_markers.m_right_frames.size();
          idx++ )
    {
        uint32_t left_id = m_frame_markers.m_left_frames[ idx ];
        if ( left_id > gi.eventend )
        {
            if ( !markers_set )
            {
                // Nothing was drawn, so this marker is off screen to right
                m_frame_markers.m_frame_marker_left = idx - 1;
                m_frame_markers.m_frame_marker_right = idx;
                markers_set = true;
            }
            break;
        }

        uint32_t right_id = m_frame_markers.m_right_frames[ idx ];
        trace_event_t &left_event = get_event( left_id );
        trace_event_t &right_event = get_event( right_id );
        float left_x = gi.ts_to_screenx( left_event.ts );
        float right_x = gi.ts_to_screenx( right_event.ts );
        ImU32 col = ( idx & 0x1 ) ? col_FrameMarkerBk1 : col_FrameMarkerBk0;

        // If markers were set but the one we picked had the left x off
        // the screen and this one doesn't, choose it.
        if ( markers_set &&
             ( m_frame_markers.m_frame_marker_selected == -1 ) &&
             ( left_x > gi.rc.x ) && ( left_x < midx ) )
        {
            markers_set = false;
        }

        if ( !markers_set )
        {
            if ( left_x > gi.rc.x )
            {
                // Left of this frame is on screen and it's the first
                // frame we're drawing. It's our "selected" frame.
                m_frame_markers.m_frame_marker_left = idx - 1;
                m_frame_markers.m_frame_marker_right = idx + 1;
                m_frame_markers.m_frame_marker_selected = idx;

                col = col_FrameMarkerSelected;
            }
            else
            {
                // Left of this frame is off screen to left.
                m_frame_markers.m_frame_marker_left = idx;
                m_frame_markers.m_frame_marker_right = idx + 1;
            }

            markers_set = true;
        }

        imgui_drawrect_filled( left_x, gi.rc.y, right_x - left_x, gi.rc.h,
                               s_clrs().get( col ) );

        if ( gi.mouse_pos_in_rect( { left_x, gi.rc.y, right_x - left_x, gi.rc.h } ) )
            gi.hovered_framemarker_frame = idx;
    }

    if ( !markers_set )
    {
        // Markers never set, so everything is to our left.
        m_frame_markers.m_frame_marker_left = ( int )m_frame_markers.m_left_frames.size() - 1;
    }
}

void TraceWin::graph_render_mouse_pos( graph_info_t &gi )
{
    // Don't render mouse position if we're resizing the graph (it flashes).
    if ( m_graph.mouse_captured == MOUSE_CAPTURED_RESIZE_GRAPH )
        return;

    // Draw location line for mouse if is over graph
    if ( gi.mouse_over )
    {
        imgui_drawrect_filled( gi.mouse_pos.x, gi.rc.y, imgui_scale( 2.0f ), gi.rc.h,
                               s_clrs().get( col_Graph_MousePos ) );
    }

    // Render markers A/B if in range
    for ( size_t i = 0; i < ARRAY_SIZE( m_graph.ts_markers ); i++ )
    {
        if ( ( m_graph.ts_markers[ i ] >= gi.ts0 ) && ( m_graph.ts_markers[ i ] < gi.ts1 ) )
        {
            float x = gi.ts_to_screenx( m_graph.ts_markers[ i ] );

            imgui_drawrect_filled( x, gi.rc.y, imgui_scale( 2.0f ), gi.rc.h,
                                   s_clrs().get( col_Graph_MarkerA + i ) );
        }
    }
}

void TraceWin::graph_render_eventids( graph_info_t &gi )
{
    const struct
    {
        uint32_t eventid;
        ImU32 color;
    } events[] =
    {
        { gi.hovered_eventid, s_clrs().get( col_Graph_HovEvent, 120 ) },
        { gi.selected_eventid, s_clrs().get( col_Graph_SelEvent, 120 ) },
    };

    for ( const auto &item : events )
    {
        if ( is_valid_id( item.eventid ) )
        {
            trace_event_t &event = get_event( item.eventid );

            if ( event.ts >= gi.ts0 && event.ts <= gi.ts1 )
            {
                float x = gi.ts_to_screenx( event.ts );

                imgui_drawrect_filled( x, gi.rc.y, imgui_scale( 1.0f ), gi.rc.h, item.color );
            }
        }
    }
}

void TraceWin::graph_render_mouse_selection( graph_info_t &gi )
{
    // Draw mouse selection location
    if ( ( m_graph.mouse_captured == MOUSE_CAPTURED_ZOOM ) ||
         ( m_graph.mouse_captured == MOUSE_CAPTURED_SELECT_AREA ) )
    {
        float mousex0 = m_graph.mouse_capture_pos.x;
        float mousex1 = gi.mouse_pos.x;

        imgui_drawrect_filled( mousex0, gi.rc.y, mousex1 - mousex0, gi.rc.h,
                               s_clrs().get( col_Graph_ZoomSel ) );
    }
}

void TraceWin::graph_render_eventlist_selection( graph_info_t &gi )
{
    if ( s_opts().getb( OPT_ShowEventList ) )
    {
        // Draw rectangle for visible event list contents
        if ( is_valid_id( m_eventlist.start_eventid ) &&
             is_valid_id( m_eventlist.end_eventid ) &&
             ( m_eventlist.end_eventid > 0 ) )
        {
            trace_event_t &event0 = get_event( m_eventlist.start_eventid );
            trace_event_t &event1 = get_event( m_eventlist.end_eventid - 1 );
            float xstart = gi.ts_to_screenx( event0.ts );
            float xend = gi.ts_to_screenx( event1.ts );

            imgui_drawrect( xstart, gi.rc.y + imgui_scale( 20 ),
                            xend - xstart, gi.rc.h - imgui_scale( 30 ),
                            s_clrs().get( col_EventList_Sel ) );
        }
    }
}

static void render_row_label( float x, float y, row_info_t &ri )
{
    ImU32 color = ri.tgid_info ? ri.tgid_info->color :
                s_clrs().get( col_Graph_RowLabelText );
    std::string label = string_format( "%u) %s", ri.id, ri.row_name.c_str() );

    if ( ri.scale_ts > 1.0f )
        label += s_textclrs().bright_str( string_format( " (%.1fx)", ri.scale_ts ) );

    imgui_draw_text( x, y, color, label.c_str(), true );
    y += ImGui::GetTextLineHeight();

    if ( ri.minval <= ri.maxval )
    {
        label = string_format( "min:%.2f max:%.2f", ri.minval, ri.maxval );
        imgui_draw_text( x, y, color, label.c_str(), true );
    }
    else if ( ri.num_events )
    {
        const char *suffix = ( ri.num_events > 1 ) ? "s" : "";

        label = string_format( "%u event%s", ri.num_events, suffix );
        imgui_draw_text( x, y, color, label.c_str(), true );
    }
}

void TraceWin::graph_render_row_labels( graph_info_t &gi )
{
    if ( gi.prinfo_zoom )
    {
        if ( gi.prinfo_zoom_hw )
        {
            float y = gi.rc.y + gi.rc.h - gi.prinfo_zoom_hw->row_h;

            render_row_label( gi.rc.x, y, *gi.prinfo_zoom_hw );
        }

        render_row_label( gi.rc.x, gi.rc.y, *gi.prinfo_zoom );
    }
    else
    {
        for ( row_info_t &ri : gi.row_info )
        {
            float y = gi.rc.y + ri.row_y;

            render_row_label( gi.rc.x, y, ri );
        }
    }
}

void TraceWin::graph_range_check_times()
{
    const std::vector< trace_event_t > &events = m_trace_events.m_events;

    if ( m_graph.length_ts < m_graph.s_min_length )
    {
        m_graph.length_ts = m_graph.s_min_length;
        m_graph.recalc_timebufs = true;
    }
    else if ( m_graph.length_ts > m_graph.s_max_length )
    {
        m_graph.length_ts = m_graph.s_max_length;
        m_graph.recalc_timebufs = true;
    }

    // Sanity check the graph start doesn't go completely off the rails.
    if ( m_graph.start_ts < events.front().ts - NSECS_PER_MSEC )
    {
        m_graph.start_ts = events.front().ts - NSECS_PER_MSEC;
        m_graph.recalc_timebufs = true;
    }
    else if ( m_graph.start_ts > events.back().ts )
    {
        m_graph.start_ts = events.back().ts;
        m_graph.recalc_timebufs = true;
    }
}

void TraceWin::graph_zoom( int64_t center_ts, int64_t ts0, bool zoomin, int64_t newlenin )
{
    int64_t origlen = m_graph.length_ts;
    int64_t amt = zoomin ? -( origlen / 2 ) : ( origlen / 2 );
    int64_t newlen = ( newlenin != INT64_MAX ) ? newlenin :
            Clamp< int64_t >( origlen + amt, m_graph.s_min_length, m_graph.s_max_length );

    if ( newlen != origlen )
    {
        double scale = ( double )newlen / origlen;

        m_graph.start_ts = center_ts - ( int64_t )( ( center_ts - ts0 ) * scale );
        m_graph.length_ts = newlen;
        m_graph.recalc_timebufs = true;
    }
}

bool TraceWin::is_graph_row_zoomable()
{
    if ( m_graph.mouse_over_row_name.empty() )
        return false;

    if ( m_graph.zoom_row_name == m_graph.mouse_over_row_name )
        return false;

    switch ( m_graph.mouse_over_row_type )
    {
    case LOC_TYPE_AMDTimeline:
    case LOC_TYPE_AMDTimeline_hw:
    case LOC_TYPE_Plot:
    case LOC_TYPE_Print:
    case LOC_TYPE_i915Request:
        return true;
    default:
        return false;
    }
}

void TraceWin::zoom_graph_row()
{
    m_graph.zoom_row_name = m_graph.mouse_over_row_name;

    if ( m_graph.mouse_over_row_type == LOC_TYPE_AMDTimeline_hw )
    {
        // Trim " hw" from end of string so, for example, we zoom "gfx" and not "gfx hw".
        m_graph.zoom_row_name.resize( m_graph.zoom_row_name.size() - 3 );
    }
}

bool TraceWin::frame_markers_enabled()
{
    return !m_frame_markers.m_left_frames.empty() &&
            s_opts().getb( OPT_RenderFrameMarkers );
}

void TraceWin::frame_markers_goto( int target, bool fit_frame )
{
    if ( ( size_t )target < m_frame_markers.m_left_frames.size() )
    {
        float pct = 0.05f;
        uint32_t left_eventid = m_frame_markers.m_left_frames[ target ];
        const trace_event_t &left_event = get_event( left_eventid );

        if ( fit_frame )
        {
            int64_t len = m_frame_markers.get_frame_len( m_trace_events, target );

            m_graph.start_ts = left_event.ts - len * pct;
            m_graph.length_ts = len * ( 1 + 2 * pct );
        }
        else
        {
            int64_t len = m_graph.length_ts;
            int64_t start_ts = left_event.ts - len * pct;

            m_graph.start_ts = start_ts;
        }

        m_graph.recalc_timebufs = true;
    }
}

void TraceWin::graph_handle_hotkeys( graph_info_t &gi )
{
    // If there are no actions, bail.
    if ( !s_actions().count() )
        return;

    if ( frame_markers_enabled() )
    {
        int target = -1;
        bool fit_frame = s_actions().peek( action_frame_marker_prev_fit ) ||
                         s_actions().peek( action_frame_marker_next_fit );

        if ( s_actions().get( action_frame_marker_prev_fit ) ||
             s_actions().get( action_frame_marker_prev ) )
        {
            target = m_frame_markers.m_frame_marker_left;
        }
        if ( s_actions().get( action_frame_marker_next_fit ) ||
             s_actions().get( action_frame_marker_next ) )
        {
            target = m_frame_markers.m_frame_marker_right;
        }

        frame_markers_goto( target, fit_frame );
    }

    if ( s_actions().get( action_toggle_frame_filters ) )
        m_row_filters_enabled = !m_row_filters_enabled;

    bool show_hovered_pid = s_actions().get( action_graph_show_hovered_pid );
    bool show_hovered_tgid = s_actions().get( action_graph_show_hovered_tgid );

    if ( show_hovered_pid || show_hovered_tgid )
    {
        if ( !m_graph.cpu_timeline_pids.empty() )
        {
            // If we're already filtering some stuff, just clear it.
            m_graph.cpu_filter_pid = 0;
            m_graph.cpu_filter_tgid = 0;
            m_graph.cpu_timeline_pids.clear();
        }
        else if ( !gi.sched_switch_bars.empty() )
        {
            // Hovering over cpu graph
            int event_id = gi.sched_switch_bars[ 0 ];
            const trace_event_t &event = get_event( event_id );

            m_graph.cpu_filter_pid = event.pid;
        }
        else if ( !gi.hovered_items.empty() )
        {
            // Hovering over graph row of some sort
            int event_id = gi.hovered_items[ 0 ].eventid;
            const trace_event_t &event = get_event( event_id );

            m_graph.cpu_filter_pid = event.pid;
        }

        if ( m_graph.cpu_filter_pid )
        {
            m_graph.cpu_timeline_pids.insert( m_graph.cpu_filter_pid );

            if ( show_hovered_tgid )
            {
                const tgid_info_t *tgid_info = m_trace_events.tgid_from_pid( m_graph.cpu_filter_pid );

                if ( tgid_info )
                {
                    m_graph.cpu_filter_tgid = tgid_info->tgid;

                    for ( int pid : tgid_info->pids )
                        m_graph.cpu_timeline_pids.insert( pid );
                }
            }
        }
    }

    if ( s_actions().get( action_graph_zoom_row ) )
    {
        if ( !m_graph.zoom_row_name.empty() )
            m_graph.zoom_row_name.clear();
        else if ( is_graph_row_zoomable() )
            zoom_graph_row();
    }

    if ( s_actions().get( action_cpugraph_hide_systemevents ) )
    {
        m_graph.cpu_hide_system_events = !m_graph.cpu_hide_system_events;
    }

    if ( gi.mouse_over &&
         s_actions().get( action_graph_zoom_mouse ) )
    {
        if ( m_graph.zoom_loc.first != INT64_MAX )
        {
            m_graph.start_ts = m_graph.zoom_loc.first;
            m_graph.length_ts = m_graph.zoom_loc.second;
            m_graph.recalc_timebufs = true;

            m_graph.zoom_loc = std::make_pair( INT64_MAX, INT64_MAX );
        }
        else
        {
            int64_t newlen = 3 * NSECS_PER_MSEC;
            int64_t mouse_ts = gi.screenx_to_ts( gi.mouse_pos.x );

            m_graph.zoom_loc = std::make_pair( m_graph.start_ts, m_graph.length_ts );

            graph_zoom( mouse_ts, gi.ts0, false, newlen );
        }
    }

    if ( gi.mouse_over &&
         !m_graph.mouse_over_row_name.empty() &&
         s_actions().get( action_graph_hide_row ) )
    {
        const std::string &row_name = m_graph.mouse_over_row_name;

        m_graph.rows.show_row( row_name, GraphRows::HIDE_ROW );
    }

    if ( m_graph.has_focus || m_eventlist.has_focus )
    {
        bool gotoA = s_actions().get( action_graph_goto_markerA );
        bool gotoB = s_actions().get( action_graph_goto_markerB );

        if ( gotoA || gotoB )
        {
            int idx = gotoA ? 0 : 1;

            if ( graph_marker_valid( idx ) )
            {
                m_graph.start_ts = m_graph.ts_markers[ idx ] - m_graph.length_ts / 2;
                m_graph.recalc_timebufs = true;
            }
        }

        if ( gi.mouse_over )
        {
            if ( s_actions().get( action_graph_set_markerA ) )
                graph_marker_set( 0, m_graph.ts_marker_mouse );
            if ( s_actions().get( action_graph_set_markerB ) )
                graph_marker_set( 1, m_graph.ts_marker_mouse );
        }
    }

    if ( m_graph.has_focus || m_eventlist.has_focus )
    {
        for ( int action = action_graph_save_location1; action <= action_graph_save_location5; action++ )
        {
            if ( s_actions().get( ( action_t )action ) )
            {
                int index = action - action_graph_save_location1;

                m_graph.saved_locs[ index ] = std::make_pair( m_graph.start_ts, m_graph.length_ts );
            }
        }

        for ( int action = action_graph_restore_location1; action <= action_graph_restore_location5; action++ )
        {
            int index = action - action_graph_restore_location1;

            if ( m_graph.saved_locs[ index ].second && s_actions().get( ( action_t )action ) )
            {
                m_graph.start_ts = m_graph.saved_locs[ index ].first;
                m_graph.length_ts = m_graph.saved_locs[ index ].second;
                m_graph.recalc_timebufs = true;
            }
        }
    }
}

void TraceWin::graph_handle_keyboard_scroll( graph_info_t &gi )
{
    // If we don't have focus or there are no actions, bail
    if ( !m_graph.has_focus || !s_actions().count() )
        return;

    int64_t start_ts = m_graph.start_ts;
    const std::vector< trace_event_t > &events = m_trace_events.m_events;

    if ( s_actions().get( action_scroll_up ) )
    {
        m_graph.start_y += ImGui::GetTextLineHeightWithSpacing() * 4;
    }
    else if ( s_actions().get( action_scroll_down ) )
    {
        m_graph.start_y -= ImGui::GetTextLineHeightWithSpacing() * 4;
    }
    else if ( s_actions().get( action_scroll_pageup ) )
    {
        m_graph.start_y += ( gi.rc.h - ImGui::GetTextLineHeightWithSpacing() * 4 );
    }
    else if ( s_actions().get( action_scroll_pagedown ) )
    {
        m_graph.start_y -= ( gi.rc.h - ImGui::GetTextLineHeightWithSpacing() * 4 );
    }
    else if ( s_actions().get( action_scroll_left ) )
    {
        start_ts = start_ts - 9 * m_graph.length_ts / 10;
    }
    else if ( s_actions().get( action_scroll_right ) )
    {
        start_ts = start_ts + 9 * m_graph.length_ts / 10;
    }
    else if ( s_actions().get( action_scroll_home ) )
    {
        start_ts = events.front().ts - NSECS_PER_MSEC;
    }
    else if ( s_actions().get( action_scroll_end ) )
    {
        start_ts = events.back().ts - m_graph.length_ts + NSECS_PER_MSEC;
    }

    if ( start_ts != m_graph.start_ts )
    {
        m_graph.start_ts = start_ts;
        m_graph.recalc_timebufs = true;
    }
}

void TraceWin::graph_render_options()
{
    ImGui::PushItemWidth( imgui_scale( 120.0f ) );
    if ( ImGui::InputText( "##Start", m_graph.time_start_buf, sizeof( m_graph.time_start_buf ),
                           ImGuiInputTextFlags_EnterReturnsTrue ) )
    {
        m_graph.start_ts = timestr_to_ts( m_graph.time_start_buf );
        m_graph.recalc_timebufs = true;
    }
    ImGui::PopItemWidth();

    ImGui::SameLine();
    if ( imgui_input_text2( "Length:", m_graph.time_length_buf, 120.0f,
                            ImGuiInputTextFlags_EnterReturnsTrue ) )
    {
        m_graph.length_ts = timestr_to_ts( m_graph.time_length_buf );
        m_graph.recalc_timebufs = true;
    }

    for ( size_t i = 0; i < ARRAY_SIZE( m_graph.ts_markers ); i++ )
    {
        if ( graph_marker_valid( i ) )
        {
            char label[ 64 ];
            snprintf_safe( label, "Marker %c:", ( char )( 'A' + i ) );

            ImGui::PushID( i );
            ImGui::SameLine();
            if ( imgui_input_text2( label, m_graph.marker_bufs[ i ] ) )
                graph_marker_set( i, 0, m_graph.marker_bufs[ i ] );
            ImGui::PopID();
        }
    }
    if ( graph_marker_valid( 0 ) && graph_marker_valid( 1 ) )
    {
        ImGui::SameLine();
        if ( imgui_input_text2( "AB Delta:", m_graph.marker_delta_buf ) )
            graph_marker_set( 1, m_graph.ts_markers[ 0 ] + timestr_to_ts( m_graph.marker_delta_buf ) );
    }

    ImGui::SameLine();
    ImGui::Spacing();
    ImGui::SameLine();
    bool m_do_graph_zoom_in = ImGui::SmallButton( "Zoom In" );

    ImGui::SameLine();
    bool m_do_graph_zoom_out = ImGui::SmallButton( "Zoom Out" );

    if ( m_do_graph_zoom_in || m_do_graph_zoom_out )
    {
        int64_t ts0 = m_graph.start_ts + m_graph.length_ts / 2;

        graph_zoom( ts0, m_graph.start_ts, m_do_graph_zoom_in );
    }

    {
        ImGui::SameLine();
        ImGui::PushItemWidth( imgui_scale( 120.0f ) );

        float posx = ImGui::GetCursorPosX();
        float newposx = ImGui::GetContentRegionMax().x - imgui_scale( 120.0f );

        ImGui::SetCursorPosX( std::max< float >( posx, newposx ) );

        if ( ImGui::InputText( "##End", m_graph.time_end_buf, sizeof( m_graph.time_end_buf ),
                               ImGuiInputTextFlags_EnterReturnsTrue ) )
        {
            m_graph.length_ts = timestr_to_ts( m_graph.time_end_buf ) - m_graph.start_ts;
            m_graph.recalc_timebufs = true;
        }

        ImGui::PopItemWidth();
    }

    if ( m_graph.recalc_timebufs )
    {
        strcpy_safe( m_graph.time_start_buf, ts_to_timestr( m_graph.start_ts, 4 ).c_str() );
        strcpy_safe( m_graph.time_end_buf, ts_to_timestr( m_graph.start_ts + m_graph.length_ts, 4 ).c_str() );
        strcpy_safe( m_graph.time_length_buf, ts_to_timestr( m_graph.length_ts, 4 ).c_str() );

        m_graph.recalc_timebufs = false;
    }
}

void TraceWin::graph_render_rows( graph_info_t &gi )
{
    uint32_t mouse_over_id = ( uint32_t )-1;

    for ( row_info_t &ri : gi.row_info )
    {
        float y = gi.rcwin.y + ri.row_y + gi.start_y;

        // If the mouse is over this row, render it now
        if ( gi.mouse_pos_in_rect( { gi.rc.x, y, gi.rc.w, ri.row_h } ) )
        {
            gi.set_pos_y( y, ri.row_h, &ri );
            graph_render_single_row( gi );

            mouse_over_id = ri.id;
            break;
        }
    }

    // Go through all rows and render them
    for ( row_info_t &ri : gi.row_info )
    {
        if ( ri.id != mouse_over_id )
        {
            float y = gi.rcwin.y + ri.row_y + gi.start_y;

            gi.set_pos_y( y, ri.row_h, &ri );

            if ( !imgui_is_rect_clipped( gi.rc ) )
                graph_render_single_row( gi );
        }
    }
}

void TraceWin::graph_render_zoomed_rows( graph_info_t &gi )
{
    float zoomhw_h = 0;
    bool render_zoomhw_after = false;
    row_info_t *ri = gi.prinfo_zoom_hw;

    if ( gi.prinfo_zoom_hw )
    {
        float y = gi.rcwin.y + gi.rcwin.h - ri->row_h;

        // Zoom hw height
        zoomhw_h = ri->row_h + ImGui::GetStyle().FramePadding.y;

        // If mouse is over our zoom hw row, render it now. Otherwise render after.
        render_zoomhw_after = !gi.mouse_pos_in_rect( { gi.rc.x, y, gi.rc.w, ri->row_h } );
        if ( !render_zoomhw_after )
        {
            gi.set_pos_y( y, ri->row_h, ri );
            graph_render_single_row( gi );
        }
    }

    gi.timeline_render_user = true;
    gi.set_pos_y( gi.rcwin.y, gi.rcwin.h - zoomhw_h, gi.prinfo_zoom );
    graph_render_single_row( gi );

    if ( render_zoomhw_after )
    {
        gi.set_pos_y( gi.rcwin.y + gi.rcwin.h - ri->row_h, ri->row_h, ri );
        graph_render_single_row( gi );
    }
}

void TraceWin::graph_render()
{
    GPUVIS_TRACE_BLOCK( __func__ );

    graph_info_t gi( *this );

    // Initialize our row size, location, etc information based on our graph row list
    gi.init_rows( m_graph.rows.m_graph_rows_list );

    // Make sure ts start and length values are sane
    graph_range_check_times();

    if ( gi.prinfo_zoom )
    {
        ImGui::SameLine();

        std::string label = string_format( "Unzoom '%s'", m_graph.zoom_row_name.c_str() );
        if ( ImGui::Button( label.c_str() ) )
            m_graph.zoom_row_name.clear();
    }

    if ( s_actions().get( action_focus_graph ) )
        ImGui::SetNextWindowFocus();

    ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 1.0f, 1.0f ) );
    ImGui::BeginChild( "eventgraph", ImVec2( 0.0f, gi.visible_graph_height ), false,
                       ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse );
    {
        // Initialize graphics info struct
        gi.init();
        gi.set_ts( m_graph.start_ts, m_graph.length_ts );

        // Clear graph background
        imgui_drawrect_filled( gi.rcwin, s_clrs().get( col_Graph_Bk ) );

        if ( gi.prinfo_zoom )
            graph_render_zoomed_rows( gi );
        else
            graph_render_rows( gi );

        // Render full screen stuff: graph ticks, vblanks, cursor pos, etc.
        gi.set_pos_y( gi.rcwin.y, gi.rcwin.h, NULL );

        graph_render_time_ticks( gi, imgui_scale( 16.0f ), imgui_scale( 4.0f ) );
        graph_render_vblanks( gi );
        graph_render_framemarker_frames( gi );
        graph_render_mouse_pos( gi );
        graph_render_eventids( gi );
        graph_render_mouse_selection( gi );
        graph_render_eventlist_selection( gi );

        // Render row labels (taking panning into consideration)
        gi.set_pos_y( gi.rcwin.y + ( gi.prinfo_zoom ? 0.0f : gi.start_y ), gi.rcwin.h, NULL );
        graph_render_row_labels( gi );

        // Handle right, left, pgup, pgdown, etc in graph
        graph_handle_keyboard_scroll( gi );

        // Render mouse tooltips, mouse selections, etc
        gi.set_pos_y( gi.rcwin.y, gi.rcwin.h, NULL );
        graph_handle_mouse( gi );

        // Handle graph hotkeys
        graph_handle_hotkeys( gi );
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();

    // Horizontal scrollbar
    graph_render_hscrollbar( gi );

    // Draggable resize graph row bar
    graph_render_resizer( gi );
}

void TraceWin::graph_render_hscrollbar( graph_info_t &gi )
{
    float scrollbar_size = ImGui::GetStyle().ScrollbarSize;
    int64_t min_ts = m_trace_events.m_events.front().ts - NSECS_PER_MSEC;
    int64_t max_ts = m_trace_events.m_events.back().ts;
    float pos = gi.rc.w * ( gi.ts0 - min_ts ) * gi.tsdxrcp;
    float width = gi.rc.w * ( max_ts - min_ts ) * gi.tsdxrcp;

    int target = -1;
    bool frame_markers = frame_markers_enabled();

    float w1 = ImGui::GetContentRegionAvailWidth();

    ImGui::SmallButton( "<<" );
    if ( ImGui::IsItemActive() )
    {
        m_graph.start_ts -= m_graph.length_ts / 12.0f;
        m_graph.recalc_timebufs = true;
    }
    ImGui::SameLine();

    if ( frame_markers )
    {
        if ( ImGui::SmallButton( "<" ) )
            target = m_frame_markers.m_frame_marker_left;
        ImGui::SameLine();
    }

    float w2 = ImGui::GetContentRegionAvailWidth();
    w2 = w2 - ( w1 - w2 );

    {
        int style_count = 0;

        if ( m_frame_markers.m_frame_marker_selected != -1 )
        {
            static ImGuiCol s_cols[] =
            {
                ImGuiCol_ScrollbarBg,
                ImGuiCol_ScrollbarGrab,
                ImGuiCol_ScrollbarGrabHovered,
                ImGuiCol_ScrollbarGrabActive
            };
            ImVec2 cursorpos = ImGui::GetCursorPos();
            std::string str = string_format( " (Frame #%d)", m_frame_markers.m_frame_marker_selected );
            ImVec2 textsize = ImGui::CalcTextSize( str.c_str() );

            for ( size_t i = 0; i < ARRAY_SIZE( s_cols ); i++ )
            {
                ImU32 col = ImGui::GetColorU32( s_cols[ i ] );

                ImGui::PushStyleColor( s_cols[ i ], ( col & ~IM_COL32_A_MASK ) | 0x7f000000 );
                style_count++;
            }

            textsize.x = cursorpos.x + ( w2 - textsize.x ) / 2;
            imgui_draw_text( textsize.x, cursorpos.y, s_clrs().get( col_BrightText ), str.c_str() );
        }

        ImGui::SetNextWindowContentSize( ImVec2( width, 0 ) );
        ImGui::BeginChild( "#graph_scrollbar", ImVec2( w2, scrollbar_size ),
                           false, ImGuiWindowFlags_AlwaysHorizontalScrollbar );

        if ( pos != m_graph.scroll_pos )
        {
            // Graph pos changed: scroll_x should be 0..maxX
            m_graph.scroll_x = ImGui::GetScrollMaxX() * pos / width;
            m_graph.scroll_pos = pos;

            ImGui::SetScrollX( m_graph.scroll_x );
        }
        else if ( m_graph.scroll_x != ImGui::GetScrollX() )
        {
            // Scrollbar changed: pct should be 0..1
            float pct = ImGui::GetScrollX() / ImGui::GetScrollMaxX();

            m_graph.start_ts = min_ts + pct * ( max_ts - min_ts );
            m_graph.recalc_timebufs = true;
        }

        ImGui::EndChild();

        ImGui::PopStyleColor( style_count );
    }
    ImGui::SameLine();

    if ( frame_markers )
    {
        if ( ImGui::SmallButton( ">" ) )
            target = m_frame_markers.m_frame_marker_right;
        ImGui::SameLine();
    }

    ImGui::SmallButton( ">>" );
    if ( ImGui::IsItemActive() )
    {
        m_graph.start_ts += m_graph.length_ts / 12.0f;
        m_graph.recalc_timebufs = true;
    }

    frame_markers_goto( target, true );
}

void TraceWin::graph_render_resizer( graph_info_t &gi )
{
    bool mouse_captured = ( m_graph.mouse_captured == MOUSE_CAPTURED_RESIZE_GRAPH );

    if ( s_opts().getb( OPT_ShowEventList ) || mouse_captured )
    {
        option_id_t opt = gi.prinfo_zoom ? OPT_GraphHeightZoomed : OPT_GraphHeight;

        ImGui::Button( "##resize_graph", ImVec2( ImGui::GetContentRegionAvailWidth(), imgui_scale( 4.0f ) ) );

        if ( mouse_captured || ImGui::IsItemHovered() )
            ImGui::SetMouseCursor( ImGuiMouseCursor_ResizeNS );

        if ( mouse_captured )
        {
            if ( !ImGui::IsMouseDown( 0 ) )
                m_graph.mouse_captured = MOUSE_NOT_CAPTURED;
            else
                s_opts().setf( opt, m_graph.mouse_capture_pos.y + ImGui::GetMouseDragDelta( 0 ).y );
        }
        else if ( ImGui::IsItemClicked() )
        {
            m_graph.mouse_captured = MOUSE_CAPTURED_RESIZE_GRAPH;
            m_graph.mouse_capture_pos.y = s_opts().getf( opt );
        }
    }
}

int TraceWin::graph_marker_menuitem( const char *label, bool check_valid, action_t action )
{
    int ret = -1;

    if ( !check_valid || graph_marker_valid( 0 ) || graph_marker_valid( 1 ) )
    {
        if ( ImGui::BeginMenu( label ) )
        {
            for ( size_t i = 0; i < ARRAY_SIZE( m_graph.ts_markers ); i++ )
            {
                if ( !check_valid || graph_marker_valid( i ) )
                {
                    std::string shortcut;
                    std::string mlabel = s_textclrs().bright_str( std::string( 1, ( 'A' + i ) ) );

                    ImGui::PushID( label );

                    if ( action != action_nil )
                        shortcut = s_actions().hotkey_str( ( action_t )( action + i ) );
                    if ( graph_marker_valid( i ) )
                        mlabel += " (" + ts_to_timestr( m_graph.ts_markers[ i ], 2 ) + ")";

                    if ( ImGui::MenuItem( mlabel.c_str(), shortcut.c_str() ) )
                        ret = i;

                    ImGui::PopID();
                }
            }

            ImGui::EndMenu();
        }
    }

    return ret;
}

bool TraceWin::graph_has_saved_locs()
{
    for ( size_t i = 0; i < m_graph.saved_locs.size(); i++ )
    {
        if ( m_graph.saved_locs[ i ].second )
            return true;
    }

    return false;
}

bool TraceWin::graph_render_popupmenu( graph_info_t &gi )
{
    option_id_t optid = OPT_Invalid;

    if ( !ImGui::BeginPopup( "GraphPopup" ) )
        return false;

    const std::string &row_name = m_graph.mouse_over_row_name;
    std::string row_name_bright = s_textclrs().bright_str( row_name );

    if ( !row_name.empty() )
    {
        imgui_text_bg( ImGui::GetStyleColorVec4( ImGuiCol_Header ), "Options for '%s'",
                       row_name_bright.c_str() );
    }
    else
    {
        imgui_text_bg( ImGui::GetStyleColorVec4( ImGuiCol_Header ), "Options" );
    }
    ImGui::Separator();

    // Zoom in / out
    if ( m_graph.zoom_loc.first != INT64_MAX )
    {
        std::string len = ts_to_timestr( m_graph.zoom_loc.second, 2 );
        std::string label = string_format( "Zoom out to %s", len.c_str() );

        if ( ImGui::MenuItem( label.c_str(), s_actions().hotkey_str( action_graph_zoom_mouse ).c_str() ) )
        {
            m_graph.start_ts = m_graph.zoom_loc.first;
            m_graph.length_ts = m_graph.zoom_loc.second;
            m_graph.recalc_timebufs = true;

            m_graph.zoom_loc = std::make_pair( INT64_MAX, INT64_MAX );
        }
    }
    else if ( ImGui::MenuItem( "Zoom in to 3.00ms", s_actions().hotkey_str( action_graph_zoom_mouse ).c_str() ) )
    {
        int64_t newlen = 3 * NSECS_PER_MSEC;

        m_graph.zoom_loc = std::make_pair( m_graph.start_ts, m_graph.length_ts );
        graph_zoom( m_graph.ts_marker_mouse, gi.ts0, false, newlen );
    }

    // Unzoom row
    if ( !m_graph.zoom_row_name.empty() )
    {
        std::string label = string_format( "Unzoom row '%s'", m_graph.zoom_row_name.c_str() );

        if ( ImGui::MenuItem( label.c_str(), s_actions().hotkey_str( action_graph_zoom_row ).c_str() ) )
        {
            m_graph.zoom_row_name.clear();
        }
    }

    // Zoom / Hide row
    if ( !row_name.empty() )
    {
        std::string label;

        if ( is_graph_row_zoomable() )
        {
            label = string_format( "Zoom row '%s'", row_name_bright.c_str() );

            if ( ImGui::MenuItem( label.c_str(), s_actions().hotkey_str( action_graph_zoom_row ).c_str() ) )
                zoom_graph_row();
        }

        optid = get_comm_option_id( row_name.c_str(), m_graph.mouse_over_row_type );
        label = string_format( "Hide row '%s'", row_name_bright.c_str() );

        if ( ImGui::MenuItem( label.c_str(), s_actions().hotkey_str( action_graph_hide_row ).c_str() ) )
            m_graph.rows.show_row( row_name, GraphRows::HIDE_ROW );

        label = string_format( "Hide row '%s' and below", row_name_bright.c_str() );
        if ( ImGui::MenuItem( label.c_str() ) )
            m_graph.rows.show_row( row_name, GraphRows::HIDE_ROW_AND_ALL_BELOW );

        if ( m_graph.mouse_over_row_type == LOC_TYPE_Comm )
        {
            const tgid_info_t *tgid_info = m_trace_events.tgid_from_commstr( row_name.c_str() );

            if ( tgid_info )
            {
                label = string_format( "Hide rows for process '%s'", tgid_info->commstr_clr );

                if ( ImGui::MenuItem( label.c_str() ) )
                    m_graph.rows.show_tgid_rows( tgid_info, GraphRows::HIDE_ROW );
            }
        }
    }

    // Show Row...
    if ( !m_graph.hidden_rows.empty() )
    {
        std::vector< const tgid_info_t * > tgids_hidden;

        if ( ImGui::BeginMenu( "Show row" ) )
        {
            if ( ImGui::MenuItem( "All Rows" ) )
                m_graph.rows.show_row( "", GraphRows::SHOW_ALL_ROWS );

            ImGui::Separator();

            for ( const GraphRows::graph_rows_info_t &entry : m_graph.hidden_rows )
            {
                const tgid_info_t *tgid_info;

                if ( ( entry.type == LOC_TYPE_Comm ) &&
                     ( tgid_info = m_trace_events.tgid_from_commstr( entry.row_name.c_str() ) ) )
                {
                    if ( std::find( tgids_hidden.begin(), tgids_hidden.end(), tgid_info ) == tgids_hidden.end() )
                    {
                        std::string label = string_format( "Process '%s' (%lu threads)",
                                                           tgid_info->commstr_clr,
                                                           tgid_info->pids.size() );

                        if ( ImGui::MenuItem( label.c_str() ) )
                            m_graph.rows.show_tgid_rows( tgid_info, GraphRows::SHOW_ROW );

                        tgids_hidden.push_back( tgid_info );
                    }
                }
            }

            if ( tgids_hidden.size() )
                ImGui::Separator();

            for ( const GraphRows::graph_rows_info_t &entry : m_graph.hidden_rows )
            {
                const char *entry_name = entry.row_name.c_str();
                ftrace_row_info_t *ftrace_row_info = m_trace_events.get_ftrace_row_info( entry_name );
                size_t entry_count = ftrace_row_info ? ftrace_row_info->count : entry.event_count;
                const char *commstr = ( entry.type == LOC_TYPE_Comm ) ?
                            m_trace_events.tgidcomm_from_commstr( entry_name ) : entry_name;
                const std::string label = string_format( "%s (%lu events)",
                                                         commstr, entry_count );

                if ( ImGui::MenuItem( label.c_str() ) )
                {
                    m_graph.rows.show_row( entry_name, GraphRows::SHOW_ROW );
                }
            }

            ImGui::EndMenu();
        }
    }

    // Move row after...
    if ( !row_name.empty() )
    {
        std::string move_label = string_format( "Move '%s' after", row_name_bright.c_str() );

        if ( ImGui::BeginMenu( move_label.c_str() ) )
        {
            for ( const GraphRows::graph_rows_info_t &entry : m_graph.rows.m_graph_rows_list )
            {
                if ( !entry.hidden && ( entry.row_name != row_name ) )
                {
                    const char *commstr = ( entry.type == LOC_TYPE_Comm ) ?
                                m_trace_events.tgidcomm_from_commstr( entry.row_name.c_str() ) :
                                entry.row_name.c_str();
                    if ( ImGui::MenuItem( commstr ) )
                    {
                        m_graph.rows.move_row( row_name, entry.row_name );
                        ImGui::CloseCurrentPopup();
                        break;
                    }
                }
            }

            ImGui::EndMenu();
        }
    }

    // Create Plot for hovered event
    if ( is_valid_id( gi.hovered_eventid ) &&
         strncmp( row_name.c_str(), "plot:", 5 ) )
    {
        const trace_event_t &event = m_trace_events.m_events[ gi.hovered_eventid ];
        const std::string plot_str = CreatePlotDlg::get_plot_str( event );

        if ( !plot_str.empty() )
        {
            std::string plot_label = std::string( "Create Plot for " ) + plot_str;

            if ( ImGui::MenuItem( plot_label.c_str() ) )
                m_create_plot_eventid = event.id;
        }
    }

    // Change row size. Ie "Gfx size: 10"
    if ( !gi.prinfo_zoom && ( optid != OPT_Invalid ) )
        s_opts().render_imgui_opt( optid );

    ImGui::Separator();

    if ( !row_name.empty() )
    {
        float valf = m_graph.rows.get_row_scale_ts( row_name );
        std::string label = string_format( "Scale row time: %sx", "%.02f" );

        ImGui::PushItemWidth( imgui_scale( 200.0f ) );
        if ( ImGui::SliderFloat( "##opt_valf", &valf, 1.0f, 100.0f, label.c_str() ) )
            m_graph.rows.m_graph_row_scale_ts.m_map[ row_name ] = string_format( "%.02f", valf );
        ImGui::PopItemWidth();

        ImGui::Separator();
    }

    // Set / Goto / Clear Markers
    {
        int idx = graph_marker_menuitem( "Set Marker", false, action_graph_set_markerA );
        if ( idx >= 0 )
            graph_marker_set( idx, m_graph.ts_marker_mouse );

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

    // Save / Restore Locations
    {
        auto get_location_label_lambda = [this]( size_t i )
        {
            auto &pair = m_graph.saved_locs[ i ];
            std::string start = ts_to_timestr( pair.first, 6, "" );
            std::string len = ts_to_timestr( pair.second, 6, "" );

            return string_format( "Start:%s Length:%s", start.c_str(), len.c_str() );
        };

        if ( ImGui::BeginMenu( "Save Location" ) )
        {
            for ( size_t i = 0; i < m_graph.saved_locs.size(); i++ )
            {
                std::string label = get_location_label_lambda( i );
                action_t action = ( action_t )( action_graph_save_location1 + i );

                if ( ImGui::MenuItem( label.c_str(),
                    s_actions().hotkey_str( action ).c_str() ) )
                {
                    m_graph.saved_locs[ i ] = std::make_pair( m_graph.start_ts, m_graph.length_ts );
                    break;
                }
            }

            ImGui::EndMenu();
        }

        if ( graph_has_saved_locs() && ImGui::BeginMenu( "Restore Location" ) )
        {
            for ( size_t i = 0; i < m_graph.saved_locs.size(); i++ )
            {
                if ( m_graph.saved_locs[ i ].second )
                {
                    std::string label = get_location_label_lambda( i );
                    action_t action = ( action_t )( action_graph_restore_location1 + i );

                    if ( ImGui::MenuItem( label.c_str(),
                            s_actions().hotkey_str( action ).c_str() ) )
                    {
                        m_graph.start_ts = m_graph.saved_locs[ i ].first;
                        m_graph.length_ts = m_graph.saved_locs[ i ].second;
                        m_graph.recalc_timebufs = true;
                    }
                }
            }

            ImGui::EndMenu();
        }
    }

    ImGui::Separator();

    // New Graph Row
    if ( ImGui::MenuItem( "Add New Graph Row..." ) )
    {
        m_create_graph_row_eventid = is_valid_id( gi.hovered_eventid ) ?
                    gi.hovered_eventid : m_trace_events.m_events.size();
    }

    // Graph Row Filters
    if ( ImGui::BeginMenu( "Row Filters") )
    {
        const char *enablelabel = m_row_filters_enabled ?
                    "Disable Row Filters" : "Enable Row Filters";
        const std::string shortcut = s_actions().hotkey_str( action_toggle_frame_filters );

        if ( ImGui::MenuItem( "Create Row Filter..." ) )
        {
            m_create_row_filter_dlg_show = true;
            m_create_row_filter_dlg_rowname = m_graph.mouse_over_row_name;
        }

        if ( ImGui::MenuItem( enablelabel, shortcut.c_str() ) )
            m_row_filters_enabled = !m_row_filters_enabled;

        ImGui::Separator();

        if ( !m_graph.mouse_over_row_name.empty() &&
             !m_create_row_filter_dlg.m_previous_filters.empty() )
        {
            RowFilters rowfilters( m_graph_row_filters, m_graph.mouse_over_row_name );

            // Go through all the row filters
            for ( const std::string &val : m_create_row_filter_dlg.m_previous_filters )
            {
                size_t idx = rowfilters.find_filter( val );
                bool selected = ( idx != ( size_t )-1 );

                if ( ImGui::MenuItem( val.c_str(), NULL, selected, m_row_filters_enabled ) )
                {
                    rowfilters.toggle_filter( m_trace_events, idx, val );
                }
            }
        }

        ImGui::EndMenu();
    }

    if ( m_graph.cpu_filter_tgid || m_graph.cpu_filter_pid )
    {
        std::string label = string_format( "Clear %s Filter: %d",
            m_graph.cpu_filter_tgid ? "tgid" : "pid", m_graph.cpu_filter_pid );

        if ( ImGui::MenuItem( label.c_str(), s_actions().hotkey_str( action_graph_show_hovered_pid ).c_str() ) )
        {
            m_graph.cpu_filter_tgid = 0;
            m_graph.cpu_filter_pid = 0;
            m_graph.cpu_timeline_pids.clear();
        }
    }
    else if ( is_valid_id( gi.hovered_eventid ) )
    {
        const trace_event_t &event = get_event( gi.hovered_eventid );
        const tgid_info_t *tgid_info = m_trace_events.tgid_from_pid( event.pid );

        std::string label = string_format( "Set pid filter: %d", event.pid );
        if ( ImGui::MenuItem( label.c_str(), s_actions().hotkey_str( action_graph_show_hovered_pid ).c_str() ) )
            m_graph.cpu_filter_pid = event.pid;

        if ( tgid_info )
        {
            label = string_format( "Set tgid filter: %d", tgid_info->tgid );
            if ( ImGui::MenuItem( label.c_str(), s_actions().hotkey_str( action_graph_show_hovered_tgid ).c_str() ) )
            {
                m_graph.cpu_filter_pid = tgid_info->tgid;
                m_graph.cpu_filter_tgid = tgid_info->tgid;
            }
        }

        if ( m_graph.cpu_filter_pid )
        {
            m_graph.cpu_timeline_pids.insert( m_graph.cpu_filter_pid );

            if ( m_graph.cpu_filter_tgid )
            {
                for ( int pid : tgid_info->pids )
                    m_graph.cpu_timeline_pids.insert( pid );
            }
        }
    }

    // Frame Markers
    {
        if ( is_valid_id( gi.hovered_eventid ) &&
             ImGui::MenuItem( "Set Frame Markers..." ) )
        {
            const trace_event_t &event = get_event( gi.hovered_eventid );

            m_create_filter_eventid = event.id;
        }

        if ( ImGui::MenuItem( "Edit Frame Markers..." ) )
            m_create_filter_eventid = m_trace_events.m_events.size();

        if ( m_frame_markers.m_left_frames.size() &&
             ImGui::MenuItem( "Clear Frame Markers" ) )
        {
            m_frame_markers.m_left_frames.clear();
            m_frame_markers.m_right_frames.clear();
        }
    }

    ImGui::Separator();

    s_opts().render_imgui_options();

    if ( s_actions().get( action_escape ) )
        ImGui::CloseCurrentPopup();

    ImGui::EndPopup();
    return true;
}

static std::string task_state_to_str( int state )
{
    std::string ret;
    static const struct
    {
        int mask;
        const char *name;
    } s_vals[] =
    {
#define _XTAG( _x ) { _x, #_x }
        _XTAG( TASK_RUNNING ),
        _XTAG( TASK_INTERRUPTIBLE ),
        _XTAG( TASK_UNINTERRUPTIBLE ),
        _XTAG( TASK_STOPPED ),
        _XTAG( TASK_TRACED ),
        _XTAG( EXIT_DEAD ),
        _XTAG( EXIT_ZOMBIE ),
        _XTAG( TASK_PARKED ),
        _XTAG( TASK_DEAD ),
#undef _XTAG
    };

    if ( !state )
        return "TASK_RUNNING";

    for ( size_t i = 0; i < ARRAY_SIZE( s_vals ); i++ )
    {
        if ( state & s_vals[ i ].mask )
        {
            if ( !ret.empty() )
                ret += " ";
            ret += s_vals[ i ].name;
        }
    }

    return ret;
}

void TraceWin::graph_mouse_tooltip_rowinfo( std::string &ttip, graph_info_t &gi, int64_t mouse_ts )
{
    const std::string &row_name = m_graph.mouse_over_row_name;
    uint32_t hashval = hashstr32( row_name );
    row_filter_t *row_filters = m_graph_row_filters.get_val( hashval );

    if ( !row_name.empty() )
    {
        ttip += "\nRow: ";
        if ( m_graph.mouse_over_row_type == LOC_TYPE_Comm )
            ttip += m_trace_events.tgidcomm_from_commstr( row_name.c_str() );
        else
            ttip += s_textclrs().bright_str( m_graph.mouse_over_row_name );
    }

    if ( m_graph.mouse_over_row_type == LOC_TYPE_Plot )
    {
        GraphPlot &plot = m_trace_events.get_plot( row_name.c_str() );
        ttip += "\nFilter: " + plot.m_filter_str;
    }
    else if ( !row_name.empty() && ( row_name != m_graph.mouse_over_row_filter_expr ) )
    {
        ttip += "\nFilter: " + m_graph.mouse_over_row_filter_expr;
    }

    if ( row_filters && !row_filters->filters.empty() )
    {
        std::string str;

        if ( m_row_filters_enabled )
            str = "\nRow Filters (enabled):";
        else
            str = "\nRow Filters (disabled):";

        for ( const std::string &filter : row_filters->filters )
            str += "\n  " + filter;

        ttip += s_textclrs().brightcomp_str( str );
    }

    if ( m_graph.cpu_filter_pid || m_graph.cpu_filter_tgid )
    {
        std::string str;

        if ( m_graph.cpu_filter_tgid )
            str = string_format( "\nTgid filter: %d", m_graph.cpu_filter_tgid );
        else
            str = string_format( "\nPid filter: %d", m_graph.cpu_filter_pid );

        ttip += s_textclrs().brightcomp_str( str );
    }
}

void TraceWin::graph_mouse_tooltip_vblanks( std::string &ttip, graph_info_t &gi, int64_t mouse_ts )
{
    const std::vector< uint32_t > *vblank_locs = m_trace_events.get_tdopexpr_locs( "$name=drm_vblank_event" );

    if ( vblank_locs )
    {
        int64_t prev_vblank_ts = INT64_MAX;
        int64_t next_vblank_ts = INT64_MAX;
        int eventid = ts_to_eventid( mouse_ts );
        size_t idx = vec_find_eventid( *vblank_locs, eventid );
        size_t idxmax = std::min< size_t >( idx + 20, vblank_locs->size() );

        for ( idx = ( idx > 10 ) ? ( idx - 10 ) : 0; idx < idxmax; idx++ )
        {
            trace_event_t &event = get_event( vblank_locs->at( idx ) );

            if ( s_opts().getcrtc( event.crtc ) )
            {
                if ( event.ts < mouse_ts )
                {
                    if ( mouse_ts - event.ts < prev_vblank_ts )
                        prev_vblank_ts = mouse_ts - event.ts;
                }
                if ( event.ts > mouse_ts )
                {
                    if ( event.ts - mouse_ts < next_vblank_ts )
                        next_vblank_ts = event.ts - mouse_ts;
                }
            }
        }

        if ( prev_vblank_ts != INT64_MAX )
            ttip += "\nPrev vblank: -" + ts_to_timestr( prev_vblank_ts, 2 );
        if ( next_vblank_ts != INT64_MAX )
            ttip += "\nNext vblank: " + ts_to_timestr( next_vblank_ts, 2 );
    }
}

void TraceWin::graph_mouse_tooltip_markers( std::string &ttip, graph_info_t &gi, int64_t mouse_ts )
{
    if ( graph_marker_valid( 0 ) )
        ttip += "\nMarker A: " + ts_to_timestr( m_graph.ts_markers[ 0 ] - mouse_ts, 2 );
    if ( graph_marker_valid( 1 ) )
        ttip += "\nMarker B: " + ts_to_timestr( m_graph.ts_markers[ 1 ] - mouse_ts, 2 );

    if ( gi.hovered_framemarker_frame != -1 )
    {
        int64_t ts = m_frame_markers.get_frame_len( m_trace_events, gi.hovered_framemarker_frame );

        ttip += string_format( "\n\nFrame %d (", gi.hovered_framemarker_frame );
        ttip += ts_to_timestr( ts, 4 ) + ")";
    }
}

void TraceWin::graph_mouse_tooltip_sched_switch( std::string &ttip, graph_info_t &gi, int64_t mouse_ts )
{
    if ( gi.sched_switch_bars.empty() )
        return;

    ttip += "\n";

    for ( uint32_t id : gi.sched_switch_bars )
    {
        trace_event_t &event = get_event( id );
        const char *prev_comm = get_event_field_val( event, "prev_comm" );

        if ( prev_comm )
        {
            int prev_pid = event.pid;
            int prev_state = atoi( get_event_field_val( event, "prev_state" ) );
            int task_state = prev_state & ( TASK_REPORT_MAX - 1 );
            const std::string task_state_str = task_state_to_str( task_state );
            std::string timestr = ts_to_timestr( event.duration, 4 );

            ttip += string_format( "\n%s%u%s sched_switch %s%s-%d%s %sCpu:%d%s (%s) %s",
                                   gi.clr_bright, event.id, gi.clr_def,
                                   gi.clr_brightcomp, prev_comm, prev_pid, gi.clr_def,
                                   gi.clr_bright, event.cpu, gi.clr_def,
                                   timestr.c_str(),
                                   task_state_str.c_str() );

            int64_t *val = m_trace_events.m_sched_switch_time_pid.get_val( prev_pid );
            if ( val )
            {
                ttip += string_format( " (Time Pct:%.2f%%)",
                                       ( *val * 100.0 / m_trace_events.m_sched_switch_time_total ) );
            }
        }
    }
}

void TraceWin::graph_mouse_tooltip_hovered_amd_fence_signaled( std::string &ttip, graph_info_t &gi, int64_t mouse_ts )
{
    if ( !is_valid_id( gi.hovered_fence_signaled ) )
        return;

    const trace_event_t &event_hov = get_event( gi.hovered_fence_signaled );
    uint32_t gfxcontext_hash = m_trace_events.get_event_gfxcontext_hash( event_hov );
    const std::vector< uint32_t > *plocs = m_trace_events.get_gfxcontext_locs( gfxcontext_hash );

    ttip += string_format( "\n\n%s",
                               m_trace_events.tgidcomm_from_commstr( event_hov.user_comm ) );

    for ( uint32_t id : *plocs )
    {
        const trace_event_t &event = get_event( id );
        const char *name = event.get_timeline_name( event.name );
        std::string timestr = ts_to_timestr( event.duration, 4 );

        if ( gi.hovered_items.empty() )
            m_eventlist.highlight_ids.push_back( id );

        ttip += string_format( "\n  %s%u%s %s duration: %s",
                                   gi.clr_bright, event.id, gi.clr_def,
                                   name,
                                   s_textclrs().mstr( timestr, event_hov.color ).c_str() );
    }

    plocs = m_trace_events.m_gfxcontext_msg_locs.get_locations_u32( gfxcontext_hash );
    if ( plocs )
    {
        ttip += "\n";

        for ( uint32_t id : *plocs )
        {
            const trace_event_t &event = get_event( id );
            const char *msg = get_event_field_val( event, "msg" );

            ttip += string_format( "\n  %s%s%s", gi.clr_bright, msg, gi.clr_def );
        }
    }
}

void TraceWin::graph_mouse_tooltip_hovered_items( std::string &ttip, graph_info_t &gi, int64_t mouse_ts )
{
    int64_t dist_ts = INT64_MAX;

    gi.hovered_eventid = INVALID_ID;

    if ( gi.hovered_items.empty() )
        return;

    // Sort hovered items array by id
    std::sort( gi.hovered_items.begin(), gi.hovered_items.end(),
               [=]( const graph_info_t::hovered_t& lx, const graph_info_t::hovered_t &rx )
    {
        return lx.eventid < rx.eventid;
    } );

    ttip += "\n";

    // Show tooltip with the closest events we could drum up
    for ( size_t i = 0; i < gi.hovered_items.size(); i++ )
    {
        graph_info_t::hovered_t &hov = gi.hovered_items[ i ];
        trace_event_t &event = get_event( hov.eventid );
        i915_type_t i915_type = get_i915_reqtype( event );

        m_eventlist.highlight_ids.push_back( event.id );

        if ( !i && ( i915_type < i915_req_Max ) )
        {
            ttip += "\n";
            ttip += m_trace_events.tgidcomm_from_commstr( event.comm );
        }

        // Add event id and distance from cursor to this event
        ttip += string_format( "\n%s%u%s %c%s",
                                   gi.clr_bright, hov.eventid, gi.clr_def,
                                   hov.neg ? '-' : ' ',
                                   ts_to_timestr( hov.dist_ts, 4 ).c_str() );

        // If this isn't an ftrace print event, add the event name
        if ( !event.is_ftrace_print() )
            ttip += std::string( " " ) + event.name;

        // If this is a vblank event, add the crtc
        if ( event.crtc >= 0 )
            ttip += std::to_string( event.crtc );

        if ( i915_type < i915_req_Max )
        {
            const char *ctxstr = get_event_field_val( event, "ctx", NULL );

            if ( ctxstr )
            {
                ttip += string_format( " key:[%s%s%s-%s%u%s]",
                                           gi.clr_bright, ctxstr, gi.clr_def,
                                           gi.clr_bright, event.seqno, gi.clr_def );
            }
            else
            {
                ttip += string_format( " gkey:[%s%u%s]", gi.clr_bright, event.seqno, gi.clr_def );
            }

            const char *global = get_event_field_val( event, "global_seqno", NULL );
            if ( !global )
                global = get_event_field_val( event, "global", NULL );
            if ( global && atoi( global ) )
                ttip += string_format( " gkey:[%s%s%s]", gi.clr_bright, global, gi.clr_def );

            if ( ( event.color_index >= col_Graph_Bari915Queue ) &&
                 ( event.color_index <= col_Graph_Bari915CtxCompleteDelay ) )
            {
                char buf[ 6 ];
                const char *str;
                ImU32 color = s_clrs().get( event.color_index );

                if ( event.color_index == col_Graph_Bari915Queue )
                    str = " queue: ";
                else if ( event.color_index == col_Graph_Bari915SubmitDelay )
                    str = " submit-delay: ";
                else if ( event.color_index == col_Graph_Bari915ExecuteDelay )
                    str = " execute-delay: ";
                else if ( event.color_index == col_Graph_Bari915Execute )
                    str = " execute: ";
                else // if ( event.color_index == col_Graph_Bari915CtxCompleteDelay )
                    str = " context-complete-delay: ";

                ttip += s_textclrs().set( buf, color );
                ttip += str;
            }
        }
        else if ( event.is_ftrace_print() )
        {
            // Add colored string for ftrace print events
            const char *buf = get_event_field_val( event, "buf" );

            if ( buf[ 0 ] )
            {
                ttip += " " + s_textclrs().mstr( buf, event.color );
                ttip += m_trace_events.get_ftrace_ctx_str( event );
            }
        }
        else if ( event.is_sched_switch() )
        {
            const char *prev_comm_str = get_event_field_val( event, "prev_comm" );

            if ( prev_comm_str[ 0 ] )
            {
                int prev_pid = event.pid;
                const char *prev_comm = m_trace_events.comm_from_pid( prev_pid, prev_comm_str );

                ttip += string_format( " %s-%d", prev_comm, prev_pid );
            }
        }

        if ( event.has_duration() )
        {
            std::string timestr = ts_to_timestr( event.duration, 4 );

            ttip += " (" + timestr + ")" + gi.clr_def;
        }

        if ( hov.dist_ts < dist_ts )
        {
            gi.hovered_eventid = hov.eventid;
            dist_ts = hov.dist_ts;
        }
    }
}

void TraceWin::graph_mouse_tooltip( graph_info_t &gi, int64_t mouse_ts )
{
    std::string ttip;

    m_eventlist.highlight_ids.clear();

    if ( gi.mouse_pos_scaled_ts != INT64_MIN )
    {
        ttip += string_format( "\"%s\" Time: %s\nGraph ",
                                   m_graph.mouse_over_row_name.c_str(),
                                   ts_to_timestr( gi.mouse_pos_scaled_ts, 6, "" ).c_str() );
    }
    ttip += "Time: " + ts_to_timestr( mouse_ts, 6, "" );

    graph_mouse_tooltip_rowinfo( ttip, gi, mouse_ts );
    graph_mouse_tooltip_vblanks( ttip, gi, mouse_ts );
    graph_mouse_tooltip_markers( ttip, gi, mouse_ts );
    graph_mouse_tooltip_sched_switch( ttip, gi, mouse_ts );
    graph_mouse_tooltip_hovered_items( ttip, gi, mouse_ts );
    graph_mouse_tooltip_hovered_amd_fence_signaled( ttip, gi, mouse_ts );

    ImGui::SetTooltip( "%s", ttip.c_str() );

    if ( s_actions().get( action_graph_pin_tooltip ) )
    {
        m_ttip.str = ttip;
        m_ttip.visible = true;
    }
}

void TraceWin::graph_handle_mouse_captured( graph_info_t &gi )
{
    // Uncapture mouse if user hits escape
    if ( m_graph.mouse_captured && s_actions().get( action_escape ) )
    {
        m_graph.mouse_captured = MOUSE_NOT_CAPTURED;
        return;
    }

    bool is_mouse_down = ImGui::IsMouseDown( 0 );
    bool mouse_pos_valid = ImGui::IsMousePosValid( &gi.mouse_pos );

    // When mouse goes up and mouse is out of window area, mouse_pos goes
    //  to invalid (-FLT_MAX) values. So store away the last known valid
    //  mouse pos here so we can zoom there on mouse up.
    if ( mouse_pos_valid )
        m_graph.mouse_capture_last = gi.mouse_pos;

    if ( ( m_graph.mouse_captured == MOUSE_CAPTURED_ZOOM ) ||
         ( m_graph.mouse_captured == MOUSE_CAPTURED_SELECT_AREA ) )
    {
        // shift + click: zoom area
        int64_t event_ts0 = gi.screenx_to_ts( m_graph.mouse_capture_pos.x );
        int64_t event_ts1 = gi.screenx_to_ts( m_graph.mouse_capture_last.x );

        if ( event_ts0 > event_ts1 )
            std::swap( event_ts0, event_ts1 );

        if ( is_mouse_down && mouse_pos_valid )
        {
            std::string time_buf0 = ts_to_timestr( event_ts0, 6, "" );
            std::string time_buf1 = ts_to_timestr( event_ts1 - event_ts0, 6 );

            // Show tooltip with starting time and length of selected area.
            ImGui::SetTooltip( "%s (%s)", time_buf0.c_str(), time_buf1.c_str() );
        }
        else if ( m_graph.mouse_captured == MOUSE_CAPTURED_ZOOM )
        {
            m_graph.zoom_loc = std::make_pair( m_graph.start_ts, m_graph.length_ts );

            m_graph.start_ts = event_ts0;
            m_graph.length_ts = event_ts1 - event_ts0;
            m_graph.recalc_timebufs = true;
        }
    }
    else if ( m_graph.mouse_captured == MOUSE_CAPTURED_PAN )
    {
        // click: pan
        if ( is_mouse_down && mouse_pos_valid )
        {
            float dx = gi.mouse_pos.x - m_graph.mouse_capture_pos.x;
            int64_t tsdiff = gi.dx_to_ts( dx );

            m_graph.start_ts -= tsdiff;
            m_graph.recalc_timebufs = true;

            m_graph.start_y += gi.mouse_pos.y - m_graph.mouse_capture_pos.y;

            m_graph.mouse_capture_pos = gi.mouse_pos;
        }
    }

    if ( !is_mouse_down )
    {
        // Mouse is no longer down, uncapture mouse...
        m_graph.mouse_captured = MOUSE_NOT_CAPTURED;
    }
}

// Mouse is over our active graph window
void TraceWin::graph_handle_mouse_over( graph_info_t &gi )
{
    ImGuiIO& io = ImGui::GetIO();
    int64_t mouse_ts = gi.screenx_to_ts( gi.mouse_pos.x );

    m_graph.ts_marker_mouse = mouse_ts;

    // Set the tooltip
    graph_mouse_tooltip( gi, mouse_ts );

    // Check for clicking, wheeling, etc.
    if ( ImGui::IsMouseDoubleClicked( 0 ) )
    {
        if ( gi.mouse_pos_scaled_ts != INT64_MIN )
        {
            // Double clicking on a scaled graph row - move to that location
            m_graph.start_ts = gi.mouse_pos_scaled_ts - m_graph.length_ts / 2;
            m_graph.recalc_timebufs = true;
        }
    }
    else if ( ImGui::IsMouseClicked( 0 ) )
    {
        // click: pan
        // shift + click: zoom
        // ctrl + click: select area
        uint32_t mask = ( io.KeyCtrl << 0 ) | ( io.KeyAlt << 1 ) | ( io.KeyShift << 2 );

        if ( !mask )
            m_graph.mouse_captured = MOUSE_CAPTURED_PAN;
        else if ( mask == ( 1 << 2 ) )
            m_graph.mouse_captured = MOUSE_CAPTURED_ZOOM;
        else if ( mask == ( 1 << 0 ) )
            m_graph.mouse_captured = MOUSE_CAPTURED_SELECT_AREA;

        if ( m_graph.mouse_captured != MOUSE_NOT_CAPTURED )
            m_graph.mouse_capture_pos = gi.mouse_pos;
    }
    else if ( ImGui::IsMouseClicked( 1 ) )
    {
        // right click: popup menu
        m_graph.popupmenu = true;

        m_graph.hidden_rows = m_graph.rows.get_hidden_rows_list();

        ImGui::OpenPopup( "GraphPopup" );
    }
    else if ( io.MouseWheel )
    {
        bool zoomin = ( io.MouseWheel > 0.0f );

        graph_zoom( mouse_ts, gi.ts0, zoomin );
    }
}

void TraceWin::graph_handle_mouse( graph_info_t &gi )
{
    // If we've got an active popup menu, render it.
    if ( m_graph.popupmenu )
    {
        m_graph.popupmenu = TraceWin::graph_render_popupmenu( gi );
        return;
    }

    m_graph.ts_marker_mouse = -1;

    if ( m_graph.mouse_captured )
        graph_handle_mouse_captured( gi );
    else if ( gi.mouse_over )
        graph_handle_mouse_over( gi );

    // Update graph last_hovered_eventid
    m_graph.last_hovered_eventid = gi.hovered_eventid;
}
