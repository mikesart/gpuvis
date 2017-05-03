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
#include <future>
#include <set>
#include <sys/stat.h>
#include <sys/types.h>
#include <unordered_map>
#include <vector>
#include <array>

#include <SDL.h>

#include "GL/gl3w.h"
#include "gpuvis.h"

/*
  TODO mikesart: Check if entire rows are clipped when drawing...

  From DanG:
    * Add Browse button to load a trace file
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

class event_renderer_t
{
public:
    event_renderer_t( float y_in, float w_in, float h_in );

    void add_event( float x );
    void done();

    void set_y( float y_in, float h_in );

protected:
    void start( float x );
    void draw();

public:
    float x0, x1;
    uint32_t num_events;

    float y, w, h;
};

typedef std::function< uint32_t ( class graph_info_t &gi ) > RenderGraphRowCallback;

struct row_info_t
{
    uint32_t id;
    std::string row_name;

    float row_y;
    float row_h;

    TraceEvents::loc_type_t loc_type;
    const std::vector< uint32_t > *plocs;

    RenderGraphRowCallback render_cb;
};

class graph_info_t
{
public:
    void init_row_info( TraceWin *win, const std::vector< GraphRows::graph_rows_info_t > &graph_rows );

    void init( TraceWin *win, float x, float w );
    void set_pos_y( float y, float h, const row_info_t *ri );

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

    struct hovered_t
    {
        bool neg;
        int64_t dist_ts;
        uint32_t eventid;
    };
    const size_t hovered_max = 6;
    std::vector< hovered_t > hovered_items;

    // Id of hovered / selected fence signaled event
    uint32_t hovered_fence_signaled = INVALID_ID;

    bool do_zoom_gfx;
    bool timeline_render_user;
    bool graph_only_filtered;

    std::vector< row_info_t > row_info;
    const row_info_t *prinfo_cur = nullptr;
    const row_info_t *prinfo_gfx = nullptr;
    const row_info_t *prinfo_gfx_hw = nullptr;

    float text_h;
    float row_h;
    float visible_graph_height;
    float total_graph_height;
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

static void imgui_draw_text( float x, float y, const char *text, ImU32 color )
{
#if 0
    //$ Don't draw background behind row label text - think it obscures the graph a bit.
    ImVec2 textsize = ImGui::CalcTextSize( text );

    ImGui::GetWindowDrawList()->AddRectFilled(
        ImVec2( x, y ), ImVec2( x + textsize.x, y + textsize.y ),
        col_get( col_RowLabelBk) );
#endif

    ImGui::GetWindowDrawList()->AddText( ImVec2( x, y ), color, text );
}

const event_field_t *find_event_field( const std::vector< event_field_t > &fields, const char *name )
{
    for ( const event_field_t &field : fields )
    {
        if ( !strcmp( field.key, name ) )
            return &field;
    }

    return NULL;
}

const char *get_event_field_val( const std::vector< event_field_t > &fields, const char *name )
{
    const event_field_t *field = find_event_field( fields, name );

    return field ? field->value : "";
}

/*
 * event_renderer_t
 */
event_renderer_t::event_renderer_t( float y_in, float w_in, float h_in )
{
    y = y_in;
    w = w_in;
    h = h_in;

    start( -1.0f );
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

void event_renderer_t::add_event( float x )
{
    if ( x0 < 0.0f )
    {
        // First event
        start( x );
    }
    else if ( x - x1 <= 1.0f )
    {
        // New event real close to last event
        x1 = x;
        num_events++;
    }
    else
    {
        // New event is away from current group, so draw.
        draw();

        // Start a new group
        start( x );
    }
}

void event_renderer_t::done()
{
    if ( x0 != -1 )
    {
        draw();
        start( -1.0f );
    }
}

void event_renderer_t::start( float x )
{
    num_events = 0;
    x0 = x;
    x1 = x + .0001f;
}

void event_renderer_t::draw()
{
    int index = std::min< int >( col_1Event + num_events, col_6Event );
    ImU32 color = col_get( ( colors_t )index );
    float min_width = std::min< float >( num_events + 1.0f, 4.0f );
    float width = std::max< float >( x1 - x0, min_width );

    imgui_drawrect( x0, width, y, h, color );
}

static option_id_t get_comm_option_id( TraceLoader &loader, const std::string &row_name )
{
    option_id_t *optid = loader.m_name_optid_map.get_val( row_name );

    if ( optid )
        return *optid;

    if ( !strncmp( row_name.c_str(), "plot:", 5 ) )
        return loader.add_option_graph_rowsize( row_name.c_str() );

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

        plocs = win->m_trace_events.get_locs( row_name.c_str(), &rinfo.loc_type );

        rinfo.row_y = total_graph_height;
        rinfo.row_h = text_h * 2;
        rinfo.row_name = row_name;

        if ( !plocs )
        {
            // Nothing to render
            rinfo.render_cb = nullptr;
        }
        else if ( rinfo.loc_type == TraceEvents::LOC_TYPE_Print )
        {
            // ftrace print row
            optid = get_comm_option_id( win->m_loader, rinfo.row_name );
            rinfo.render_cb = std::bind( &TraceWin::graph_render_print_timeline, win, _1 );
        }
        else if ( rinfo.loc_type == TraceEvents::LOC_TYPE_Plot )
        {
            optid = get_comm_option_id( win->m_loader, rinfo.row_name );
            rinfo.render_cb = std::bind( &TraceWin::graph_render_plot, win, _1 );
        }
        else if ( rinfo.loc_type == TraceEvents::LOC_TYPE_Timeline )
        {
            optid = get_comm_option_id( win->m_loader, rinfo.row_name );
            rinfo.render_cb = std::bind( &TraceWin::graph_render_row_timeline, win, _1 );
        }
        else if ( rinfo.loc_type == TraceEvents::LOC_TYPE_Timeline_hw )
        {
            rinfo.row_h = 2 * text_h;
            rinfo.render_cb = std::bind( &TraceWin::graph_render_hw_row_timeline, win, _1 );
        }
        else
        {
            // LOC_Type_Comm or LOC_TYPE_Tdopexpr hopefully
            rinfo.render_cb = std::bind( &TraceWin::graph_render_row_events, win, _1 );
        }

        if ( optid != OPT_Invalid )
        {
            int rows = ( optid != OPT_Invalid ) ?
                        Clamp< int >( win->m_loader.get_opt( optid ), 2, 50 ) : 4;

            rinfo.row_h = rows * text_h;
        }

        rinfo.id = id++;
        rinfo.plocs = plocs;
        row_info.push_back( rinfo );

        total_graph_height += rinfo.row_h + graph_row_padding;
    }

    total_graph_height += imgui_scale( 2.0f );
    total_graph_height = std::max< float >( total_graph_height, 4 * row_h );

    prinfo_gfx = find_row( "gfx" );
    prinfo_gfx_hw = find_row( "gfx hw" );
}

