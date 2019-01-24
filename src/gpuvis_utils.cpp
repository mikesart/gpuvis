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
#include <fcntl.h>
#include <sys/stat.h>

#ifndef WIN32
#include <unistd.h>
#else
#define WIN32_LEAN_AND_MEAN 1
#include <windows.h>
#endif

#include <string>
#include <vector>
#include <algorithm>
#include <iterator>
#include <cctype>
#include <sstream>
#include <unordered_map>
#include <functional>

#include <SDL.h>

#include "GL/gl3w.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"   // BeginColumns(), EndColumns() WIP
#include "imgui/imgui_freetype.h"

#include "gpuvis_macros.h"
#include "stlini.h"
#include "gpuvis_utils.h"

#include "proggy_tiny.cpp"
#include "Droid_Sans.cpp"
#include "Roboto_Regular.cpp"
#include "RobotoCondensed_Regular.cpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

static SDL_threadID g_main_tid = -1;
static std::vector< char * > g_log;
static std::vector< char * > g_thread_log;
static bool g_log_error = false;
static SDL_mutex *g_mutex = nullptr;

static float g_scale = 1.0f;

enum font_id_t
{
    FontID_Unknown = -1,
    FontID_ProggyTiny = 0,
    FontID_ProggyClean,
    FontID_RobotoRegular,
    FontID_RobotoCondensed,
    FontID_DroidSans,
    FontID_TTFFile
};
struct font_info
{
    const char *name;
    const void *ttf_data;
    int ttf_size;
} g_font_info[] =
{
    { "Proggy Tiny (10)", ProggyTiny_compressed_data, ProggyTiny_compressed_size },
    { "Proggy Clean (13)", NULL, 0 },
    { "Roboto Regular", Roboto_Regular_compressed_data, Roboto_Regular_compressed_size },
    { "Roboto Condensed", RobotoCondensed_Regular_compressed_data, RobotoCondensed_Regular_compressed_size },
    { "Droid Sans", Droid_Sans_compressed_data, Droid_Sans_compressed_size },
};

font_id_t get_font_id( const char *name, const char *filename )
{
    if ( filename && get_file_size( filename ) )
        return FontID_TTFFile;

    for ( size_t i = 0; i < ARRAY_SIZE( g_font_info ); i++ )
    {
        if ( !strcasecmp( name, g_font_info[ i ].name ) )
            return ( font_id_t )i;
    }

    return FontID_Unknown;
}

/*
 * log routines
 */
