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
#include <unistd.h>

#include <algorithm>
#include <array>
#include <future>
#include <unordered_map>
#include <vector>
#include <set>

#include <SDL2/SDL.h>

#include "GL/gl3w.h"
#include "gpuvis.h"

//$ TODO: Restore window size and position
//$ TODO: Restore column sizes?
//$ TODO: Small font for events?

class TraceEventWin
{
public:
    TraceEventWin() {}
    ~TraceEventWin() {}

public:
    bool render( const char *name, TraceEvents &trace_events );

protected:
    void render_time_delta_button_init( TraceEvents &trace_events );
    void render_time_delta_button( TraceEvents &trace_events );

    bool render_time_goto_button( TraceEvents &trace_events );

public:
    bool m_inited = false;

    bool m_do_gotoevent = false;
    int m_gotoevent = 0;
    int m_eventstart = 0;
    int m_eventend = INT32_MAX;
    bool m_open = false;
    uint32_t m_selected = ( uint32_t )-1;

    char m_timegoto_buf[ 32 ];

    char m_timedelta_buf[ 32 ] = { 0 };
    unsigned long long m_tsdelta = ( unsigned long long )-1;
};

static bool imgui_input_int( int *val, float w, const char *label, const char *label2 )
{
    bool ret = ImGui::Button( label );
    float scale = ImGui::GetIO().FontGlobalScale;

    ImGui::SameLine();
    ImGui::PushItemWidth( w * scale );
    ret |= ImGui::InputInt( label2, val, 0, 0 );
    ImGui::PopItemWidth();

    return ret;
}

static bool imgui_key_pressed( ImGuiKey key )
{
    return ImGui::IsKeyPressed( ImGui::GetKeyIndex( key ) );
}

void TraceEventWin::render_time_delta_button_init( TraceEvents &trace_events )
{
    // Default to minimum time stamp.
    unsigned long long ts = trace_events.m_ts_min;

    // Try to grab all the vblank event locations.
    std::vector< uint32_t > *vblank_locs = trace_events.get_event_locs( "drm_vblank_event" );

    if ( vblank_locs )
    {
        std::vector< trace_event_t > &events = trace_events.m_trace_events;

        // Use first vblank, but then try to find first vblank where pid != 0.
        ts = vblank_locs->at( 0 );

        for ( uint32_t i : *vblank_locs )
        {
            if ( events[ i ].pid )
            {
                ts = events[ i ].ts;
                break;
            }
        }
    }

    unsigned long msecs = ts / MSECS_PER_SEC;
    unsigned long nsecs = ts - msecs * MSECS_PER_SEC;

    snprintf( m_timedelta_buf, sizeof( m_timedelta_buf ), "%lu.%06lu", msecs, nsecs );
    m_tsdelta = ts;
}

void TraceEventWin::render_time_delta_button( TraceEvents &trace_events )
{
    if ( m_tsdelta == ( unsigned long long )-1 )
        render_time_delta_button_init( trace_events );

    bool time_delta = ImGui::Button( "Time Delta:" );

    ImGui::SameLine();
    ImGui::PushItemWidth( 150 );
    time_delta |= ImGui::InputText( "##TimeDelta", m_timedelta_buf, sizeof( m_timedelta_buf ), 0, 0 );
    ImGui::PopItemWidth();

    if ( time_delta )
    {
        const char *dot = strchr( m_timedelta_buf, '.' );
        unsigned long msecs = strtoull( m_timedelta_buf, NULL, 10 );
        unsigned long nsecs = dot ? strtoul( dot + 1, NULL, 10 ) : 0;

        while ( nsecs && ( nsecs * 10 < MSECS_PER_SEC ) )
            nsecs *= 10;

        m_tsdelta = msecs / MSECS_PER_SEC + nsecs + msecs * MSECS_PER_SEC;
    }
}

