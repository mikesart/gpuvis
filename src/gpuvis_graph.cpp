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
#include <functional>
#include <set>
#include <unordered_map>
#include <vector>

#include <SDL.h>

#include "imgui/imgui.h"

#include "gpuvis_macros.h"
#include "stlini.h"
#include "trace-cmd/trace-read.h"
#include "gpuvis_utils.h"
#include "gpuvis.h"

/*
  **** TODO list... ****

  Check if entire rows are clipped when drawing...

  Feedback:
    - the gfx waterfall view was confusing to everyone, zoomed in or not.
    They were all expecting something like the gpuview "stacked" view where
    it only overlaps if it has to. Not sure if it's just a matter of habit
    or if we should have it as an alternate method.
*/

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

/*
    Linux scheduler events:

    sched_switch (scheduler context-switch)
      prev_comm: thread_1
      prev_pid: 1154
      prev_state: [0|1|64] TASK_RUNNING:0, TASK_INTERRUPTABLE:1, TASK_UNINTERRUPTIBLE:2, TASK_DEAD:64, etc.
      next_comm: swapper/2
      next_pid: 0

    sched_wakeup / sched_wakeup_new (tracepoint called when task is actually woken)
      pid: 1144
      success: 1
      target_cpu: 4

    sched_migrate_task (task migrated to new cpu)
      com: rcu_sched
      pid: 8
      orig_cpu: 1
      dest_cpu: 4

    sched_process_exec (exec)
      filename: /home/mikesart/dev/amdgpu/pthreads
      pid: 1152
      old_pid: 1152

    sched_process_fork (do_fork)
      parent_comm: thread_main
      parent_pid: 1152
      child_comm: thread_main
      child_pid: 1154

    sched_process_exit (task exiting)
      Comm: thread_1-1154
      comm: thread_1
      pid: 1154

    sched_wait_task (waiting on task to unschedule)
    sched_process_wait (waiting task)
 */

class event_renderer_t
{
public:
    event_renderer_t( float y_in, float w_in, float h_in );

    void add_event( float x, ImU32 color );
    void done();

    void set_y( float y_in, float h_in );

protected:
    void start( float x, ImU32 color );
    void draw();

public:
    float x0, x1;
    uint32_t num_events;
    ImU32 event_color;

    float y, w, h;

    float m_width = 1.0f;
    float m_maxwidth = imgui_scale( 4.0f );
};

typedef std::function< uint32_t ( class graph_info_t &gi ) > RenderGraphRowCallback;

struct row_info_t
{
    uint32_t id;

    loc_type_t row_type;
    std::string row_name;
    std::string row_filter;
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

    RenderGraphRowCallback render_cb;
};

class graph_info_t
{
public:
    void init_row_info( TraceWin *win, const std::vector< GraphRows::graph_rows_info_t > &graph_rows );

    void init( TraceWin *win, float x, float w );
    void set_ts( TraceWin *win, int64_t start_ts, int64_t length_ts );
    void set_pos_y( float y, float h, row_info_t *ri );

    float ts_to_x( int64_t ts );
    float ts_to_screenx( int64_t ts );

    int64_t screenx_to_ts( float x_in );
    int64_t dx_to_ts( float x_in );

    bool pt_in_graph( const ImVec2 &posin );
    bool mouse_pos_in_graph();
    bool mouse_pos_in_rect( float x, float w, float y, float h );

    row_info_t *find_row( const char *name );

    bool add_mouse_hovered_event( float x, const trace_event_t &event );

public:
    float x, y, w, h;

    int64_t ts0;
    int64_t ts1;
    int64_t tsdx;
    double tsdxrcp;

    uint32_t eventstart;
    uint32_t eventend;

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
    const size_t hovered_max = 6;
    std::vector< hovered_t > hovered_items;

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

    float text_h;
    float row_h;
    float visible_graph_height;
    float total_graph_height;

    // row_info id we need to make sure is visible
    size_t show_row_id = ( size_t )-1;
};

static void imgui_drawrect( float x, float w, float y, float h, ImU32 color )
{
    if ( w < 0.0f )
    {
        x += w;
        w = -w;
    }

    if ( w <= 1.0f )
        ImGui::GetWindowDrawList()->AddLine( ImVec2( x, y - 0.5f ), ImVec2( x, y + h - 0.5f ), color );
    else
        ImGui::GetWindowDrawList()->AddRectFilled( ImVec2( x, y ), ImVec2( x + w, y + h ), color );
}

static void imgui_draw_text( float x, float y, const char *text, ImU32 color, bool draw_background = false )
{
    if ( draw_background )
    {
        ImVec2 textsize = ImGui::CalcTextSize( text );

        ImGui::GetWindowDrawList()->AddRectFilled(
                    ImVec2( x - 1, y - 1 ), ImVec2( x + textsize.x + 2, y + textsize.y + 2 ),
                    s_clrs().get( col_Graph_RowLabelTextBk ) );
    }

    ImGui::GetWindowDrawList()->AddText( ImVec2( x, y ), color, text );
}

const char *get_event_field_val( const trace_event_t &event, const char *name )
{
    for ( const event_field_t &field : event.fields )
    {
        if ( !strcmp( field.key, name ) )
            return field.value;
    }

    return "";
}

/*
 * event_renderer_t
 */
event_renderer_t::event_renderer_t( float y_in, float w_in, float h_in )
{
    y = y_in;
    w = w_in;
    h = h_in;

    start( -1.0f, 0 );
}

void event_renderer_t::set_y( float y_in, float h_in )
{
    if ( y != y_in || h != h_in )
    {
        done();

        y = y_in;
        h = h_in;
    }
}

void event_renderer_t::add_event( float x, ImU32 color )
{
    if ( x0 < 0.0f )
    {
        // First event
        start( x, color );
    }
    else if ( ( x - x1 > 1.0f ) || ( event_color != color ) )
    {
        // New event is away from current group or new color
        draw();

        // Start a new group
        start( x, color );
    }
    else
    {
        // New event real close to last event with same color
        x1 = x;
        num_events++;
    }
}

void event_renderer_t::done()
{
    if ( x0 != -1 )
    {
        draw();
        start( -1.0f, 0 );
    }
}

void event_renderer_t::start( float x, ImU32 color )
{
    num_events = 0;
    event_color = color;

    x0 = x;
    x1 = x + .0001f;
}

void event_renderer_t::draw()
{
    int index = std::min< int >( col_Graph_1Event + num_events, col_Graph_6Event );
    ImU32 color = event_color ? event_color : s_clrs().get( index );
    float min_width = std::min< float >( num_events + m_width, m_maxwidth );
    float width = std::max< float >( x1 - x0, min_width );

    imgui_drawrect( x0, width, y, h, color );
}

static option_id_t get_comm_option_id( const std::string &row_name, loc_type_t row_type )
{
    option_id_t optid = s_opts().get_opt_graph_rowsize_id( row_name );

    if ( optid != OPT_Invalid )
        return optid;

    if ( row_type == LOC_TYPE_Print ||
         row_type == LOC_TYPE_Plot ||
         row_type == LOC_TYPE_Timeline )
    {
        return s_opts().add_opt_graph_rowsize( row_name.c_str() );
    }

    return OPT_Invalid;
}

/*
 * graph_info_t
 */
