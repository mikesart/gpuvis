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
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <string>

#include <SDL.h>

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"   // BeginColumns(), EndColumns() WIP
#include "gpuvis_macros.h"
#include "stlini.h"
#include "trace-cmd/trace-read.h"
#include "gpuvis_utils.h"
#include "gpuvis.h"

void FrameMarkers::init()
{
    std::vector< INIEntry > entries = s_ini().GetSectionEntries( "$framemarkers_filters$" );

    for ( const INIEntry &entry : entries )
    {
        const std::vector< std::string > filter = string_explode( entry.second, '\t' );

        if ( filter.size() == 1 )
            m_previous_filters.push_back( { filter[ 0 ], "" } );
        else if ( filter.size() == 2 )
            m_previous_filters.push_back( { filter[ 0 ], filter[ 1 ] } );
    }

    if ( m_previous_filters.empty() )
    {
        // Add some default filters
        m_previous_filters.push_back( { "$name = drm_vblank_event && $crtc = 0", "" } );
        m_previous_filters.push_back( { "$name = drm_vblank_event && $crtc = 1", "" } );
        m_previous_filters.push_back( { "$buf =~ \"[Compositor] Before wait query\"",
                                        "$buf =~ \"[Compositor] After wait query\"" } );
    }

    strcpy_safe( dlg.m_left_marker_buf, m_previous_filters[ 0 ].first.c_str() );
    strcpy_safe( dlg.m_right_marker_buf, m_previous_filters[ 0 ].second.c_str() );
}

void FrameMarkers::shutdown()
{
    for ( size_t i = 0; i < m_previous_filters.size(); i++ )
    {
        char key[ 32 ];
        std::string value = m_previous_filters[ i ].first;

        value += "\t";
        value += m_previous_filters[ i ].second;

        snprintf_safe( key, "%02lu", i );

        s_ini().PutStr( key, value.c_str(), "$framemarkers_filters$" );
    }
}

void FrameMarkers::clear_dlg()
{
    dlg.m_checked = false;

    dlg.m_left_filter_err_str.clear();
    dlg.m_right_filter_err_str.clear();

    dlg.m_left_plocs = NULL;
    dlg.m_right_plocs = NULL;
}

void FrameMarkers::set_tooltip()
{
    std::string tooltip;

    tooltip += s_textclrs().bright_str( "Frame marker filters\n\n" );

    tooltip += "Examples:\n";

    tooltip += "  Left frame: $name = drm_vblank_event && $crtc = 0\n";
    tooltip += "  Right frame: $name = drm_vblank_event && $crtc = 0\n\n";

    tooltip += "  Left frame: $buf =~ \"[Compositor] Sleep - begin\"\n";
    tooltip += "  Right frame: $buf =~ \"[Compositor] Sleep - end\"\n";

    ImGui::SetTooltip( "%s", tooltip.c_str() );
}

bool FrameMarkers::show_dlg( TraceEvents &trace_events, uint32_t eventid )
{
    clear_dlg();

    if ( is_valid_id( eventid ) && ( eventid < trace_events.m_events.size() ) )
    {
        const trace_event_t &event = trace_events.m_events[ eventid ];

        dlg.m_left_marker_buf[ 0 ] = 0;
        dlg.m_right_marker_buf[ 0 ] = 0;

        if ( event.is_vblank() )
        {
            snprintf_safe( dlg.m_left_marker_buf, "$name = %s && $crtc == %d", event.name, event.crtc );
        }
        else if ( event.is_ftrace_print() )
        {
            const char *buf = get_event_field_val( event, "buf" );

            if ( buf[ 0 ] )
                snprintf_safe( dlg.m_left_marker_buf, "$buf =~ \"%s\"", buf );
        }

        if ( !dlg.m_left_marker_buf[ 0 ] )
            snprintf_safe( dlg.m_left_marker_buf, "$name = %s", event.name );
    }

    if ( !dlg.m_left_marker_buf[ 0 ] )
        strcpy_safe( dlg.m_left_marker_buf, "$name = drm_vblank_event && $crtc = 0" );

    ImGui::OpenPopup( "Set Frame Markers" );
    return true;
}