bool TraceEventWin::render_time_goto_button( TraceEvents &trace_events )
{
    bool time_goto = ImGui::Button( "Goto Time:" );

    if ( !m_inited )
        snprintf( m_timegoto_buf, sizeof( m_timegoto_buf ), "0.0" );

    ImGui::SameLine();
    ImGui::PushItemWidth( 150 );
    time_goto |= ImGui::InputText( "##TimeGoto", m_timegoto_buf, sizeof( m_timegoto_buf ), 0, 0 );
    ImGui::PopItemWidth();

    if ( time_goto )
    {
        trace_event_t x;
        const char *dot = strchr( m_timegoto_buf, '.' );
        long msecs = strtol( m_timegoto_buf, NULL, 10 );
        unsigned long nsecs = dot ? strtoul( dot + 1, NULL, 10 ) : 0;
        long neg = ( msecs < 0 ) ? -1 : +1;
        std::vector< trace_event_t > &events = trace_events.m_trace_events;

        while ( nsecs && ( nsecs * 10 < MSECS_PER_SEC ) )
            nsecs *= 10;
        if ( neg < 0 )
            msecs = -msecs;

        long delta = ( msecs / MSECS_PER_SEC + nsecs + msecs * MSECS_PER_SEC );

        x.ts = m_tsdelta + neg * delta;

        auto eventidx = std::lower_bound( events.begin(), events.end(), x,
                                          []( const trace_event_t &f1, const trace_event_t &f2 ) { return f1.ts < f2.ts; } );

        m_gotoevent = eventidx - events.begin();
        if ( ( neg < 0 ) && ( m_gotoevent > 0 ) )
            m_gotoevent--;
    }

    return time_goto;
}

static void draw_minimap_marker( float x, float y, float width, float height, ImU32 color,
                                 float rounding = 0.0f )
{
    ImGui::GetWindowDrawList()->AddRectFilled(
        ImVec2( x, y ),
        ImVec2( x + width, y + height ),
        color, rounding, 0xf );
}