void graph_info_t::init_row_info( TraceWin *win, const std::vector< GraphRows::graph_rows_info_t > &graph_rows )
{
    uint32_t id = 0;

    imgui_push_smallfont();

    float graph_row_padding = ImGui::GetStyle().FramePadding.y;

    text_h = ImGui::GetTextLineHeightWithSpacing();
    row_h = text_h * 2 + graph_row_padding;

    total_graph_height = graph_row_padding;

    imgui_pop_smallfont();

    for ( const GraphRows::graph_rows_info_t &grow : graph_rows )
    {
        row_info_t rinfo;
        option_id_t optid = OPT_Invalid;
        const std::vector< uint32_t > *plocs;
        const std::string &row_name = grow.row_name;

        if ( grow.hidden )
            continue;

        plocs = win->m_trace_events.get_locs( grow.row_filter.c_str(), &rinfo.row_type );

        rinfo.row_y = total_graph_height;
        rinfo.row_h = text_h * 2;
        rinfo.row_name = row_name;
        rinfo.row_filter = grow.row_filter;
        rinfo.scale_ts = win->m_graph.rows.get_row_scale( row_name );

        if ( !plocs )
        {
            // Nothing to render
            rinfo.render_cb = nullptr;
        }
        else if ( rinfo.row_type == LOC_TYPE_Print )
        {
            // ftrace print row
            optid = get_comm_option_id( rinfo.row_name, rinfo.row_type );
            rinfo.render_cb = std::bind( &TraceWin::graph_render_print_timeline, win, _1 );
        }
        else if ( rinfo.row_type == LOC_TYPE_Plot )
        {
            optid = get_comm_option_id( rinfo.row_name, rinfo.row_type );
            rinfo.render_cb = std::bind( &TraceWin::graph_render_plot, win, _1 );
        }
        else if ( rinfo.row_type == LOC_TYPE_Timeline )
        {
            optid = get_comm_option_id( rinfo.row_name, rinfo.row_type );
            rinfo.render_cb = std::bind( &TraceWin::graph_render_row_timeline, win, _1 );
        }
        else if ( rinfo.row_type == LOC_TYPE_Timeline_hw )
        {
            rinfo.row_h = 2 * text_h;
            rinfo.render_cb = std::bind( &TraceWin::graph_render_hw_row_timeline, win, _1 );
        }
        else
        {
            // LOC_TYPE_Comm or LOC_TYPE_Tdopexpr hopefully

            if ( rinfo.row_type == LOC_TYPE_Comm )
            {
                const char *pidstr = strrchr( row_name.c_str(), '-' );

                if ( pidstr )
                {
                    rinfo.pid = atoi( pidstr + 1 );
                    rinfo.tgid_info = win->m_trace_events.tgid_from_pid( rinfo.pid );
                }

                if ( win->m_graph.show_row_name && ( row_name == win->m_graph.show_row_name ) )
                {
                    show_row_id = id;
                    win->m_graph.show_row_name = NULL;
                }

                // If we're graphing only filtered events, check if this comm has any events
                if ( s_opts().getb( OPT_GraphOnlyFiltered ) &&
                     s_opts().getb( OPT_Graph_HideEmptyFilteredRows ) &&
                     !win->m_eventlist.filtered_events.empty() )
                {
                    bool no_events = true;

                    for ( size_t idx : *plocs )
                    {
                        const trace_event_t &event = win->get_event( idx );

                        if ( ( event.pid == rinfo.pid ) && !event.is_filtered_out )
                        {
                            no_events = false;
                            break;
                        }
                    }

                    if ( no_events )
                        continue;
                }
            }

            rinfo.render_cb = std::bind( &TraceWin::graph_render_row_events, win, _1 );
        }

        if ( optid != OPT_Invalid )
        {
            int rows = ( optid != OPT_Invalid ) ? s_opts().geti( optid ) : 4;

            rinfo.row_h = Clamp< int >( rows, 2, 50 ) * text_h;
        }

        rinfo.id = id++;
        rinfo.plocs = plocs;
        row_info.push_back( rinfo );

        total_graph_height += rinfo.row_h + graph_row_padding;
    }

    total_graph_height += imgui_scale( 2.0f );
    total_graph_height = std::max< float >( total_graph_height, 4 * row_h );
}

void graph_info_t::set_ts( TraceWin *win, int64_t start_ts, int64_t length_ts )
{
    ts0 = start_ts;
    ts1 = ts0 + length_ts;

    eventstart = win->ts_to_eventid( ts0 );
    eventend = win->ts_to_eventid( ts1 );

    tsdx = ts1 - ts0 + 1;
    tsdxrcp = 1.0 / tsdx;
}

void graph_info_t::init( TraceWin *win, float x_in, float w_in )
{
    x = x_in;
    w = w_in;

    mouse_pos = ImGui::IsRootWindowOrAnyChildFocused() ?
                ImGui::GetMousePos() : ImGui::GetIO().MousePosInvalid;

    // Check if we're supposed to render filtered events only
    graph_only_filtered = s_opts().getb( OPT_GraphOnlyFiltered ) &&
                          !win->m_eventlist.filtered_events.empty();

    timeline_render_user = s_opts().getb( OPT_TimelineRenderUserSpace );

    const std::vector< trace_event_t > &events = win->m_trace_events.m_events;

    // First check if they're hovering a timeline event in the event list
    uint32_t event_hov = win->m_eventlist.hovered_eventid;

    // If not, check if they're hovering a timeline event in the graph
    if ( !is_valid_id( event_hov ) || !events[ event_hov ].is_timeline() )
        event_hov = win->m_graph.hovered_eventid;

    if ( is_valid_id( event_hov ) && events[ event_hov ].is_timeline() )
    {
        // Find the fence signaled event for this timeline
        std::string context = get_event_gfxcontext_str( events[ event_hov ] );
        const std::vector< uint32_t > *plocs = win->m_trace_events.get_gfxcontext_locs( context.c_str() );

        // Mark it as hovered so it'll have a selection rectangle
        hovered_fence_signaled = plocs->back();
    }
}

void graph_info_t::set_pos_y( float y_in, float h_in, row_info_t *ri )
{
    y = y_in;
    h = h_in;

    prinfo_cur = ri;

    mouse_over = mouse_pos.x >= x &&
            mouse_pos.x <= x + w &&
            mouse_pos.y >= y &&
            mouse_pos.y <= y + h;
}

float graph_info_t::ts_to_x( int64_t ts )
{
    return w * ( ts - ts0 ) * tsdxrcp;
}

float graph_info_t::ts_to_screenx( int64_t ts )
{
    return x + ts_to_x( ts );
}

int64_t graph_info_t::screenx_to_ts( float x_in )
{
    double val = ( x_in - x ) / w;

    return ts0 + val * tsdx;
}
int64_t graph_info_t::dx_to_ts( float x_in )
{
    return ( x_in / w ) * tsdx;
}

bool graph_info_t::pt_in_graph( const ImVec2 &posin )
{
    return ( posin.x >= x && posin.x <= x + w &&
             posin.y >= y && posin.y <= y + h );
}

bool graph_info_t::mouse_pos_in_graph()
{
    return pt_in_graph( mouse_pos );
}

bool graph_info_t::mouse_pos_in_rect( float x0, float width, float y0, float height )
{
    return ( mouse_pos.x >= x0 &&
             mouse_pos.x <= x0 + width &&
             mouse_pos.y >= y0 &&
             mouse_pos.y <= y0 + height );
}

row_info_t *graph_info_t::find_row( const char *name )
{
    for ( row_info_t &ri : row_info )
    {
        if ( ri.row_name == name )
            return &ri;
    }
    return NULL;
}

bool graph_info_t::add_mouse_hovered_event( float xin, const trace_event_t &event )
{
    bool inserted = false;
    float xdist_mouse = xin - mouse_pos.x;
    bool neg = xdist_mouse < 0.0f;

    if ( neg )
        xdist_mouse = -xdist_mouse;

    if ( xdist_mouse < imgui_scale( 8.0f ) )
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

void CreateGraphRowDlg::init()
{
    std::vector< INIEntry > entries = s_ini().GetSectionEntries( "$graphrow_filters$" );

    for ( const INIEntry &entry : entries )
        m_previous_filters.push_back( entry.second );

    if ( m_previous_filters.empty() )
    {
        // Add some default filters
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

    m_passes = 0;

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

    if ( m_passes++ < 2 )
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
        ImGui::GetColorVec4( disabled ? ImGuiCol_TextDisabled : ImGuiCol_Text ) );

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
    if ( ImGui::Button( "Cancel", button_size ) || s_keybd().is_escape_down() || ret )
        ImGui::CloseCurrentPopup();

    ImGui::EndPopup();
    return ret;
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

        if ( x >= gi.x + gi.w )
            break;
    }

    if ( points.size() )
    {
        bool closed = false;
        float thickness = 2.0f;
        bool anti_aliased = true;

        gi.prinfo_cur->minval = minval;
        gi.prinfo_cur->maxval = maxval;

        float pad = 0.15f * ( maxval - minval );
        if ( !pad )
            pad = 1.0f;
        minval -= pad;
        maxval += pad;

        float rcpdenom = gi.h / ( maxval - minval );
        for ( ImVec2 &pt : points )
            pt.y = gi.y + ( maxval - pt.y ) * rcpdenom;

        ImGui::GetWindowDrawList()->AddPolyline( points.data(), points.size(),
                                                 color_line, closed, thickness, anti_aliased );

        for ( const ImVec2 &pt : points )
        {
            imgui_drawrect( pt.x - imgui_scale( 1.5f ), imgui_scale( 3.0f ),
                            pt.y - imgui_scale( 1.5f ), imgui_scale( 3.0f ),
                            color_point );
        }
    }

    return points.size();
}

