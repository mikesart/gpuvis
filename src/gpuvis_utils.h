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

template < typename K, typename V >
class util_umap
{
public:
    util_umap() {}
    ~util_umap() {}

    V *get_val( const K key, const V &defval )
    {
        auto res = m_map.emplace( key, defval );
        return &res.first->second;
    }

    V *get_val( const K key )
    {
        auto i = m_map.find( key );
        if ( i != m_map.end() )
            return &i->second;
        return NULL;
    }

    void set_val( const K key, const V &val )
    {
        auto res = m_map.emplace( key, val );

       /*
        * If the insertion takes place (because no other element existed with the
        * same key), the function returns a pair object, whose first component is an
        * iterator to the inserted element, and whose second component is true.
        *
        * Otherwise, the pair object returned has as first component an iterator
        * pointing to the element in the container with the same key, and false as its
        * second component.
        */
        if ( !res.second )
            res.first->second = val;
    }

public:
    typedef std::unordered_map< K, V > map_t;
    map_t m_map;
};

class StrPool
{
public:
    StrPool() {}
    ~StrPool() {}

    const char *getstr( const char *str, size_t len = ( size_t )-1 );
    const char *findstr( uint32_t hashval );

public:
    util_umap< uint32_t, std::string > m_pool;
};

class FontInfo
{
public:
    FontInfo() {}
    ~FontInfo() {}

    void load_font( CIniFile &inifile, const char *section, const char *defname, float defsize );
    void render_font_options( bool m_use_freetype );

protected:
    void update_ini();

public:
    CIniFile *m_inifile = nullptr;
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

void imgui_set_custom_style( bool dark, float alpha );

ImU32 imgui_col_from_hashval( uint32_t hashval, float sat = 0.9f, float alpha = 1.0f );
ImU32 imgui_hsv( float h, float s, float v, float a );
ImU32 imgui_col_complement( ImU32 col );
ImU32 imgui_vec4_to_u32( const ImVec4 &vec );

void imgui_text_bg( const char *str, const ImVec4 &bgcolor );

inline char *strncasestr( const char *haystack, const char *needle, size_t needle_len )
{
   for ( ; *haystack; haystack++ )
   {
       if ( !strncasecmp( haystack, needle, needle_len ) )
           return ( char * )haystack;
   }
   return NULL;
}


// Print color marked up text.
// We've added a quick hack in ImFont::RenderText() which checks for:
//   ESC + RGBA bytes
// This class helps embed these 5 byte color esc sequences.
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
    static multi_text_color red;
    static multi_text_color def;
};

bool imgui_push_smallfont();
void imgui_pop_smallfont();

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

typedef uint32_t colors_t;
enum : uint32_t
{
#define _XTAG( _name, _color ) col_ ## _name,
#include "gpuvis_colors.inl"
#undef _XTAG
    col_Max
};

class Cols
{
public:
    Cols() {}
    ~Cols() {}

    static void init( CIniFile &inifile );
    static void shutdown( CIniFile &inifile );

    static ImU32 get( colors_t col, ImU32 alpha = ( uint32_t )-1 );
    static ImVec4 get4( colors_t col, float alpha = -1.0f );
    static float getalpha( colors_t col );

    static void set( colors_t col, ImU32 color );
    static const char *name( colors_t col );

    static bool is_default( colors_t col );
    static void reset( colors_t col );

private:
    struct colordata_t
    {
        const char *name;
        ImU32 color;
        const ImU32 defcolor;
        bool modified;
    };
    static colordata_t s_colordata[ col_Max ];
};

#endif // _GPUVIS_UTILS_H
