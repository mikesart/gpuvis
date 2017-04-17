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

#include <sys/stat.h>
#include <sys/types.h>

#include <string>
#include <vector>
#include <algorithm>
#include <cctype>

#include <SDL.h>

#include "imgui/imgui.h"
#include "gpuvis_macros.h"
#include "gpuvis_utils.h"
#include "stlini.h"

static SDL_threadID g_main_tid = -1;
static std::vector< char * > g_log;
static std::vector< char * > g_thread_log;
static SDL_mutex *g_mutex = nullptr;

/*
 * log routines
 */
void logf_init()
{
    g_main_tid = SDL_ThreadID();
    g_mutex = SDL_CreateMutex();
}

void logf_shutdown()
{
    SDL_DestroyMutex( g_mutex );
    g_mutex = NULL;
}

const std::vector< char * > &logf_get()
{
    return g_log;
}

void logf( const char *fmt, ... )
{
    va_list args;
    char *buf = NULL;

    va_start( args, fmt );
    vasprintf( &buf, fmt, args );
    va_end( args );

    if ( buf )
    {
        if ( SDL_ThreadID() == g_main_tid )
        {
            g_log.push_back( buf );
        }
        else
        {
            SDL_LockMutex( g_mutex );
            g_thread_log.push_back( buf );
            SDL_UnlockMutex( g_mutex );
        }
    }
}

void logf_update()
{
    if ( g_thread_log.size() )
    {
        SDL_LockMutex( g_mutex );

        for ( char *str : g_thread_log )
            g_log.push_back( str );
        g_thread_log.clear();

        SDL_UnlockMutex( g_mutex );
    }
}

void logf_clear()
{
    logf_update();

    for ( char *str : g_log )
        free( str );
    g_log.clear();
}

std::string string_format( const char *fmt, ... )
{
    std::string str;
    int size = 512;

    for ( ;; )
    {
        va_list ap;

        va_start( ap, fmt );
        str.resize( size );
        int n = vsnprintf( ( char * )str.c_str(), size, fmt, ap );
        va_end( ap );

        if ( ( n > -1 ) && ( n < size ) )
        {
            str.resize( n );
            return str;
        }

        size = ( n > -1 ) ? ( n + 1 ) : ( size * 2 );
    }
}

/*
 * http://stackoverflow.com/questions/216823/whats-the-best-way-to-trim-stdstring
 */
// trim from start (in place)
void string_ltrim( std::string &s )
{
    s.erase( s.begin(), std::find_if( s.begin(), s.end(),
             std::not1( std::ptr_fun< int, int >( std::isspace ) ) ) );
}

// trim from end (in place)
void string_rtrim( std::string &s )
{
    s.erase( std::find_if( s.rbegin(), s.rend(),
             std::not1( std::ptr_fun< int, int >( std::isspace ) ) ).base(), s.end() );
}

// trim from both ends (in place)
void string_trim( std::string &s )
{
    string_ltrim( s );
    string_rtrim( s );
}

// trim from start (copying)
std::string string_ltrimmed( std::string s )
{
    string_ltrim( s );
    return s;
}

// trim from end (copying)
std::string string_rtrimmed( std::string s )
{
    string_rtrim( s );
    return s;
}

// trim from both ends (copying)
std::string string_trimmed( std::string s )
{
    string_trim( s );
    return s;
}

std::string gen_random_str( size_t len )
{
    std::string str;
    static const char s_chars[] =
        " :-0123456789"
        "abcdefghijklmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ";

    str.resize( len + 1 );

    for ( size_t i = 0; i < len; ++i )
    {
        str[ i ] = s_chars[ rand() % ( sizeof( s_chars ) - 1 ) ];
    }

    str[ len ] = 0;
    return str;
}

size_t get_file_size( const char *filename )
{
    struct stat st;

    if ( !stat( filename, &st ) )
        return st.st_size;

    return 0;
}

ImU32 imgui_hsv( float h, float s, float v, float a )
{
    ImColor color = ImColor::HSV( h, s, v, a );

    return ( ImU32 )color;
}