void graph_info_t::init( TraceWin *win, float x_in, float w_in )
{
    x = x_in;
    w = w_in;

    ts0 = win->m_graph.start_ts + win->m_eventlist.tsoffset;
    ts1 = ts0 + win->m_graph.length_ts;

    eventstart = win->ts_to_eventid( ts0 );
    eventend = win->ts_to_eventid( ts1 );

    tsdx = ts1 - ts0 + 1;
    tsdxrcp = 1.0 / tsdx;

    mouse_pos = ImGui::IsRootWindowOrAnyChildFocused() ?
                ImGui::GetMousePos() : ImVec2( -1024, -1024 );

    // Check if we're supposed to render filtered events only
    graph_only_filtered = win->m_loader.m_options[ OPT_GraphOnlyFiltered ].val &&
                          !win->m_eventlist.filtered_events.empty();

    timeline_render_user = !!win->m_loader.get_opt( OPT_TimelineRenderUserSpace );

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

void graph_info_t::set_pos_y( float y_in, float h_in, const row_info_t *ri )
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

static size_t str_get_digit_loc( const char *str )
{
    const char *buf = str;

    for ( ; *buf; buf++ )
    {
        if ( isdigit( *buf ) )
            return buf - str;
    }

    return 0;
}

const char *CreatePlotDlg::get_plot_str( const trace_event_t &event )
{
    if ( event.is_ftrace_print() )
    {
        const char *buf = get_event_field_val( event.fields, "buf" );

        if ( str_get_digit_loc( buf ) )
            return buf;
    }

    return NULL;
}

bool CreatePlotDlg::init( TraceEvents &trace_events, uint32_t eventid )
{
    m_plot = NULL;
    m_plot_name = "";

    if ( !is_valid_id( eventid ) )
        return false;

    const trace_event_t &event = trace_events.m_events[ eventid ];
    const char *buf = get_event_field_val( event.fields, "buf" );
    size_t digit_loc = str_get_digit_loc( buf );

    m_plot_buf = buf;
    m_plot_err_str.clear();

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
    if ( digit_loc )
    {
        std::string shortstr;
        std::string fullstr = string_ltrimmed( std::string( buf, digit_loc ) );

        // Skip the [Blah blah] section for the plot name
        if ( fullstr[ 0 ] == '[' )
        {
            char *right_bracket = strchr( &fullstr[ 0 ], ']' );

            if ( right_bracket )
                shortstr = std::string( right_bracket + 1 );
        }
        if ( shortstr.empty() )
            shortstr = fullstr;

        std::string namestr = string_trimmed( string_remove_punct( shortstr ) );
        strcpy_safe( m_plot_name_buf, namestr.c_str() );

        std::string filter_str = string_format( "$buf =~ \"%s\"", fullstr.c_str() );
        strcpy_safe( m_plot_filter_buf, filter_str.c_str() );

        fullstr += "%f";
        strcpy_safe( m_plot_scanf_buf, fullstr.c_str() );

        ImGui::OpenPopup( "Create Plot" );
        return true;
    }

    return false;
}

template < size_t T >
static void plot_input_text( const char *label, char ( &buf )[ T ], float x, float w, ImGuiTextEditCallback callback = nullptr )
{
    ImGuiInputTextFlags flags = callback ? ImGuiInputTextFlags_CallbackCharFilter : 0;

    ImGui::PushID( label );

    ImGui::AlignFirstTextHeightToWidgets();
    ImGui::Text( "%s", label );

    ImGui::SameLine();
    ImGui::PushItemWidth( w );
    ImGui::SetCursorPos( { x, ImGui::GetCursorPos().y } );
    ImGui::InputText( "##plot_input_text", buf, sizeof( buf ), flags, callback );

    ImGui::PopID();
}

bool CreatePlotDlg::render_dlg( TraceEvents &trace_events )
{
    if ( !ImGui::BeginPopupModal( "Create Plot", NULL, ImGuiWindowFlags_AlwaysAutoResize ) )
        return false;

    float w = imgui_scale( 350.0f );
    const ImVec2 button_size = { imgui_scale( 120.0f ), 0.0f };
    const ImVec2 text_size = ImGui::CalcTextSize( "Plot Scan Str: " );
    float x = ImGui::GetCursorPos().x + text_size.x;

    ImGui::TextColored( ImVec4( 1, 1, 0, 1 ), "%s", m_plot_buf.c_str() );
    ImGui::NewLine();

    struct TextFilters {
        static int FilterImGuiLetters( ImGuiTextEditCallbackData *data )
                { return ( ( data->EventChar < 256 ) && ispunct( data->EventChar ) ); }
    };
    plot_input_text( "Plot Name:", m_plot_name_buf, x, w, TextFilters::FilterImGuiLetters );

    plot_input_text( "Plot Filter:", m_plot_filter_buf, x, w, TextFilters::FilterImGuiLetters );

    if ( m_plot_err_str.size() )
        ImGui::TextColored( ImVec4( 1, 0, 0, 1), "%s", m_plot_err_str.c_str() );

    plot_input_text( "Plot Scan Str:", m_plot_scanf_buf, x, w, TextFilters::FilterImGuiLetters );

    ImGui::NewLine();

    bool disabled = !m_plot_name_buf[ 0 ] || !m_plot_filter_buf[ 0 ] || !m_plot_scanf_buf[ 0 ];
    if ( disabled )
        ImGui::PushStyleColor( ImGuiCol_Text, ImGui::GetColorVec4( ImGuiCol_TextDisabled ) );

    if ( ImGui::Button( "Create", button_size ) && !disabled )
    {
        m_plot_err_str.clear();
        const std::vector< uint32_t > *plocs = trace_events.get_tdopexpr_locs(
                    m_plot_filter_buf, &m_plot_err_str );

        if ( !plocs && m_plot_err_str.empty() )
        {
            m_plot_err_str = "WARNING: No events found.";
        }
        else
        {
            m_plot_name = std::string( "plot:" ) + m_plot_name_buf;

            uint32_t hashval = fnv_hashstr32( m_plot_name.c_str() );
            GraphPlot &plot = trace_events.m_graph_plots.m_map[ hashval ];

            if ( plot.init( trace_events, m_plot_name,
                            m_plot_filter_buf, m_plot_scanf_buf ) )
            {
                m_plot = &plot;
                ImGui::CloseCurrentPopup();
            }
            else
            {
                m_plot_err_str = "WARNING: No plot data values found.";
            }
        }
    }

    if ( disabled )
        ImGui::PopStyleColor();

    ImGui::SameLine();
    if ( ImGui::Button( "Cancel", button_size ) || imgui_key_pressed( ImGuiKey_Escape ) )
        ImGui::CloseCurrentPopup();

    ImGui::EndPopup();

    return !!m_plot;
}

void CreatePlotDlg::add_plot( CIniFile &inifile, GraphRows &rows )
{
    size_t print_row_index = rows.find_row( "print", rows.m_graph_rows_list.size() - 1 );
    auto it = rows.m_graph_rows_list.begin() + print_row_index + 1;

    rows.m_graph_rows_list.insert( it,
                { TraceEvents::LOC_TYPE_Plot, m_plot->m_plotdata.size(), m_plot_name, false } );

    std::string val = string_format( "%s\t%s", m_plot->m_filter_str.c_str(), m_plot->m_scanf_str.c_str() );
    inifile.PutStr( m_plot_name.c_str(), val.c_str(), "$graph_plots$" );
}

bool GraphPlot::init( TraceEvents &trace_events, const std::string &name,
                      const std::string &filter_str, const std::string scanf_str )
{
    m_name = name;
    m_filter_str = filter_str;
    m_scanf_str = scanf_str;

    std::string errstr;
    const std::vector< uint32_t > *plocs = trace_events.get_tdopexpr_locs( m_filter_str.c_str(), &errstr );

    if ( plocs )
    {
        // Find the "%f" specifier
        const char *valstr = strstr( m_scanf_str.c_str(), "%f" );

        if ( valstr && ( valstr > m_scanf_str.c_str() ) )
        {
            // Grab the text before the %f token
            size_t prefixlen = valstr - m_scanf_str.c_str();
            std::string prefixstr = std::string( m_scanf_str.c_str(), prefixlen );

            m_color_line = trace_events.m_events[ plocs->front() ].color;
            m_color_point = imgui_col_complement( m_color_line );

            for ( uint32_t idx : *plocs )
            {
                const char *valfstr;
                const trace_event_t &event = trace_events.m_events[ idx ];
                const char *buf = get_event_field_val( event.fields, "buf" );

                // If we have a printk string, search for our prefix string in it
                if ( buf && ( valfstr = strcasestr( buf, prefixstr.c_str() ) ) )
                {
                    char *endptr;
                    const char *nptr = valfstr + prefixstr.size();
                    float valf = strtof( nptr, &endptr );

                    if ( endptr != nptr )
                    {
                        m_minval = std::min< float >( m_minval, valf );
                        m_maxval = std::max< float >( m_maxval, valf );

                        m_plotdata.push_back( { event.ts, event.id, valf } );
                    }
                }
            }

            if ( m_minval == m_maxval )
            {
                m_minval--;
                m_maxval++;
            }

            for ( plotdata_t &data : m_plotdata )
                data.valf_norm = ( data.valf - m_minval ) / ( m_maxval - m_minval );
        }
    }

    return !m_plotdata.empty();
}

uint32_t GraphPlot::find_ts_index( int64_t ts0 )
{
    auto lambda = []( const GraphPlot::plotdata_t &lhs, int64_t ts )
                            { return lhs.ts < ts; };
    auto i = std::lower_bound( m_plotdata.begin(), m_plotdata.end(), ts0, lambda );

    if ( i != m_plotdata.end() )
    {
        size_t index = i - m_plotdata.begin();

        return ( index > 0 ) ? ( index - 1 ) : 0;
    }

    return ( uint32_t )-1;
}

uint32_t TraceWin::graph_render_plot( graph_info_t &gi )
{
    std::vector< ImVec2 > points;
    const char *row_name = gi.prinfo_cur->row_name.c_str();
    uint32_t hashval = fnv_hashstr32( row_name );
    GraphPlot &plot = m_trace_events.m_graph_plots.m_map[ hashval ];
    uint32_t index0 = plot.find_ts_index( gi.ts0 );
    uint32_t index1 = plot.find_ts_index( gi.ts1 );

    if ( index1 == ( uint32_t)-1 )
        index1 = plot.m_plotdata.size();

    points.reserve( index1 - index0 + 10 );

    for ( size_t idx = index0; idx < plot.m_plotdata.size(); idx++ )
    {
        GraphPlot::plotdata_t &data = plot.m_plotdata[ idx ];
        float x = gi.ts_to_screenx( data.ts );
        float y = gi.y + gi.h * ( 1.0f - data.valf_norm );

        points.push_back( ImVec2( x, y ) );

        // Check if we're mouse hovering this event
        if ( gi.mouse_over )
            gi.add_mouse_hovered_event( x, get_event( data.eventid ) );

        if ( x >= gi.x + gi.w )
            break;
    }

    if ( points.size() )
    {
        bool anti_aliased = true;
        bool closed = false;
        float thickness = 2.0f;

        ImGui::GetWindowDrawList()->AddPolyline( points.data(), points.size(),
                                                 plot.m_color_line, closed, thickness, anti_aliased );

        for ( const ImVec2 &pt : points )
        {
            imgui_drawrect( pt.x - imgui_scale( 1.5f ), imgui_scale( 3.0f ),
                            pt.y - imgui_scale( 1.5f ), imgui_scale( 3.0f ),
                            plot.m_color_point );
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
    bool timeline_labels = m_loader.get_opt( OPT_PrintTimelineLabels ) != 0;
    uint32_t row_count = std::max< uint32_t >( 1, gi.h / gi.text_h - 1 );

    row_draw_info.resize( row_count + 1 );

    // We need to start drawing to the left of 0 for timeline_labels
    int64_t ts = timeline_labels ? gi.screenx_to_ts( gi.x - m_trace_events.m_buf_size_max_x ) : gi.ts0;
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
            if ( x - x0 > print_info->buf_size.x )
            {
                imgui_drawrect( x0, print_info->buf_size.x + 1.0f, y + imgui_scale( 2.0f ), print_info->buf_size.y,
                                draw_info.event->color );
                imgui_draw_text( x0, y + imgui_scale( 2.0f ), print_info->buf, IM_COL32_WHITE );
            }
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

            imgui_drawrect( x0, print_info->buf_size.x + 1.0f, y + imgui_scale( 2.0f ), print_info->buf_size.y, event->color );
            imgui_draw_text( x0, y + imgui_scale( 2.0f ), print_info->buf, IM_COL32_WHITE );
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
    ImU32 col_event = col_get( col_1Event );

    ImRect hov_rect;
    ImU32 last_color = 0;
    float y = gi.y;
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
                ImGui::GetWindowDrawList()->AddText(
                            ImVec2( x0 + imgui_scale( 2.0f ), y + imgui_scale( 2.0f ) ),
                            col_get( col_BarText ), label );
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

    if ( hov_rect.Min.x < gi.x + gi.w )
        ImGui::GetWindowDrawList()->AddRect( hov_rect.Min, hov_rect.Max, col_get( col_BarSelRect ) );

    imgui_pop_smallfont();

    return num_events;
}

uint32_t TraceWin::graph_render_row_timeline( graph_info_t &gi )
{
    imgui_push_smallfont();

    ImRect hov_rect;
    uint32_t num_events = 0;
    ImU32 col_hwrunning = col_get( col_BarHwRunning );
    ImU32 col_userspace = col_get( col_BarUserspace );
    ImU32 col_hwqueue = col_get( col_BarHwQueue );
    ImU32 color_1event = col_get( col_1Event );
    const std::vector< uint32_t > &locs = *gi.prinfo_cur->plocs;

    uint32_t timeline_row_count = gi.h / gi.text_h;

    bool render_timeline_events = !!m_loader.get_opt( OPT_TimelineEvents );
    bool render_timeline_labels = !!m_loader.get_opt( OPT_TimelineLabels );

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
                        ImGui::GetWindowDrawList()->AddText( ImVec2( x_text, y + imgui_scale( 1.0f ) ),
                                                             col_get( col_BarText ), cs_ioctl.user_comm );
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
                            if ( gi.add_mouse_hovered_event( x_user_start, cs_ioctl ) && ( hov_rect.Min.x == FLT_MAX ) )
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

    if ( hov_rect.Min.x < gi.x + gi.w )
        ImGui::GetWindowDrawList()->AddRect( hov_rect.Min, hov_rect.Max, col_get( col_BarSelRect ) );

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

        float x = gi.ts_to_screenx( event.ts );

        if ( eventid == m_eventlist.hovered_eventid )
            draw_hovered_event = true;
        else if ( eventid == m_eventlist.selected_eventid )
            draw_selected_event = true;

        // Check if we're mouse hovering this event
        if ( gi.mouse_over )
            gi.add_mouse_hovered_event( x, event );

        event_renderer.add_event( x );
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
                    col_get( col_HovEvent ) );
    }

    if ( draw_selected_event )
    {
        trace_event_t &event = get_event( m_eventlist.selected_eventid );
        float x = gi.ts_to_screenx( event.ts );

        ImGui::GetWindowDrawList()->AddCircleFilled(
                    ImVec2( x, gi.y + gi.h / 2.0f ),
                    imgui_scale( 5.0f ),
                    col_get( col_SelEvent ) );
    }

    return num_events;
}

void TraceWin::graph_render_row( graph_info_t &gi )
{
    const std::string row_name = gi.prinfo_cur->row_name;

    if ( gi.mouse_over )
        m_graph.mouse_over_row_name = row_name;

    // Draw background
    ImGui::GetWindowDrawList()->AddRectFilled(
        ImVec2( gi.x, gi.y ),
        ImVec2( gi.x + gi.w, gi.y + gi.h ),
        col_get( col_GraphRowBk ) );

    // Call the render callback function
    uint32_t num_events = gi.prinfo_cur->render_cb ? gi.prinfo_cur->render_cb( gi ) : 0;

    // Draw row label
    std::string label = string_format( "%u) %s", gi.prinfo_cur->id, row_name.c_str() );
    imgui_draw_text( gi.x, gi.y, label.c_str(),
                     col_get( col_RowLabel ) );

    if ( num_events )
    {
        label = string_format( "%u events", num_events );
        imgui_draw_text( gi.x, gi.y + ImGui::GetTextLineHeight(), label.c_str(),
                         col_get( col_RowLabel ) );
    }
}

void TraceWin::graph_render_vblanks( graph_info_t &gi )
{
    // Draw time ticks every millisecond
    int64_t tsstart = std::max< int64_t >( gi.ts0 / MSECS_PER_SEC - 1, 0 ) * MSECS_PER_SEC;
    float x0 = gi.ts_to_x( tsstart );
    float dx = gi.w * MSECS_PER_SEC * gi.tsdxrcp;

    if ( dx > imgui_scale( 4.0f ) )
    {
        for ( ; x0 <= gi.w; x0 += dx )
        {
            imgui_drawrect( gi.x + x0, imgui_scale( 1.0f ),
                            gi.y, imgui_scale( 16.0f ),
                            col_get( col_TimeTick ) );

            if ( dx >= imgui_scale( 35.0f ) )
            {
                for ( int i = 1; i < 4; i++ )
                {
                    imgui_drawrect( gi.x + x0 + i * dx / 4, imgui_scale( 1.0f ),
                                    gi.y, imgui_scale( 4.0f ),
                                    col_get( col_TimeTick ) );
                }
            }
        }
    }

    // Draw vblank events on every graph.
    const std::vector< uint32_t > *vblank_locs = m_trace_events.get_tdopexpr_locs( "$name=drm_vblank_event" );

    if ( vblank_locs )
    {
        for ( size_t idx = vec_find_eventid( *vblank_locs, gi.eventstart );
              idx < vblank_locs->size();
              idx++ )
        {
            uint32_t id = vblank_locs->at( idx );

            if ( id > gi.eventend )
                break;

            trace_event_t &event = get_event( id );

            if ( m_loader.get_opt_crtc( event.crtc ) )
            {
                // drm_vblank_event0: blue, drm_vblank_event1: red
                colors_t col = ( event.crtc > 0 ) ? col_VBlank1 : col_VBlank0;
                float x = gi.ts_to_screenx( event.ts );

                imgui_drawrect( x, imgui_scale( 1.0f ),
                                gi.y, gi.h,
                                col_get( col ) );
            }
        }
    }

    // Draw location line for mouse if mouse is over graph
    if ( m_graph.is_mouse_over &&
         gi.mouse_pos.x >= gi.x &&
         gi.mouse_pos.x <= gi.x + gi.w )
    {
        imgui_drawrect( gi.mouse_pos.x, imgui_scale( 2.0f ),
                        gi.y, gi.h,
                        col_get( col_MousePos ) );
    }

    if ( is_valid_id( m_eventlist.hovered_eventid ) )
    {
        trace_event_t &event = get_event( m_eventlist.hovered_eventid );

        if ( event.ts >= gi.ts0 && event.ts <= gi.ts1 )
        {
            float x = gi.ts_to_screenx( event.ts );

            imgui_drawrect( x, imgui_scale( 1.0f ),
                            gi.y, gi.h,
                            col_get( col_HovEvent, 120 ) );
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
                            col_get( col_SelEvent, 120 ) );
        }
    }

    // Draw mouse selection location
    if ( ( m_graph.mouse_captured == MOUSE_CAPTURED_ZOOM ) ||
         ( m_graph.mouse_captured == MOUSE_CAPTURED_SELECT_AREA ) )
    {
        float mousex0 = m_graph.mouse_capture_pos.x;
        float mousex1 = gi.mouse_pos.x;

        imgui_drawrect( mousex0, mousex1 - mousex0,
                        gi.y, gi.h,
                        col_get( col_ZoomSel ) );
    }

    if ( m_eventlist.show )
    {
        // Draw rectangle for visible event list contents
        if ( is_valid_id( m_eventlist.start_eventid ) &&
             is_valid_id( m_eventlist.end_eventid ) )
        {
            trace_event_t &event0 = get_event( m_eventlist.start_eventid );
            trace_event_t &event1 = get_event( m_eventlist.end_eventid - 1 );
            float xstart = gi.ts_to_screenx( event0.ts );
            float xend = gi.ts_to_screenx( event1.ts );

            ImGui::GetWindowDrawList()->AddRect(
                        ImVec2( xstart, gi.y + imgui_scale( 20 ) ),
                        ImVec2( xend, gi.y + gi.h - imgui_scale( 30 ) ),
                        col_get( col_EventListSel ) );
        }
    }
}

