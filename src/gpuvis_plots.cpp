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
#include <functional>
#include <algorithm>
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

const std::string CreatePlotDlg::get_plot_str( const trace_event_t &event )
{
    if ( event.is_ftrace_print() )
    {
        const char *buf = get_event_field_val( event, "buf" );

        if ( str_get_digit_loc( buf ) )
            return s_textclrs().bright_str( buf ) + "...";
    }
    else if ( event.has_duration() )
    {
        return s_textclrs().bright_str( event.name ) + " duration...";
    }

    return "";
}

bool CreatePlotDlg::init( TraceEvents &trace_events, uint32_t eventid )
{
    m_plot = NULL;
    m_plot_name.clear();

    if ( !is_valid_id( eventid ) )
        return false;

    const trace_event_t &event = trace_events.m_events[ eventid ];

    if ( event.is_ftrace_print() )
    {
        const char *buf = get_event_field_val( event, "buf" );
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
    }
    else if ( event.has_duration() )
    {
        m_plot_buf = s_textclrs().bright_str( event.name ) + " duration";
        m_plot_err_str.clear();

        snprintf_safe( m_plot_name_buf, "%s duration", event.name );
        snprintf_safe( m_plot_filter_buf, "$name = \"%s\"", event.name );
        strcpy_safe( m_plot_scanf_buf, "$duration" );

        ImGui::OpenPopup( "Create Plot" );
    }

    return false;
}

bool CreatePlotDlg::render_dlg( TraceEvents &trace_events )
{
    if ( !ImGui::BeginPopupModal( "Create Plot", NULL, ImGuiWindowFlags_AlwaysAutoResize ) )
        return false;

    ParsePlotStr parse_plot_str;
    float w = imgui_scale( 350.0f );
    const ImVec2 button_size = { imgui_scale( 120.0f ), 0.0f };
    const ImVec2 text_size = ImGui::CalcTextSize( "Plot Scan Str: " );
    float x = ImGui::GetCursorPos().x + text_size.x;

    if ( parse_plot_str.init( m_plot_scanf_buf ) &&
         parse_plot_str.parse( m_plot_buf.c_str() ) )
    {
        const char *buf = m_plot_buf.c_str();
        const char *val_start = parse_plot_str.m_val_start;
        const char *val_end = parse_plot_str.m_val_end;
        int buf_len = ( int )( val_start - buf );
        int val_len = ( int )( val_end - val_start );
        const char *clr_bright = s_textclrs().str( TClr_Bright );
        const char *clr_brightcomp = s_textclrs().str( TClr_BrightComp );

        ImGui::Text( "%s%.*s%s%.*s%s%s",
                     clr_bright, buf_len, buf,
                     clr_brightcomp, val_len, val_start,
                     clr_bright, val_end );
    }
    else
    {
        ImGui::TextColored( s_clrs().getv4( col_BrightText ), "%s", m_plot_buf.c_str() );
    }

    ImGui::NewLine();

    struct PlotNameFilter {
        static int FilterPunct( ImGuiTextEditCallbackData *data )
                { return ( ( data->EventChar < 256 ) && ispunct( data->EventChar ) ); }
    };
    imgui_input_text( "Plot Name:", m_plot_name_buf, x, w, PlotNameFilter::FilterPunct );
    if ( ImGui::IsWindowAppearing() )
        ImGui::SetKeyboardFocusHere( -1 );

    imgui_input_text( "Plot Filter:", m_plot_filter_buf, x, w );

    if ( !m_plot_err_str.empty() )
        ImGui::TextColored( ImVec4( 1, 0, 0, 1), "%s", m_plot_err_str.c_str() );

    imgui_input_text( "Plot Scan Str:", m_plot_scanf_buf, x, w );

    ImGui::NewLine();

    bool disabled = !m_plot_name_buf[ 0 ] || !m_plot_filter_buf[ 0 ] || !m_plot_scanf_buf[ 0 ];
    if ( disabled )
        ImGui::PushStyleColor( ImGuiCol_Text, ImGui::GetStyleColorVec4( ImGuiCol_TextDisabled ) );

    if ( ImGui::Button( "Create", button_size ) && !disabled )
    {
        const std::vector< uint32_t > *plocs = trace_events.get_tdopexpr_locs(
                    m_plot_filter_buf, &m_plot_err_str );

        if ( !plocs && m_plot_err_str.empty() )
        {
            m_plot_err_str = "WARNING: No events found.";
        }
        else
        {
            m_plot_name = std::string( "plot:" ) + m_plot_name_buf;

            GraphPlot &plot = trace_events.get_plot( m_plot_name.c_str() );

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
    if ( ImGui::Button( "Cancel", button_size ) || s_actions().get( action_escape ) )
        ImGui::CloseCurrentPopup();

    ImGui::EndPopup();

    return !!m_plot;
}

bool GraphPlot::init( TraceEvents &trace_events, const std::string &name,
                      const std::string &filter_str, const std::string scanf_str )
{
    m_name = name;
    m_filter_str = filter_str;
    m_scanf_str = scanf_str;

    m_minval = FLT_MAX;
    m_maxval = FLT_MIN;
    m_plotdata.clear();

    std::string errstr;
    const std::vector< uint32_t > *plocs = trace_events.get_tdopexpr_locs( m_filter_str.c_str(), &errstr );

    if ( plocs )
    {
        if ( scanf_str == "$duration" )
        {
            for ( uint32_t idx : *plocs )
            {
                const trace_event_t &event = trace_events.m_events[ idx ];

                if ( event.has_duration() )
                {
                    float valf = event.duration * ( 1.0 / NSECS_PER_MSEC );

                    m_minval = std::min< float >( m_minval, valf );
                    m_maxval = std::max< float >( m_maxval, valf );

                    m_plotdata.push_back( { event.ts, event.id, valf } );
                }
            }
        }
        else
        {
            ParsePlotStr parse_plot_str;

            if ( parse_plot_str.init( m_scanf_str.c_str() ) )
            {
                for ( uint32_t idx : *plocs )
                {
                    const trace_event_t &event = trace_events.m_events[ idx ];
                    const char *buf = get_event_field_val( event, "buf" );

                    if ( parse_plot_str.parse( buf ) )
                    {
                        float valf = parse_plot_str.m_valf;

                        m_minval = std::min< float >( m_minval, valf );
                        m_maxval = std::max< float >( m_maxval, valf );

                        m_plotdata.push_back( { event.ts, event.id, valf } );
                    }
                }
            }
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

bool ParsePlotStr::init( const char *scanf_str )
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

bool ParsePlotStr::parse( const char *buf )
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
