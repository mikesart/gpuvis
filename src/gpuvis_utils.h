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

#ifndef _GPUVIS_UTILS_H
#define _GPUVIS_UTILS_H

// ini file singleton
CIniFile &s_ini();

// Color singletons
class Clrs &s_clrs();
class TextClrs &s_textclrs();

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

void logf_init();
void logf_shutdown();
void logf( const char *fmt, ... ) ATTRIBUTE_PRINTF( 1, 2 );
void logf_update();
void logf_clear();
const std::vector< char * > &logf_get();

// Helper routines to parse / create compute strings. Ie:
//   comp_[1-2].[0-3].[0-8]
// val is an index value from 0..(2*4*9)-1
std::string comp_str_create_val( uint32_t val );
std::string comp_str_create_abc( uint32_t a, uint32_t b, uint32_t c );
bool comp_str_parse( const char *comp, uint32_t &a, uint32_t &b, uint32_t &c );
bool comp_val_to_abc( uint32_t val, uint32_t &a, uint32_t &b, uint32_t &c );
uint32_t comp_abc_to_val( uint32_t a, uint32_t b, uint32_t c );

float imgui_scale( float val );
void imgui_set_scale( float val );
bool imgui_key_pressed( ImGuiKey key );

void imgui_set_custom_style( float alpha );

ImU32 imgui_col_from_hashval( uint32_t hashval, float sat = 0.9f, float alpha = 1.0f );
ImU32 imgui_hsv( float h, float s, float v, float a );
ImU32 imgui_col_complement( ImU32 col );

void imgui_text_bg( const char *str, const ImVec4 &bgcolor );

bool imgui_mousepos_valid( const ImVec2 &pos );

bool imgui_push_smallfont();
void imgui_pop_smallfont();

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
        ImGui::AlignFirstTextHeightToWidgets();
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

#define IM_COL32_R( _x ) ( ( ( _x ) >> IM_COL32_R_SHIFT ) & 0xFF )
#define IM_COL32_G( _x ) ( ( ( _x ) >> IM_COL32_G_SHIFT ) & 0xFF )
#define IM_COL32_B( _x ) ( ( ( _x ) >> IM_COL32_B_SHIFT ) & 0xFF )
#define IM_COL32_A( _x ) ( ( ( _x ) >> IM_COL32_A_SHIFT ) & 0xFF )

inline char *strncasestr( const char *haystack, const char *needle, size_t needle_len )
{
   for ( ; *haystack; haystack++ )
   {
       if ( !strncasecmp( haystack, needle, needle_len ) )
           return ( char * )haystack;
   }
   return NULL;
}

class FontInfo
{
public:
    FontInfo() {}
    ~FontInfo() {}

    void load_font( const char *section, const char *defname, float defsize );
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

    bool render( colors_t idx, ImU32 *pcolor );

public:
    float m_s = 0.9f;
    float m_v = 0.9f;
    float m_a = 1.0f;
};

#endif // _GPUVIS_UTILS_H