void logf_init()
{
    g_main_tid = SDL_ThreadID();
    g_mutex = SDL_CreateMutex();
    if ( !g_mutex )
        logf( "[Error] SDL_CreateMutex() failed." );
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
    int ret;
    va_list args;
    char *buf = NULL;

    va_start( args, fmt );
    ret = vasprintf( &buf, fmt, args );
    va_end( args );

    if ( ret >= 0 )
    {
        if ( SDL_ThreadID() == g_main_tid )
        {
            g_log_error |= !strncasecmp( buf, "[error]", 7 );

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

bool logf_update()
{
    bool ret = g_log_error;

    g_log_error = false;

    if ( g_thread_log.size() )
    {
        SDL_LockMutex( g_mutex );

        for ( char *str : g_thread_log )
        {
            ret |= !strncasecmp( str, "[error]", 7 );

            g_log.push_back( str );
        }
        g_thread_log.clear();

        SDL_UnlockMutex( g_mutex );
    }

    return ret;
}

void logf_clear()
{
    logf_update();

    for ( char *str : g_log )
        free( str );
    g_log.clear();
}

int64_t timestr_to_ts( const char *buf )
{
    double val;

    if ( sscanf( buf, "%lf", &val ) != 1 )
        val = 0.0;

    return ( int64_t )( val * NSECS_PER_MSEC );
}

std::string ts_to_timestr( int64_t event_ts, int precision, const char *suffix )
{
    double val = event_ts * ( 1.0 / NSECS_PER_MSEC );

    return string_format( "%.*lf%s", precision, val, suffix ? suffix : " ms" );
}

std::string string_formatv( const char *fmt, va_list ap )
{
    std::string str;
    int size = 512;

    for ( ;; )
    {
        str.resize( size );
        int n = vsnprintf( ( char * )str.c_str(), size, fmt, ap );

        if ( ( n > -1 ) && ( n < size ) )
        {
            str.resize( n );
            return str;
        }

        size = ( n > -1 ) ? ( n + 1 ) : ( size * 2 );
    }
}

std::string string_format( const char *fmt, ... )
{
    va_list ap;
    std::string str;

    va_start( ap, fmt );
    str = string_formatv( fmt, ap );
    va_end( ap );

    return str;
}

std::string string_strftime()
{
    char buf[ 512 ];
    time_t t = time( NULL );
    struct tm *tmp = localtime( &t );

    strftime( buf, sizeof( buf ), "%Y-%m-%d_%H-%M-%S", tmp );
    buf[ sizeof( buf ) - 1 ] = 0;

    return std::string( buf );
}

void string_replace_char( std::string &s, const char search, const char replace )
{
    size_t pos = 0;

    while ( ( pos = s.find( search, pos ) ) != std::string::npos )
        s[ pos ] = replace;
}

void string_replace_str( std::string &s, const std::string &search, const std::string &replace )
{
    for ( size_t pos = 0;; pos += replace.length() )
    {
        pos = s.find( search, pos );
        if ( pos == std::string::npos )
            break;

        s.erase( pos, search.length() );
        s.insert( pos, replace );
    }
}

// http://stackoverflow.com/questions/12966957/is-there-an-equivalent-in-c-of-phps-explode-function
std::vector< std::string > string_explode( std::string const &s, char delim )
{
    std::vector< std::string > result;
    std::istringstream iss( s );

    for ( std::string token; std::getline( iss, token, delim ); )
    {
        result.push_back( std::move( token ) );
    }

    return result;
}

// http://kacperkolodziej.com/articles/programming/253-cpp-implementation-of-implode-and-explode-functions-from-php.html
std::string string_implode( std::vector< std::string > &elements, const std::string &delimiter )
{
    std::string full;

    for ( std::vector< std::string >::iterator it = elements.begin(); it != elements.end(); ++it )
    {
        full += ( *it );
        if ( it != elements.end() - 1 )
            full += delimiter;
    }

    return full;
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

std::string string_remove_punct( const std::string &s )
{
    std::string ret;
    std::remove_copy_if( s.begin(), s.end(),
                         std::back_inserter( ret ),
                         std::ptr_fun< int, int >( &std::ispunct ) );

    return ret;
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

void str_strip_whitespace( char *str )
{
    size_t len;
    const char *front = str;

    while ( isspace( *front ) )
        front++;

    len = strlen( front );
    memmove( str, front, len + 1 );

    while ( ( len > 0 ) && isspace( str[ len - 1 ] ) )
        len--;
    str[ len ] = '\0';
}

char *strstr_ignore_spaces( char *haystack, const char *needle, size_t *len )
{
    while ( *haystack )
    {
        if ( *haystack == *needle )
        {
            char *hay = haystack + 1;
            const char *need = needle + 1;

            for (;;)
            {
                while ( isspace( *hay ) )
                    hay++;
                while ( isspace( *need ) )
                    need++;

                if ( !need[ 0 ] )
                {
                    if ( len )
                        *len = hay - haystack;
                    return haystack;
                }

                if ( *hay++ != *need++ )
                    break;
            }
        }

        haystack++;
    }

    return NULL;
}

void remove_substrings( char *str, const char *fmt, ... )
{
    char *val;
    size_t len;
    va_list args;
    char substr[ 1024 ];

    va_start( args, fmt );
    vsnprintf_safe( substr, fmt, args );
    va_end( args );

    while ( ( val = strstr_ignore_spaces( str, substr, &len ) ) )
        memmove( val, val + len, strlen( val + len ) + 1 );
}

size_t get_file_size( const char *filename )
{
    struct stat st;

    if ( !stat( filename, &st ) )
        return st.st_size;

    return 0;
}

const char *get_path_filename( const char *filename )
{
    for ( const char *str = filename; *str; str++ )
    {
        if ( ( str[ 0 ] == '/' || str[ 0 ] == '\\' ) && str[ 1 ] )
            filename = str + 1;
    }
    return filename;
}

std::string get_realpath( const char *filename )
{
    char buf[ PATH_MAX ];

    if ( !filename || !filename[ 0 ] )
        return "";

#if defined( WIN32 )

    char *basename;
    DWORD len = GetFullPathName( filename, sizeof( buf ), buf, &basename );

    if ( !len || ( len > sizeof( buf ) - 1 ) )
        return filename;
    return buf;

#else

    const char *rp = realpath( filename, buf );

    return rp ? rp : filename;

#endif
}

bool copy_file( const char *filename, const char *newfilename )
{
    bool success = false;

    if ( !filename[ 0 ] || !newfilename[ 0 ] )
        return false;
    if ( !strcasecmp( filename, newfilename ) )
        return false;

#if defined( WIN32 )

    success = !!CopyFile( filename, newfilename, FALSE );

#else

    char buf[ BUFSIZ ];
    int source = TEMP_FAILURE_RETRY( open( filename, O_RDONLY, 0 ) );
    int dest = TEMP_FAILURE_RETRY( open( newfilename, O_WRONLY | O_CREAT, 0644 ) );

    if ( source < 0 )
        logf( "[Error] Opening file '%s' failed: %d", filename, errno );
    else if ( dest < 0 )
        logf ( "[Error] Opening file '%s' failed: %d", newfilename, errno );
    else
    {
        for ( ;; )
        {
            ssize_t size = TEMP_FAILURE_RETRY( read( source, buf, BUFSIZ ) );

            if ( size > 0 )
                size = TEMP_FAILURE_RETRY( write( dest, buf, size ) );

            if ( size == 0 )
            {
                success = true;
                break;
            }

            if ( size < 0 )
            {
                logf( "[Error] copy_file failed: %d\n", errno );
                break;
            }
        }
    }

    if ( source >= 0 )
        close( source );
    if ( dest >= 0 )
        close( dest );

#endif

    return success;
}

// Parse a "comp_[1-2].[0-3].[0-8]" string. Returns true on success.
bool comp_str_parse( const char *comp, uint32_t &a, uint32_t &b, uint32_t &c )
{
    // comp_[1-2].[0-3].[0-8]
    if ( !strncmp( comp, "comp_", 5 ) &&
         ( comp[ 5 ] == '1' || comp[ 5 ] == '2' ) &&
         ( comp[ 6 ] == '.' ) &&
         isdigit( comp[ 7 ] ) &&
         ( comp[ 8 ] == '.' ) &&
         isdigit( comp[ 9 ] ) )
    {
        a = comp[ 5 ] - '0';
        b = comp[ 7 ] - '0';
        c = comp[ 9 ] - '0';

        return ( b <= 3 ) && ( c <= 8 );
    }

    return false;
}

// Create "comp_[1-2].[0-3].[0-8]" string
std::string comp_str_create_abc( uint32_t a, uint32_t b, uint32_t c )
{
    return string_format( "comp_%u.%u.%u", a, b, c );
}

// Return a/b/c values from an index
bool comp_val_to_abc( uint32_t val, uint32_t &a, uint32_t &b, uint32_t &c )
{
    c = val % 9;                // [0-8]
    b = ( val / 9 ) % 4;        // [0-3]
    a = ( val / 36 ) + 1;       // [1-2]

    return ( a <= 2 );
}

uint32_t comp_abc_to_val( uint32_t a, uint32_t b, uint32_t c )
{
    return ( a - 1 ) * 36 + ( b * 9 ) + c;
}

// Return comp_ string from an index
std::string comp_str_create_val( uint32_t val )
{
    uint32_t a, b, c;

    return comp_val_to_abc( val, a, b, c ) ?
                comp_str_create_abc( a, b, c ) : "";
}

ImU32 imgui_col_from_hashval( uint32_t hashval, float sat, float alpha )
{
    float h = ( hashval & 0xff ) / 255.0f;
    float v = ( ( hashval >> 8 ) & 0xff ) / ( 2.0f * 255.0f ) + 0.5f;

    return imgui_hsv( h, sat, v, alpha );
}

ImU32 imgui_hsv( float h, float s, float v, float a )
{
    ImColor color = ImColor::HSV( h, s, v, a );

    return ( ImU32 )color;
}

ImU32 imgui_col_complement( ImU32 col )
{
    float h, s, v;
    ImVec4 color = ImColor( col );
    ImGui::ColorConvertRGBtoHSV( color.x, color.y, color.z, h, s, v );

    h += 0.5f;
    if ( h > 1.0f )
        h -= 1.0f;

    return imgui_hsv( h, s, v, 1.0f );
}

void imgui_text_bg( const ImVec4& bgcolor, const char *fmt, ... )
{
    va_list args;
    char buf[ 512 ];

    va_start( args, fmt );
    vsnprintf_safe( buf, fmt, args );
    va_end( args );

    ImGui::PushStyleColor( ImGuiCol_HeaderHovered, bgcolor );
    ImGui::Selectable( buf, true, ImGuiSelectableFlags_SpanAllColumns );
    ImGui::PopStyleColor();
}

void imgui_push_smallfont()
{
    ImFontAtlas *atlas = ImGui::GetIO().Fonts;
    int index = ( atlas->Fonts.Size > 1 ) ? 1 : 0;

    ImGui::PushFont( atlas->Fonts[ index ] );
}

void imgui_pop_font()
{
    ImGui::PopFont();
}

bool imgui_collapsingheader( const char *label, bool *has_focus, ImGuiTreeNodeFlags flags )
{
    bool ret;

    if ( *has_focus )
        ImGui::PushStyleColor( ImGuiCol_Header, ImGui::GetStyleColorVec4( ImGuiCol_HeaderHovered ) );

    ret = ImGui::CollapsingHeader( label, flags );

    ImGui::PopStyleColor( *has_focus );

    *has_focus = false;
    return ret;
}

bool imgui_begin_columns( const char* str_id, int columns_count, ImGuiColumnsFlags flags )
{
    ImGui::BeginColumns( str_id, columns_count, flags );

    ImGuiColumnsSet *columns = ImGui::GetCurrentWindow()->DC.ColumnsSet;
    return columns->IsFirstFrame;
}

bool imgui_end_columns()
{
    ImGuiColumnsSet *columns = ImGui::GetCurrentWindow()->DC.ColumnsSet;

    ImGui::EndColumns();

    return columns->IsBeingResized;
}

bool imgui_begin_columns( const char *title,
                          const std::initializer_list< const char * > &headers,
                          bool *resized )
{
    bool inited = imgui_begin_columns( title, headers.size() );
    bool temp = false;

    if ( !resized )
        resized = &temp;

    for ( const char *str : headers )
    {
        ImGui::TextColored( s_clrs().getv4( col_BrightText ), "%s", str );
        ImGui::NextColumn();
    }
    ImGui::Separator();

    // If we were just initialized or resized...
    if ( inited || ( *resized && ImGui::IsMouseReleased( 0 ) ) )
    {
        // Go through the columns and save/restore the column width.
        // Skip the last column - it should size to edge of window.
        for ( size_t i = 0; i < headers.size() - 1; i++ )
        {
            std::string key = string_format( "column_width_%s%lu", title, i );

            if ( inited )
            {
                // Try to restore the column widths
                float val = s_ini().GetFloat( key.c_str(), -1.0f );
                if ( val <= 0.0f )
                    break;

                ImGui::SetColumnWidth( i, val );
            }
            else
            {
                // Save the column widths
                s_ini().PutFloat( key.c_str(), ImGui::GetColumnWidth( i ) );
            }
        }

        // Clear the resized flag
        *resized = false;
    }

    return inited;
}

float imgui_scale( float val )
{
    return val * g_scale;
}
void imgui_set_scale( float val )
{
    g_scale = Clamp< float >( val, 0.25f, 6.0f );
}

static colors_t col_index_from_imguicol( ImGuiCol col )
{
    switch ( col )
    {
    case ImGuiCol_Text: return col_ImGui_Text;
    case ImGuiCol_TextDisabled: return col_ImGui_TextDisabled;
    case ImGuiCol_WindowBg: return col_ImGui_WindowBg;
    case ImGuiCol_ChildBg: return col_ImGui_ChildBg;
    case ImGuiCol_PopupBg: return col_ImGui_PopupBg;
    case ImGuiCol_Border: return col_ImGui_Border;
    case ImGuiCol_BorderShadow: return col_ImGui_BorderShadow;
    case ImGuiCol_FrameBg: return col_ImGui_FrameBg;
    case ImGuiCol_FrameBgHovered: return col_ImGui_FrameBgHovered;
    case ImGuiCol_FrameBgActive: return col_ImGui_FrameBgActive;
    case ImGuiCol_TitleBg: return col_ImGui_TitleBg;
    case ImGuiCol_TitleBgActive: return col_ImGui_TitleBgActive;
    case ImGuiCol_TitleBgCollapsed: return col_ImGui_TitleBgCollapsed;
    case ImGuiCol_MenuBarBg: return col_ImGui_MenuBarBg;
    case ImGuiCol_ScrollbarBg: return col_ImGui_ScrollbarBg;
    case ImGuiCol_ScrollbarGrab: return col_ImGui_ScrollbarGrab;
    case ImGuiCol_ScrollbarGrabHovered: return col_ImGui_ScrollbarGrabHovered;
    case ImGuiCol_ScrollbarGrabActive: return col_ImGui_ScrollbarGrabActive;
    case ImGuiCol_CheckMark: return col_ImGui_CheckMark;
    case ImGuiCol_SliderGrab: return col_ImGui_SliderGrab;
    case ImGuiCol_SliderGrabActive: return col_ImGui_SliderGrabActive;
    case ImGuiCol_Button: return col_ImGui_Button;
    case ImGuiCol_ButtonHovered: return col_ImGui_ButtonHovered;
    case ImGuiCol_ButtonActive: return col_ImGui_ButtonActive;
    case ImGuiCol_Header: return col_ImGui_Header;
    case ImGuiCol_HeaderHovered: return col_ImGui_HeaderHovered;
    case ImGuiCol_HeaderActive: return col_ImGui_HeaderActive;
    case ImGuiCol_Separator: return col_ImGui_Separator;
    case ImGuiCol_SeparatorHovered: return col_ImGui_SeparatorHovered;
    case ImGuiCol_SeparatorActive: return col_ImGui_SeparatorActive;
    case ImGuiCol_ResizeGrip: return col_ImGui_ResizeGrip;
    case ImGuiCol_ResizeGripHovered: return col_ImGui_ResizeGripHovered;
    case ImGuiCol_ResizeGripActive: return col_ImGui_ResizeGripActive;
    case ImGuiCol_CloseButton: return col_ImGui_CloseButton;
    case ImGuiCol_CloseButtonHovered: return col_ImGui_CloseButtonHovered;
    case ImGuiCol_CloseButtonActive: return col_ImGui_CloseButtonActive;
    case ImGuiCol_PlotLines: return col_Max;
    case ImGuiCol_PlotLinesHovered: return col_Max;
    case ImGuiCol_PlotHistogram: return col_Max;
    case ImGuiCol_PlotHistogramHovered: return col_Max;
    case ImGuiCol_TextSelectedBg: return col_ImGui_TextSelectedBg;
    case ImGuiCol_ModalWindowDarkening: return col_ImGui_ModalWindowDarkening;
    case ImGuiCol_DragDropTarget: return col_Max;
    case ImGuiCol_NavHighlight: return col_Max;
    case ImGuiCol_NavWindowingHighlight: return col_Max;
    }

    assert( 0 );
    return col_Max;
}

void imgui_set_custom_style( float alpha )
{
    ImGuiStyle& style = ImGui::GetStyle();

    style.Alpha = 1.0f;

    for ( int i = 0; i < ImGuiCol_COUNT; i++ )
    {
        colors_t icol = col_index_from_imguicol( i );

        if ( icol != col_Max )
        {
            // Get imgui color reference
            ImVec4 &col = style.Colors[ i ];

            // Set to new color
            col = s_clrs().getv4( icol );

            if ( col.w < 1.00f )
            {
                // Apply alpha
                col.x *= alpha;
                col.y *= alpha;
                col.z *= alpha;
                col.w *= alpha;
            }
        }
    }
}

void TipWindows::set_tooltip( const char *name, bool *visible, const char *str )
{
    if ( !*visible || !str || !str[ 0 ] )
        return;

    std::string name_str = s_textclrs().bright_str( name );

    ImGui::Begin( name_str.c_str(), visible, ImGuiWindowFlags_AlwaysAutoResize );
    ImGui::Text( "%s", str );
    ImGui::End();
}

void ImageBuf::CreateEmpty( int w, int h )
{
    Clear();

    Width = w;
    Height = h;

    Data = ( uint32_t * )malloc( Width * Height * 4 );
    memset( Data, 0, Width * Height * 4 );
}

void ImageBuf::CreateFromCaptureGL( int x, int y, int w, int h )
{
    Clear();

    Width = w;
    Height = h;

    Data = (  uint32_t * )malloc( Width * Height * 4 );

    glPixelStorei( GL_PACK_ALIGNMENT, 1 );
    glReadPixels( x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, Data );

    RemoveAlpha();
}

int ImageBuf::SaveFile( const char *filename )
{
    return stbi_write_png( filename, Width, Height, 4, Data, Width * 4 );
}

void ImageBuf::RemoveAlpha()
{
    uint32_t *p = Data;
    int n = Width * Height;

    while (n-- > 0)
    {
        *p |= 0xFF000000;
        p++;
    }
}

void ImageBuf::BlitTo( ImageBuf *dst, int src_x, int src_y, int dst_x, int dst_y, int w, int h )
{
    ImageBuf *src = this;

    IM_ASSERT( dst != src );
    IM_ASSERT( dst != NULL );
    IM_ASSERT( src_x >= 0 && src_y >= 0 );
    IM_ASSERT( src_x + w <= src->Width );
    IM_ASSERT( src_y + h <= src->Height );
    IM_ASSERT( dst_x >= 0 && dst_y >= 0 );
    IM_ASSERT( dst_x + w <= dst->Width );
    IM_ASSERT( dst_y + h <= dst->Height );

    for ( int y = 0; y < h; y++ )
        memcpy( dst->Data + dst_x + (dst_y + y) * dst->Width, src->Data + src_x + (src_y + y) * src->Width, w * 4 );
}

void ImageBuf::FlipVertical()
{
    int comp = 4;
    int stride = Width * comp;
    unsigned char *line_tmp = new unsigned char[ stride ];
    unsigned char *line_a = ( unsigned char * )Data;
    unsigned char *line_b = ( unsigned char * )Data + ( stride * ( Height - 1 ) );

    while ( line_a < line_b )
    {
        memcpy( line_tmp, line_a, stride );
        memcpy( line_a, line_b, stride );
        memcpy( line_b, line_tmp, stride );

        line_a += stride;
        line_b -= stride;
    }

    delete [] line_tmp;
}

uint32_t *ImageBuf::GetPtr( int x, int y )
{
    return &Data[ x + y * Width ];
}

uint32_t ImageBuf::GetPixel( int x, int y ) const
{
    return Data[ x + y * Width ];
}

bool imgui_save_screenshot( const char *filename )
{
    ImageBuf Output;
    ImGuiIO& io = ImGui::GetIO();

    Output.CreateFromCaptureGL( 0, 0,
                ( int )io.DisplaySize.x, ( int )io.DisplaySize.y );
    Output.FlipVertical();

    return !!Output.SaveFile( filename );
}

void FontInfo::update_ini()
{
    const char *section = m_section.c_str();

    s_ini().PutStr( "name", m_name.c_str(), section );
    s_ini().PutStr( "filename", m_filename.c_str(), section );
    s_ini().PutFloat( "size", m_size, section );
    s_ini().PutInt( "OverSampleH", m_font_cfg.OversampleH, section );
    s_ini().PutInt( "OverSampleV", m_font_cfg.OversampleV, section );
    s_ini().PutInt( "PixelSnapH", m_font_cfg.PixelSnapH, section );
    s_ini().PutFloat( "GlyphExtraSpacing", m_font_cfg.GlyphExtraSpacing.x, section );
    s_ini().PutInt( "RasterizerFlags", m_font_cfg.RasterizerFlags, section );
    s_ini().PutFloat( "RasterizerMultiply", m_font_cfg.RasterizerMultiply, section );
}

void FontInfo::load_font( const char *section, const char *defname, float defsize, const ImWchar *glyph_ranges )
{
    ImGuiIO &io = ImGui::GetIO();

    if ( !glyph_ranges )
    {
        static const ImWchar def_ranges[] =
        {
            // Basic Latin + Latin Supplement
            // https://en.wikipedia.org/wiki/Latin-1_Supplement_(Unicode_block)
            // ISO 8859-1: 0080-00FF. Controls C1 (0080–009F) are not graphic.
            0x0020, 0x007F,
            0x00A0, 0x00FF,
            0,
        };

        glyph_ranges = &def_ranges[ 0 ];
    }

    m_section = section;
    m_font_cfg = ImFontConfig();

    if ( m_reset )
    {
        m_name = defname;
        m_size = defsize;
        m_filename.clear();

        m_reset = false;
    }
    else
    {
        m_name = s_ini().GetStr( "name", defname, section );
        m_filename = s_ini().GetStr( "filename", "", section );
        m_size = s_ini().GetFloat( "size", defsize, section );

        m_font_cfg.OversampleH = s_ini().GetInt( "OversampleH", m_font_cfg.OversampleH, section );
        m_font_cfg.OversampleV = s_ini().GetInt( "OversampleV", m_font_cfg.OversampleV, section );
        m_font_cfg.PixelSnapH = !!s_ini().GetInt( "PixelSnapH", m_font_cfg.PixelSnapH, section );
        m_font_cfg.GlyphExtraSpacing.x = s_ini().GetFloat( "GlyphExtraSpacing", m_font_cfg.GlyphExtraSpacing.x, section );
        m_font_cfg.RasterizerFlags = s_ini().GetInt( "RasterizerFlags", ImGuiFreeType::LightHinting, section );
        m_font_cfg.RasterizerMultiply = s_ini().GetFloat( "RasterizerMultiply", m_font_cfg.RasterizerMultiply, section );
    }

    m_font_id = get_font_id( m_name.c_str(), m_filename.c_str() );

    if ( !m_filename.empty() )
        strcpy_safe( m_input_filename, m_filename.c_str() );

    m_input_filename_err.clear();

    float fontsize = Clamp< float >( imgui_scale( m_size ), 6.0, 96.0f );
    if ( m_font_id == FontID_TTFFile )
    {
        ImFont *font = io.Fonts->AddFontFromFileTTF( m_filename.c_str(), fontsize, &m_font_cfg, glyph_ranges );

        if ( font )
        {
            m_name = get_path_filename( m_filename.c_str() );
        }
        else
        {
            m_input_filename_err = string_format( "WARNING: AddFontFromFileTTF %s failed.\n", m_filename.c_str() );
            m_font_id = get_font_id( m_name.c_str(), NULL );
        }
    }

    if ( m_font_id != FontID_TTFFile )
    {
        if ( m_font_id == FontID_Unknown )
            m_font_id = FontID_ProggyClean;

        m_name = g_font_info[ m_font_id ].name;

        if ( g_font_info[ m_font_id ].ttf_data )
        {
            io.Fonts->AddFontFromMemoryCompressedTTF(
                        g_font_info[ m_font_id ].ttf_data,
                        g_font_info[ m_font_id ].ttf_size,
                        fontsize, &m_font_cfg, glyph_ranges );
        }
        else
        {
            m_font_cfg.SizePixels = fontsize;
            io.Fonts->AddFontDefault( &m_font_cfg );
        }
    }

    snprintf_safe( m_font_cfg.Name, "%s, %.1fpx", m_name.c_str(), fontsize );

    update_ini();

    m_changed = false;
}

static bool listbox_get_fontname( void *unused, int i, const char **name )
{
    if ( ( i >= 0 ) && ( ( size_t )i < ARRAY_SIZE( g_font_info ) ) )
    {
        *name = g_font_info[ i ].name;
        return true;
    }

    return false;
}

void FontInfo::render_font_options( bool m_use_freetype )
{
    bool changed = false;

    ImGui::PushID( this );

    {
        ImGui::PushItemWidth( imgui_scale( 200.0f ) );

        ImGui::Text( "%s", "Embedded Fonts:" );
        ImGui::SameLine();

        changed |= ImGui::ListBox("##font", &m_font_id, listbox_get_fontname,
                                  g_font_info, ARRAY_SIZE( g_font_info ), ARRAY_SIZE( g_font_info ) );
        if ( changed )
        {
            m_name = g_font_info[ m_font_id ].name;
            m_filename.clear();
        }

        ImGui::PopItemWidth();
    }

    {
        ImGui::PushItemWidth( imgui_scale( 400.0f ) );
        ImGui::AlignTextToFramePadding();
        ImGui::Text( "TTF Filename:" );
        ImGui::SameLine();

        if ( ImGui::InputText( "##ttf_filename", m_input_filename, sizeof( m_input_filename ),
                               ImGuiInputTextFlags_EnterReturnsTrue, 0 ) &&
             m_input_filename[ 0 ] )
        {
            if ( !get_file_size( m_input_filename ) )
            {
                m_input_filename_err = string_format( "ERROR: %s not found.", m_input_filename );
            }
            else
            {
                m_filename = m_input_filename;
                m_input_filename_err.clear();
                changed = true;
            }
        }

        ImGui::PopItemWidth();

        if ( !m_input_filename_err.empty() )
            ImGui::TextColored( ImVec4( 1, 0, 0, 1 ), "%s", m_input_filename_err.c_str() );
    }

    {
        ImGui::PushItemWidth( imgui_scale( 200.0f ) );

        changed |= ImGui::SliderFloat( "##size", &m_size, 7, 64, "Size: %.1f" );

        ImGui::SameLine();
        changed |= ImGui::SliderFloat( "##extra_spacing", &m_font_cfg.GlyphExtraSpacing.x, 0, 4, "Extra Spacing: %.2f" );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "%s", "Extra spacing (in pixels) between glyphs." );

        changed |= ImGui::SliderFloat( "##Brighten", &m_font_cfg.RasterizerMultiply, 0.20f, 4.0f, "Brighten: %.2f" );

        if ( !m_use_freetype )
        {
            ImGui::SameLine();
            changed |= ImGui::SliderInt( "##oversample_h", &m_font_cfg.OversampleH, 1, 4, "OverSampleH: %.0f" );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "%s", "Rasterize at higher quality for sub-pixel positioning." );

#if 0
            // imgui doesn't currently do sub-pixel on Y axis.
            ImGui::SameLine();
            changed |= ImGui::SliderInt( "##oversample_v", &m_font_cfg.OversampleV, 1, 4, "OverSampleV: %.0f" );
#endif
        }

        changed |= ImGui::Checkbox( "PixelSnapH", &m_font_cfg.PixelSnapH );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "%s", "Align every glyph to pixel boundary." );

        if ( m_use_freetype )
        {
            static const struct
            {
                const char *name;
                uint32_t flag;
                const char *descr;
            } s_FreeTypeFlags[] =
            {
                { "No hinting", ImGuiFreeType::NoHinting,
                        "Disable hinting.\nThis generally generates 'blurrier' bitmap glyphs when\n"
                        "the glyph are rendered in any of the anti-aliased modes." },
                { "No auto-hint", ImGuiFreeType::NoAutoHint,
                        "Disable auto-hinter." },
                { "Force auto-hint", ImGuiFreeType::ForceAutoHint,
                        "Prefer auto-hinter over the font's native hinter." },
                { "Light hinting", ImGuiFreeType::LightHinting,
                        "A lighter hinting algorithm for gray-level modes.\nMany generated glyphs are fuzzier but"
                        "better resemble their original shape.\nThis is achieved by snapping glyphs to the pixel grid"
                        "only vertically (Y-axis),\nas is done by Microsoft's ClearType and Adobe's proprietary"
                        "font renderer.\nThis preserves inter-glyph spacing in horizontal text." },
                { "Mono hinting", ImGuiFreeType::MonoHinting,
                        "Strong hinting algorithm that should be used for monochrome output." },
                { "Bold", ImGuiFreeType::Bold, "Artificially embolden the font." },
            };

            for ( size_t i = 0; i < ARRAY_SIZE( s_FreeTypeFlags ); i++ )
            {
                bool val = !!( m_font_cfg.RasterizerFlags & s_FreeTypeFlags[ i ].flag );

                if ( s_FreeTypeFlags[ i ].flag != ImGuiFreeType::LightHinting )
                    ImGui::SameLine();

                if ( ImGui::Checkbox( s_FreeTypeFlags[ i ].name, &val ) )
                {
                    m_font_cfg.RasterizerFlags ^= s_FreeTypeFlags[ i ].flag;
                    changed = true;
                }

                if ( ImGui::IsItemHovered() )
                    ImGui::SetTooltip( "%s", s_FreeTypeFlags[ i ].descr );
            }
        }

        ImGui::PopItemWidth();
    }

    if ( changed )
    {
        update_ini();
        m_changed = true;
    }

    ImGui::PopID();
}

bool ColorPicker::render( ImU32 color, bool is_alpha, ImU32 defcolor )
{
    bool ret = false;
    const float w = imgui_scale( 125.0f );

    {
        static const char s_text[] = "Lorem ipsum dolor sit amet, consectetur adipiscing elit";
        const ImVec2 size = ImGui::CalcTextSize( s_text );
        const ImVec2 size2 = ImGui::CalcTextSize( " ffffffff" );

        ImGui::BeginChild( "color_sample", ImVec2( 0, size.y * 4 ), true );

        ImGui::TextColored( ImColor( color ), s_text );

        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled( pos, ImVec2( pos.x + size.x + size2.x, pos.y + size.y ), color );
        ImGui::EndChild();
    }

    if ( is_alpha  )
    {
        ImColor col = color;
        float val = IM_COL32_A( color ) * ( 1.0f / 255.0f );
        ImGuiColorEditFlags flags = ImGuiColorEditFlags_AlphaPreview;

        ImGui::PushItemWidth( w );
        ret = ImGui::SliderFloat( "##alpha_val", &val, 0.0f, 1.0f, "%.02f" );
        ImGui::PopItemWidth();

        if ( ret )
            m_color = ImColor( val, val, val, val );

        ImGui::ColorButton( "colorpicker##alpha", col, flags, ImVec2( w, w ) );
    }
    else
    {
        ImColor col = color;
        ImColor defcol = defcolor;
        ImGuiColorEditFlags flags = ImGuiColorEditFlags_NoOptions;

        flags |= ImGuiColorEditFlags_AlphaBar;
        flags |= ImGuiColorEditFlags_AlphaPreview;
        flags |= ImGuiColorEditFlags_RGB;
        flags |= ImGuiColorEditFlags_Uint8;
        flags |= ImGuiColorEditFlags_PickerHueBar;

        ImGui::NewLine();
        if ( ImGui::ColorPicker4( "colorpicker##argb", &col.Value.x, flags, &defcol.Value.x ) )
        {
            ret = true;
            m_color = ( ImColor )col;
        }
    }

    return ret;
}

Clrs::colordata_t Clrs::s_colordata[ col_Max ] =
{
#define _XTAG( _name, _color, _desc ) { #_name, _color, _color, false, _desc },
  #include "gpuvis_colors.inl"
#undef _XTAG
};

void Clrs::init()
{
    for ( colors_t i = 0; i < col_Max; i++ )
    {
        const char *key = s_colordata[ i ].name;
        uint64_t val = s_ini().GetUint64( key, UINT64_MAX, "$imgui_colors$" );

        if ( val != UINT64_MAX )
        {
            s_colordata[ i ].color = ( ImU32 )val;
        }
    }
}

void Clrs::shutdown()
{
    for ( colors_t i = 0; i < col_Max; i++ )
    {
        if ( s_colordata[ i ].modified )
        {
            const char *key = s_colordata[ i ].name;

            if ( is_default( i ) )
                s_ini().PutStr( key, "", "$imgui_colors$" );
            else
                s_ini().PutUint64( key, s_colordata[ i ].color, "$imgui_colors$" );
        }
    }
}

ImU32 Clrs::get( colors_t col, ImU32 alpha )
{
    if ( alpha <= 0xff )
        return ( s_colordata[ col ].color & ~IM_COL32_A_MASK ) | ( alpha << IM_COL32_A_SHIFT );

    return s_colordata[ col ].color;
}

ImU32 Clrs::getdef( colors_t col )
{
    return s_colordata[ col ].defcolor;
}

ImVec4 Clrs::getv4( colors_t col, float alpha )
{
    ImVec4 color = ImColor( s_colordata[ col ].color );

    if ( alpha >= 0.0f )
        color.w = alpha;
    return color;
}

float Clrs::getalpha( colors_t col )
{
    return IM_COL32_A( s_colordata[ col ].color ) * ( 1.0f / 255.0f );
}

void Clrs::set( colors_t col, ImU32 color )
{
    if ( s_colordata[ col ].color != color )
    {
        s_colordata[ col ].color = color;
        s_colordata[ col ].modified = true;
    }
}

void Clrs::reset( colors_t col )
{
    set( col, s_colordata[ col ].defcolor );
}

const char *Clrs::name( colors_t col )
{
    // Skip "col_" prefix
    return s_colordata[ col ].name + 4;
}

const char *Clrs::desc( colors_t col )
{
    return s_colordata[ col ].desc;
}

bool Clrs::is_default( colors_t col )
{
    return ( s_colordata[ col ].color == s_colordata[ col ].defcolor );
}

bool Clrs::is_alpha_color( colors_t col )
{
    switch( col )
    {
    case col_ThemeAlpha:
    case col_Graph_PrintLabelSat:
    case col_Graph_PrintLabelAlpha:
    case col_Graph_TimelineLabelSat:
    case col_Graph_TimelineLabelAlpha:
        return true;
    }

    return false;
}

bool Clrs::is_imgui_color( colors_t col )
{
    if ( ( col >= col_ImGui_Text ) || ( col == col_ThemeAlpha ) )
        return true;

    return false;
}

char *TextClrs::set( char ( &dest )[ 6 ], ImU32 color )
{
    dest[ 0 ] = '\033';
    dest[ 1 ] = std::max< uint8_t >( IM_COL32_R( color ), 1 );
    dest[ 2 ] = std::max< uint8_t >( IM_COL32_G( color ), 1 );
    dest[ 3 ] = std::max< uint8_t >( IM_COL32_B( color ), 1 );
    dest[ 4 ] = std::max< uint8_t >( IM_COL32_A( color ), 1 );
    dest[ 5 ] = 0;

    return dest;
}

void TextClrs::update_colors()
{
    ImU32 col_bright = s_clrs().get( col_BrightText );
    ImU32 col_bright_comp = imgui_col_complement( col_bright );

    set( m_buf[ TClr_Def ], ImGui::GetColorU32( ImGuiCol_Text ) );
    set( m_buf[ TClr_Bright ], col_bright );
    set( m_buf[ TClr_BrightComp ], col_bright_comp );
}

const std::string TextClrs::mstr( const std::string &str_in, ImU32 color )
{
    char buf[ 6 ];

    return set( buf, color ) + str_in + m_buf[ TClr_Def ];
}

void Keybd::update( const SDL_KeyboardEvent &key )
{
    if ( key.type == SDL_KEYDOWN )
    {
        // Mark keystate as down w/ mod state
        m_keystate[ key.keysym.scancode ] = 0x80000000 | key.keysym.mod;

        // Tell action handler we got a keydown
        s_actions().keydown( key.keysym.sym, key.keysym.mod, !!key.repeat );
    }
    else if ( key.type == SDL_KEYUP )
    {
        // Clear key state
        m_keystate[ key.keysym.scancode ] = 0;
    }
}

void Keybd::clear()
{
    memset( m_keystate, 0, sizeof( m_keystate ) );
}

void Actions::init()
{
    clear();

    m_actionmap.push_back( { action_help, KMOD_NONE, SDLK_F1, "Help dialog" } );
    m_actionmap.push_back( { action_quit, KMOD_CTRL, SDLK_q, "Quit GpuVis" } );
    m_actionmap.push_back( { action_save_screenshot, KMOD_NONE, SDLK_F12, "Capture screenshot" } );

    m_actionmap.push_back( { action_menu_file, KMOD_ALT, SDLK_f, "File Menu" } );
    m_actionmap.push_back( { action_menu_options, KMOD_ALT, SDLK_o, "Options Menu" } );

    m_actionmap.push_back( { action_open, KMOD_CTRL | KMOD_SHIFT, SDLK_o, "Open Trace File dialog" } );
    m_actionmap.push_back( { action_trace_info, KMOD_CTRL | KMOD_SHIFT, SDLK_i, "Display trace information" } );

    m_actionmap.push_back( { action_focus_graph, KMOD_CTRL | KMOD_SHIFT, SDLK_g, "Set focus to events graph" } );
    m_actionmap.push_back( { action_focus_eventlist, KMOD_CTRL | KMOD_SHIFT, SDLK_e, "Set focus to event list" } );

    m_actionmap.push_back( { action_toggle_show_eventlist, KMOD_NONE, SDLK_F11, "Toggle showing event list" } );

    m_actionmap.push_back( { action_graph_show_hovered_pid, KMOD_CTRL | KMOD_SHIFT, SDLK_s, "Toggle showing only first hovered pid events" } );
    m_actionmap.push_back( { action_graph_show_hovered_tgid, KMOD_CTRL | KMOD_SHIFT, SDLK_t, "Toggle showing only first hovered tgid events" } );

    m_actionmap.push_back( { action_cpugraph_hide_systemevents, KMOD_CTRL | KMOD_SHIFT, SDLK_h, "CPU Graph: Toggle hiding sched_switch 'system' events" } );

    m_actionmap.push_back( { action_graph_zoom_row, KMOD_CTRL | KMOD_SHIFT, SDLK_z, "Graph: Toggle hovered row timeline fullscreen" } );
    m_actionmap.push_back( { action_graph_zoom_mouse, KMOD_NONE, SDLK_z, "Graph: Toggle hovered location zoom to 3ms / restore pre-zoom" } );
    m_actionmap.push_back( { action_graph_hide_row, 0, SDLK_h, "Graph: Hide hovered row" } );

    m_actionmap.push_back( { action_toggle_vblank0, KMOD_CTRL | KMOD_SHIFT, SDLK_m, "Graph: Toggle showing vblank 0 markers" } );
    m_actionmap.push_back( { action_toggle_vblank1, KMOD_CTRL | KMOD_SHIFT, SDLK_n, "Graph: Toggle showing vblank 1 markers" } );
    m_actionmap.push_back( { action_toggle_framemarkers, KMOD_CTRL | KMOD_SHIFT, SDLK_f, "Graph: Toggle showing Frame Markers" } );
    m_actionmap.push_back( { action_toggle_frame_filters, KMOD_CTRL | KMOD_SHIFT, SDLK_r, "Graph: Toggle Frame Filters" } );

    m_actionmap.push_back( { action_frame_marker_prev_fit, KMOD_CTRL | KMOD_REPEAT, SDLK_LEFT, "Graph: Show and fit previous frame marker frame" } );
    m_actionmap.push_back( { action_frame_marker_next_fit, KMOD_CTRL | KMOD_REPEAT, SDLK_RIGHT, "Graph: Show and fit next frame marker frame" } );
    m_actionmap.push_back( { action_frame_marker_prev, KMOD_CTRL | KMOD_SHIFT | KMOD_REPEAT, SDLK_LEFT, "Graph: Show previous frame marker frame" } );
    m_actionmap.push_back( { action_frame_marker_next, KMOD_CTRL| KMOD_SHIFT | KMOD_REPEAT, SDLK_RIGHT, "Graph: Show next frame marker frame" } );

    m_actionmap.push_back( { action_graph_set_markerA, KMOD_CTRL | KMOD_SHIFT, SDLK_a, "Graph: Set marker A" } );
    m_actionmap.push_back( { action_graph_set_markerB, KMOD_CTRL | KMOD_SHIFT, SDLK_b, "Graph: Set marker B" } );
    m_actionmap.push_back( { action_graph_goto_markerA, KMOD_CTRL, SDLK_a, "Graph: Goto marker A" } );
    m_actionmap.push_back( { action_graph_goto_markerB, KMOD_CTRL, SDLK_b, "Graph: Goto marker B" } );

    m_actionmap.push_back( { action_graph_save_location1, KMOD_CTRL | KMOD_SHIFT, SDLK_1, "Graph: Save location 1" } );
    m_actionmap.push_back( { action_graph_save_location2, KMOD_CTRL | KMOD_SHIFT, SDLK_2, "Graph: Save location 2" } );
    m_actionmap.push_back( { action_graph_save_location3, KMOD_CTRL | KMOD_SHIFT, SDLK_3, "Graph: Save location 3" } );
    m_actionmap.push_back( { action_graph_save_location4, KMOD_CTRL | KMOD_SHIFT, SDLK_4, "Graph: Save location 4" } );
    m_actionmap.push_back( { action_graph_save_location5, KMOD_CTRL | KMOD_SHIFT, SDLK_5, "Graph: Save location 5" } );

    m_actionmap.push_back( { action_graph_restore_location1, KMOD_CTRL, SDLK_1, "Graph: Restore location 1" } );
    m_actionmap.push_back( { action_graph_restore_location2, KMOD_CTRL, SDLK_2, "Graph: Restore location 2" } );
    m_actionmap.push_back( { action_graph_restore_location3, KMOD_CTRL, SDLK_3, "Graph: Restore location 3" } );
    m_actionmap.push_back( { action_graph_restore_location4, KMOD_CTRL, SDLK_4, "Graph: Restore location 4" } );
    m_actionmap.push_back( { action_graph_restore_location5, KMOD_CTRL, SDLK_5, "Graph: Restore location 5" } );

    m_actionmap.push_back( { action_graph_pin_tooltip, KMOD_CTRL, SDLK_p, "Graph: Pin current graph tooltip" } );

    m_actionmap.push_back( { action_scroll_up, KMOD_REPEAT, SDLK_UP, "Graph: Scroll / event list up" } );
    m_actionmap.push_back( { action_scroll_down, KMOD_REPEAT, SDLK_DOWN, "Graph: Scroll / event list down" } );

    m_actionmap.push_back( { action_scroll_left, KMOD_REPEAT, SDLK_LEFT, "Graph: Scroll / event list left" } );
    m_actionmap.push_back( { action_scroll_right, KMOD_REPEAT, SDLK_RIGHT, "Graph: Scroll  event list right" } );

    m_actionmap.push_back( { action_scroll_pageup, KMOD_REPEAT, SDLK_PAGEUP, "Graph: Page / event list up" } );
    m_actionmap.push_back( { action_scroll_pagedown, KMOD_REPEAT, SDLK_PAGEDOWN, "Graph: Page / event list down" } );

    m_actionmap.push_back( { action_scroll_home, KMOD_NONE, SDLK_HOME, "Graph: Scroll / event list to start" } );
    m_actionmap.push_back( { action_scroll_end, KMOD_NONE, SDLK_END, "Graph: Scroll / event list to end" } );

    m_actionmap.push_back( { action_escape, KMOD_NONE, SDLK_ESCAPE, NULL } );
    m_actionmap.push_back( { action_return, KMOD_NONE, SDLK_RETURN, NULL } );
}

void Actions::clear()
{
    m_action_count = 0;
    memset( m_actions, 0, sizeof( m_actions ) );
}

void Actions::keydown( SDL_Keycode keycode, uint32_t modstate, bool repeat )
{
    modstate &= ( KMOD_CTRL | KMOD_ALT | KMOD_SHIFT );

    if ( modstate & KMOD_CTRL )
        modstate |= KMOD_CTRL;
    if ( modstate & KMOD_ALT )
        modstate |= KMOD_ALT;
    if ( modstate & KMOD_SHIFT )
        modstate |= KMOD_SHIFT;

    for ( const actionmap_t &map : m_actionmap )
    {
        // If this is a repeat key and the action handler doesn't have bit set, bail
        if ( repeat && !( map.modstate & KMOD_REPEAT ) )
            continue;

        if ( ( map.key == keycode ) &&
             ( ( map.modstate & ~KMOD_REPEAT ) == modstate ) )
        {
            set( map.action );
        }
    }
}

bool Actions::get( action_t action )
{
    if ( m_actions[ action ] )
    {
        m_action_count--;
        m_actions[ action ] = false;
        return true;
    }

    return false;
}

bool Actions::peek( action_t action )
{
    return m_actions[ action ];
}

void Actions::set( action_t action )
{
    if ( !m_actions[ action ] )
    {
        m_action_count++;
        m_actions[ action ] = true;
    }
}

const std::string Actions::hotkey_str( action_t action )
{
    if ( action != action_nil )
    {
        for ( const actionmap_t &map : m_actionmap )
        {
            if ( map.action == action )
            {
                std::string str;

                if ( map.modstate & KMOD_CTRL )
                    str += "Ctrl+";
                if ( map.modstate & KMOD_ALT )
                    str += "Alt+";
                if ( map.modstate & KMOD_SHIFT )
                    str += "Shift+";

                str += SDL_GetKeyName( map.key );

                return str;
            }
        }
    }

    return "";
}

char *StrAlloc::allocmem( size_t len )
{
    char *ptr;

    if ( len >= 512 )
    {
        ptr = ( char * )malloc( len );

        m_chunks.push_back( ptr );
        return ptr;
    }

    if ( !m_ptr || ( len > m_avail ) )
    {
        m_avail = 64 * 1024;
        m_ptr = ( char * )malloc( m_avail );

        m_chunks.push_back( m_ptr );
    }

    ptr = m_ptr;
    m_avail -= len;
    m_ptr += len;

    m_totsize += len;
    return ptr;
}

char *StrAlloc::dupestr( const char *str, size_t len )
{
    char *ptr = allocmem( len + 1 );

    memcpy( ptr, str, len );
    ptr[ len ] = 0;
    return ptr;
}

StrAlloc::~StrAlloc()
{
    for ( char *ptr : m_chunks )
        free( ptr );
    m_chunks.clear();

    m_ptr = nullptr;
    m_avail = 0;
}

/*
 * StrPool
 */
const char *StrPool::getstr( const char *str, size_t len )
{
    if ( len == ( size_t )-1 )
        len = strlen( str );

    uint32_t hashval = hashstr32( str, len );
    const char **ret = m_pool.get_val( hashval );

    if ( !ret )
    {
        char *str2 = m_alloc.dupestr( str, len );

        ret = m_pool.get_val( hashval, str2 );
    }

    return *ret;
}

const char *StrPool::getstrf( const char *fmt, ... )
{
    va_list args;
    char buf[ 512 ];

    va_start( args, fmt );
    vsnprintf_safe( buf, fmt, args );
    va_end( args );

    return getstr( buf );
}

uint32_t StrPool::getu32( const char *str, size_t len )
{
    if ( len == ( size_t )-1 )
        len = strlen( str );

    uint32_t hashval = hashstr32( str, len );
    const char **ret = m_pool.get_val( hashval );

    if ( !ret )
    {
        char *str2 = m_alloc.dupestr( str, len );

        m_pool.get_val( hashval, str2 );
    }

    return hashval;
}

uint32_t StrPool::getu32f( const char *fmt, ... )
{
    va_list args;
    char buf[ 512 ];

    va_start( args, fmt );
    vsnprintf_safe( buf, fmt, args );
    va_end( args );

    return getu32( buf );
}

const char *StrPool::findstr( uint32_t hashval )
{
    const char **str = m_pool.get_val( hashval );

    return str ? *str : NULL;
}

#if defined( WIN32 )

#include <shlwapi.h>

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

#endif // WIN32