bool TraceEventWin::render( const char *name, TraceEvents &trace_events )
{
    std::vector< trace_event_t > &events = trace_events.m_trace_events;
    size_t event_count = events.size();
    float scale = ImGui::GetIO().FontGlobalScale;

    ImGuiWindowFlags winflags = ImGuiWindowFlags_MenuBar;
    ImGui::SetNextWindowSize( ImVec2( 1280, 1024 ), ImGuiSetCond_FirstUseEver );
    ImGui::Begin( name, &m_open, winflags );

    ImGui::Text( "Events: %lu\n", event_count );

    if ( m_selected != ( uint32_t )-1 )
    {
        ImGui::SameLine();
        ImGui::Text( "Selected: %u\n", m_selected );
    }

    imgui_input_int( &m_eventstart, 75.0f, "Event Start:", "##EventStart" );
    ImGui::SameLine();
    imgui_input_int( &m_eventend, 75.0f, "Event End:", "##EventEnd" );
    ImGui::SameLine();
    render_time_delta_button( trace_events );

    m_do_gotoevent |= imgui_input_int( &m_gotoevent, 75.0f, "Goto Event:", "##GotoEvent" );
    ImGui::SameLine();
    m_do_gotoevent |= render_time_goto_button( trace_events );

    m_gotoevent = std::min< uint32_t >( m_gotoevent, event_count - 1 );
    m_eventstart = std::min< uint32_t >( m_eventstart, event_count - 1 );
    m_eventend = std::min< uint32_t >( std::max< uint32_t >( m_eventend, m_eventstart ), event_count - 1 );

    event_count = m_eventend - m_eventstart + 1;

    // Set focus on event list first time we open.
    if ( !m_inited && ImGui::IsWindowFocused() )
        ImGui::SetNextWindowFocus();

    // Events list
    ImVec2 avail = ImGui::GetContentRegionAvail();
    const float map_width = 16.0f * scale;
    {
        // Set the child window size to hold count of items + header + separator
        float lineh = ImGui::GetTextLineHeightWithSpacing();
        ImGui::SetNextWindowContentSize( { 0.0f, ( event_count + 1 ) * lineh + 1 } );
        ImGui::BeginChild( "eventlistbox", ImVec2( avail.x - ( map_width + 6.0f ), 0.0f ) );

        float winh = ImGui::GetWindowHeight();

        if ( ImGui::IsWindowFocused() )
        {
            int scroll_lines = 0;

            if ( imgui_key_pressed( ImGuiKey_PageDown ) )
                scroll_lines = ( winh / lineh - 5 );
            else if ( imgui_key_pressed( ImGuiKey_PageUp ) )
                scroll_lines = -( winh / lineh - 5 );
            else if ( imgui_key_pressed( ImGuiKey_DownArrow ) )
                scroll_lines = 1;
            else if ( imgui_key_pressed( ImGuiKey_UpArrow ) )
                scroll_lines = -1;
            else if ( imgui_key_pressed( ImGuiKey_Home ) )
                scroll_lines = -event_count;
            else if ( imgui_key_pressed( ImGuiKey_End ) )
                scroll_lines = event_count;

            if ( scroll_lines )
                ImGui::SetScrollY( ImGui::GetScrollY() + scroll_lines * lineh );
        }

        if ( m_do_gotoevent )
        {
            ImGui::SetScrollY( std::max< int >( 0, m_gotoevent - m_eventstart ) * lineh );
            m_do_gotoevent = false;
        }

        float scrolly = ImGui::GetScrollY();
        uint32_t start_idx = ( scrolly >= lineh ) ? ( uint32_t )( scrolly / lineh - 1 ) : 0;
        uint32_t rows = ( winh + 1 ) / lineh;
        uint32_t end_idx = std::min< uint32_t >( start_idx + 2 + rows, event_count );

        // Draw columns
        std::array< const char *, 5 > columns = { "Id", "CPU", "Time Stamp", "Task", "Event" };
        ImGui::Columns( columns.size(), "events" );
        for ( const char *str : columns )
        {
            ImGui::TextColored( ImVec4( 1, 1, 0, 1 ), "%s", str );
            ImGui::NextColumn();
        }
        ImGui::Separator();

        if ( start_idx > 0 )
        {
            // Move cursor position down to where we've scrolled.
            ImGui::SetCursorPosY( ImGui::GetCursorPosY() + lineh * ( start_idx - 1 ) );

            // Scoot to next row (fixes top row occasionally drawing half).
            for ( size_t i = 0; i < columns.size(); i++ )
                ImGui::NextColumn();
        }

        // Draw events
        for ( uint32_t i = start_idx; i < end_idx; i++ )
        {
            char label[ 32 ];
            int colors_pushed = 0;
            trace_event_t &event = events[ m_eventstart + i ];
            bool selected = ( m_selected == i );
            bool ts_negative = ( m_tsdelta > event.ts );
            unsigned long long ts = ts_negative ? ( m_tsdelta - event.ts ) : ( event.ts - m_tsdelta );
            unsigned long msecs = ts / MSECS_PER_SEC;
            unsigned long nsecs = ts - msecs * MSECS_PER_SEC;
            bool is_vblank = !strcmp( event.name, "drm_vblank_event" );

            if ( is_vblank && !selected )
            {
                // If this is a vblank and it's not selected, draw a blue background by
                //  pretending this row is selected.
                ImGui::PushStyleColor( ImGuiCol_Header, ImVec4( 0.0f, 0.0f, 1.0f, 1.0f ) );
                selected = true;
                colors_pushed++;
            }

            snprintf( label, sizeof( label ), "%u", event.id );
            if ( ImGui::Selectable( label, selected, ImGuiSelectableFlags_SpanAllColumns ) )
                m_selected = i;
            ImGui::NextColumn();

            ImGui::Text( "%u", event.cpu );
            ImGui::NextColumn();
            ImGui::Text( "%s%lu.%06lu", ts_negative ? "-" : "", msecs, nsecs );
            ImGui::NextColumn();
            ImGui::Text( "%s", event.comm );
            ImGui::NextColumn();
            ImGui::Text( "%s", event.name );
            ImGui::NextColumn();

            ImGui::PopStyleColor( colors_pushed );
        }

        ImGui::Columns( 1 );
        ImGui::EndChild();

        // Draw a zoomed minimap off to the right to help locate events
        {
            ImGui::SameLine();

            const float fw = 3.0f;
            ImVec2 pos = ImGui::GetCursorScreenPos();
            uint32_t event0 = std::max< int >( ( int )start_idx + m_eventstart - rows * 50, m_eventstart );
            uint32_t event1 = std::min< uint32_t >( event0 + rows * 100, m_eventend - 1 );
            float event_height = ( event1 + 1 - event0 );
            static const ImU32 col_vblank = IM_COL32( 0, 0, 255, 255 );
            static const ImU32 col_viewable = IM_COL32( 128, 128, 128, 128 );
            static const ImU32 col_background = IM_COL32( 255, 255, 255, 50 );
            static const ImU32 col_selected = ImGui::GetColorU32( ImGuiCol_TextSelectedBg );

            // Draw background
            draw_minimap_marker( pos.x, pos.y, map_width, avail.y, col_background );

            // Draw blue marker lines for vblanks
            std::vector< uint32_t > *vblank_locs = trace_events.get_event_locs( "drm_vblank_event" );
            if ( vblank_locs )
            {
                for ( uint32_t id : *vblank_locs )
                {
                    if ( id > event1 )
                        break;
                    if ( id >= event0 )
                    {
                        float pos0 = ( id - event0 ) / event_height;

                        draw_minimap_marker( pos.x + fw, pos.y + avail.y * pos0,
                                             map_width - 2 * fw, 3, col_vblank );
                    }
                }
            }

            // Draw marker for selected event
            if ( m_selected + m_eventstart >= event0 && m_selected + m_eventstart <= event1 )
            {
                float pos0 = ( m_selected + m_eventstart - event0 ) / event_height;

                draw_minimap_marker( pos.x + fw, pos.y + avail.y * pos0,
                                     map_width - 2 * fw, 3, col_selected );
            }

            // Draw rectangle ~ showing visible event location.
            float pos0 = ( start_idx + m_eventstart - event0 ) / event_height;
            float pos1 = ( end_idx + m_eventstart - event0 ) / event_height;

            draw_minimap_marker( pos.x + fw - 1, pos.y + avail.y * pos0,
                                 map_width - 2 * ( fw - 1 ), avail.y * ( pos1 - pos0 ),
                                 col_viewable, 3.0f );

            if ( ImGui::IsMouseClicked( 0 ) && ImGui::IsMouseHoveringWindow() )
            {
                ImVec2 mouse_pos = ImGui::GetMousePos();
                float clickpos = ( mouse_pos.y - pos.y ) / avail.y;
                uint32_t event = event0 + ( event1 - event0 ) * clickpos;

                if ( ( event >= event0 ) && ( event <= event1 ) )
                {
                    m_do_gotoevent = true;
                    m_gotoevent = event;
                }
            }
        }
    }

    ImGui::End();

    m_inited = true;
    return m_open;
}