void TraceWin::graph_range_check_times()
{
    const std::vector< trace_event_t > &events = m_trace_events.m_events;

    if ( m_graph.length_ts < m_graph.s_min_length )
    {
        m_graph.length_ts = m_graph.s_min_length;
        m_graph.do_length_timestr = true;
    }
    else if ( m_graph.length_ts > m_graph.s_max_length )
    {
        m_graph.length_ts = m_graph.s_max_length;
        m_graph.do_length_timestr = true;
    }

    // Sanity check the graph start doesn't go completely off the rails.
    if ( m_graph.start_ts + m_eventlist.tsoffset < events.front().ts - 1 * MSECS_PER_SEC )
    {
        m_graph.start_ts = events.front().ts - m_eventlist.tsoffset - 1 * MSECS_PER_SEC;
        m_graph.do_start_timestr = true;
    }
    else if ( m_graph.start_ts + m_eventlist.tsoffset > events.back().ts )
    {
        m_graph.start_ts = events.back().ts - m_eventlist.tsoffset;
        m_graph.do_start_timestr = true;
    }
}

void TraceWin::graph_handle_hotkeys()
{
    if ( m_graph.saved_locs.size() < 9 )
        m_graph.saved_locs.resize( 9 );

    if ( ImGui::GetIO().KeyCtrl )
    {
        bool keyshift = ImGui::GetIO().KeyShift;

        if ( keyshift && ImGui::IsKeyPressed( 'z' ) )
        {
            m_loader.m_options[ OPT_TimelineZoomGfx ].val ^= 1;
        }
        else
        {
            for ( int key = '1'; key <= '9'; key++ )
            {
                if ( ImGui::IsKeyPressed( key ) )
                {
                    int index = key - '1';

                    if ( keyshift )
                    {
                        // ctrl+shift+#: save location
                        m_graph.saved_locs[ index ] = std::make_pair( m_graph.start_ts, m_graph.length_ts );
                    }
                    else if ( m_graph.saved_locs[ index ].second )
                    {
                        // ctrl+#: goto location
                        m_graph.start_ts = m_graph.saved_locs[ index ].first;
                        m_graph.length_ts = m_graph.saved_locs[ index ].second;
                        m_graph.do_start_timestr = true;
                        m_graph.do_length_timestr = true;
                    }
                    break;
                }
            }
        }
    }
}

