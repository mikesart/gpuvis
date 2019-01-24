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

#ifndef GPUVIS_UTILS_H_
#define GPUVIS_UTILS_H_

#include <future>

// ini file singleton
CIniFile &s_ini();

// Color singletons
class Clrs &s_clrs();
class TextClrs &s_textclrs();

// Keyboard singletons
class Keybd &s_keybd();
class Actions &s_actions();

using std::placeholders::_1;
using std::placeholders::_2;
using std::placeholders::_3;
using std::placeholders::_4;
using std::placeholders::_5;

#define NSECS_PER_MSEC 1000000LL
#define NSECS_PER_SEC  1000000000LL

// Timer routines. Use like:
//   util_time_t t0 = util_get_time();
//   sleep_for_ms( 2000 );
//   printf( "%.2fms\n", util_time_to_ms( t0, util_get_time() ) );
typedef std::chrono::time_point< std::chrono::high_resolution_clock > util_time_t;

inline util_time_t util_get_time()
{
    return std::chrono::high_resolution_clock::now();
}
inline float util_time_to_ms( util_time_t start, util_time_t end )
{
    auto diff = end - start;
    return ( float )std::chrono::duration< double, std::milli >( diff ).count();
}

inline const char *util_basename( const char *s )
{
    const char *slash = strrchr( s, '/' );

    if ( !slash )
        slash = strrchr( s, '\\' );

    return slash ? ( slash + 1 ) : s;
}

void logf_init();
void logf_shutdown();
void logf( const char *fmt, ... ) ATTRIBUTE_PRINTF( 1, 2 );
bool logf_update();
void logf_clear();
const std::vector< char * > &logf_get();

struct rect_t
{
    float x = FLT_MAX;
    float y = FLT_MAX;
    float w = FLT_MAX;
    float h = FLT_MAX;

    rect_t() {}
    rect_t( float _x, float _y, float _w, float _h ) :
        x( _x ), y( _y ), w( _w ), h( _h ) {}

    bool point_in_rect( const ImVec2 &pt ) const
    {
        return ( pt.x >= x && pt.x < x + w &&
                 pt.y >= y && pt.y < y + h );
    }
};

// Convert a time string to a time stamp
int64_t timestr_to_ts( const char *buf );
// Convert a time stamp to a time string
std::string ts_to_timestr( int64_t event_ts, int precision, const char *suffix = NULL );

// Helper routines to parse / create compute strings. Ie:
//   comp_[1-2].[0-3].[0-8]
// val is an index value from 0..(2*4*9)-1
std::string comp_str_create_val( uint32_t val );
std::string comp_str_create_abc( uint32_t a, uint32_t b, uint32_t c );
bool comp_str_parse( const char *comp, uint32_t &a, uint32_t &b, uint32_t &c );
bool comp_val_to_abc( uint32_t val, uint32_t &a, uint32_t &b, uint32_t &c );
uint32_t comp_abc_to_val( uint32_t a, uint32_t b, uint32_t c );

class TipWindows
{
public:
    TipWindows() {}
    ~TipWindows() {}

    void update();
    void set_tooltip( const char *name, bool *visible, const char *str );
};

float imgui_scale( float val );
void imgui_set_scale( float val );

void imgui_set_custom_style( float alpha );

ImU32 imgui_col_from_hashval( uint32_t hashval, float sat = 0.9f, float alpha = 1.0f );
ImU32 imgui_hsv( float h, float s, float v, float a );
ImU32 imgui_col_complement( ImU32 col );

void imgui_text_bg( const ImVec4& bgcolor, const char *fmt, ... ) ATTRIBUTE_PRINTF( 2, 3 );

void imgui_push_smallfont();
void imgui_pop_font();

bool imgui_collapsingheader( const char *label, bool *has_focus, ImGuiTreeNodeFlags flags = 0 );

bool imgui_begin_columns( const char* str_id, int columns_count, ImGuiColumnsFlags flags = 0 );
bool imgui_end_columns();

bool imgui_begin_columns( const char *title,
                          const std::initializer_list< const char * > &headers,
                          bool *resized = NULL );