void GPUVisCon::init( CIniFile *inifile )
{
    m_clear_color = inifile->GetVec4( "clearcolor", ImColor( 114, 144, 154 ) );

    clear_log();
    logf( "Welcome to gpuvis" );

    m_commands.insert( "clear" );
    m_commands.insert( "help" );
    m_commands.insert( "history" );
    m_commands.insert( "quit" );
}

void GPUVisCon::shutdown( CIniFile *inifile )
{
    inifile->PutVec4( "clearcolor", m_clear_color );

    m_log.clear();
    m_history.clear();
}

void GPUVisCon::clear_log()
{
    m_log.clear();
    m_scroll_to_bottom = true;
}

void GPUVisCon::logf( const char *fmt, ... )
{
    va_list args;
    char *buf = NULL;

    va_start( args, fmt );
    vasprintf( &buf, fmt, args );
    va_end( args );

    if ( buf )
    {
        m_log.push_back( buf );
        free( buf );
    }
    m_scroll_to_bottom = true;
}

void GPUVisCon::render( const char *title )
{
    ImGui::SetNextWindowSize( ImVec2( 520, 600 ), ImGuiSetCond_FirstUseEver );

    if ( !ImGui::Begin( title, &m_open ) )
    {
        ImGui::End();
        return;
    }

    ImGui::Text( "%.2f ms/frame (%.1f FPS)",
                 1000.0f / ImGui::GetIO().Framerate,
                 ImGui::GetIO().Framerate );

    ImGui::Separator();

    ImGui::Text( "Clear Color:" );
    ImGui::SameLine();
    ImGui::ColorEdit3( "", ( float * )&m_clear_color );

    ImGui::Separator();

    ImGui::Text( "Imgui debug: " );
    ImGui::SameLine();
    if ( ImGui::Button( "Style Editor" ) )
        m_show_imgui_style_editor ^= 1;
    ImGui::SameLine();
    if ( ImGui::Button( "Metrics" ) )
        m_show_imgui_metrics_editor ^= 1;
    ImGui::SameLine();
    if ( ImGui::Button( "Test Window" ) )
        m_show_imgui_test_window ^= 1;

    ImGui::Separator();
    ImGui::Text( "Log Filter:" );
    ImGui::SameLine();
    ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 0, 0 ) );
    m_filter.Draw( "##log-filter", 180 );
    ImGui::PopStyleVar();

    ImGui::SameLine();
    if ( ImGui::SmallButton( "Clear" ) )
        clear_log();

    ImGui::SameLine();
    if ( ImGui::SmallButton( "Scroll to bottom" ) )
        m_scroll_to_bottom = true;

    {
        ImGui::BeginChild( "ScrollingRegion", ImVec2( 0, -ImGui::GetItemsLineHeightWithSpacing() ), false, ImGuiWindowFlags_HorizontalScrollbar );

        if ( ImGui::BeginPopupContextWindow() )
        {
            if ( ImGui::Selectable( "Clear" ) )
                clear_log();
            ImGui::EndPopup();
        }

        // Display every line as a separate entry so we can change their color or add custom widgets. If you only want raw text you can use ImGui::TextUnformatted(log.begin(), log.end());
        // NB- if you have thousands of entries this approach may be too inefficient and may require user-side clipping to only process visible items.
        // You can seek and display only the lines that are visible using the ImGuiListClipper helper, if your elements are evenly spaced and you have cheap random access to the elements.
        // To use the clipper we could replace the 'for (int i = 0; i < Items.Size; i++)' loop with:
        //     ImGuiListClipper clipper(Items.Size);
        //     while (clipper.Step())
        //         for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++)
        // However take note that you can not use this code as is if a filter is active because it breaks the 'cheap random-access' property. We would need random-access on the post-filtered list.
        // A typical application wanting coarse clipping and filtering may want to pre-compute an array of indices that passed the filtering test, recomputing this array when user changes the filter,
        // and appending newly elements as they are inserted. This is left as a task to the user until we can manage to improve this example code!
        // If your items are of variable size you may want to implement code similar to what ImGuiListClipper does. Or split your data into fixed height items to allow random-seeking into your list.

        // Tighten spacing
        ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 4, 1 ) );

        for ( const std::string &str : m_log )
        {
            const char *item = str.c_str();

            if ( !m_filter.PassFilter( item ) )
                continue;

            ImVec4 col = ImVec4( 1.0f, 1.0f, 1.0f, 1.0f );

            if ( strstr( item, "[error]" ) )
                col = ImColor( 1.0f, 0.4f, 0.4f, 1.0f );
            else if ( strncmp( item, "# ", 2 ) == 0 )
                col = ImColor( 1.0f, 0.78f, 0.58f, 1.0f );

            ImGui::PushStyleColor( ImGuiCol_Text, col );
            ImGui::TextUnformatted( item );
            ImGui::PopStyleColor();
        }

        if ( m_scroll_to_bottom )
        {
            ImGui::SetScrollHere();
            m_scroll_to_bottom = false;
        }

        ImGui::PopStyleVar();
        ImGui::EndChild();
    }

    ImGui::Separator();

    // Command-line
    ImGui::Text( "Command:" );
    ImGui::SameLine();
    if ( ImGui::InputText( "##log-command", m_inputbuf.data(), m_inputbuf.size(),
                           ImGuiInputTextFlags_EnterReturnsTrue |
                               ImGuiInputTextFlags_CallbackCompletion |
                               ImGuiInputTextFlags_CallbackHistory |
                               ImGuiInputTextFlags_CallbackCharFilter,
                           &text_edit_cb_stub, ( void * )this ) )
    {
        char *input_end = &m_inputbuf[ strlen( m_inputbuf.data() ) ];

        while ( input_end > m_inputbuf.data() && input_end[ -1 ] == ' ' )
            input_end--;
        *input_end = 0;

        if ( m_inputbuf[ 0 ] )
            exec_command( m_inputbuf.data() );

        strcpy( m_inputbuf.data(), "" );
    }

    // Keep auto focus on the input box
    if ( ImGui::IsItemHovered() ||
        ( ImGui::IsRootWindowOrAnyChildFocused() && !ImGui::IsAnyItemActive() && !ImGui::IsMouseClicked( 0 ) ) )
    {
        // Auto focus previous widget
        ImGui::SetKeyboardFocusHere( -1 );
    }

    if ( m_show_imgui_test_window )
        ImGui::ShowTestWindow( &m_show_imgui_test_window );

    if ( m_show_imgui_style_editor )
    {
        ImGui::Begin( "Style Editor", &m_show_imgui_style_editor );
        ImGui::ShowStyleEditor();
        ImGui::End();
    }

    if ( m_show_imgui_metrics_editor )
        ImGui::ShowMetricsWindow( &m_show_imgui_metrics_editor );

    ImGui::End();
}