uint32_t TraceWin::graph_render_print_timeline( graph_info_t &gi )
{
    imgui_push_smallfont();

    struct row_draw_info_t
    {
        float x = 0.0f;
        const trace_event_t *event = nullptr;
        const TraceEvents::event_print_info_t *print_info = nullptr;
    };
    std::vector< row_draw_info_t > row_draw_info;

    uint32_t num_events = 0;
    const std::vector< uint32_t > &locs = *gi.prinfo_cur->plocs;
    bool timeline_labels = s_opts().getb( OPT_PrintTimelineLabels ) &&
            !s_keybd().is_alt_down();

    uint32_t row_count = std::max< uint32_t >( 1, gi.h / gi.text_h - 1 );

    row_draw_info.resize( row_count + 1 );

    // We need to start drawing to the left of 0 for timeline_labels
    int64_t ts = timeline_labels ? gi.screenx_to_ts( gi.x - m_trace_events.m_rect_size_max_x ) : gi.ts0;
    uint32_t eventstart = ts_to_eventid( ts );

    static float dx = imgui_scale( 3.0f );

    for ( size_t idx = vec_find_eventid( locs, eventstart );
          idx < locs.size();
          idx++ )
    {
        uint32_t eventid = locs[ idx ];
        const trace_event_t &event = get_event( eventid );
        uint32_t row_id = event.graph_row_id ? ( event.graph_row_id % row_count + 1 ) : 0;
        float x = gi.ts_to_screenx( event.ts );
        float y = gi.y + row_id * gi.text_h;

        if ( eventid > gi.eventend )
            break;
        else if ( gi.graph_only_filtered && event.is_filtered_out )
            continue;

        // Check if we drew something on this row already
        if ( row_draw_info[ row_id ].print_info )
        {
            const row_draw_info_t &draw_info = row_draw_info[ row_id ];
            float x0 = draw_info.x + dx;
            const TraceEvents::event_print_info_t *print_info = draw_info.print_info;

            // If we did and there is room, draw the ftrace print buf
            if ( x - x0 > print_info->rect_size.x )
                imgui_draw_text( x0, y + imgui_scale( 2.0f ), print_info->buf, draw_info.event->color );
        }

        // Otherwise draw a little tick for it
        imgui_drawrect( x, imgui_scale( 2.0f ), y, gi.text_h, event.color );

        // Check if we're mouse hovering this event
        if ( gi.mouse_over && gi.mouse_pos.y >= y && gi.mouse_pos.y <= y + gi.text_h )
            gi.add_mouse_hovered_event( x, event );

        num_events++;

        if ( timeline_labels )
        {
            row_draw_info[ row_id ].x = x;
            row_draw_info[ row_id ].print_info = m_trace_events.m_print_buf_info.get_val( event.id );
            row_draw_info[ row_id ].event = &event;
        }
    }

    for ( uint32_t row_id = 0; row_id < row_draw_info.size(); row_id++ )
    {
        const row_draw_info_t &draw_info = row_draw_info[ row_id ];
        const TraceEvents::event_print_info_t *print_info = draw_info.print_info;

        if ( print_info )
        {
            float x0 = draw_info.x + dx;
            float y = gi.y + row_id * gi.text_h;
            const trace_event_t *event = draw_info.event;

            imgui_draw_text( x0, y + imgui_scale( 2.0f ), print_info->buf, event->color );
        }
    }

    imgui_pop_smallfont();

    return num_events;
}

uint32_t TraceWin::graph_render_hw_row_timeline( graph_info_t &gi )
{
    imgui_push_smallfont();

    float row_h = gi.h;
    uint32_t num_events = 0;
    ImU32 col_event = s_clrs().get( col_Graph_1Event );

    rect_t hov_rect;
    ImU32 last_color = 0;
    float y = gi.y;
    bool draw_label = !s_keybd().is_alt_down();
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

            imgui_drawrect( x0, x1 - x0, y, row_h, fence_signaled.color );

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
                    const tgid_info_t *tgid_info = m_trace_events.tgid_from_commstr( fence_signaled.user_comm );

                    ImGui::GetWindowDrawList()->AddText(
                                ImVec2( x0 + imgui_scale( 2.0f ), y + imgui_scale( 2.0f ) ),
                                s_clrs().get( col_Graph_BarText ), label );

                    if ( tgid_info )
                    {
                        char tgidstr[ 64 ];
                        const ImVec2 rect_min( x0, y );
                        const ImVec2 rect_max( x1, y + row_h );

                        ImGui::PushClipRect( rect_min, rect_max, true );

                        snprintf_safe( tgidstr, "(%s)", tgid_info->commstr );
                        ImGui::GetWindowDrawList()->AddText(
                                    ImVec2( x0 + imgui_scale( 2.0f ), y + size.y + imgui_scale( 2.0f ) ),
                                    s_clrs().get( col_Graph_BarText ), tgidstr );

                        ImGui::PopClipRect();
                    }
                }
            }

            // If we drew the same color last time, draw a separator.
            if ( last_color == fence_signaled.color )
                imgui_drawrect( x0, 1.0, y, row_h, col_event );
            else
                last_color = fence_signaled.color;

            // Check if this fence_signaled is selected / hovered
            if ( ( gi.hovered_fence_signaled == fence_signaled.id ) ||
                 gi.mouse_pos_in_rect( x0, x1 - x0, y, row_h ) )
            {
                hov_rect = { x0, y, x1, y + row_h };

                if ( !is_valid_id( gi.hovered_fence_signaled ) )
                    gi.hovered_fence_signaled = fence_signaled.id;
            }

            num_events++;
        }
    }

    if ( hov_rect.min.x < gi.x + gi.w )
        ImGui::GetWindowDrawList()->AddRect( hov_rect.min, hov_rect.max, s_clrs().get( col_Graph_BarSelRect ) );

    imgui_pop_smallfont();

    return num_events;
}

uint32_t TraceWin::graph_render_row_timeline( graph_info_t &gi )
{
    imgui_push_smallfont();

    rect_t hov_rect;
    uint32_t num_events = 0;
    ImU32 col_hwrunning = s_clrs().get( col_Graph_BarHwRunning );
    ImU32 col_userspace = s_clrs().get( col_Graph_BarUserspace );
    ImU32 col_hwqueue = s_clrs().get( col_Graph_BarHwQueue );
    ImU32 color_1event = s_clrs().get( col_Graph_1Event );
    const std::vector< uint32_t > &locs = *gi.prinfo_cur->plocs;

    uint32_t timeline_row_count = gi.h / gi.text_h;

    bool render_timeline_events = s_opts().getb( OPT_TimelineEvents );
    bool render_timeline_labels = s_opts().getb( OPT_TimelineLabels ) &&
            !s_keybd().is_alt_down();

    for ( size_t idx = vec_find_eventid( locs, gi.eventstart );
          idx < locs.size();
          idx++ )
    {
        const trace_event_t &fence_signaled = get_event( locs[ idx ] );

        if ( fence_signaled.is_fence_signaled() &&
             is_valid_id( fence_signaled.id_start ) )
        {
            const trace_event_t &sched_run_job = get_event( fence_signaled.id_start );
            const trace_event_t &cs_ioctl = is_valid_id( sched_run_job.id_start ) ?
                        get_event( sched_run_job.id_start ) : sched_run_job;

            //$ TODO mikesart: can we bail out of this loop at some point if
            //  our start times for all the graphs are > gi.ts1?
            if ( cs_ioctl.ts < gi.ts1 )
            {
                bool hovered = false;
                float y = gi.y + ( fence_signaled.graph_row_id % timeline_row_count ) * gi.text_h;

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
                    gi.mouse_pos_in_rect( xleft, x_hw_end - xleft, y, gi.text_h ) )
                {
                    // Mouse is hovering over this fence_signaled.
                    hovered = true;
                    hov_rect = { x_user_start, y, x_hw_end, y + gi.text_h };

                    if ( !is_valid_id( gi.hovered_fence_signaled ) )
                        gi.hovered_fence_signaled = fence_signaled.id;
                }

                // Draw user bar
                if ( hovered || gi.timeline_render_user )
                    imgui_drawrect( x_user_start, x_hwqueue_start - x_user_start, y, gi.text_h, col_userspace );

                // Draw hw queue bar
                if ( x_hwqueue_end != x_hwqueue_start )
                    imgui_drawrect( x_hwqueue_start, x_hwqueue_end - x_hwqueue_start, y, gi.text_h, col_hwqueue );

                // Draw hw running bar
                imgui_drawrect( x_hwqueue_end, x_hw_end - x_hwqueue_end, y, gi.text_h, col_hwrunning );

                if ( render_timeline_labels )
                {
                    const ImVec2 size = ImGui::CalcTextSize( cs_ioctl.user_comm );
                    float x_text = std::max< float >( x_hwqueue_start, gi.x ) + imgui_scale( 2.0f );

                    if ( x_hw_end - x_text >= size.x )
                    {
                        const tgid_info_t *tgid_info = m_trace_events.tgid_from_pid( cs_ioctl.pid );

                        ImGui::GetWindowDrawList()->AddText( ImVec2( x_text, y + imgui_scale( 1.0f ) ),
                                                             s_clrs().get( col_Graph_BarText ), cs_ioctl.user_comm );

                        if ( tgid_info )
                        {
                            char tgidstr[ 64 ];
                            const ImVec2 rect_min( x_text, y + imgui_scale( 1.0f ) );
                            const ImVec2 rect_max( x_hw_end, rect_min.y + size.y );

                            ImGui::PushClipRect( rect_min, rect_max, true );

                            snprintf_safe( tgidstr, "  (%s)", tgid_info->commstr );
                            ImGui::GetWindowDrawList()->AddText( ImVec2( x_text + size.x, y + imgui_scale( 1.0f ) ),
                                                                 s_clrs().get( col_Graph_BarText ), tgidstr );

                            ImGui::PopClipRect();
                        }
                    }
                }

                if ( render_timeline_events )
                {
                    if ( cs_ioctl.id != sched_run_job.id )
                    {
                        // Draw event line for start of user
                        imgui_drawrect( x_user_start, 1.0, y, gi.text_h, color_1event );

                        // Check if we're mouse hovering starting event
                        if ( gi.mouse_over && gi.mouse_pos.y >= y && gi.mouse_pos.y <= y + gi.text_h )
                        {
                            // If we are hovering, and no selection bar is set, do it.
                            if ( gi.add_mouse_hovered_event( x_user_start, cs_ioctl ) && ( hov_rect.min.x == FLT_MAX ) )
                            {
                                hov_rect = { x_user_start, y, x_hw_end, y + gi.text_h };

                                // Draw user bar for hovered events if they weren't already drawn
                                if ( !hovered && !gi.timeline_render_user )
                                    imgui_drawrect( x_user_start, x_hwqueue_start - x_user_start, y, gi.text_h, col_userspace );
                            }
                        }
                    }

                    // Draw event line for hwqueue start and hw end
                    imgui_drawrect( x_hwqueue_start, 1.0, y, gi.text_h, color_1event );
                    imgui_drawrect( x_hw_end, 1.0, y, gi.text_h, color_1event );
                }

                num_events++;
            }
        }
    }

    if ( hov_rect.min.x < gi.x + gi.w )
        ImGui::GetWindowDrawList()->AddRect( hov_rect.min, hov_rect.max, s_clrs().get( col_Graph_BarSelRect ) );

    imgui_pop_smallfont();

    return num_events;
}