// Does ImGui InputText with two new flags to put label on left or have label be a button.
#define ImGuiInputText2FlagsLeft_LabelOnRight  ( 1 << 29 )
#define ImGuiInputText2FlagsLeft_LabelIsButton ( 1 << 30 )
template < size_t T >
bool imgui_input_text2( const char *label, char ( &buf ) [ T ], float w = 120.0f,
                        ImGuiInputTextFlags flags = 0, ImGuiTextEditCallback callback = NULL,
                        void *user_data = NULL )
{
    bool ret = false;

    ImGui::PushID( label );

    if ( flags & ImGuiInputText2FlagsLeft_LabelIsButton )
    {
        ret = ImGui::Button( label );
        label = "##imgui_input_text2";
    }
    else if ( !( flags & ImGuiInputText2FlagsLeft_LabelOnRight ) )
    {
        ImGui::AlignTextToFramePadding();
        ImGui::Text( "%s", label );
        label = "##imgui_input_text2";
    }
    flags &= ~( ImGuiInputText2FlagsLeft_LabelIsButton | ImGuiInputText2FlagsLeft_LabelOnRight );

    ImGui::SameLine();

    if ( w )
        ImGui::PushItemWidth( imgui_scale( w ) );
    ret |= ImGui::InputText( label, buf, sizeof( buf ), flags, callback, user_data );
    if ( w )
        ImGui::PopItemWidth();

    ImGui::PopID();
    return ret;
}

template < size_t T >
static bool imgui_input_text( const char *label, char ( &buf )[ T ], float x, float w, ImGuiTextEditCallback callback = nullptr )
{
    bool ret = false;
    ImGuiInputTextFlags flags = callback ? ImGuiInputTextFlags_CallbackCharFilter : 0;

    ImGui::PushID( label );

    ImGui::AlignTextToFramePadding();
    ImGui::Text( "%s", label );

    ImGui::SameLine();
    ImGui::PushItemWidth( w );
    ImGui::SetCursorPos( { x, ImGui::GetCursorPos().y } );
    ret = ImGui::InputText( "##imgui_input_text", buf, sizeof( buf ), flags, callback );
    ImGui::PopItemWidth();

    ImGui::PopID();

    return ret;
}

// From:
//   https://github.com/ocornut/imgui/wiki/screenshot_tool
//
// Helper class for simple bitmap manipulation (not particularly efficient!)
class ImageBuf
{
public:
    ImageBuf() {}
    ~ImageBuf() { Clear(); }

    void Clear() { free( Data ); Data = NULL; }

    void CreateEmpty( int w, int h );
    void CreateFromCaptureGL( int x, int y, int w, int h );

    int SaveFile( const char *filename );

    void RemoveAlpha();
    void BlitTo( ImageBuf *dst, int src_x, int src_y, int dst_x, int dst_y, int w, int h );

    void FlipVertical();

    uint32_t *GetPtr( int x, int y );
    uint32_t GetPixel( int x, int y ) const;

public:
    int Width = 0;
    int Height = 0;
    uint32_t *Data = nullptr;
};

bool imgui_save_screenshot( const char *filename );

#define IM_COL32_R( _x ) ( ( ( _x ) >> IM_COL32_R_SHIFT ) & 0xFF )
#define IM_COL32_G( _x ) ( ( ( _x ) >> IM_COL32_G_SHIFT ) & 0xFF )
#define IM_COL32_B( _x ) ( ( ( _x ) >> IM_COL32_B_SHIFT ) & 0xFF )
#define IM_COL32_A( _x ) ( ( ( _x ) >> IM_COL32_A_SHIFT ) & 0xFF )

class FontInfo
{
public:
    FontInfo() {}
    ~FontInfo() {}

    void load_font( const char *section, const char *defname, float defsize, const ImWchar *glyph_ranges = NULL );
    void render_font_options( bool m_use_freetype );

protected:
    void update_ini();

public:
    float m_size = 0.0f;
    std::string m_filename;
    std::string m_section;
    std::string m_name;
    ImFontConfig m_font_cfg;
    int m_font_id = -1;

    bool m_reset = false;
    bool m_changed = false;
    std::string m_input_filename_err;
    char m_input_filename[ PATH_MAX ] = { 0 };
};

// Print color marked up text.
// We've added a quick hack in ImFont::RenderText() which checks for:
//   ESC + RGBA bytes
// This class helps embed these 5 byte color esc sequences.
enum text_colors_t
{
    TClr_Def,
    TClr_Bright,
    TClr_BrightComp,
    TClr_Max
};
class TextClrs
{
public:
    TextClrs() {}
    ~TextClrs() {}

    const char *str( text_colors_t clr )
        { return m_buf[ clr ]; }

    const std::string mstr( const std::string &str_in, ImU32 color );
    const std::string bright_str( const std::string &str_in )
        {  return m_buf[ TClr_Bright ] + str_in + m_buf[ TClr_Def ]; }
    const std::string brightcomp_str( const std::string &str_in )
        {  return m_buf[ TClr_BrightComp ] + str_in + m_buf[ TClr_Def ]; }

