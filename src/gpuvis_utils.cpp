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
#include <sstream>
#include <unordered_map>

#include <SDL.h>

#include "imgui/imgui.h"
#include "imgui/imgui_freetype.h"
#include "gpuvis_macros.h"
#include "gpuvis_utils.h"
#include "stlini.h"

#include "proggy_tiny.cpp"
#include "RobotoCondensed_Regular.cpp"

static SDL_threadID g_main_tid = -1;
static std::vector< char * > g_log;
static std::vector< char * > g_thread_log;
static SDL_mutex *g_mutex = nullptr;

static float g_scale = 1.0f;

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

size_t get_file_size( const char *filename )
{
    struct stat st;

    if ( !stat( filename, &st ) )
        return st.st_size;

    return 0;
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

ImU32 imgui_col_from_hashval( uint32_t hashval )
{
    float h = ( hashval & 0xffffff ) / 16777215.0f;
    float v = ( hashval >> 24 ) / ( 2.0f * 255.0f ) + 0.5f;

    return imgui_hsv( h, .9f, v, 1.0f );
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

ImU32 imgui_col_complement( ImU32 col )
{
    float h, s, v;
    ImVec4 color = imgui_u32_to_vec4( col );
    ImGui::ColorConvertRGBtoHSV( color.x, color.y, color.z, h, s, v );

    h += 0.5f;
    if ( h > 1.0f )
        h -= 1.0f;

    return imgui_hsv( h, s, v, 1.0f );
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
    return val * g_scale;
}
void imgui_set_scale( float val )
{
    g_scale = Clamp< float >( val, 0.25f, 6.0f );
}

bool imgui_key_pressed( ImGuiKey key )
{
    return ImGui::IsKeyPressed( ImGui::GetKeyIndex( key ) );
}

void imgui_ini_settings( CIniFile &inifile, bool save )
{
    ImGuiStyle &style = ImGui::GetStyle();
    const char section[] = "$imgui_settings$";

    if ( save )
    {
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

void FontInfo::update_ini()
{
    const char *section = m_section.c_str();

    m_inifile->PutStr( "name", m_name.c_str(), section );
    m_inifile->PutStr( "filename", m_filename.c_str(), section );
    m_inifile->PutFloat( "size", m_size / g_scale, section );
    m_inifile->PutInt( "OverSampleH", m_font_cfg.OversampleH, section );
    m_inifile->PutInt( "OverSampleV", m_font_cfg.OversampleV, section );
    m_inifile->PutInt( "PixelSnapH", m_font_cfg.PixelSnapH, section );
    m_inifile->PutFloat( "GlyphExtraSpacing", m_font_cfg.GlyphExtraSpacing.x, section );
    m_inifile->PutInt( "FreetypeFlags", m_font_cfg.FreetypeFlags, section );
    m_inifile->PutFloat( "Brighten", m_font_cfg.Brighten, section );
}

void FontInfo::load_font( CIniFile &inifile, const char *section, const char *defname, float defsize )
{
    m_section = section;
    m_inifile = &inifile;
    m_font_cfg = ImFontConfig();

    if ( m_reset )
    {
        m_name = defname;
        m_filename = "";
        m_size = defsize;

        m_reset = false;
    }
    else
    {
        m_name = inifile.GetStr( "name", defname, section );
        m_filename = inifile.GetStr( "filename", "", section );
        m_size = inifile.GetFloat( "size", defsize, section ) * g_scale;

        m_font_cfg.OversampleH = inifile.GetInt( "OversampleH", m_font_cfg.OversampleH, section );
        m_font_cfg.OversampleV = inifile.GetInt( "OversampleV", m_font_cfg.OversampleV, section );
        m_font_cfg.PixelSnapH = !!inifile.GetInt( "PixelSnapH", m_font_cfg.PixelSnapH, section );
        m_font_cfg.GlyphExtraSpacing.x = inifile.GetFloat( "GlyphExtraSpacing", m_font_cfg.GlyphExtraSpacing.x, section );
        m_font_cfg.FreetypeFlags = inifile.GetInt( "FreetypeFlags", m_font_cfg.FreetypeFlags, section );
        m_font_cfg.Brighten = inifile.GetFloat( "Brighten", m_font_cfg.Brighten, section );
    }

    m_font_type = get_type();

    if ( !m_filename.empty() )
        strcpy_safe( m_input_filename, m_filename.c_str() );

    m_input_filename_err = "";

    ImGuiIO &io = ImGui::GetIO();
    static const ImWchar ranges[] =
    {
        // Basic Latin + Latin Supplement
        // https://en.wikipedia.org/wiki/Latin-1_Supplement_(Unicode_block)
        // ISO 8859-1: 0080-00FF. Controls C1 (0080–009F) are not graphic.
        0x0020, 0x007F,
        0x00A0, 0x00FF,
        0,
    };
    if ( m_font_type == TYPE_TTFFile )
    {
        ImFont *font = io.Fonts->AddFontFromFileTTF( m_filename.c_str(), m_size, &m_font_cfg, &ranges[ 0 ] );

        if ( !font )
        {
            m_input_filename_err = string_format( "WARNING: AddFontFromFileTTF %s failed.\n", m_filename.c_str() );
            m_font_type = get_type( false );
        }
        else
        {
            m_name = m_font_cfg.Name;
        }
    }

    if ( m_font_type == TYPE_RobotoCondensed )
    {
        m_name = "Roboto Condensed";

        io.Fonts->AddFontFromMemoryCompressedTTF(
                    RobotoCondensed_Regular_compressed_data, RobotoCondensed_Regular_compressed_size, m_size,
                    &m_font_cfg, &ranges[ 0 ] );
    }
    else if ( m_font_type == TYPE_ProggyTiny )
    {
        m_name = "Proggy Tiny";

        io.Fonts->AddFontFromMemoryCompressedTTF(
                    ProggyTiny_compressed_data, ProggyTiny_compressed_size, m_size,
                    &m_font_cfg, &ranges[ 0 ] );
    }
    else
    {
        m_name = "Proggy Clean";

        m_font_cfg.SizePixels = m_size;
        io.Fonts->AddFontDefault( &m_font_cfg );
    }

    snprintf_safe( m_font_cfg.Name, "%s, %.1fpx", m_name.c_str(), m_size );

    update_ini();

    m_changed = false;
}

FontInfo::font_type_t FontInfo::get_type( bool check_filename )
{
    if ( check_filename && !m_filename.empty() && get_file_size( m_filename.c_str() ) )
        return TYPE_TTFFile;
    else if ( strcasestr( m_name.c_str(), "roboto" ) )
        return TYPE_RobotoCondensed;
    else if ( strcasestr( m_name.c_str(), "proggy tiny" ) )
        return TYPE_ProggyTiny;

    return TYPE_ProggyClean;
}

void FontInfo::render_font_options( bool m_use_freetype )
{
    static const char *fonts[] =
    {
        "Proggy Clean (13)",
        "Proggy Tiny (10)",
        "Roboto Condensed"
    };
    bool changed = false;

    ImGui::PushID( this );

    {
        ImGui::PushItemWidth( imgui_scale( 200.0f ) );

        ImGui::Text( "%s", "Embedded Fonts:" );
        ImGui::SameLine();

        changed |= ImGui::ListBox( "##font", &m_font_type, fonts, ARRAY_SIZE( fonts ), ARRAY_SIZE( fonts ) );
        if ( changed )
        {
            m_name = fonts[ m_font_type ];
            m_filename.clear();
        }

        ImGui::PopItemWidth();
    }

    {
        ImGui::PushItemWidth( imgui_scale( 400.0f ) );
        ImGui::AlignFirstTextHeightToWidgets();
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
                m_input_filename_err = "";
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

        changed |= ImGui::SliderFloat( "##Brighten", &m_font_cfg.Brighten, 0, 4, "Brighten: %.2f" );

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
                { "Disable hinting", ImGuiFreeType::DisableHinting,
                        "Disable hinting.\nThis generally generates 'blurrier' bitmap glyphs when\n"
                        "the glyph are rendered in any of the anti-aliased modes." },
                { "Force auto-hint", ImGuiFreeType::ForceAutoHint,
                        "Prefer auto-hinter over the font's native hinter." },
                { "No auto-hint", ImGuiFreeType::NoAutoHint, "Disable auto-hinter." },
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
                bool val = m_font_cfg.FreetypeFlags & s_FreeTypeFlags[ i ].flag;

                if ( s_FreeTypeFlags[ i ].flag != ImGuiFreeType::LightHinting )
                    ImGui::SameLine();

                if ( ImGui::Checkbox( s_FreeTypeFlags[ i ].name, &val ) )
                {
                    m_font_cfg.FreetypeFlags ^= s_FreeTypeFlags[ i ].flag;
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

#endif