void GPUVisCon::exec_command( const char *command_line )
{
    logf( "# %s\n", command_line );

    // Insert into history. First find match and delete it so it can be pushed to the back. This isn't trying to be smart or optimal.
    m_history_pos = -1;

    for ( size_t i = 0; i < m_history.size(); i++ )
    {
        if ( !strcasecmp( m_history[ i ].c_str(), command_line ) )
        {
            m_history.erase( m_history.begin() + i );
            break;
        }
    }
    m_history.push_back( command_line );

    // Process command
    if ( !strcasecmp( command_line, "clear" ) )
    {
        clear_log();
    }
    else if ( !strcasecmp( command_line, "quit" ) )
    {
        m_quit = true;
    }
    else if ( !strcasecmp( command_line, "help" ) )
    {
        logf( "Commands:" );

        for ( const std::string &cmd : m_commands )
            logf( "- %s", cmd.c_str() );
    }
    else if ( !strcasecmp( command_line, "history" ) )
    {
        for ( size_t i = m_history.size() >= 20 ? m_history.size() - 20 : 0; i < m_history.size(); i++ )
            logf( "%3lu: %s\n", i, m_history[ i ].c_str() );
    }
    else
    {
        logf( "Unknown command: '%s'\n", command_line );
    }
}

int GPUVisCon::text_edit_cb_completion( ImGuiTextEditCallbackData *data )
{
    if ( m_completions.empty() )
    {
        const char *word_end = data->Buf + data->CursorPos;
        const char *word_start = word_end;

        // Locate beginning of current word
        while ( word_start > data->Buf )
        {
            const char c = word_start[ -1 ];

            if ( c == ' ' || c == '\t' || c == ',' || c == ';' )
                break;
            word_start--;
        }

        const char *comp_str = word_start;
        size_t comp_len = word_end - word_start;

        if ( comp_len )
        {
            for ( const std::string &str : m_commands )
            {
                if ( !strncasecmp( str.c_str(), comp_str, comp_len ) )
                {
                    m_completions.push_back( str.c_str() );
                    printf( "%s\n", str.c_str() );
                }
            }
            for ( int i = m_history.size() - 1; i >= 0; i-- )
            {
                std::string str( comp_str, comp_len );

                if ( !strncasecmp( m_history[ i ].c_str(), comp_str, comp_len ) &&
                     m_commands.find( m_history[ i ].c_str() ) == m_commands.end() )
                {
                    m_completions.push_back( m_history[ i ].c_str() );
                    printf( "%s\n",  m_history[ i ].c_str() );
                }
            }

        }

        m_completion_index = 0;
    }

    if ( m_completion_index < m_completions.size() )
    {
        const char *str = m_completions[ m_completion_index ];
        size_t len = strlen( str );

        // Delete line and replace it
        data->DeleteChars( 0, data->BufTextLen );
        data->InsertChars( 0, str );
        data->InsertChars( len, " " );
        data->CursorPos = len + 1;

        if (++m_completion_index >= m_completions.size() )
            m_completion_index = 0;
    }

    return 0;
}