uint32_t TraceWin::graph_render_row_events( graph_info_t &gi )
{
    uint32_t num_events = 0;
    bool draw_hovered_event = false;
    bool draw_selected_event = false;
    const std::vector< uint32_t > &locs = *gi.prinfo_cur->plocs;
    event_renderer_t event_renderer( gi.y + 4, gi.w, gi.h - 8 );
    bool hide_sched_switch = s_opts().getb( OPT_HideSchedSwitchEvents );

    // Calculate how many pixels .0001ms takes
    const float dx = ( .0001f * NSECS_PER_MSEC ) * gi.w * gi.tsdxrcp;

    // Scale width of drawn event from 0..4 when .0001ms takes .1 - 1.5 pixels
    const float minx = 0.1f;
    const float maxx = 1.5f;
    event_renderer.m_width = std::max< float >( 1.0f,
        event_renderer.m_maxwidth * ( dx - minx ) / ( maxx - minx ) );

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

        float x = gi.ts_to_screenx( event.ts );

        if ( eventid == m_eventlist.hovered_eventid )
            draw_hovered_event = true;
        else if ( eventid == m_eventlist.selected_eventid )
            draw_selected_event = true;

        // Check if we're mouse hovering this event
        if ( gi.mouse_over )
            gi.add_mouse_hovered_event( x, event );

        event_renderer.add_event( x, event.color );
        num_events++;
    }

    event_renderer.done();

    if ( draw_hovered_event )
    {
        trace_event_t &event = get_event( m_eventlist.hovered_eventid );
        float x = gi.ts_to_screenx( event.ts );

        ImGui::GetWindowDrawList()->AddCircleFilled(
                    ImVec2( x, gi.y + gi.h / 2.0f ),
                    imgui_scale( 5.0f ),
                    s_clrs().get( col_Graph_HovEvent ) );
    }

    if ( draw_selected_event )
    {
        trace_event_t &event = get_event( m_eventlist.selected_eventid );
        float x = gi.ts_to_screenx( event.ts );

        ImGui::GetWindowDrawList()->AddCircleFilled(
                    ImVec2( x, gi.y + gi.h / 2.0f ),
                    imgui_scale( 5.0f ),
                    s_clrs().get( col_Graph_SelEvent ) );
    }

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

            for ( size_t idx = vec_find_eventid( *plocs, gi.eventstart );
                  idx < plocs->size();
                  idx++ )
            {
                float row_h = gi.text_h;
                float y = gi.y + ( gi.h - row_h ) / 2;
                const trace_event_t &sched_switch = get_event( plocs->at( idx ) );

                if ( sched_switch.duration != ( uint32_t )-1 )
                {
                    float x0 = gi.ts_to_screenx( sched_switch.ts - sched_switch.duration );
                    float x1 = gi.ts_to_screenx( sched_switch.ts );
                    int running = !!( sched_switch.flags & TRACE_FLAG_SCHED_SWITCH_TASK_RUNNING );

                    // Bail if we're off the right side of our graph
                    if ( x0 > gi.x + gi.w )
                        break;

                    imgui_drawrect( x0, x1 - x0, y, row_h, colors[ running ] );

                    if ( gi.mouse_over &&
                         gi.mouse_pos.x > x0 && gi.mouse_pos.x <= x1 &&
                         gi.mouse_pos.y >= y && gi.mouse_pos.y <= y + gi.row_h )
                    {
                        gi.sched_switch_bars.push_back( sched_switch.id );

                        ImGui::GetWindowDrawList()->AddRect( ImVec2( x0, y ),
                                                             ImVec2( x1, y + row_h ),
                                                             s_clrs().get( col_Graph_BarSelRect ) );
                    }
                }
            }
        }
    }

    return num_events;
}

void TraceWin::graph_render_row( graph_info_t &gi )
{
    if ( gi.mouse_over )
    {
        m_graph.mouse_over_row_name = gi.prinfo_cur->row_name;
        m_graph.mouse_over_row_filter = gi.prinfo_cur->row_filter;
        m_graph.mouse_over_row_type = gi.prinfo_cur->row_type;
    }

    // Draw background
    ImGui::GetWindowDrawList()->AddRectFilled(
        ImVec2( gi.x, gi.y ),
        ImVec2( gi.x + gi.w, gi.y + gi.h ),
        s_clrs().get( col_Graph_RowBk ) );

    if ( gi.prinfo_cur->tgid_info )
    {
        ImGui::GetWindowDrawList()->AddLine(
                    ImVec2( gi.x, gi.y + gi.h + 1 ),
                    ImVec2( gi.x + gi.w, gi.y + gi.h + 1 ),
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
            gi.set_ts( this, start_ts, length_ts );

            if ( gi.mouse_pos_in_graph() )
                gi.mouse_pos_scaled_ts = gi.screenx_to_ts( gi.mouse_pos.x );
        }

        // Call the render callback function
        num_events = gi.prinfo_cur->render_cb( gi );

        if ( scale_ts > 0.0f )
        {
            float x0 = gi.ts_to_screenx( m_graph.start_ts );
            float x1 = gi.ts_to_screenx( m_graph.start_ts + m_graph.length_ts );

            ImGui::GetWindowDrawList()->AddRectFilled(
                        ImVec2( x0, gi.y ), ImVec2( x1, gi.y + gi.h ),
                        0x5fffffff, 9.0f, 0x0f );
            gi.set_ts( this, m_graph.start_ts, m_graph.length_ts );
        }
    }

    gi.prinfo_cur->num_events = num_events;
}

void TraceWin::graph_render_time_ticks( class graph_info_t &gi )
{
    // Draw time ticks every millisecond
    int64_t tsstart = std::max< int64_t >( gi.ts0 / NSECS_PER_MSEC - 1, 0 ) * NSECS_PER_MSEC;
    float dx = gi.w * NSECS_PER_MSEC * gi.tsdxrcp;

    if ( dx <= imgui_scale( 4.0f ) )
    {
        tsstart = std::max< int64_t >( gi.ts0 / NSECS_PER_SEC - 1, 0 ) * NSECS_PER_SEC;
        dx = gi.w * NSECS_PER_SEC * gi.tsdxrcp;
    }

    if ( dx > imgui_scale( 4.0f ) )
    {
        float x0 = gi.ts_to_x( tsstart );

        for ( ; x0 <= gi.w; x0 += dx )
        {
            imgui_drawrect( gi.x + x0, imgui_scale( 1.0f ),
                            gi.y, imgui_scale( 16.0f ),
                            s_clrs().get( col_Graph_TimeTick ) );

            if ( dx >= imgui_scale( 35.0f ) )
            {
                for ( int i = 1; i < 4; i++ )
                {
                    imgui_drawrect( gi.x + x0 + i * dx / 4, imgui_scale( 1.0f ),
                                    gi.y, imgui_scale( 4.0f ),
                                    s_clrs().get( col_Graph_TimeTick ) );
                }
            }
        }
    }
}

