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

class CIniFile;

using std::placeholders::_1;
using std::placeholders::_2;
using std::placeholders::_3;
using std::placeholders::_4;
using std::placeholders::_5;

#define MSECS_PER_SEC 1000000LL

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

float imgui_scale( float val );
bool imgui_key_pressed( ImGuiKey key );

ImU32 imgui_hsv( float h, float s, float v, float a );
ImVec4 imgui_u32_to_vec4( ImU32 col );
ImU32 imgui_vec4_to_u32( const ImVec4 &vec );

void imgui_text_bg( const char *str, const ImVec4 &bgcolor );

// Print color marked up text.
class multi_text_color
{
public:
    multi_text_color() {}
    multi_text_color( const ImVec4 &color ) { set( color ); }
    ~multi_text_color() {}

    const char *c_str() { return buf; }

    const std::string m_str( const char *str )
    {
        return std::string( buf ) + str + def.c_str();
    }

    const std::string m_str( const ImVec4 &color, const char *str )
    {
        set( color );
        return m_str( str );
    }

    void set( const ImVec4 &color )
    {
        buf[ 0 ] = '\033';
        buf[ 1 ] = std::max< uint8_t >( 1, color.x * 255.0f );
        buf[ 2 ] = std::max< uint8_t >( 1, color.y * 255.0f );
        buf[ 3 ] = std::max< uint8_t >( 1, color.z * 255.0f );
        buf[ 4 ] = std::max< uint8_t >( 1, color.w * 255.0f );
        buf[ 5 ] = 0;
    }

public:
    char buf[ 6 ];

    static multi_text_color yellow;
    static multi_text_color def;
};
void imgui_multicolored_text( const char *text, const ImVec4 &color0 = ImGui::GetColorVec4( ImGuiCol_Text ) );

void imgui_set_tooltip( const std::string &str );
void imgui_add_tooltip( const std::string &str );
void imgui_render_tooltip();

bool imgui_push_smallfont();
void imgui_pop_smallfont();

void imgui_load_fonts();
void imgui_ini_settings( CIniFile &inifile, bool save = false );

class ColorPicker
{
public:
    ColorPicker() {}
    ~ColorPicker() {}

    bool render( ImU32 *pcolor );

public:
    float m_s = 0.9f;
    float m_v = 0.9f;
    float m_a = 1.0f;
};

enum colors_t
{
#define _XTAG( _name, _color ) col_ ## _name,
#include "gpuvis_colors.inl"
#undef _XTAG
    col_Max
};

void col_init( CIniFile &inifile );
void col_shutdown( CIniFile &inifile );

ImU32 col_get( colors_t col, ImU32 alpha = ( uint32_t )-1 );
void col_set( colors_t col, ImU32 color );
const char *col_get_name( colors_t col );

#endif // _GPUVIS_UTILS_H