int GPUVisCon::text_edit_cb_history( ImGuiTextEditCallbackData *data )
{
    const int prev_history_pos = m_history_pos;

    if ( data->EventKey == ImGuiKey_UpArrow )
    {
        if ( m_history_pos == -1 )
            m_history_pos = m_history.size() - 1;
        else if ( m_history_pos > 0 )
            m_history_pos--;
    }
    else if ( data->EventKey == ImGuiKey_DownArrow )
    {
        if ( m_history_pos != -1 )
        {
            m_history_pos++;
            if ( m_history_pos >= ( int )m_history.size() )
                m_history_pos = -1;
        }
    }

    if ( prev_history_pos != m_history_pos )
    {
        const char *str = ( m_history_pos >= 0 ) ? m_history[ m_history_pos ].c_str() : "";

        strncpy( data->Buf, str, data->BufSize );
        data->Buf[ data->BufSize - 1 ] = 0;

        data->CursorPos = data->SelectionStart = data->SelectionEnd = data->BufTextLen = strlen( data->Buf );
        data->BufDirty = true;
    }

    return 0;
}

int GPUVisCon::text_edit_cb_stub( ImGuiTextEditCallbackData *data )
{
    int ret = 0;
    GPUVisCon *console = ( GPUVisCon * )data->UserData;

    if ( data->EventFlag == ImGuiInputTextFlags_CallbackCompletion )
    {
        ret = console->text_edit_cb_completion( data );
    }
    else
    {
        console->m_completions.clear();

        if ( data->EventFlag == ImGuiInputTextFlags_CallbackHistory )
            ret = console->text_edit_cb_history( data );
    }

    return ret;
}