static float get_vblank_xdiffs( TraceWin *win, graph_info_t &gi, const std::vector< uint32_t > *vblank_locs )
{
    float xdiff = 0.0f;
    float xlast = 0.0f;
    uint32_t count = 0;

    for ( size_t idx = vec_find_eventid( *vblank_locs, gi.eventstart );
          idx < vblank_locs->size();
          idx++ )
    {
        uint32_t id = vblank_locs->at( idx );
        trace_event_t &event = win->get_event( id );

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
        float xdiff = get_vblank_xdiffs( this, gi, vblank_locs ) / imgui_scale( 1.0f );
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
                // drm_vblank_event0: blue, drm_vblank_event1: red
                colors_t col = ( event.crtc > 0 ) ? col_VBlank1 : col_VBlank0;
                float x = gi.ts_to_screenx( event.ts );

                imgui_drawrect( x, imgui_scale( 1.0f ),
                                gi.y, gi.h,
                                s_clrs().get( col, alpha ) );
            }
        }
    }
}

void TraceWin::graph_render_framemarker_frames( class graph_info_t &gi )
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
    float midx = gi.x + gi.w / 2.0f;

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
             ( left_x > gi.x ) && ( left_x < midx ) )
        {
            markers_set = false;
        }

        if ( !markers_set )
        {
            if ( left_x > gi.x )
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

        imgui_drawrect( left_x, right_x - left_x, gi.y, gi.h, s_clrs().get( col ) );

        if ( gi.mouse_pos_in_rect( left_x, right_x - left_x, gi.y, gi.h ) )
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
    // Draw location line for mouse if mouse is over graph
    if ( m_graph.is_mouse_over &&
         gi.mouse_pos.x >= gi.x &&
         gi.mouse_pos.x <= gi.x + gi.w )
    {
        imgui_drawrect( gi.mouse_pos.x, imgui_scale( 2.0f ),
                        gi.y, gi.h,
                        s_clrs().get( col_Graph_MousePos ) );
    }

    // Render markers A/B if in range
    for ( size_t i = 0; i < ARRAY_SIZE( m_graph.ts_markers ); i++ )
    {
        if ( ( m_graph.ts_markers[ i ] >= gi.ts0 ) && ( m_graph.ts_markers[ i ] < gi.ts1 ) )
        {
            float x = gi.ts_to_screenx( m_graph.ts_markers[ i ] );

            imgui_drawrect( x, imgui_scale( 2.0f ),
                            gi.y, gi.h, s_clrs().get( col_Graph_MarkerA + i ) );
        }
    }
}

void TraceWin::graph_render_eventids( class graph_info_t &gi )
{
    if ( is_valid_id( m_eventlist.hovered_eventid ) )
    {
        trace_event_t &event = get_event( m_eventlist.hovered_eventid );

        if ( event.ts >= gi.ts0 && event.ts <= gi.ts1 )
        {
            float x = gi.ts_to_screenx( event.ts );

            imgui_drawrect( x, imgui_scale( 1.0f ),
                            gi.y, gi.h,
                            s_clrs().get( col_Graph_HovEvent, 120 ) );
        }
    }

    if ( is_valid_id( m_eventlist.selected_eventid ) )
    {
        trace_event_t &event = get_event( m_eventlist.selected_eventid );

        if ( event.ts >= gi.ts0 && event.ts <= gi.ts1 )
        {
            float x = gi.ts_to_screenx( event.ts );

            imgui_drawrect( x, imgui_scale( 1.0f ),
                            gi.y, gi.h,
                            s_clrs().get( col_Graph_SelEvent, 120 ) );
        }
    }
}

void TraceWin::graph_render_mouse_selection( class graph_info_t &gi )
{
    // Draw mouse selection location
    if ( ( m_graph.mouse_captured == MOUSE_CAPTURED_ZOOM ) ||
         ( m_graph.mouse_captured == MOUSE_CAPTURED_SELECT_AREA ) )
    {
        float mousex0 = m_graph.mouse_capture_pos.x;
        float mousex1 = gi.mouse_pos.x;

        imgui_drawrect( mousex0, mousex1 - mousex0,
                        gi.y, gi.h,
                        s_clrs().get( col_Graph_ZoomSel ) );
    }
}

void TraceWin::graph_render_eventlist_selection( class graph_info_t &gi )
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

            ImGui::GetWindowDrawList()->AddRect(
                        ImVec2( xstart, gi.y + imgui_scale( 20 ) ),
                        ImVec2( xend, gi.y + gi.h - imgui_scale( 30 ) ),
                        s_clrs().get( col_EventList_Sel ) );
        }
    }
}

static void render_row_label( float x, float y, row_info_t &ri )
{
    ImU32 col = s_clrs().get( col_Graph_RowLabelText );

    if ( ri.tgid_info )
        col = ri.tgid_info->color;

    std::string label = string_format( "%u) %s", ri.id, ri.row_name.c_str() );

    if ( ri.scale_ts > 1.0f )
        label += s_textclrs().bright_str( string_format( " (%.1fx)", ri.scale_ts ) );

    imgui_draw_text( x, y, label.c_str(), col, true );
    y += ImGui::GetTextLineHeight();

    if ( ri.minval <= ri.maxval )
    {
        label = string_format( "min:%.2f max:%.2f", ri.minval, ri.maxval );
        imgui_draw_text( x, y, label.c_str(), col, true );
    }
    else if ( ri.num_events )
    {
        label = string_format( "%u events", ri.num_events );
        imgui_draw_text( x, y, label.c_str(), col, true );
    }
}