void TraceWin::graph_handle_keyboard_scroll()
{
    if ( !ImGui::IsWindowFocused() )
        return;

    int64_t start_ts = m_graph.start_ts + m_eventlist.tsoffset;
    const std::vector< trace_event_t > &events = m_trace_events.m_events;

    if ( imgui_key_pressed( ImGuiKey_UpArrow ) )
    {
        m_graph.start_y += ImGui::GetTextLineHeightWithSpacing() * 4;
    }
    else if ( imgui_key_pressed( ImGuiKey_DownArrow ) )
    {
        m_graph.start_y -= ImGui::GetTextLineHeightWithSpacing() * 4;
    }
    else if ( imgui_key_pressed( ImGuiKey_LeftArrow ) )
    {
        start_ts = std::max< int64_t >( start_ts - 9 * m_graph.length_ts / 10,
                                        -MSECS_PER_SEC );
    }
    else if ( imgui_key_pressed( ImGuiKey_RightArrow ) )
    {
        start_ts = std::min< int64_t >( start_ts + 9 * m_graph.length_ts / 10,
                                        events.back().ts - m_graph.length_ts + MSECS_PER_SEC );
    }
    else if ( imgui_key_pressed( ImGuiKey_Home ) )
    {
        start_ts = events.front().ts - MSECS_PER_SEC;
    }
    else if ( imgui_key_pressed( ImGuiKey_End ) )
    {
        start_ts = events.back().ts - m_graph.length_ts + MSECS_PER_SEC;
    }

    start_ts -= m_eventlist.tsoffset;
    if ( start_ts != m_graph.start_ts )
    {
        m_graph.start_ts = start_ts;
        m_graph.do_start_timestr = true;
    }
}