ImVec4 imgui_u32_to_vec4( ImU32 col )
{
    return ImGui::ColorConvertU32ToFloat4( col );
}

ImU32 imgui_vec4_to_u32( const ImVec4 &vec )
{
    return ImGui::ColorConvertFloat4ToU32( vec );
}

void imgui_text_bg( const char *str, const ImVec4 &bgcolor )
{
    ImGui::PushStyleColor( ImGuiCol_HeaderHovered, bgcolor );
    ImGui::Selectable( str, true, ImGuiSelectableFlags_SpanAllColumns );
    ImGui::PopStyleColor();
}

bool imgui_push_smallfont()
{
    ImFontAtlas *atlas = ImGui::GetIO().Fonts;

    if ( atlas->Fonts.Size > 1 )
    {
        ImGui::PushFont( atlas->Fonts[ 1 ] );
        return true;
    }

    return false;
}

void imgui_pop_smallfont()
{
    ImFontAtlas *atlas = ImGui::GetIO().Fonts;

    if ( atlas->Fonts.Size > 1 )
        ImGui::PopFont();
}

float imgui_scale( float val )
{
    return val * ImGui::GetIO().FontGlobalScale;
}

bool imgui_key_pressed( ImGuiKey key )
{
    return ImGui::IsKeyPressed( ImGui::GetKeyIndex( key ) );
}

void imgui_load_fonts()
{
#include "proggy_tiny.cpp"

    ImGuiIO &io = ImGui::GetIO();

    // Add default font
    io.Fonts->AddFontDefault();

    // Add ProggyTiny font
    io.Fonts->AddFontFromMemoryCompressedTTF(
        ProggyTiny_compressed_data, ProggyTiny_compressed_size, 10.0f );
}

void imgui_ini_settings( CIniFile &inifile, bool save )
{
    ImGuiIO &io = ImGui::GetIO();
    ImGuiStyle &style = ImGui::GetStyle();
    const char section[] = "$imgui_settings$";

    if ( save )
    {
        inifile.PutFloat( "win_scale", io.FontGlobalScale, section );

        for ( int i = 0; i < ImGuiCol_COUNT; i++ )
        {
            const ImVec4 &col = style.Colors[ i ];
            const char *name = ImGui::GetStyleColName( i );

            inifile.PutVec4( name, col, section );
        }
    }
    else
    {
        ImVec4 defcol = { -1.0f, -1.0f, -1.0f, -1.0f };

        io.FontGlobalScale = inifile.GetFloat( "win_scale", 1.0f, section );

        for ( int i = 0; i < ImGuiCol_COUNT; i++ )
        {
            const char *name = ImGui::GetStyleColName( i );

            ImVec4 col = inifile.GetVec4( name, defcol, section );
            if ( col.w == -1.0f )
            {
                // Default to no alpha for our windows...
                if ( i == ImGuiCol_WindowBg )
                    ImGui::GetStyle().Colors[ i ].w = 1.0f;
            }
            else
            {
                style.Colors[ i ] = col;
            }
        }
    }
}

bool ColorPicker::render( ImU32 *pcolor )
{
    bool ret = false;

    ImGui::PushItemWidth( imgui_scale( 125.0f ) );
    ImGui::SliderFloat( "##s_value", &m_s, 0.0f, 1.0f, "sat %.2f");
    ImGui::PopItemWidth();

    ImGui::SameLine( 0, imgui_scale( 20.0f ) );
    ImGui::PushItemWidth( imgui_scale( 125.0f ) );
    ImGui::SliderFloat( "##v_value", &m_v, 0.0f, 1.0f, "val %.2f");
    ImGui::PopItemWidth();

    ImGui::SameLine( 0, imgui_scale( 20.0f ) );
    ImGui::PushItemWidth( imgui_scale( 125.0f ) );
    ImGui::SliderFloat( "##a_value", &m_a, 0.0f, 1.0f, "alpha %.2f");
    ImGui::PopItemWidth();

    for ( int i = 0; i < 64; i++ )
    {
        float h = i / 63.0f;
        ImColor col = imgui_hsv( h, m_s, m_v, m_a );
        std::string name = string_format( "%08x", ( ImU32 )col );

        if ( i % 8 )
            ImGui::SameLine();

        ImGui::PushID( i );
        ImGui::PushStyleColor( ImGuiCol_Button, col );
        ImGui::PushStyleColor( ImGuiCol_ButtonActive, col );

        if ( ImGui::Button( name.c_str(), ImVec2( imgui_scale( 80.0f ), 0.0f ) ) )
        {
            ret = true;
            *pcolor = ( ImU32 )col;
        }

        ImGui::PopStyleColor( 2 );
        ImGui::PopID();
    }

    return ret;
}