void TraceWin::graph_render_row_labels( graph_info_t &gi )
{
    if ( gi.prinfo_zoom )
    {
        if ( gi.prinfo_zoom_hw )
        {
            float y = gi.y + gi.h - gi.prinfo_zoom_hw->row_h;

            render_row_label( gi.x, y, *gi.prinfo_zoom_hw );
        }

        render_row_label( gi.x, gi.y, *gi.prinfo_zoom );
    }
    else
    {
        for ( row_info_t &ri : gi.row_info )
        {
            float y = gi.y + ri.row_y;

            render_row_label( gi.x, y, ri );
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
    if ( !m_graph.mouse_over_row_name.empty() )
    {
        if ( m_graph.zoom_row_name != m_graph.mouse_over_row_name )
        {
            if ( m_graph.mouse_over_row_type == LOC_TYPE_Timeline ||
                 m_graph.mouse_over_row_type == LOC_TYPE_Timeline_hw ||
                 m_graph.mouse_over_row_type == LOC_TYPE_Plot ||
                 m_graph.mouse_over_row_type == LOC_TYPE_Print )
            {
                return true;
            }
        }
    }

    return false;
}

void TraceWin::zoom_graph_row()
{
    m_graph.zoom_row_name = m_graph.mouse_over_row_name;

    if ( m_graph.mouse_over_row_type == LOC_TYPE_Timeline_hw )
    {
        // Trim " hw" from end of string so, for example, we zoom "gfx" and not "gfx hw".
        m_graph.zoom_row_name.resize( m_graph.zoom_row_name.size() - 3 );
    }
}

void TraceWin::graph_handle_hotkeys( graph_info_t &gi )
{
    // If there are no actions, bail.
    if ( !s_actions().count() )
        return;

    if ( !m_frame_markers.m_left_frames.empty() &&
         s_opts().getb( OPT_RenderFrameMarkers ) )
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

    if ( s_actions().get( action_graph_zoom_row ) )
    {
        if ( !m_graph.zoom_row_name.empty() )
            m_graph.zoom_row_name.clear();
        else if ( is_graph_row_zoomable() )
            zoom_graph_row();
    }

    if ( m_graph.is_mouse_over &&
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

        if ( m_graph.is_mouse_over )
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
        m_graph.start_y += ( gi.h - ImGui::GetTextLineHeightWithSpacing() * 4 );
    }
    else if ( s_actions().get( action_scroll_pagedown ) )
    {
        m_graph.start_y -= ( gi.h - ImGui::GetTextLineHeightWithSpacing() * 4 );
    }
    else if ( s_actions().get( action_scroll_left ) )
    {
        start_ts = std::max< int64_t >( start_ts - 9 * m_graph.length_ts / 10,
                                        -NSECS_PER_MSEC );
    }
    else if ( s_actions().get( action_scroll_right ) )
    {
        start_ts = std::min< int64_t >( start_ts + 9 * m_graph.length_ts / 10,
                                        events.back().ts - m_graph.length_ts + NSECS_PER_MSEC );
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

static void calc_process_graph_height( TraceWin *win, graph_info_t &gi )
{
    // Zoom mode if we have a gfx row and zoom option is set
    option_id_t optid;
    float max_graph_size;
    const float valf_min = 4.0f * gi.row_h;

    if ( s_opts().getb( OPT_GraphFullscreen ) )
    {
        gi.visible_graph_height = Clamp< float >( gi.total_graph_height,
                valf_min, ImGui::GetContentRegionAvail().y );
        return;
    }

    if ( gi.prinfo_zoom )
    {
        optid = OPT_GraphHeightZoomed;
        max_graph_size = imgui_scale( 60.0f ) * gi.row_h;
    }
    else
    {
        optid = OPT_GraphHeight;
        max_graph_size = gi.total_graph_height;
    }

    // Set up min / max sizes and clamp value in that range
    float valf = s_opts().getf( optid );
    float valf_max = Clamp< float >( max_graph_size, valf_min, ImGui::GetWindowHeight() );

    // First time initialization - start with about 15 rows
    if ( !valf )
        valf = 15.0f * gi.row_h;

    valf = Clamp< float >( valf, valf_min, valf_max );
    s_opts().setf( optid, valf, valf_min, valf_max );

    gi.visible_graph_height = valf;
}

void TraceWin::graph_render()
{
    graph_info_t gi;

    if ( m_trace_events.m_rect_size_max_x == -1.0f )
    {
        imgui_push_smallfont();
        m_trace_events.update_ftraceprint_colors();
        imgui_pop_smallfont();
    }

    // Initialize our row size, location, etc information based on our graph rows
    gi.init_row_info( this, m_graph.rows.m_graph_rows_list );

    if ( !m_graph.zoom_row_name.empty() )
    {
        gi.prinfo_zoom = gi.find_row( m_graph.zoom_row_name.c_str() );
        if ( gi.prinfo_zoom )
            gi.prinfo_zoom_hw = gi.find_row( ( m_graph.zoom_row_name + " hw" ).c_str() );
    }

    if ( gi.prinfo_zoom )
    {
        ImGui::SameLine();

        std::string label = string_format( "Unzoom '%s'", m_graph.zoom_row_name.c_str() );
        if ( ImGui::Button( label.c_str() ) )
            m_graph.zoom_row_name.clear();
    }

    // Figure out gi.visible_graph_height
    calc_process_graph_height( this, gi );

    // Make sure ts start and length values are mostly sane
    graph_range_check_times();

    if ( s_actions().get( action_focus_graph ) )
        ImGui::SetNextWindowFocus();

    ImGui::BeginChild( "EventGraph", ImVec2( 0, gi.visible_graph_height ), true );
    {
        ImVec2 windowpos = ImVec2( ImGui::GetWindowClipRectMin().x, ImGui::GetWindowPos().y );
        ImVec2 windowsize = ImGui::GetWindowSize();

        m_graph.has_focus = ImGui::IsWindowFocused();

        // Clear graph background
        imgui_drawrect( windowpos.x, windowsize.x,
                        windowpos.y, windowsize.y, s_clrs().get( col_Graph_Bk ) );

        // Initialize our graphics info struct
        gi.set_ts( this, m_graph.start_ts, m_graph.length_ts );
        gi.init( this, windowpos.x, windowsize.x );

        // If we have a show row id, make sure it's visible
        if ( gi.show_row_id != ( size_t )-1 )
        {
            const row_info_t &rinfo = gi.row_info[ gi.show_row_id ];

            if ( ( rinfo.row_y < -m_graph.start_y ) ||
                 ( rinfo.row_y + rinfo.row_h > gi.visible_graph_height - m_graph.start_y ) )
            {
                m_graph.start_y = -rinfo.row_y + gi.visible_graph_height / 3;
            }
        }
        // Range check mouse pan values
        m_graph.start_y = Clamp< float >( m_graph.start_y,
                                          gi.visible_graph_height - gi.total_graph_height, 0.0f );

        // If we don't have a popup menu, clear the mouse over row name
        if ( !m_graph.popupmenu )
        {
            m_graph.mouse_over_row_name = "";
            m_graph.mouse_over_row_type = LOC_TYPE_Max;
        }

        // If we have a gfx graph and we're zoomed, render only that
        float start_y = gi.prinfo_zoom ? 0 : m_graph.start_y;
        if ( gi.prinfo_zoom )
        {
            float gfx_hw_row_h = 0;

            if ( gi.prinfo_zoom_hw )
            {
                row_info_t &ri = *gi.prinfo_zoom_hw;
                gfx_hw_row_h = ri.row_h + ImGui::GetStyle().FramePadding.y;

                gi.set_pos_y( windowpos.y + windowsize.y - ri.row_h, ri.row_h, &ri );
                graph_render_row( gi );
            }

            gi.timeline_render_user = true;
            gi.set_pos_y( windowpos.y, windowsize.y - gfx_hw_row_h, gi.prinfo_zoom );
            graph_render_row( gi );
        }
        else
        {
            // Pass 0: Render all !timeline rows
            // Pass 1: Render all timeline rows
            for ( int pass = 0; pass < 2; pass++ )
            {
                bool render_timelines = !!pass;

                for ( row_info_t &ri : gi.row_info )
                {
                    bool is_timeline = ( ri.row_type == LOC_TYPE_Timeline );

                    if ( is_timeline == render_timelines )
                    {
                        gi.set_pos_y( windowpos.y + ri.row_y + start_y, ri.row_h, &ri );
                        graph_render_row( gi );
                    }
                }
            }
        }

        // Render full graph ticks, vblanks, cursor pos, etc.
        gi.set_pos_y( windowpos.y, windowsize.y, NULL );
        graph_render_time_ticks( gi );
        graph_render_vblanks( gi );
        graph_render_framemarker_frames( gi );
        graph_render_mouse_pos( gi );
        graph_render_eventids( gi );
        graph_render_mouse_selection( gi );
        graph_render_eventlist_selection( gi );

        // Render row labels last (taking panning into consideration)
        gi.set_pos_y( windowpos.y + start_y, windowsize.y, NULL );
        graph_render_row_labels( gi );

        ImU32 color = s_clrs().get( col_Graph_LocationText );
        if ( color & IM_COL32_A_MASK )
        {
            float fontscale = 6.0f;
            int64_t ts = gi.ts0 + ( gi.ts1 - gi.ts0 );
            std::string str = ts_to_timestr( ts / 1000, 4, "" );
            ImVec2 textsize = ImGui::CalcTextSize( str.c_str() );

            ImVec2 pos = ImVec2( windowpos.x + ( windowsize.x - textsize.x * fontscale ) / 2,
                                 windowpos.y + ( windowsize.y - textsize.y * fontscale ) / 2 );

            ImGui::GetWindowDrawList()->AddText( ImGui::GetFont(),
                                                 ImGui::GetFontSize() * fontscale,
                                                 pos, color, str.c_str() );

            if ( m_frame_markers.m_frame_marker_selected != -1 )
            {
                str = string_format( "Frame #%d", m_frame_markers.m_frame_marker_selected );
                textsize = ImGui::CalcTextSize( str.c_str() );

                pos.y += textsize.y * fontscale;
                pos.x = windowpos.x + ( windowsize.x - textsize.x * fontscale ) / 2;
                ImGui::GetWindowDrawList()->AddText( ImGui::GetFont(),
                                                     ImGui::GetFontSize() * fontscale,
                                                     pos, color, str.c_str() );
            }
        }

        // Handle right, left, pgup, pgdown, etc in graph
        graph_handle_keyboard_scroll( gi );

        // Handle hotkeys. Ie: Ctrl+Shift+1, etc
        graph_handle_hotkeys( gi );

        // Render mouse tooltips, mouse selections, etc
        gi.set_pos_y( windowpos.y, windowsize.y, NULL );
        graph_handle_mouse( gi );
    }
    ImGui::EndChild();

    if ( !s_opts().getb( OPT_GraphFullscreen ) )
    {
        ImGui::Button( "##resize_graph", ImVec2( ImGui::GetContentRegionAvailWidth(), imgui_scale( 4.0f ) ) );

        if ( ImGui::IsItemHovered() )
            ImGui::SetMouseCursor( ImGuiMouseCursor_ResizeNS );

        if ( ImGui::IsItemActive() && imgui_mousepos_valid( gi.mouse_pos ) )
        {
            option_id_t opt = gi.prinfo_zoom ? OPT_GraphHeightZoomed : OPT_GraphHeight;

            if ( ImGui::IsMouseClicked( 0 ) )
                m_graph.resize_graph_click_pos = s_opts().getf( opt );

            s_opts().setf( opt, m_graph.resize_graph_click_pos + ImGui::GetMouseDragDelta( 0 ).y );
        }
    }

    m_graph.show_row_name = NULL;
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

    imgui_text_bg( "Options", ImGui::GetColorVec4( ImGuiCol_Header ) );
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
    else if ( m_graph.is_mouse_over )
    {
        if ( ImGui::MenuItem( "Zoom in to 3.00ms", s_actions().hotkey_str( action_graph_zoom_mouse ).c_str() ) )
        {
            int64_t newlen = 3 * NSECS_PER_MSEC;
            const ImVec2 pos = ImGui::GetWindowPos();
            int64_t mouse_ts = gi.screenx_to_ts( pos.x );

            m_graph.zoom_loc = std::make_pair( m_graph.start_ts, m_graph.length_ts );

            graph_zoom( mouse_ts, gi.ts0, false, newlen );
        }
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
    if ( !m_graph.mouse_over_row_name.empty() )
    {
        std::string label;

        if ( is_graph_row_zoomable() )
        {
            label = string_format( "Zoom row '%s'", m_graph.mouse_over_row_name.c_str() );

            if ( ImGui::MenuItem( label.c_str(), s_actions().hotkey_str( action_graph_zoom_row ).c_str() ) )
                zoom_graph_row();
        }

        optid = get_comm_option_id( m_graph.mouse_over_row_name.c_str(), m_graph.mouse_over_row_type );
        label = string_format( "Hide row '%s'", m_graph.mouse_over_row_name.c_str() );

        if ( ImGui::MenuItem( label.c_str() ) )
            m_graph.rows.show_row( m_graph.mouse_over_row_name, GraphRows::HIDE_ROW );

        label = string_format( "Hide row '%s' and below", m_graph.mouse_over_row_name.c_str() );
        if ( ImGui::MenuItem( label.c_str() ) )
            m_graph.rows.show_row( m_graph.mouse_over_row_name, GraphRows::HIDE_ROW_AND_ALL_BELOW );

        if ( m_graph.mouse_over_row_type == LOC_TYPE_Comm )
        {
            const tgid_info_t *tgid_info = m_trace_events.tgid_from_commstr( m_graph.mouse_over_row_name.c_str() );

            if ( tgid_info )
            {
                label = string_format( "Hide rows for process '%s'", tgid_info->commstr_clr );

                if ( ImGui::MenuItem( label.c_str() ) )
                    m_graph.rows.show_tgid( tgid_info, GraphRows::HIDE_ROW );
            }
        }
    }

    // Show Row...
    if ( !m_graph.rows_hidden_rows.empty() )
    {
        std::vector< const tgid_info_t * > tgids_hidden;

        if ( ImGui::BeginMenu( "Show row" ) )
        {
            if ( ImGui::MenuItem( "All Rows" ) )
                m_graph.rows.show_row( "", GraphRows::SHOW_ALL_ROWS );

            ImGui::Separator();

            for ( const GraphRows::graph_rows_info_t &entry : m_graph.rows_hidden_rows )
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
                            m_graph.rows.show_tgid( tgid_info, GraphRows::SHOW_ROW );

                        tgids_hidden.push_back( tgid_info );
                    }
                }
            }

            if ( tgids_hidden.size() )
                ImGui::Separator();

            for ( const GraphRows::graph_rows_info_t &entry : m_graph.rows_hidden_rows )
            {
                const char *commstr = ( entry.type == LOC_TYPE_Comm ) ?
                            m_trace_events.tgidcomm_from_commstr( entry.row_name.c_str() ) :
                            entry.row_name.c_str();
                const std::string label = string_format( "%s (%lu events)",
                                                         commstr, entry.event_count );

                if ( ImGui::MenuItem( label.c_str() ) )
                {
                    m_graph.rows.show_row( entry.row_name.c_str(), GraphRows::SHOW_ROW );
                }
            }

            ImGui::EndMenu();
        }
    }

    // Move row after...
    if ( !m_graph.mouse_over_row_name.empty() )
    {
        std::string move_label = string_format( "Move '%s' after", m_graph.mouse_over_row_name.c_str() );

        if ( ImGui::BeginMenu( move_label.c_str() ) )
        {
            for ( const GraphRows::graph_rows_info_t &entry : m_graph.rows.m_graph_rows_list )
            {
                if ( !entry.hidden && ( entry.row_name != m_graph.mouse_over_row_name ) )
                {
                    const char *commstr = ( entry.type == LOC_TYPE_Comm ) ?
                                m_trace_events.tgidcomm_from_commstr( entry.row_name.c_str() ) :
                                entry.row_name.c_str();
                    if ( ImGui::MenuItem( commstr ) )
                    {
                        m_graph.rows.move_row( m_graph.mouse_over_row_name, entry.row_name );
                        ImGui::CloseCurrentPopup();
                        break;
                    }
                }
            }

            ImGui::EndMenu();
        }
    }

    // Create Plot for hovered event
    if ( is_valid_id( m_graph.hovered_eventid ) &&
         strncmp( m_graph.mouse_over_row_name.c_str(), "plot:", 5 ) )
    {
        const trace_event_t &event = m_trace_events.m_events[ m_graph.hovered_eventid ];
        const std::string plot_str = CreatePlotDlg::get_plot_str( event );

        if ( !plot_str.empty() )
        {
            std::string plot_label = std::string( "Create Plot for " ) + plot_str;

            if ( ImGui::MenuItem( plot_label.c_str() ) )
                m_create_plot_eventid = event.id;
        }
    }

    // Change row size. Ie "Gfx size: 10"
    if ( optid != OPT_Invalid )
        s_opts().render_imgui_opt( optid );

    ImGui::Separator();

    if ( !m_graph.mouse_over_row_name.empty() )
    {
        float valf = m_graph.rows.get_row_scale( m_graph.mouse_over_row_name );
        std::string label = string_format( "Zoom row '%s' out: %sx", m_graph.mouse_over_row_name.c_str(), "%.02f" );

        if ( ImGui::SliderFloat( "##opt_valf", &valf, 1.0f, 100.0f, label.c_str() ) )
            m_graph.rows.m_graph_row_scale_ts.m_map[ m_graph.mouse_over_row_name ] = string_format( "%.02f", valf );
    }

    ImGui::Separator();

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
        m_create_graph_row_eventid = is_valid_id( m_graph.hovered_eventid ) ?
                    m_graph.hovered_eventid : m_trace_events.m_events.size();
    }

    // Frame Markers
    {
        if ( is_valid_id( m_graph.hovered_eventid ) &&
             ImGui::MenuItem( "Set Frame Markers..." ) )
        {
            const trace_event_t &event = m_trace_events.m_events[ m_graph.hovered_eventid ];

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

    if ( s_keybd().is_escape_down() )
        ImGui::CloseCurrentPopup();

    ImGui::EndPopup();
    return true;
}

void TraceWin::graph_handle_mouse_captured( graph_info_t &gi )
{
    // Uncapture mouse if user hits escape
    if ( m_graph.mouse_captured && s_keybd().is_escape_down() )
    {
        m_graph.mouse_captured = MOUSE_NOT_CAPTURED;
        ImGui::CaptureMouseFromApp( false );

        return;
    }

    bool is_mouse_down = ImGui::IsMouseDown( 0 );

    if ( ( m_graph.mouse_captured == MOUSE_CAPTURED_ZOOM ) ||
         ( m_graph.mouse_captured == MOUSE_CAPTURED_SELECT_AREA ) )
    {
        // shift + click: zoom area
        int64_t event_ts0 = gi.screenx_to_ts( m_graph.mouse_capture_pos.x );
        int64_t event_ts1 = gi.screenx_to_ts( gi.mouse_pos.x );

        if ( event_ts0 > event_ts1 )
            std::swap( event_ts0, event_ts1 );

        if ( is_mouse_down )
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
        if ( is_mouse_down && imgui_mousepos_valid( gi.mouse_pos ) )
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
        ImGui::CaptureMouseFromApp( false );
    }

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
        _XTAG( TASK_DEAD ),
        _XTAG( TASK_WAKEKILL ),
        _XTAG( TASK_WAKING ),
        _XTAG( TASK_PARKED ),
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

void TraceWin::graph_set_mouse_tooltip( class graph_info_t &gi, int64_t mouse_ts )
{
    std::string time_buf;
    bool sync_event_list_to_graph = s_opts().getb( OPT_SyncEventListToGraph ) &&
            s_opts().getb( OPT_ShowEventList );

    if ( gi.mouse_pos_scaled_ts != INT64_MIN )
    {
        time_buf += string_format( "\"%s\" Time: %s\nGraph ",
                                   m_graph.mouse_over_row_name.c_str(),
                                   ts_to_timestr( gi.mouse_pos_scaled_ts, 6, "" ).c_str() );
    }
    time_buf += "Time: " + ts_to_timestr( mouse_ts, 6, "" );

    if ( m_graph.mouse_over_row_name != m_graph.mouse_over_row_filter )
        time_buf += "\nFilter: " + m_graph.mouse_over_row_filter + "\n";

    if ( !m_graph.mouse_over_row_name.empty() &&
         ( m_graph.mouse_over_row_type == LOC_TYPE_Comm ) )
    {
        const std::string &row_name = m_graph.mouse_over_row_name;
        const char *commstr = m_trace_events.tgidcomm_from_commstr( row_name.c_str() );

        time_buf += std::string( "\n" ) + commstr;
    }

    m_eventlist.highlight_ids.clear();

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
            time_buf += "\nPrev vblank: -" + ts_to_timestr( prev_vblank_ts, 2 );
        if ( next_vblank_ts != INT64_MAX )
            time_buf += "\nNext vblank: " + ts_to_timestr( next_vblank_ts, 2 );
    }

    if ( graph_marker_valid( 0 ) )
        time_buf += "\nMarker A: " + ts_to_timestr( m_graph.ts_markers[ 0 ] - mouse_ts, 2 );
    if ( graph_marker_valid( 1 ) )
        time_buf += "\nMarker B: " + ts_to_timestr( m_graph.ts_markers[ 1 ] - mouse_ts, 2 );

    if ( gi.hovered_framemarker_frame != -1 )
    {
        int64_t ts = m_frame_markers.get_frame_len( m_trace_events, gi.hovered_framemarker_frame );

        time_buf += string_format( "\n\nFrame %d (", gi.hovered_framemarker_frame );
        time_buf += ts_to_timestr( ts, 4 ) + ")";
    }

    if ( !gi.sched_switch_bars.empty() )
    {
        time_buf += "\n";

        for ( uint32_t id : gi.sched_switch_bars )
        {
            trace_event_t &event = get_event( id );

            const char *prev_pid_str = get_event_field_val( event, "prev_pid" );

            if ( prev_pid_str )
            {
                int prev_pid = atoi( prev_pid_str );
                int task_state = atoi( get_event_field_val( event, "prev_state" ) ) & ( TASK_STATE_MAX - 1 );
                const std::string task_state_str = task_state_to_str( task_state );
                const char *prev_comm = m_trace_events.comm_from_pid( prev_pid, prev_pid_str );
                std::string timestr = ts_to_timestr( event.duration, 4 );

                time_buf += string_format( "\n%s%u%s sched_switch %s (%s) %s",
                                           s_textclrs().str( TClr_Bright ), event.id, s_textclrs().str( TClr_Def ),
                                           prev_comm, timestr.c_str(),
                                           task_state_str.c_str() );
            }
        }
    }

    m_graph.hovered_eventid = INVALID_ID;
    if ( !gi.hovered_items.empty() )
    {
        // Sort hovered items array by id
        std::sort( gi.hovered_items.begin(), gi.hovered_items.end(),
                   [=]( const graph_info_t::hovered_t& lx, const graph_info_t::hovered_t &rx )
        {
            return lx.eventid < rx.eventid;
        } );

        time_buf += "\n";

        // Show tooltip with the closest events we could drum up
        for ( graph_info_t::hovered_t &hov : gi.hovered_items )
        {
            trace_event_t &event = get_event( hov.eventid );

            m_eventlist.highlight_ids.push_back( event.id );

            // Add event id and distance from cursor to this event
            time_buf += string_format( "\n%s%u%s %c%s",
                                       s_textclrs().str( TClr_Bright ), hov.eventid, s_textclrs().str( TClr_Def ),
                                       hov.neg ? '-' : ' ',
                                       ts_to_timestr( hov.dist_ts, 4 ).c_str() );

            // If this isn't an ftrace print event, add the event name
            if ( !event.is_ftrace_print() )
                time_buf += std::string( " " ) + event.name;

            // If this is a vblank event, add the crtc
            if ( event.crtc >= 0 )
                time_buf += std::to_string( event.crtc );

            // Add colored string for ftrace print events
            if ( event.is_ftrace_print() )
            {
                const char *buf = get_event_field_val( event, "buf" );

                if ( buf[ 0 ] )
                    time_buf += " " + s_textclrs().mstr( buf, event.color );
            }
            else if ( event.is_sched_switch() && ( event.duration != ( uint32_t )-1 ) )
            {
                const char *prev_pid_str = get_event_field_val( event, "prev_pid" );

                if ( prev_pid_str )
                {
                    int prev_pid = atoi( prev_pid_str );
                    const char *prev_comm = m_trace_events.comm_from_pid( prev_pid, prev_pid_str );
                    std::string timestr = ts_to_timestr( event.duration, 4 );

                    time_buf += string_format( " %s (%s)", prev_comm, timestr.c_str() );
                }
            }
        }

        // Mark the first event in the list as our hovered graph event
        m_graph.hovered_eventid = gi.hovered_items[ 0 ].eventid;

        if ( sync_event_list_to_graph && !m_eventlist.do_gotoevent )
        {
            m_eventlist.do_gotoevent = true;
            m_eventlist.goto_eventid = gi.hovered_items[ 0 ].eventid;
        }
    }

    if ( is_valid_id( gi.hovered_fence_signaled ) )
    {
        const trace_event_t &event_hov = get_event( gi.hovered_fence_signaled );
        std::string context = get_event_gfxcontext_str( event_hov );
        const std::vector< uint32_t > *plocs = m_trace_events.get_gfxcontext_locs( context.c_str() );

        time_buf += string_format( "\n\n%s",
                                   m_trace_events.tgidcomm_from_commstr( event_hov.user_comm ) );

        for ( uint32_t id : *plocs )
        {
            const trace_event_t &event = get_event( id );
            const char *name = event.get_timeline_name( event.name );
            std::string timestr = ts_to_timestr( event.duration, 4 );

            if ( gi.hovered_items.empty() )
                m_eventlist.highlight_ids.push_back( id );

            time_buf += string_format( "\n  %s%u%s %s duration: %s",
                                       s_textclrs().str( TClr_Bright ), event.id, s_textclrs().str( TClr_Def ),
                                       name,
                                       s_textclrs().mstr( timestr, event_hov.color ).c_str() );
        }

        if ( sync_event_list_to_graph && !m_eventlist.do_gotoevent )
        {
            // Sync event list to first event id in this context
            m_eventlist.do_gotoevent = true;
            m_eventlist.goto_eventid = plocs->at( 0 );
        }
    }

    ImGui::SetTooltip( "%s", time_buf.c_str() );
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

    // Check if mouse if over our graph and we've got focus
    m_graph.is_mouse_over = gi.mouse_pos_in_graph() &&
                         ImGui::IsRootWindowOrAnyChildFocused();

    // If we don't own the mouse and we don't have focus, bail.
    if ( !m_graph.mouse_captured && !m_graph.is_mouse_over )
        return;

    if ( m_graph.mouse_captured )
    {
        graph_handle_mouse_captured( gi );
        return;
    }

    // Mouse is over our active graph window
    {
        int64_t mouse_ts = gi.screenx_to_ts( gi.mouse_pos.x );

        m_graph.ts_marker_mouse = mouse_ts;

        // Set the tooltip
        graph_set_mouse_tooltip( gi, mouse_ts );

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
            if ( s_keybd().is_ctrl_down() )
            {
                // ctrl + click: select area
                m_graph.mouse_captured = MOUSE_CAPTURED_SELECT_AREA;
                ImGui::CaptureMouseFromApp( true );
                m_graph.mouse_capture_pos = gi.mouse_pos;
            }
            else if ( s_keybd().is_shift_down() )
            {
                // shift + click: zoom
                m_graph.mouse_captured = MOUSE_CAPTURED_ZOOM;
                ImGui::CaptureMouseFromApp( true );
                m_graph.mouse_capture_pos = gi.mouse_pos;
            }
            else
            {
                // click: pan
                m_graph.mouse_captured = MOUSE_CAPTURED_PAN;
                ImGui::CaptureMouseFromApp( true );
                m_graph.mouse_capture_pos = gi.mouse_pos;
            }
        }
        else if ( ImGui::IsMouseClicked( 1 ) )
        {
            // right click: popup menu
            m_graph.popupmenu = true;

            m_graph.rows_hidden_rows = m_graph.rows.get_hidden_rows_list();

            ImGui::OpenPopup( "GraphPopup" );
        }
        else if ( ImGui::GetIO().MouseWheel )
        {
            bool zoomin = ( ImGui::GetIO().MouseWheel > 0.0f );

            graph_zoom( mouse_ts, gi.ts0, zoomin );
        }
    }
}