static void calc_process_graph_height( TraceWin *win, graph_info_t &gi )
{
    // Zoom mode if we have a gfx row and zoom option is set
    option_id_t optid;
    float max_graph_size;

    gi.do_zoom_gfx = gi.prinfo_gfx && win->m_loader.get_opt( OPT_TimelineZoomGfx );

    if ( gi.do_zoom_gfx )
    {
        optid = OPT_GraphHeightZoomed;
        max_graph_size = imgui_scale( 60.0f ) * gi.row_h;
    }
    else
    {
        optid = OPT_GraphHeight;
        max_graph_size = gi.total_graph_height;
    }

    TraceLoader::option_t &opt = win->m_loader.m_options[ optid ];

    // First time initialization - start with about 15 rows
    if ( !opt.valf )
        opt.valf = 15.0f * gi.row_h;

    // Set up min / max sizes and clamp value in that range
    opt.valf_min = 4.0f * gi.row_h;
    opt.valf_max = Clamp< float >( max_graph_size, opt.valf_min, ImGui::GetWindowHeight() );
    opt.valf = Clamp< float >( opt.valf, opt.valf_min, opt.valf_max );

    gi.visible_graph_height = opt.valf;
}

void TraceWin::graph_render_process()
{
    graph_info_t gi;

    // Initialize our row size, location, etc information based on our graph rows
    gi.init_row_info( this, m_graph.rows.m_graph_rows_list );

    if ( gi.prinfo_gfx )
    {
        // Checkbox to toggle zooming gfx timeline view
        ImGui::SameLine();
        ImGui::CheckboxInt( "Zoom gfx timeline", &m_loader.m_options[ OPT_TimelineZoomGfx ].val );
    }

    // Figure out gi.visible_graph_height
    calc_process_graph_height( this, gi );

    // Make sure ts start and length values are mostly sane
    graph_range_check_times();

    ImGui::BeginChild( "EventGraph", ImVec2( 0, gi.visible_graph_height ), true );
    {
        ImVec2 windowpos = ImVec2( ImGui::GetWindowClipRectMin().x, ImGui::GetWindowPos().y );
        ImVec2 windowsize = ImGui::GetWindowSize();

        // Clear graph background
        imgui_drawrect( windowpos.x, windowsize.x,
                        windowpos.y, windowsize.y, col_get( col_GraphBk ) );

        // Initialize our graphics info struct
        gi.init( this, windowpos.x, windowsize.x );

        // Range check mouse pan values
        m_graph.start_y = Clamp< float >( m_graph.start_y,
                                          gi.visible_graph_height - gi.total_graph_height, 0.0f );

        // If we don't have a popup menu, clear the mouse over row name
        if ( !m_graph.popupmenu )
        {
            m_graph.mouse_over_row_name = "";
            m_graph.rename_comm_buf[ 0 ] = 0;
        }

        // If we have a gfx graph and we're zoomed, render only that
        if ( gi.do_zoom_gfx )
        {
            float gfx_hw_row_h = 0;

            if ( gi.prinfo_gfx_hw )
            {
                const row_info_t &ri = *gi.prinfo_gfx_hw;
                gfx_hw_row_h = ri.row_h + ImGui::GetStyle().FramePadding.y;

                gi.set_pos_y( windowpos.y + windowsize.y - ri.row_h, ri.row_h, &ri );
                graph_render_row( gi );
            }

            gi.timeline_render_user = true;
            gi.set_pos_y( windowpos.y, windowsize.y - gfx_hw_row_h, gi.prinfo_gfx );
            graph_render_row( gi );
        }
        else
        {
            // Pass 0: Render all !timeline rows
            // Pass 1: Render all timeline rows
            for ( int pass = 0; pass < 2; pass++ )
            {
                bool render_timelines = !!pass;

                for ( const row_info_t &ri : gi.row_info )
                {
                    bool is_timeline = ( ri.loc_type == TraceEvents::LOC_TYPE_Timeline );

                    if ( is_timeline == render_timelines )
                    {
                        gi.set_pos_y( windowpos.y + ri.row_y + m_graph.start_y, ri.row_h, &ri );
                        graph_render_row( gi );
                    }
                }
            }
        }

        // Render full graph lines: vblanks, mouse cursors, etc...
        gi.set_pos_y( windowpos.y, windowsize.y, NULL );
        graph_render_vblanks( gi );

        // Handle right, left, pgup, pgdown, etc in graph
        graph_handle_keyboard_scroll();

        // Handle hotkeys. Ie: Ctrl+Shift+1, etc
        graph_handle_hotkeys();

        // Render mouse tooltips, mouse selections, etc
        graph_handle_mouse( gi );

        if ( is_valid_id( m_graph.create_plot_eventid ) )
        {
            m_create_plot_dlg.init( m_trace_events, m_graph.create_plot_eventid );
            m_graph.create_plot_eventid = INVALID_ID;
        }
        if ( m_create_plot_dlg.render_dlg( m_trace_events ) )
            m_create_plot_dlg.add_plot( m_loader.m_inifile, m_graph.rows );
    }
    ImGui::EndChild();

    ImGui::Button( "##resize_graph", ImVec2( ImGui::GetContentRegionAvailWidth(), imgui_scale( 4.0f ) ) );
    if ( ImGui::IsItemHovered() )
        ImGui::SetMouseCursor( ImGuiMouseCursor_ResizeNS );
    if ( ImGui::IsItemActive() )
    {
        option_id_t opt = gi.do_zoom_gfx ? OPT_GraphHeightZoomed : OPT_GraphHeight;

        if ( ImGui::IsMouseClicked( 0 ) )
            m_graph.resize_graph_click_pos = m_loader.m_options[ opt ].valf;

        m_loader.m_options[ opt ].valf = m_graph.resize_graph_click_pos + ImGui::GetMouseDragDelta( 0 ).y;
    }
}