    void update_colors();

    static char *set( char ( &dest )[ 6 ], ImU32 color );

public:
    char m_buf[ TClr_Max ][ 6 ];
};

class TextClr
{
public:
    TextClr( ImU32 color ) { TextClrs::set( m_buf, color ); }
    ~TextClr() {}

    const char *str() { return m_buf; }

public:
    char m_buf[ 6 ];
};

typedef uint32_t colors_t;
enum : uint32_t
{
#define _XTAG( _name, _color, _desc ) _name,
#include "gpuvis_colors.inl"
#undef _XTAG
    col_Max
};

class Clrs
{
public:
    Clrs() {}
    ~Clrs() {}

    void init();
    void shutdown();

    ImU32 get( colors_t col, ImU32 alpha = ( uint32_t )-1 );
    ImVec4 getv4( colors_t col, float alpha = -1.0f );
    float getalpha( colors_t col );
    ImU32 getdef( colors_t col );

    void set( colors_t col, ImU32 color );
    void reset( colors_t col );

    const char *name( colors_t col );
    const char *desc( colors_t col );

    bool is_default( colors_t col );

    // True if this is an alpha or saturation only color
    bool is_alpha_color( colors_t col );
    bool is_imgui_color( colors_t col );

private:
    struct colordata_t
    {
        const char *name;
        ImU32 color;
        const ImU32 defcolor;
        bool modified;
        const char *desc;
    };
    static colordata_t s_colordata[ col_Max ];
};

class ColorPicker
{
public:
    ColorPicker() {}
    ~ColorPicker() {}

    bool render( ImU32 color, bool is_alpha, ImU32 defcolor );

public:
    ImU32 m_color = 0;
    float m_s = 0.9f;
    float m_v = 0.9f;
    float m_a = 1.0f;
};

// Useful SDL functions:
//   const char *SDL_GetKeyName( SDL_Keycode key );
//   const char *SDL_GetScancodeName( SDL_Scancode scancode );
class Keybd
{
public:
    Keybd() { clear(); }
    ~Keybd() {}

    void clear();
    void update( const SDL_KeyboardEvent &key );

public:
    uint32_t m_keystate[ SDL_NUM_SCANCODES ];
};

enum action_t
{
    action_nil,

    action_help,
    action_open,
    action_quit,
    action_trace_info,

    action_menu_file,
    action_menu_options,

    action_scroll_up,
    action_scroll_down,
    action_scroll_left,
    action_scroll_right,
    action_scroll_pageup,
    action_scroll_pagedown,
    action_scroll_home,
    action_scroll_end,

    action_toggle_vblank0,
    action_toggle_vblank1,
    action_toggle_framemarkers,
    action_toggle_frame_filters,

    action_toggle_show_eventlist,

    action_graph_zoom_row,
    action_graph_zoom_mouse,
    action_graph_hide_row,

    action_graph_show_hovered_pid,
    action_graph_show_hovered_tgid,

    action_cpugraph_hide_systemevents,

    action_graph_set_markerA,
    action_graph_set_markerB,
    action_graph_goto_markerA,
    action_graph_goto_markerB,

    action_graph_save_location1,
    action_graph_save_location2,
    action_graph_save_location3,
    action_graph_save_location4,
    action_graph_save_location5,

    action_graph_restore_location1,
    action_graph_restore_location2,
    action_graph_restore_location3,
    action_graph_restore_location4,
    action_graph_restore_location5,

    action_graph_pin_tooltip,

    action_focus_graph,
    action_focus_eventlist,

    action_frame_marker_prev_fit,
    action_frame_marker_next_fit,
    action_frame_marker_prev,
    action_frame_marker_next,

    action_save_screenshot,

    action_escape,
    action_return,

    action_max
};

class Actions
{
public:
    Actions() {}
    ~Actions() {}

    void init();
    void clear();

    bool get( action_t action );
    bool peek( action_t action );
    size_t count() { return m_action_count; }

    void set( action_t action );

    const std::string hotkey_str( action_t action );

    void keydown( SDL_Keycode keycode, uint32_t modstate, bool repeat );

public:
    // modstate actionmap bit for allowing repeating keys
    #define KMOD_REPEAT 0x80000000

    struct actionmap_t
    {
        action_t action;
        uint32_t modstate;
        SDL_Keycode key;
        const char *desc;
    };
    std::vector< actionmap_t > m_actionmap;

    uint32_t m_action_count = 0;
    bool m_actions[ action_max ];
};

#endif // GPUVIS_UTILS_H_