bool FrameMarkers::render_dlg( TraceEvents &trace_events )
{
    bool item_hovered = false;
    char right_marker_buf[ 512 ];
    float w = imgui_scale( 450.0f );
    const char left_text[] = "Left Frame: ";
    const char right_text[] = "Right Frame: ";
    const ImVec2 button_size = { imgui_scale( 250.0f ), 0.0f };
    float x = ImGui::GetCursorPos().x + ImGui::CalcTextSize( right_text ).x;

    if ( !ImGui::BeginPopupModal( "Set Frame Markers", NULL, ImGuiWindowFlags_AlwaysAutoResize ) )
        return false;

    ImGui::Text( "%s", "Frame marker filters" );

    // Left Frame Filter
    {
        if ( imgui_input_text( left_text, dlg.m_left_marker_buf, x, w ) )
            clear_dlg();

        if ( ImGui::IsWindowAppearing() )
            ImGui::SetKeyboardFocusHere( -1 );

        item_hovered |= ImGui::IsItemHovered();

        if ( !dlg.m_left_filter_err_str.empty() )
            ImGui::TextColored( ImVec4( 1, 0, 0, 1 ), "%s", dlg.m_left_filter_err_str.c_str() );
        else if ( dlg.m_left_plocs )
            ImGui::TextColored( ImVec4( 0, 1, 0, 1 ), "%lu events found", dlg.m_left_plocs->size() );
    }

    // Right Frame Filter
    {
        strcpy_safe( right_marker_buf, !dlg.m_right_marker_buf[ 0 ] ? dlg.m_left_marker_buf : dlg.m_right_marker_buf );

        if ( imgui_input_text( right_text, right_marker_buf, x, w ) )
        {
            clear_dlg();
            strcpy_safe( dlg.m_right_marker_buf, right_marker_buf );
        }

        item_hovered |= ImGui::IsItemHovered();

        if ( !dlg.m_right_filter_err_str.empty() )
            ImGui::TextColored( ImVec4( 1, 0, 0, 1 ), "%s", dlg.m_right_filter_err_str.c_str() );
        else if ( dlg.m_right_plocs )
            ImGui::TextColored( ImVec4( 0, 1, 0, 1 ), "%lu events found", dlg.m_right_plocs->size() );
    }

    if ( item_hovered )
        set_tooltip();

    ImGui::Separator();

    if ( dlg.m_checked && dlg.m_count )
    {
        ImGui::TextColored( ImVec4( 0, 1, 0, 1 ), "%u frames found", dlg.m_count );
        ImGui::Indent();
        ImGui::Text( "Min frame time: %s", ts_to_timestr( dlg.m_min_ts, 4 ).c_str() );
        ImGui::Text( "Max frame time: %s", ts_to_timestr( dlg.m_max_ts, 4 ).c_str() );
        ImGui::Text( "Avg frame time: %s", ts_to_timestr( dlg.m_tot_ts / dlg.m_count, 4 ).c_str() );
        ImGui::Unindent();

        ImGui::Separator();
    }

    if ( ImGui::CollapsingHeader( "Previous Filters", ImGuiTreeNodeFlags_DefaultOpen ) )
    {
        ImGui::BeginChild( "previous_filters", ImVec2( 0.0f, imgui_scale( 150.0f ) ) );
        ImGui::Indent();

        ImGuiSelectableFlags flags = ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_DontClosePopups;

        imgui_begin_columns( "framemarker_presets", { left_text, right_text } );

        for ( auto i : m_previous_filters )
        {
            const char *str0 = i.first.c_str();
            const char *str1 = i.second.c_str();

            ImGui::PushID( str0 );

            if ( ImGui::Selectable( str0, false, flags ) )
            {
                clear_dlg();

                strcpy_safe( dlg.m_left_marker_buf, str0 );
                strcpy_safe( dlg.m_right_marker_buf, str1 );
            }
            ImGui::NextColumn();

            ImGui::Text( "%s", str1[ 0 ] ? str1 : str0 );
            ImGui::NextColumn();
            ImGui::Separator();

            ImGui::PopID();
        }

        ImGui::EndColumns();

        ImGui::Unindent();
        ImGui::EndChild();
    }

    // "Check filters" or "Set Frame Markers" buttons
    if ( !dlg.m_checked )
    {
        if ( ImGui::Button( "Check filters", button_size ) || s_actions().get( action_return ) )
        {
            dlg.m_left_plocs = trace_events.get_tdopexpr_locs( dlg.m_left_marker_buf, &dlg.m_left_filter_err_str );
            dlg.m_right_plocs = trace_events.get_tdopexpr_locs( right_marker_buf, &dlg.m_right_filter_err_str );

            if ( !dlg.m_left_plocs )
            {
                if ( dlg.m_left_filter_err_str.empty() )
                    dlg.m_left_filter_err_str = "WARNING: No events found.";
            }
            if ( !dlg.m_right_plocs )
            {
                if ( dlg.m_right_filter_err_str.empty() )
                    dlg.m_right_filter_err_str = "WARNING: No events found.";
            }

            if ( dlg.m_left_plocs && dlg.m_right_plocs )
            {
                setup_frames( trace_events, false );
                dlg.m_checked = true;
            }
        }
    }
    else if ( ImGui::Button( "Set Frame Markers", button_size ) || s_actions().get( action_return ) )
    {
        // If left filter == right filter, zero out right filter
        if ( !strcmp( dlg.m_left_marker_buf, dlg.m_right_marker_buf ) )
            dlg.m_right_marker_buf[ 0 ] = 0;

        // Try to find this filter pair in our previous filters array
        std::pair< std::string, std::string > filter = { dlg.m_left_marker_buf, dlg.m_right_marker_buf };
        auto idx = std::find( m_previous_filters.begin(), m_previous_filters.end(), filter );

        // Erase the one we found
        if ( idx != m_previous_filters.end() )
            m_previous_filters.erase( idx );

        // Insert it at the beginning
        m_previous_filters.insert( m_previous_filters.begin(), filter );

        // Make sure we don't go over ~ 20 filters
        if ( m_previous_filters.size() > 20 )
            m_previous_filters.resize( 20 );

        setup_frames( trace_events, true );

        ImGui::CloseCurrentPopup();
    }

    // Cancel button
    ImGui::SameLine();
    if ( ImGui::Button( "Cancel", button_size ) || s_actions().get( action_escape ) )
        ImGui::CloseCurrentPopup();

    ImGui::EndPopup();

    return false;
}