bool TraceWin::graph_render_popupmenu( graph_info_t &gi )
{
    option_id_t optid = OPT_Invalid;

    if ( !ImGui::BeginPopup( "GraphPopup" ) )
        return false;

    auto get_location_label_lambda = [this]( size_t i )
    {
        auto &pair = m_graph.saved_locs[ i ];
        std::string start = ts_to_timestr( pair.first );
        std::string len = ts_to_timestr( pair.second );
        return string_format( "Start:%s Length:%s", start.c_str(), len.c_str() );
    };

    imgui_text_bg( "Options", ImGui::GetColorVec4( ImGuiCol_Header ) );
    ImGui::Separator();

    if ( !m_graph.mouse_over_row_name.empty() )
    {
        optid = get_comm_option_id( m_loader, m_graph.mouse_over_row_name.c_str() );

        std::string label = string_format( "Hide row '%s'", m_graph.mouse_over_row_name.c_str() );

        if ( ImGui::MenuItem( label.c_str() ) )
            m_graph.rows.show_row( m_graph.mouse_over_row_name, GraphRows::HIDE_ROW );

        label = string_format( "Hide row '%s' and below", m_graph.mouse_over_row_name.c_str() );
        if ( ImGui::MenuItem( label.c_str() ) )
            m_graph.rows.show_row( m_graph.mouse_over_row_name, GraphRows::HIDE_ROW_AND_ALL_BELOW );
    }

    if ( !m_graph.rows_hidden_rows.empty() )
    {
        if ( ImGui::BeginMenu( "Show row" ) )
        {
            if ( ImGui::MenuItem( "All Rows" ) )
                m_graph.rows.show_row( "", GraphRows::SHOW_ALL_ROWS );

            ImGui::Separator();

            for ( const GraphRows::graph_rows_info_t &entry : m_graph.rows_hidden_rows )
            {
                const std::string label = string_format( "%s (%lu events)",
                                                         entry.row_name.c_str(), entry.event_count );

                if ( ImGui::MenuItem( label.c_str() ) )
                {
                    m_graph.rows.show_row( entry.row_name.c_str(), GraphRows::SHOW_ROW );
                }
            }

            ImGui::EndMenu();
        }
    }

    if ( !m_graph.mouse_over_row_name.empty() )
    {
        std::string move_label = string_format( "Move '%s' after", m_graph.mouse_over_row_name.c_str() );

        if ( ImGui::BeginMenu( move_label.c_str() ) )
        {
            for ( const GraphRows::graph_rows_info_t &entry : m_graph.rows.m_graph_rows_list )
            {
                if ( !entry.hidden && ( entry.row_name != m_graph.mouse_over_row_name ) )
                {
                    if ( ImGui::MenuItem( entry.row_name.c_str() ) )
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

    {
        ImGui::AlignFirstTextHeightToWidgets();
        ImGui::Text( "New Graph Row:" );

        ImGui::SameLine();
        if ( ImGui::InputText( "##new_graph_row", m_graph.new_row_buf, sizeof( m_graph.new_row_buf ),
                               ImGuiInputTextFlags_EnterReturnsTrue ) )
        {
            m_graph.new_row_errstr.clear();

            if ( m_trace_events.get_tdopexpr_locs( m_graph.new_row_buf, &m_graph.new_row_errstr ) )
            {
                m_graph.rows.add_row( m_trace_events, m_graph.new_row_buf );
                ImGui::CloseCurrentPopup();
            }
            else if ( m_graph.new_row_errstr.empty() )
            {
                m_graph.new_row_errstr = string_format( "ERROR: no events found for '%s'", m_graph.new_row_buf );
            }
        }

        if ( ImGui::IsItemHovered() )
        {
            std::string tooltip;

            tooltip += multi_text_color::yellow.m_str( "Add a new row with filtered events\n\n" );
            tooltip += "Examples:\n";
            tooltip += "  $pid = 4615\n";
            tooltip += "  $duration >= 5.5\n";
            tooltip += "  $buf =~ \"[Compositor] Warp\"\n";
            tooltip += "  ( $timeline = gfx ) && ( $id < 10 || $id > 100 )";

            imgui_set_tooltip( tooltip );
        }

        if ( !m_graph.new_row_errstr.empty() )
            ImGui::TextColored( ImVec4( 1, 0, 0, 1), "%s", m_graph.new_row_errstr.c_str() );
    }

    if ( is_valid_id( m_graph.hovered_eventid ) )
    {
        const trace_event_t &event = m_trace_events.m_events[ m_graph.hovered_eventid ];
        const char *plot_str = CreatePlotDlg::get_plot_str( event );

        if ( plot_str )
        {
            ImGui::Text( "Create Plot for" );
            ImGui::SameLine();

            ImGui::PushStyleColor( ImGuiCol_Text, ImVec4( 1, 1, 0, 1 ) );
            if ( ImGui::MenuItem( plot_str ) )
                m_graph.create_plot_eventid = event.id;
            ImGui::PopStyleColor();
        }
    }

    if ( m_trace_events.get_comm_locs( m_graph.mouse_over_row_name.c_str() ) )
    {
        if ( !m_graph.rename_comm_buf[ 0 ] )
        {
            strcpy_safe( m_graph.rename_comm_buf, m_graph.mouse_over_row_name.c_str() );

            char *slash = strrchr( m_graph.rename_comm_buf, '-' );
            if ( slash )
                *slash = 0;
        }

        ImGui::AlignFirstTextHeightToWidgets();
        ImGui::Text( "Rename '%s':", m_graph.mouse_over_row_name.c_str() );

        ImGui::SameLine();
        if ( ImGui::InputText( "##rename_comm", m_graph.rename_comm_buf, sizeof( m_graph.rename_comm_buf ),
                               ImGuiInputTextFlags_EnterReturnsTrue ) )
        {
            if ( rename_comm_event( m_graph.mouse_over_row_name.c_str(), m_graph.rename_comm_buf ) )
                ImGui::CloseCurrentPopup();
        }
    }

    if ( optid != OPT_Invalid )
    {
        TraceLoader::option_t &opt = m_loader.m_options[ optid ];

        ImGui::AlignFirstTextHeightToWidgets();
        ImGui::Text( "%s", opt.desc.c_str() );
        ImGui::SameLine();
        ImGui::PushItemWidth( imgui_scale( 150.0f ) );
        ImGui::SliderInt( "##row_size", &opt.val, opt.val_min, opt.val_max );
    }

    ImGui::Separator();

    if ( ImGui::BeginMenu( "Save Location" ) )
    {
        for ( size_t i = 0; i < m_graph.saved_locs.size(); i++ )
        {
            std::string label = get_location_label_lambda( i );
            std::string shortcut = string_format( "Ctrl+Shift+%c", ( int )( i + '1' ) );

            if ( ImGui::MenuItem( label.c_str(), shortcut.c_str() ) )
            {
                m_graph.saved_locs[ i ] = std::make_pair( m_graph.start_ts, m_graph.length_ts );
                break;
            }
        }

        ImGui::EndMenu();
    }

    if ( ImGui::BeginMenu( "Restore Location" ) )
    {
        for ( size_t i = 0; i < m_graph.saved_locs.size(); i++ )
        {
            if ( m_graph.saved_locs[ i ].second )
            {
                std::string label = get_location_label_lambda( i );
                std::string shortcut = string_format( "Ctrl+%c", ( int )( i + '1' ) );

                if ( ImGui::MenuItem( label.c_str(), shortcut.c_str() ) )
                {
                    m_graph.start_ts = m_graph.saved_locs[ i ].first;
                    m_graph.length_ts = m_graph.saved_locs[ i ].second;
                    m_graph.do_start_timestr = true;
                    m_graph.do_length_timestr = true;
                }
            }
        }

        ImGui::EndMenu();
    }

    ImGui::Separator();

    for ( size_t i = 0; i < m_loader.m_options.size(); i++ )
    {
        TraceLoader::option_t &opt = m_loader.m_options[ i ];

        if ( opt.hidden )
            continue;

        if ( ( i >= OPT_RenderCrtc0 ) &&
             ( i <= OPT_RenderCrtc9 ) )
        {
            if ( i - OPT_RenderCrtc0 > m_loader.m_crtc_max )
                continue;
        }

        ImGui::PushID( i );

        if ( opt.type == TraceLoader::OPT_Bool )
        {
            bool val = !!opt.val;
            if ( ImGui::MenuItem( opt.desc.c_str(), "", &val ) )
                opt.val = val;
        }
        else if ( opt.type == TraceLoader::OPT_Int )
        {
            ImGui::PushItemWidth( imgui_scale( 200.0f ) );
            ImGui::SliderInt( "##slider_int", &opt.val, opt.val_min, opt.val_max, opt.desc.c_str() );
            ImGui::PopItemWidth();
        }
        else
        {
            ImGui::PushItemWidth( imgui_scale( 200.0f ) );
            ImGui::SliderFloat( "##slider_float", &opt.valf, opt.valf_min, opt.valf_max, opt.desc.c_str() );
            ImGui::PopItemWidth();
        }

        ImGui::PopID();
    }

    ImGui::EndPopup();
    return true;
}

void TraceWin::graph_handle_mouse_captured( graph_info_t &gi )
{
    // Uncapture mouse if user hits escape
    if ( m_graph.mouse_captured && imgui_key_pressed( ImGuiKey_Escape ) )
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
            std::string time_buf0 = ts_to_timestr( event_ts0, m_eventlist.tsoffset );
            std::string time_buf1 = ts_to_timestr( event_ts1 - event_ts0 );

            // Show tooltip with starting time and length of selected area.
            imgui_set_tooltip( string_format( "%s (%s ms)", time_buf0.c_str(), time_buf1.c_str() ) );
        }
        else if ( m_graph.mouse_captured == MOUSE_CAPTURED_ZOOM )
        {
            m_graph.start_ts = event_ts0 - m_eventlist.tsoffset;
            m_graph.length_ts = event_ts1 - event_ts0;
            m_graph.do_start_timestr = true;
            m_graph.do_length_timestr = true;
        }
    }
    else if ( m_graph.mouse_captured == MOUSE_CAPTURED_PAN )
    {
        // click: pan
        if ( is_mouse_down )
        {
            float dx = gi.mouse_pos.x - m_graph.mouse_capture_pos.x;
            int64_t tsdiff = gi.dx_to_ts( dx );

            m_graph.start_ts -= tsdiff;
            m_graph.do_start_timestr = true;

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

void TraceWin::graph_set_mouse_tooltip( class graph_info_t &gi, int64_t mouse_ts )
{
    std::string time_buf = "Time: " + ts_to_timestr( mouse_ts, m_eventlist.tsoffset );
    bool sync_event_list_to_graph = m_loader.get_opt( OPT_SyncEventListToGraph ) && m_eventlist.show;

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

            if ( m_loader.get_opt_crtc( event.crtc ) )
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
            time_buf += "\nPrev vblank: -" + ts_to_timestr( prev_vblank_ts, 0, 2 ) + "ms";
        if ( next_vblank_ts != INT64_MAX )
            time_buf += "\nNext vblank: " + ts_to_timestr( next_vblank_ts, 0, 2 ) + "ms";
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
            time_buf += string_format( "\n%u %c%sms",
                                       hov.eventid, hov.neg ? '-' : ' ',
                                       ts_to_timestr( hov.dist_ts, 0, 4 ).c_str() );

            // If this isn't an ftrace print event, add the event name
            if ( !event.is_ftrace_print() )
                time_buf += std::string( " " ) + event.name;

            // If this is a vblank event, add the crtc
            if ( event.crtc >= 0 )
                time_buf += std::to_string( event.crtc );

            // Add yellow string for ftrace print events
            if ( event.is_ftrace_print() )
            {
                const event_field_t *field = find_event_field( event.fields, "buf" );

                if ( field )
                    time_buf += " " +  multi_text_color::yellow.m_str( field->value );
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

        time_buf += string_format( "\n\n%s", event_hov.user_comm );

        for ( uint32_t id : *plocs )
        {
            const trace_event_t &event = get_event( id );
            const char *name = event.get_timeline_name( event.name );

            if ( gi.hovered_items.empty() )
                m_eventlist.highlight_ids.push_back( id );

            time_buf += string_format( "\n  %u %s duration: %s%sms%s", event.id, name,
                                       multi_text_color::yellow.c_str(),
                                       ts_to_timestr( event.duration, 0, 4 ).c_str(),
                                       multi_text_color::def.c_str() );
        }

        if ( sync_event_list_to_graph && !m_eventlist.do_gotoevent )
        {
            // Sync event list to first event id in this context
            m_eventlist.do_gotoevent = true;
            m_eventlist.goto_eventid = plocs->at( 0 );
        }
    }

    imgui_set_tooltip( time_buf );
}

void TraceWin::graph_handle_mouse( graph_info_t &gi )
{
    // If we've got an active popup menu, render it.
    if ( m_graph.popupmenu )
    {
        m_graph.popupmenu = TraceWin::graph_render_popupmenu( gi );
        return;
    }


    m_graph.ts_marker = -1;

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

        m_graph.ts_marker = mouse_ts;

        // Set the tooltip
        graph_set_mouse_tooltip( gi, mouse_ts );

        // Check for clicking, wheeling, etc.
        if ( ImGui::IsMouseClicked( 0 ) )
        {
            if ( ImGui::GetIO().KeyCtrl )
            {
                // ctrl + click: select area
                m_graph.mouse_captured = MOUSE_CAPTURED_SELECT_AREA;
                ImGui::CaptureMouseFromApp( true );
                m_graph.mouse_capture_pos = gi.mouse_pos;
            }
            else if ( ImGui::GetIO().KeyShift )
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
            m_graph.new_row_errstr = "";

            ImGui::OpenPopup( "GraphPopup" );
        }
        else
        {
            float mousewheel = ImGui::GetIO().MouseWheel;

            if ( mousewheel )
            {
                bool zoomin = ( mousewheel > 0.0f );
                int64_t len0 = m_graph.length_ts;
                int64_t amt = zoomin ? -( m_graph.length_ts / 2 ) : ( m_graph.length_ts / 2 );
                int64_t len1 = len0 + amt;

                if ( ( len1 > m_graph.s_min_length ) && ( len1 < m_graph.s_max_length ) )
                {
                    //$ TODO mikesart: we've gotten overflow error here:
                    // runtime error: signed integer overflow: 2023691192 * 4676142294 cannot be represented in type 'long int'
                    m_graph.start_ts = mouse_ts - len1 * ( mouse_ts - gi.ts0 ) / len0 - m_eventlist.tsoffset;
                    m_graph.length_ts = len1;

                    m_graph.do_start_timestr = true;
                    m_graph.do_length_timestr = true;
                }
            }
        }
    }
}