static int imgui_ini_save_settings_cb( CIniFile *inifile, int index, const ImGuiIniData &data )
{
    std::string section = "imguiwin_";

    section += data.Name;

    inifile->PutStr( "name", data.Name, section.c_str() );
    inifile->PutVec2( "pos", data.Pos, section.c_str() );
    inifile->PutVec2( "size", data.Size, section.c_str() );
    inifile->PutInt( "collapsed", data.Collapsed, section.c_str() );

    inifile->PutStr( std::to_string( index ).c_str(), section.c_str(), "$imguiwindows$" );
    inifile->PutStr( std::to_string( index + 1 ).c_str(), "", "$imguiwindows$" );
    return 0;
}

static int imgui_ini_load_settings_cb( CIniFile *inifile, int index, ImGuiIniData &data )
{
    std::string section = inifile->GetStr( std::to_string( index ).c_str(), "", "$imguiwindows$" );

    if ( !section.empty() )
    {
        std::string name = inifile->GetStr( "name", "", section.c_str() );

        if ( !name.empty() )
        {
            data.Pos = inifile->GetVec2( "pos", ImVec2( 0, 0 ), section.c_str() );
            data.Size = inifile->GetVec2( "size", ImVec2( 0, 0 ), section.c_str() );
            data.Collapsed = inifile->GetInt( "collapsed", 0, section.c_str() );
            data.Name = strdup( name.c_str() );
        }
        return 0;
    }

    return -1;
}

class TraceEventLoader
{
    TraceEventLoader() {}
    ~TraceEventLoader()
    {
        if ( m_mutex )
        {
            SDL_DestroyMutex( m_mutex );
            m_mutex = NULL;
        }
    }

    bool Load( const char *filename, TraceEvents *trace_events )
    {
        m_filename = filename;
        m_trace_events = trace_events;

        m_mutex = SDL_CreateMutex();

        if ( m_mutex )
        {
            m_thread = SDL_CreateThread( thread_func, "eventloader", ( void * )this );
        }

        return !!m_thread;
    }

    static int SDLCALL thread_func( void *data );
    static int event_cb( TraceEvents *trace_events, const trace_info_t &info,
                         const trace_event_t &event );

public:
    std::string m_filename;
    TraceEvents *m_trace_events = nullptr;
    SDL_Thread *m_thread = nullptr;

    SDL_mutex *m_mutex;
    std::vector< std::string > m_output;
};

int TraceEventLoader::event_cb( TraceEvents *trace_events, const trace_info_t &info,
                                const trace_event_t &event )
{
    size_t id = trace_events->m_trace_events.size();

    if ( trace_events->m_cpucount.empty() )
    {
        trace_events->m_trace_info = info;
        trace_events->m_cpucount.resize( info.cpus, 0 );
    }

    if ( event.ts < trace_events->m_ts_min )
        trace_events->m_ts_min = event.ts;
    if ( event.ts > trace_events->m_ts_max )
        trace_events->m_ts_max = event.ts;

    if ( event.cpu < trace_events->m_cpucount.size() )
        trace_events->m_cpucount[ event.cpu ]++;

    trace_events->m_trace_events.push_back( event );
    trace_events->m_trace_events[ id ].id = id;

    if ( !( id % 100 ) && isatty( 1 ) )
        printf( "\033[1000D  Reading event: %lu", id );

    trace_events->m_event_locations.add_location( event.name, id );
    trace_events->m_comm_locations.add_location( event.comm, id );

#if 0
    //$ TODO mikesart: debug test code
    if ( id > 1000 )
        return 1;
#endif
    return 0;
}

int SDLCALL TraceEventLoader::thread_func( void *data )
{
    std::vector< std::string > output;
    TraceEventLoader *loader = ( TraceEventLoader * )data;
    TraceEvents *trace_events = loader->m_trace_events;
    const char *filename = loader->m_filename.c_str();

    SDL_LockMutex( loader->m_mutex );
    SDL_UnlockMutex( loader->m_mutex );

    printf( "Reading trace file %s...\n", filename );

    EventCallback trace_cb = std::bind( event_cb, trace_events, _1, _2 );
    if ( read_trace_file( filename, trace_events->m_strpool, trace_cb ) < 0 )
    {
        fprintf( stderr, "\nERROR: read_trace_file(%s) failed.\n", filename );
        exit( -1 );
    }
    printf( "\n" );
    printf( "Events read: %lu\n", trace_events->m_trace_events.size() );

    return 0;
}

