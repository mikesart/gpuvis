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

void logf_init();
void logf_shutdown();
void logf( const char *fmt, ... ) ATTRIBUTE_PRINTF( 1, 2 );
void logf_update();
void logf_clear();
const std::vector< char * > &logf_get();

extern "C" uint32_t fnv_hashstr32( const char *str );

template < typename T >
T Clamp( const T& val, const T& lower, const T& upper )
{
    return std::max< T >( lower, std::min< T >( val, upper ) );
}

std::string string_format( const char *fmt, ... ) ATTRIBUTE_PRINTF( 1, 2 );
// trim from start (in place)
void string_ltrim( std::string &s );
// trim from end (in place)
void string_rtrim( std::string &s );
// trim from both ends (in place)
void string_trim( std::string &s );
// trim from start (copying)
std::string string_ltrimmed( std::string s );
// trim from end (copying)
std::string string_rtrimmed( std::string s );
// trim from both ends (copying)
std::string string_trimmed( std::string s );

std::string gen_random_str( size_t len );

size_t get_file_size( const char *filename );

float imgui_scale( float val );
bool imgui_key_pressed( ImGuiKey key );

ImU32 imgui_hsv( float h, float s, float v, float a );
ImVec4 imgui_u32_to_vec4( ImU32 col );
ImU32 imgui_vec4_to_u32( const ImVec4 &vec );

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