static struct
{
    const char *name;
    ImU32 color;
    bool modified;
} g_colordata[] =
{
#define _XTAG( _name, _color ) { #_name, _color },
  #include "gpuvis_colors.inl"
#undef _XTAG
};

void col_init( CIniFile &inifile )
{
    for ( int i = 0; i < col_Max; i++ )
    {
        const char *key = g_colordata[ i ].name;
        uint64_t val = inifile.GetUint64( key, UINT64_MAX, "$graph_colors$" );

        if ( val != UINT64_MAX )
        {
            g_colordata[ i ].color = ( ImU32 )val;
        }
    }
}

void col_shutdown( CIniFile &inifile )
{
    for ( int i = 0; i < col_Max; i++ )
    {
        if ( g_colordata[ i ].modified )
        {
            const char *key = g_colordata[ i ].name;

            inifile.PutUint64( key, g_colordata[ i ].color, "$graph_colors$" );
        }
    }
}

ImU32 col_get( colors_t col, ImU32 alpha )
{
    if ( alpha <= 0xff )
        return ( g_colordata[ col ].color & ~IM_COL32_A_MASK ) | ( alpha << IM_COL32_A_SHIFT );

    return g_colordata[ col ].color;
}

void col_set( colors_t col, ImU32 color )
{
    if ( g_colordata[ col ].color != color )
    {
        g_colordata[ col ].color = color;
        g_colordata[ col ].modified = true;
    }
}

const char *col_get_name( colors_t col )
{
    return g_colordata[ col ].name;
}

#if defined( WIN32 )

extern "C" int strcasecmp( const char *s1, const char *s2 )
{
    return _stricmp( s1, s2 );
}

extern "C" int strncasecmp( const char *s1, const char *s2, size_t n )
{
    return _strnicmp( s1, s2, n );
}

extern "C" char *strcasestr( const char *haystack, const char *needle )
{
    return StrStrI( haystack, needle );
}

extern "C" char *strtok_r( char *str, const char *delim, char **saveptr )
{
    return strtok_s( str, delim, saveptr );
}

extern "C" char *strerror_r(int errnum, char *buf, size_t buflen)
{
    buf[ 0 ] = 0;
    strerror_s( buf, buflen, errnum );
    return buf;
}

/*
 * asprintf functions from https://github.com/littlstar/asprintf.c.git
 * MIT Licensed
 */
extern "C" int asprintf (char **str, const char *fmt, ...)
{
  int size = 0;
  va_list args;

  // init variadic argumens
  va_start(args, fmt);

  // format and get size
  size = vasprintf(str, fmt, args);

  // toss args
  va_end(args);

  return size;
}

extern "C" int vasprintf (char **str, const char *fmt, va_list args)
{
  int size = 0;
  va_list tmpa;

  // copy
  va_copy(tmpa, args);

  // apply variadic arguments to
  // sprintf with format to get size
  size = vsnprintf(NULL, size, fmt, tmpa);

  // toss args
  va_end(tmpa);

  // return -1 to be compliant if
  // size is less than 0
  if (size < 0) { return -1; }

  // alloc with size plus 1 for `\0'
  *str = (char *) malloc(size + 1);

  // return -1 to be compliant
  // if pointer is `NULL'
  if (NULL == *str) { return -1; }

  // format string with original
  // variadic arguments and set new size
  size = vsprintf(*str, fmt, args);
  return size;
}

#endif