int main( int argc, char **argv )
{
    CIniFile inifile;
    GPUVisCon console;
    TraceEvents trace_events;
    SDL_Window *window = NULL;
    TraceEventWin eventwin0;
    TraceEventWin eventwin1;

    inifile.Open( "gpuvis", "gpuvis.ini" );

    ImGui::GetIO().IniLoadSettingCB = std::bind(
                imgui_ini_load_settings_cb, &inifile, _1, _2 );
    ImGui::GetIO().IniSaveSettingCB = std::bind(
                imgui_ini_save_settings_cb, &inifile, _1, _2 );

    int x = inifile.GetInt( "win_x", SDL_WINDOWPOS_CENTERED );
    int y = inifile.GetInt( "win_y", SDL_WINDOWPOS_CENTERED );
    int w = inifile.GetInt( "win_w", 1280 );
    int h = inifile.GetInt( "win_h", 1024 );
    ImGui::GetIO().FontGlobalScale = inifile.GetFloat( "win_scale", 1.0f );

    console.init( &inifile );

    // Setup SDL
    if ( SDL_Init( SDL_INIT_VIDEO | SDL_INIT_TIMER ) != 0 )
    {
        printf( "Error: %s\n", SDL_GetError() );
        return -1;
    }

    // Setup window
    SDL_GL_SetAttribute( SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG );
    SDL_GL_SetAttribute( SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE );
    SDL_GL_SetAttribute( SDL_GL_DOUBLEBUFFER, 1 );
    SDL_GL_SetAttribute( SDL_GL_DEPTH_SIZE, 24 );
    SDL_GL_SetAttribute( SDL_GL_STENCIL_SIZE, 8 );
    SDL_GL_SetAttribute( SDL_GL_CONTEXT_MAJOR_VERSION, 3 );
    SDL_GL_SetAttribute( SDL_GL_CONTEXT_MINOR_VERSION, 2 );

    SDL_DisplayMode current;
    SDL_GetCurrentDisplayMode( 0, &current );

    window = SDL_CreateWindow( "GPUVis", x, y, w, h,
                               SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE );

    SDL_GLContext glcontext = SDL_GL_CreateContext( window );

    gl3wInit();

    // Setup ImGui binding
    ImGui_ImplSdlGL3_Init( window );

    // Main loop
    bool done = false;
    while ( !done )
    {
        SDL_Event event;

        while ( SDL_PollEvent( &event ) )
        {
            ImGui_ImplSdlGL3_ProcessEvent( &event );

            if ( event.type == SDL_QUIT )
                done = true;
        }
        ImGui_ImplSdlGL3_NewFrame( window );

        // Render our console / options window.
        console.render( "gpuvis console" );

        // Render events for our loaded trace file.
        //        eventwin0.render( "blah0", trace_events );
        //        eventwin1.render( "blah1", trace_events );

        // Rendering
        const ImVec4 &color = console.m_clear_color;
        const ImVec2 &size = ImGui::GetIO().DisplaySize;

        glViewport( 0, 0, ( int )size.x, ( int )size.y );
        glClearColor( color.x, color.y, color.z, color.w );
        glClear( GL_COLOR_BUFFER_BIT );

        ImGui::Render();

        SDL_GL_SwapWindow( window );

        if ( console.m_quit )
            break;
    }

    // Write main window position / size to ini file.
    int top, left, bottom, right;

    SDL_GetWindowBordersSize( window, &top, &left, &bottom, &right );
    SDL_GetWindowPosition( window, &x, &y );
    SDL_GetWindowSize( window, &w, &h );
    inifile.PutInt( "win_x", x - left );
    inifile.PutInt( "win_y", y - top );
    inifile.PutInt( "win_w", w );
    inifile.PutInt( "win_h", h );

    console.shutdown( &inifile );

    // Cleanup
    ImGui_ImplSdlGL3_Shutdown();
    SDL_GL_DeleteContext( glcontext );
    SDL_DestroyWindow( window );
    SDL_Quit();

    inifile.Close();
    return 0;
}