int64_t FrameMarkers::get_frame_len( TraceEvents &trace_events, int frame )
{
    if ( ( size_t )frame < m_left_frames.size() )
    {
        uint32_t left_idx = m_left_frames[ frame ];
        uint32_t right_idx = m_right_frames[ frame ];
        const trace_event_t &left_event = trace_events.m_events[ left_idx ];
        const trace_event_t &right_event = trace_events.m_events[ right_idx ];

        return right_event.ts - left_event.ts;
    }

    return 0;
}

void FrameMarkers::setup_frames( TraceEvents &trace_events, bool set_frames )
{
    uint32_t idx = 0;
    const std::vector< uint32_t > &locs_left = *dlg.m_left_plocs;
    const std::vector< uint32_t > &locs_right = *dlg.m_right_plocs;

    dlg.m_count = 0;
    dlg.m_tot_ts = 0;
    dlg.m_min_ts = INT64_MAX;
    dlg.m_max_ts = INT64_MIN;

    if ( set_frames )
    {
        m_left_frames.clear();
        m_right_frames.clear();
    }

    // Go through all the right eventids...
    for ( uint32_t right_eventid : locs_right )
    {
        // Find entryid in left which is < this right eventid
        while ( locs_left[ idx ] < right_eventid )
        {
            // Check if this is our last left event or the next event is greater.
            if ( ( idx + 1 >= locs_left.size() ) ||
                 ( locs_left[ idx + 1 ] >= right_eventid ) )
            {
                const trace_event_t &left_event = trace_events.m_events[ locs_left[ idx ] ];
                const trace_event_t &right_event = trace_events.m_events[ right_eventid ];
                int64_t ts = right_event.ts - left_event.ts;

                dlg.m_count++;
                dlg.m_tot_ts += ts;
                dlg.m_min_ts = std::min< int64_t >( dlg.m_min_ts, ts );
                dlg.m_max_ts = std::max< int64_t >( dlg.m_max_ts, ts );

                if ( set_frames )
                {
                    m_left_frames.push_back( locs_left[ idx ] );
                    m_right_frames.push_back( right_eventid );
                }

                if ( ++idx >= locs_left.size() )
                    return;
                break;
            }

            idx++;
        }
    }
}
