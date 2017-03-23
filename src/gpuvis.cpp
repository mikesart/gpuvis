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
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <future>
#include <unordered_map>
#include <vector>
#include <set>
#include <sys/types.h>
#include <sys/stat.h>
#include <getopt.h>

#include <SDL2/SDL.h>

#include "GL/gl3w.h"
#include "gpuvis.h"

//$ TODO: Restore column sizes?
//$ TODO: Small font for events?

static SDL_threadID g_main_tid = -1;
static std::vector< char * > g_log;
static std::vector< char * > g_thread_log;
static SDL_mutex *g_mutex = nullptr;

// http://colorbrewer2.org/#type=sequential&scheme=YlOrRd&n=9
static std::array< ImU32, 6 > g_YlRd_hues =
{
    IM_COL32( 254, 178, 76, 255 ),
    IM_COL32( 253, 141, 60, 245 ),
    IM_COL32( 252, 78, 42, 235 ),
    IM_COL32( 227, 26, 28, 225 ),
    IM_COL32( 189, 0, 38, 215 ),
    IM_COL32( 128, 0, 38, 205 ),
};
static std::array< ImU32, 6 > g_GnBu_hues =
{
    IM_COL32( 127, 205, 187, 255 ),
    IM_COL32( 65, 182, 196, 245 ),
    IM_COL32( 29, 145, 192, 235 ),
    IM_COL32( 34, 94, 168, 225 ),
    IM_COL32( 37, 52, 148, 215 ),
    IM_COL32( 8, 29, 88, 205 ),
};
static std::array< ImU32, 6 > g_YlGn_hues =
{
    IM_COL32( 173, 221, 142, 255 ),
    IM_COL32( 120, 198, 121, 245 ),
    IM_COL32( 65, 171, 93, 235 ),
    IM_COL32( 35, 132, 67, 225 ),
    IM_COL32( 0, 104, 55, 215 ),
    IM_COL32( 0, 69, 41, 205 ),
};
static std::array< ImU32, 6 > g_RdPu_hues =
{
    IM_COL32( 250, 159, 181, 255 ),
    IM_COL32( 247, 104, 161, 245 ),
    IM_COL32( 221, 52, 151, 235 ),
    IM_COL32( 174, 1, 126, 225 ),
    IM_COL32( 122, 1, 119, 215 ),
    IM_COL32( 73, 0, 106, 205 ),
};
enum hue_t
{
    Hue_YlRd,
    Hue_GnBu,
    Hue_YlGn,
    Hue_RdPu,
};

ImU32 get_hue( hue_t hues, uint32_t index )
{
    switch( hues )
    {
    default:
    case Hue_YlRd: return g_YlRd_hues[ std::min< uint32_t >( index, g_YlGn_hues.size() - 1 ) ];
    case Hue_GnBu: return g_GnBu_hues[ std::min< uint32_t >( index, g_GnBu_hues.size() - 1 ) ];
    case Hue_YlGn: return g_YlGn_hues[ std::min< uint32_t >( index, g_YlGn_hues.size() - 1 ) ];
    case Hue_RdPu: return g_RdPu_hues[ std::min< uint32_t >( index, g_RdPu_hues.size() - 1 ) ];
    }
}

ImU32 get_YlRd_hue( uint32_t index )
{
    return get_hue( Hue_YlRd, index );
}

// https://www.w3schools.com/colors/colors_groups.asp
const ImU32 col_Black   = IM_COL32( 0x00, 0x00, 0x00, 0xFF );
const ImU32 col_Navy   = IM_COL32( 0x00, 0x00, 0x80, 0xFF );
const ImU32 col_DarkBlue   = IM_COL32( 0x00, 0x00, 0x8B, 0xFF );
const ImU32 col_MediumBlue   = IM_COL32( 0x00, 0x00, 0xCD, 0xFF );
const ImU32 col_Blue   = IM_COL32( 0x00, 0x00, 0xFF, 0xFF );
const ImU32 col_DarkGreen   = IM_COL32( 0x00, 0x64, 0x00, 0xFF );
const ImU32 col_Green   = IM_COL32( 0x00, 0x80, 0x00, 0xFF );
const ImU32 col_Teal   = IM_COL32( 0x00, 0x80, 0x80, 0xFF );
const ImU32 col_DarkCyan   = IM_COL32( 0x00, 0x8B, 0x8B, 0xFF );
const ImU32 col_DeepSkyBlue   = IM_COL32( 0x00, 0xBF, 0xFF, 0xFF );
const ImU32 col_DarkTurquoise   = IM_COL32( 0x00, 0xCE, 0xD1, 0xFF );
const ImU32 col_MediumSpringGreen   = IM_COL32( 0x00, 0xFA, 0x9A, 0xFF );
const ImU32 col_Lime   = IM_COL32( 0x00, 0xFF, 0x00, 0xFF );
const ImU32 col_SpringGreen   = IM_COL32( 0x00, 0xFF, 0x7F, 0xFF );
const ImU32 col_Aqua   = IM_COL32( 0x00, 0xFF, 0xFF, 0xFF );
const ImU32 col_Cyan   = IM_COL32( 0x00, 0xFF, 0xFF, 0xFF );
const ImU32 col_MidnightBlue   = IM_COL32( 0x19, 0x19, 0x70, 0xFF );
const ImU32 col_DodgerBlue   = IM_COL32( 0x1E, 0x90, 0xFF, 0xFF );
const ImU32 col_LightSeaGreen   = IM_COL32( 0x20, 0xB2, 0xAA, 0xFF );
const ImU32 col_ForestGreen   = IM_COL32( 0x22, 0x8B, 0x22, 0xFF );
const ImU32 col_SeaGreen   = IM_COL32( 0x2E, 0x8B, 0x57, 0xFF );
const ImU32 col_DarkSlateGray   = IM_COL32( 0x2F, 0x4F, 0x4F, 0xFF );
const ImU32 col_DarkSlateGrey   = IM_COL32( 0x2F, 0x4F, 0x4F, 0xFF );
const ImU32 col_LimeGreen   = IM_COL32( 0x32, 0xCD, 0x32, 0xFF );
const ImU32 col_MediumSeaGreen   = IM_COL32( 0x3C, 0xB3, 0x71, 0xFF );
const ImU32 col_Turquoise   = IM_COL32( 0x40, 0xE0, 0xD0, 0xFF );
const ImU32 col_RoyalBlue   = IM_COL32( 0x41, 0x69, 0xE1, 0xFF );
const ImU32 col_SteelBlue   = IM_COL32( 0x46, 0x82, 0xB4, 0xFF );
const ImU32 col_DarkSlateBlue   = IM_COL32( 0x48, 0x3D, 0x8B, 0xFF );
const ImU32 col_MediumTurquoise   = IM_COL32( 0x48, 0xD1, 0xCC, 0xFF );
const ImU32 col_Indigo    = IM_COL32( 0x4B, 0x00, 0x82, 0xFF );
const ImU32 col_DarkOliveGreen   = IM_COL32( 0x55, 0x6B, 0x2F, 0xFF );
const ImU32 col_CadetBlue   = IM_COL32( 0x5F, 0x9E, 0xA0, 0xFF );
const ImU32 col_CornflowerBlue   = IM_COL32( 0x64, 0x95, 0xED, 0xFF );
const ImU32 col_RebeccaPurple   = IM_COL32( 0x66, 0x33, 0x99, 0xFF );
const ImU32 col_MediumAquaMarine   = IM_COL32( 0x66, 0xCD, 0xAA, 0xFF );
const ImU32 col_DimGray   = IM_COL32( 0x69, 0x69, 0x69, 0xFF );
const ImU32 col_DimGrey   = IM_COL32( 0x69, 0x69, 0x69, 0xFF );
const ImU32 col_SlateBlue   = IM_COL32( 0x6A, 0x5A, 0xCD, 0xFF );
const ImU32 col_OliveDrab   = IM_COL32( 0x6B, 0x8E, 0x23, 0xFF );
const ImU32 col_SlateGray   = IM_COL32( 0x70, 0x80, 0x90, 0xFF );
const ImU32 col_SlateGrey   = IM_COL32( 0x70, 0x80, 0x90, 0xFF );
const ImU32 col_LightSlateGray   = IM_COL32( 0x77, 0x88, 0x99, 0xFF );
const ImU32 col_LightSlateGrey   = IM_COL32( 0x77, 0x88, 0x99, 0xFF );
const ImU32 col_MediumSlateBlue   = IM_COL32( 0x7B, 0x68, 0xEE, 0xFF );
const ImU32 col_LawnGreen   = IM_COL32( 0x7C, 0xFC, 0x00, 0xFF );
const ImU32 col_Chartreuse   = IM_COL32( 0x7F, 0xFF, 0x00, 0xFF );
const ImU32 col_Aquamarine   = IM_COL32( 0x7F, 0xFF, 0xD4, 0xFF );
const ImU32 col_Maroon   = IM_COL32( 0x80, 0x00, 0x00, 0xFF );
const ImU32 col_Purple   = IM_COL32( 0x80, 0x00, 0x80, 0xFF );
const ImU32 col_Olive   = IM_COL32( 0x80, 0x80, 0x00, 0xFF );
const ImU32 col_Gray   = IM_COL32( 0x80, 0x80, 0x80, 0xFF );
const ImU32 col_Grey   = IM_COL32( 0x80, 0x80, 0x80, 0xFF );
const ImU32 col_SkyBlue   = IM_COL32( 0x87, 0xCE, 0xEB, 0xFF );
const ImU32 col_LightSkyBlue   = IM_COL32( 0x87, 0xCE, 0xFA, 0xFF );
const ImU32 col_BlueViolet   = IM_COL32( 0x8A, 0x2B, 0xE2, 0xFF );
const ImU32 col_DarkRed   = IM_COL32( 0x8B, 0x00, 0x00, 0xFF );
const ImU32 col_DarkMagenta   = IM_COL32( 0x8B, 0x00, 0x8B, 0xFF );
const ImU32 col_SaddleBrown   = IM_COL32( 0x8B, 0x45, 0x13, 0xFF );
const ImU32 col_DarkSeaGreen   = IM_COL32( 0x8F, 0xBC, 0x8F, 0xFF );
const ImU32 col_LightGreen   = IM_COL32( 0x90, 0xEE, 0x90, 0xFF );
const ImU32 col_MediumPurple   = IM_COL32( 0x93, 0x70, 0xDB, 0xFF );
const ImU32 col_DarkViolet   = IM_COL32( 0x94, 0x00, 0xD3, 0xFF );
const ImU32 col_PaleGreen   = IM_COL32( 0x98, 0xFB, 0x98, 0xFF );
const ImU32 col_DarkOrchid   = IM_COL32( 0x99, 0x32, 0xCC, 0xFF );
const ImU32 col_YellowGreen   = IM_COL32( 0x9A, 0xCD, 0x32, 0xFF );
const ImU32 col_Sienna   = IM_COL32( 0xA0, 0x52, 0x2D, 0xFF );
const ImU32 col_Brown   = IM_COL32( 0xA5, 0x2A, 0x2A, 0xFF );
const ImU32 col_DarkGray   = IM_COL32( 0xA9, 0xA9, 0xA9, 0xFF );
const ImU32 col_DarkGrey   = IM_COL32( 0xA9, 0xA9, 0xA9, 0xFF );
const ImU32 col_LightBlue   = IM_COL32( 0xAD, 0xD8, 0xE6, 0xFF );
const ImU32 col_GreenYellow   = IM_COL32( 0xAD, 0xFF, 0x2F, 0xFF );
const ImU32 col_PaleTurquoise   = IM_COL32( 0xAF, 0xEE, 0xEE, 0xFF );
const ImU32 col_LightSteelBlue   = IM_COL32( 0xB0, 0xC4, 0xDE, 0xFF );
const ImU32 col_PowderBlue   = IM_COL32( 0xB0, 0xE0, 0xE6, 0xFF );
const ImU32 col_FireBrick   = IM_COL32( 0xB2, 0x22, 0x22, 0xFF );
const ImU32 col_DarkGoldenRod   = IM_COL32( 0xB8, 0x86, 0x0B, 0xFF );
const ImU32 col_MediumOrchid   = IM_COL32( 0xBA, 0x55, 0xD3, 0xFF );
const ImU32 col_RosyBrown   = IM_COL32( 0xBC, 0x8F, 0x8F, 0xFF );
const ImU32 col_DarkKhaki   = IM_COL32( 0xBD, 0xB7, 0x6B, 0xFF );
const ImU32 col_Silver   = IM_COL32( 0xC0, 0xC0, 0xC0, 0xFF );
const ImU32 col_MediumVioletRed   = IM_COL32( 0xC7, 0x15, 0x85, 0xFF );
const ImU32 col_IndianRed    = IM_COL32( 0xCD, 0x5C, 0x5C, 0xFF );
const ImU32 col_Peru   = IM_COL32( 0xCD, 0x85, 0x3F, 0xFF );
const ImU32 col_Chocolate   = IM_COL32( 0xD2, 0x69, 0x1E, 0xFF );
const ImU32 col_Tan   = IM_COL32( 0xD2, 0xB4, 0x8C, 0xFF );
const ImU32 col_LightGray   = IM_COL32( 0xD3, 0xD3, 0xD3, 0xFF );
const ImU32 col_LightGrey   = IM_COL32( 0xD3, 0xD3, 0xD3, 0xFF );
const ImU32 col_Thistle   = IM_COL32( 0xD8, 0xBF, 0xD8, 0xFF );
const ImU32 col_Orchid   = IM_COL32( 0xDA, 0x70, 0xD6, 0xFF );
const ImU32 col_GoldenRod   = IM_COL32( 0xDA, 0xA5, 0x20, 0xFF );
const ImU32 col_PaleVioletRed   = IM_COL32( 0xDB, 0x70, 0x93, 0xFF );
const ImU32 col_Crimson   = IM_COL32( 0xDC, 0x14, 0x3C, 0xFF );
const ImU32 col_Gainsboro   = IM_COL32( 0xDC, 0xDC, 0xDC, 0xFF );
const ImU32 col_Plum   = IM_COL32( 0xDD, 0xA0, 0xDD, 0xFF );
const ImU32 col_BurlyWood   = IM_COL32( 0xDE, 0xB8, 0x87, 0xFF );
const ImU32 col_LightCyan   = IM_COL32( 0xE0, 0xFF, 0xFF, 0xFF );
const ImU32 col_Lavender   = IM_COL32( 0xE6, 0xE6, 0xFA, 0xFF );
const ImU32 col_DarkSalmon   = IM_COL32( 0xE9, 0x96, 0x7A, 0xFF );
const ImU32 col_Violet   = IM_COL32( 0xEE, 0x82, 0xEE, 0xFF );
const ImU32 col_PaleGoldenRod   = IM_COL32( 0xEE, 0xE8, 0xAA, 0xFF );
const ImU32 col_LightCoral   = IM_COL32( 0xF0, 0x80, 0x80, 0xFF );
const ImU32 col_Khaki   = IM_COL32( 0xF0, 0xE6, 0x8C, 0xFF );
const ImU32 col_AliceBlue   = IM_COL32( 0xF0, 0xF8, 0xFF, 0xFF );
const ImU32 col_HoneyDew   = IM_COL32( 0xF0, 0xFF, 0xF0, 0xFF );
const ImU32 col_Azure   = IM_COL32( 0xF0, 0xFF, 0xFF, 0xFF );
const ImU32 col_SandyBrown   = IM_COL32( 0xF4, 0xA4, 0x60, 0xFF );
const ImU32 col_Wheat   = IM_COL32( 0xF5, 0xDE, 0xB3, 0xFF );
const ImU32 col_Beige   = IM_COL32( 0xF5, 0xF5, 0xDC, 0xFF );
const ImU32 col_WhiteSmoke   = IM_COL32( 0xF5, 0xF5, 0xF5, 0xFF );
const ImU32 col_MintCream   = IM_COL32( 0xF5, 0xFF, 0xFA, 0xFF );
const ImU32 col_GhostWhite   = IM_COL32( 0xF8, 0xF8, 0xFF, 0xFF );
const ImU32 col_Salmon   = IM_COL32( 0xFA, 0x80, 0x72, 0xFF );
const ImU32 col_AntiqueWhite   = IM_COL32( 0xFA, 0xEB, 0xD7, 0xFF );
const ImU32 col_Linen   = IM_COL32( 0xFA, 0xF0, 0xE6, 0xFF );
const ImU32 col_LightGoldenRodYellow   = IM_COL32( 0xFA, 0xFA, 0xD2, 0xFF );
const ImU32 col_OldLace   = IM_COL32( 0xFD, 0xF5, 0xE6, 0xFF );
const ImU32 col_Red   = IM_COL32( 0xFF, 0x00, 0x00, 0xFF );
const ImU32 col_Fuchsia   = IM_COL32( 0xFF, 0x00, 0xFF, 0xFF );
const ImU32 col_Magenta   = IM_COL32( 0xFF, 0x00, 0xFF, 0xFF );
const ImU32 col_DeepPink   = IM_COL32( 0xFF, 0x14, 0x93, 0xFF );
const ImU32 col_OrangeRed   = IM_COL32( 0xFF, 0x45, 0x00, 0xFF );
const ImU32 col_Tomato   = IM_COL32( 0xFF, 0x63, 0x47, 0xFF );
const ImU32 col_HotPink   = IM_COL32( 0xFF, 0x69, 0xB4, 0xFF );
const ImU32 col_Coral   = IM_COL32( 0xFF, 0x7F, 0x50, 0xFF );
const ImU32 col_DarkOrange   = IM_COL32( 0xFF, 0x8C, 0x00, 0xFF );
const ImU32 col_LightSalmon   = IM_COL32( 0xFF, 0xA0, 0x7A, 0xFF );
const ImU32 col_Orange   = IM_COL32( 0xFF, 0xA5, 0x00, 0xFF );
const ImU32 col_LightPink   = IM_COL32( 0xFF, 0xB6, 0xC1, 0xFF );
const ImU32 col_Pink   = IM_COL32( 0xFF, 0xC0, 0xCB, 0xFF );
const ImU32 col_Gold   = IM_COL32( 0xFF, 0xD7, 0x00, 0xFF );
const ImU32 col_PeachPuff   = IM_COL32( 0xFF, 0xDA, 0xB9, 0xFF );
const ImU32 col_NavajoWhite   = IM_COL32( 0xFF, 0xDE, 0xAD, 0xFF );
const ImU32 col_Moccasin   = IM_COL32( 0xFF, 0xE4, 0xB5, 0xFF );
const ImU32 col_Bisque   = IM_COL32( 0xFF, 0xE4, 0xC4, 0xFF );
const ImU32 col_MistyRose   = IM_COL32( 0xFF, 0xE4, 0xE1, 0xFF );
const ImU32 col_BlanchedAlmond   = IM_COL32( 0xFF, 0xEB, 0xCD, 0xFF );
const ImU32 col_PapayaWhip   = IM_COL32( 0xFF, 0xEF, 0xD5, 0xFF );
const ImU32 col_LavenderBlush   = IM_COL32( 0xFF, 0xF0, 0xF5, 0xFF );
const ImU32 col_SeaShell   = IM_COL32( 0xFF, 0xF5, 0xEE, 0xFF );
const ImU32 col_Cornsilk   = IM_COL32( 0xFF, 0xF8, 0xDC, 0xFF );
const ImU32 col_LemonChiffon   = IM_COL32( 0xFF, 0xFA, 0xCD, 0xFF );
const ImU32 col_FloralWhite   = IM_COL32( 0xFF, 0xFA, 0xF0, 0xFF );
const ImU32 col_Snow   = IM_COL32( 0xFF, 0xFA, 0xFA, 0xFF );
const ImU32 col_Yellow   = IM_COL32( 0xFF, 0xFF, 0x00, 0xFF );
const ImU32 col_LightYellow   = IM_COL32( 0xFF, 0xFF, 0xE0, 0xFF );
const ImU32 col_Ivory   = IM_COL32( 0xFF, 0xFF, 0xF0, 0xFF );
const ImU32 col_White   = IM_COL32( 0xFF, 0xFF, 0xFF, 0xFF );

// Programmer pink
const ImU32 col_BrightPink   = IM_COL32( 0xFF, 0x00, 0xFF, 0xFF );

/*
 * log routines
 */
static void logf_init()
{
    g_main_tid = SDL_ThreadID();
    g_mutex = SDL_CreateMutex();
}

static void logf_shutdown()
{
    SDL_DestroyMutex( g_mutex );
    g_mutex = NULL;
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

static void logf_update()
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
void string_ltrim(std::string &s)
{
    s.erase( s.begin(), std::find_if(s.begin(), s.end(),
             std::not1(std::ptr_fun<int, int>(std::isspace))) );
}

// trim from end (in place)
void string_rtrim(std::string &s)
{
    s.erase( std::find_if(s.rbegin(), s.rend(),
             std::not1(std::ptr_fun<int, int>(std::isspace))).base(), s.end() );
}

// trim from both ends (in place)
void string_trim(std::string &s)
{
    string_ltrim( s );
    string_rtrim( s );
}

// trim from start (copying)
std::string string_ltrimmed(std::string s)
{
    string_ltrim( s );
    return s;
}

// trim from end (copying)
std::string string_rtrimmed(std::string s)
{
    string_rtrim( s );
    return s;
}

// trim from both ends (copying)
std::string string_trimmed(std::string s)
{
    string_trim( s );
    return s;
}

size_t get_file_size( const char *filename )
{
    struct stat st;

    if( !stat( filename, &st ) )
        return st.st_size;

    return 0;
}

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

/*
 * StrPool
 */
const char *StrPool::getstr( const char *str )
{
    uint32_t hashval = fnv_hashstr32( str );

    auto i = m_pool.find( hashval );
    if ( i == m_pool.end() )
        m_pool[ hashval ] = std::string( str );

    return m_pool[ hashval ].c_str();
}

const char *StrPool::getstr( uint32_t hashval )
{
    auto i = m_pool.find( hashval );

    if ( i == m_pool.end() )
        return NULL;
    return m_pool[ hashval ].c_str();
}

/*
 * TraceLoader
 */
TraceLoader::state_t TraceLoader::get_state()
{
    return ( state_t )SDL_AtomicGet( &m_state );
}

bool TraceLoader::is_loading()
{
    return ( get_state() == State_Loading || get_state() == State_CancelLoading );
}

void TraceLoader::set_state( state_t state )
{
    m_filename = "";
    m_trace_events = NULL;
    m_thread = NULL;

    SDL_AtomicSet( &m_state, state );
}

void TraceLoader::cancel_load_file()
{
    // Switch to cancel loading if we're currently loading
    SDL_AtomicCAS( &m_state, State_Loading, State_CancelLoading );
}

bool TraceLoader::load_file( const char *filename )
{
    if ( is_loading() )
    {
        logf( "[Error] %s failed, currently loading %s.", __func__, m_filename.c_str() );
        return false;
    }

    if ( access( filename, R_OK ) == -1 )
    {
        logf( "[Error] %s (%s) failed: %s", __func__, filename, strerror( errno ) );
        return false;
    }
    size_t filesize = get_file_size( filename );
    std::string title = string_format( "%s (%.2f MB)", filename, filesize / ( 1024.0f * 1024.0f ) );

    // Check if we've already loaded this trace file.
    for ( TraceEvents *events : m_trace_events_list )
    {
        if ( events->m_title == title )
        {
            new_event_window( events );
            return true;
        }
    }

    set_state( State_Loading );
    m_filename = filename;

    m_trace_events = new TraceEvents;
    m_trace_events->m_filename = filename;
    m_trace_events->m_filesize = filesize;
    m_trace_events->m_title = title;

    SDL_AtomicSet( &m_trace_events->m_eventsloaded, 1 );

    m_thread = SDL_CreateThread( thread_func, "eventloader", ( void * )this );
    if ( m_thread )
    {
        new_event_window( m_trace_events );
        m_trace_events_list.push_back( m_trace_events );
        return true;
    }

    logf( "[Error] %s: SDL_CreateThread failed.", __func__ );

    delete m_trace_events;
    m_trace_events = NULL;

    set_state( State_Idle );
    return false;
}

void TraceLoader::new_event_window( TraceEvents *trace_events )
{
    size_t refcount = 0;
    std::string title = trace_events->m_title;

    for ( int i = m_trace_windows_list.size() - 1; i >= 0; i-- )
    {
        if ( m_trace_windows_list[ i ]->m_trace_events == trace_events )
            refcount++;
    }

    if ( refcount )
        title += string_format( " #%lu", refcount + 1 );

    TraceWin *win = new TraceWin( trace_events, title );

    m_trace_windows_list.push_back( win );
    win->m_setfocus = 2;
}

void TraceLoader::close_event_file( TraceEvents *trace_events, bool close_file  )
{
    for ( int i = m_trace_windows_list.size() - 1; i >= 0; i-- )
    {
        TraceWin *win = m_trace_windows_list[ i ];

        if ( win->m_open && ( win->m_trace_events == trace_events ) )
            win->m_open = false;
    }

    if ( close_file )
    {
        for ( size_t i = 0; i < m_trace_events_list.size(); i++ )
        {
            if ( m_trace_events_list[ i ] == trace_events )
            {
                delete trace_events;
                m_trace_events_list.erase( m_trace_events_list.begin() + i );
                break;
            }
        }
    }
}

int TraceLoader::new_event_cb( TraceLoader *loader, const trace_info_t &info,
                                const trace_event_t &event )
{
    TraceEvents *trace_events = loader->m_trace_events;
    size_t id = trace_events->m_events.size();

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

    trace_events->m_events.push_back( event );
    trace_events->m_events[ id ].id = id;

    trace_events->m_event_locations.add_location( event.name, id );
    trace_events->m_comm_locations.add_location( event.comm, id );

    SDL_AtomicAdd( &trace_events->m_eventsloaded, 1 );

    if ( loader->get_state() == State_CancelLoading )
        return 1;

    return 0;
}

int SDLCALL TraceLoader::thread_func( void *data )
{
    TraceLoader *loader = ( TraceLoader * )data;
    TraceEvents *trace_events = loader->m_trace_events;
    const char *filename = loader->m_filename.c_str();

    logf( "Reading trace file %s...", filename );

    EventCallback trace_cb = std::bind( new_event_cb, loader, _1, _2 );
    if ( read_trace_file( filename, trace_events->m_strpool, trace_cb ) < 0 )
    {
        logf( "[Error]: read_trace_file(%s) failed.", filename );

        SDL_AtomicSet( &trace_events->m_eventsloaded, -1 );
        loader->set_state( State_Idle );
        return -1;
    }

    logf( "Events read: %lu", trace_events->m_events.size() );

    SDL_AtomicSet( &trace_events->m_eventsloaded, 0 );
    loader->set_state( State_Loaded );
    return 0;
}

void TraceLoader::shutdown()
{
    if ( m_thread )
    {
        // Cancel any file loading going on.
        cancel_load_file();

        // Wait for our thread to die.
        SDL_WaitThread( m_thread, NULL );
        m_thread = NULL;
    }

    set_state( State_Idle );

    for ( TraceWin *win : m_trace_windows_list )
        delete win;
    m_trace_windows_list.clear();

    for ( TraceEvents *events : m_trace_events_list )
        delete events;
    m_trace_events_list.clear();
}

void TraceLoader::render()
{
    for ( int i = m_trace_windows_list.size() - 1; i >= 0; i-- )
    {
        TraceWin *win = m_trace_windows_list[ i ];

        if ( win->m_open )
            win->render( this );
        if ( !win->m_open )
        {
            delete win;
            m_trace_windows_list.erase( m_trace_windows_list.begin() + i );
        }
    }
}

/*
 * TracWin
 */
void TraceWin::render_time_delta_button_init( TraceEvents &trace_events )
{
    // Default to minimum time stamp.
    unsigned long long ts = trace_events.m_ts_min;

    // Try to grab all the vblank event locations.
    std::vector< uint32_t > *vblank_locs = trace_events.get_event_locs( "drm_vblank_event" );

    if ( vblank_locs )
    {
        std::vector< trace_event_t > &events = trace_events.m_events;

        // Use first vblank, but then try to find first vblank where pid != 0.
        ts = vblank_locs->at( 0 );

        for ( uint32_t i : *vblank_locs )
        {
            if ( events[ i ].pid )
            {
                ts = events[ i ].ts;

                m_do_gotoevent = true;
                m_goto_eventid = i;
                break;
            }
        }
    }

    m_timedelta_buf = ts_to_timestr( ts );
    m_tsdelta = ts;
}

void TraceWin::render_time_delta_button( TraceEvents &trace_events )
{
    if ( m_tsdelta == ( unsigned long long )-1 )
        render_time_delta_button_init( trace_events );

    bool time_delta = ImGui::Button( "Time Offset:" );

    ImGui::SameLine();
    ImGui::PushItemWidth( 150 );
    m_timedelta_buf.reserve( 32 );
    time_delta |= ImGui::InputText( "##TimeDelta", &m_timedelta_buf[ 0 ], m_timedelta_buf.capacity(), 0, 0 );
    ImGui::PopItemWidth();

    if ( time_delta )
    {
        m_tsdelta = timestr_to_ts( m_timedelta_buf.c_str() );
    }
}

unsigned long long TraceWin::timestr_to_ts( const char *buf, unsigned long long tsdelta )
{
    const char *dot = strchr( buf, '.' );
    long msecs = strtol( buf, NULL, 10 );
    unsigned long nsecs = dot ? strtoul( dot + 1, NULL, 10 ) : 0;
    long neg = ( msecs < 0 ) ? -1 : +1;

    while ( nsecs && ( nsecs * 10 < MSECS_PER_SEC ) )
        nsecs *= 10;
    if ( neg < 0 )
        msecs = -msecs;

    long delta = ( msecs / MSECS_PER_SEC + nsecs + msecs * MSECS_PER_SEC );

    return tsdelta + neg * delta;
}

std::string TraceWin::ts_to_timestr( unsigned long long event_ts, unsigned long long tsdelta )
{
    bool ts_negative = ( tsdelta > event_ts );
    unsigned long long ts = ts_negative ? ( tsdelta - event_ts ) : ( event_ts - tsdelta );
    unsigned long msecs = ts / MSECS_PER_SEC;
    unsigned long nsecs = ts - msecs * MSECS_PER_SEC;

    return string_format( "%s%lu.%06lu", ts_negative ? "-" : "", msecs, nsecs );
}

void TraceWin::init_graph_rows_str()
{
    m_graph_rows_str = "# Place comm / event names to graph\n";
    m_graph_rows_str += "# fence_signaled\n";
    m_graph_rows_str += "# amd_sched_job\n";

    for ( auto item : m_trace_events->m_comm_locations.m_locations )
    {
        uint32_t hashval = item.first;
        const char *comm = m_trace_events->m_strpool.getstr( hashval );

        if ( strstr( comm, "trace-cmd" ) ||
             strstr( comm, "ksoftirqd" ) ||
             strstr( comm, "kworker" ) )
        {
            m_graph_rows_str += "# ";
        }

        m_graph_rows_str += comm;
        m_graph_rows_str += "\n";
    }

    update_graph_rows_list();
}

void TraceWin::update_graph_rows_list()
{
    const char *begin = m_graph_rows_str.c_str();

    m_graph_rows.clear();
    for( ;; )
    {
        const char *end = strchr( begin, '\n' );
        std::string val = end ? std::string( begin, end - begin ) : begin;

        string_trim( val );

        if ( !val.empty() && val[ 0 ] != '#' )
            m_graph_rows.push_back( val );

        if ( !end )
            break;

        begin = end + 1;
    }
}

int TraceWin::ts_to_eventid( unsigned long long ts )
{
    trace_event_t x;
    std::vector< trace_event_t > &events = m_trace_events->m_events;

    x.ts = ts;

    auto eventidx = std::lower_bound( events.begin(), events.end(), x,
                                      []( const trace_event_t &f1, const trace_event_t &f2 ) { return f1.ts < f2.ts; } );

    int id = eventidx - events.begin();

    if ( ( size_t )id >= events.size() )
        id = events.size() - 1;
    return id;
}

int TraceWin::timestr_to_eventid( const char *buf, unsigned long long tsdelta )
{
    unsigned long long ts = timestr_to_ts( buf, tsdelta );

    return ts_to_eventid( ts );
}

bool TraceWin::render_time_goto_button( TraceEvents &trace_events )
{
    m_do_gototime |= ImGui::Button( "Goto Time:" );

    ImGui::SameLine();
    ImGui::PushItemWidth( 150 );
    m_timegoto_buf.reserve( 32 );
    m_do_gototime |= ImGui::InputText( "##TimeGoto", &m_timegoto_buf[ 0 ], m_timegoto_buf.capacity(), 0, 0 );
    ImGui::PopItemWidth();

    if ( m_do_gototime )
    {
        m_goto_eventid = timestr_to_eventid( m_timegoto_buf.c_str(), m_tsdelta );
    }

    return m_do_gototime;
}

bool TraceWin::render( class TraceLoader *loader )
{
    ImGui::SetNextWindowSize( ImVec2( 800, 600 ), ImGuiSetCond_FirstUseEver );

    // If we're told to set focus, wait until the mouse isn't down as they
    //  could have clicked on a button to set focus. Also, hack to do it
    //  twice as the button code still steals our focus if we do it once.
    if ( m_setfocus && !ImGui::IsMouseDown( 0 ) )
    {
        ImGui::SetNextWindowFocus();
        m_setfocus--;
    }

    int eventsloaded = SDL_AtomicGet( &m_trace_events->m_eventsloaded );
    if ( eventsloaded > 0 )
    {
        ImGui::Begin( m_title.c_str(), &m_open );
        ImGui::Text( "Loading events %u...", eventsloaded );

        if ( ImGui::Button( "Cancel" ) )
            loader->cancel_load_file();
        ImGui::End();

        return true;
    }
    else if ( eventsloaded == -1 )
    {
        ImGui::Begin( m_title.c_str(), &m_open );
        ImGui::Text( "Error loading filed %s...\n", m_trace_events->m_filename.c_str() );
        ImGui::End();

        return true;
    }

    return render_events();
}

bool TraceWin::render_options()
{
    size_t event_count = m_trace_events->m_events.size();
    ImGui::Text( "Events: %lu\n", event_count );

    if ( !event_count )
        return false;

    if ( m_selected != ( uint32_t )-1 )
    {
        ImGui::SameLine();
        ImGui::Text( "Selected: %u", m_selected );
    }

    imgui_input_int( &m_start_eventid, 75.0f, "Event Start:", "##EventStart" );

    ImGui::SameLine();
    imgui_input_int( &m_end_eventid, 75.0f, "Event End:", "##EventEnd" );

    ImGui::SameLine();
    render_time_delta_button( *m_trace_events );

    m_do_gotoevent |= imgui_input_int( &m_goto_eventid, 75.0f, "Goto Event:", "##GotoEvent" );

    ImGui::SameLine();
    m_do_gotoevent |= render_time_goto_button( *m_trace_events );

    return true;
}

void TraceWin::render_events_list()
{
    float scale = ImGui::GetIO().FontGlobalScale;
    size_t event_count = m_end_eventid - m_start_eventid + 1;
    std::vector< trace_event_t > &events = m_trace_events->m_events;

    // Set focus on event list first time we open.
    if ( !m_inited && ImGui::IsWindowFocused() )
        ImGui::SetNextWindowFocus();

    // Events list
    ImVec2 avail = ImGui::GetContentRegionAvail();

    {
        // Set the child window size to hold count of items + header + separator
        float lineh = ImGui::GetTextLineHeightWithSpacing();
        float y = ( avail.y < 512.0f * scale ) ? 512.0f * scale : 0.0f;

        ImGui::SetNextWindowContentSize( { 0.0f, ( event_count + 1 ) * lineh + 1 } );
        ImGui::BeginChild( "eventlistbox", ImVec2( 0.0f, y ) );

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
            ImGui::SetScrollY( std::max< int >( 0, m_goto_eventid - m_start_eventid ) * lineh );

            m_do_gotoevent = false;
            m_do_gototime = false;
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
            trace_event_t &event = events[ m_start_eventid + i ];
            bool selected = ( m_selected == i );
            bool is_vblank = !strcmp( event.name, "drm_vblank_event" );
            std::string ts_str = ts_to_timestr( event.ts, m_tsdelta );

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
            ImGui::Text( "%s", ts_str.c_str() );
            ImGui::NextColumn();
            ImGui::Text( "%s", event.comm );
            ImGui::NextColumn();
            ImGui::Text( "%s", event.name );
            ImGui::NextColumn();

            ImGui::PopStyleColor( colors_pushed );
        }

        ImGui::Columns( 1 );
        ImGui::EndChild();
    }
}

static void imgui_drawrect( float x, float w, float y, float h, ImU32 color )
{
    if ( w <= 1.0f )
        ImGui::GetWindowDrawList()->AddLine( ImVec2( x, y ), ImVec2( x, y + h ), color );
    else
        ImGui::GetWindowDrawList()->AddRectFilled( ImVec2( x, y ), ImVec2( x + w, y + h ), color );
}

class event_renderer_t
{
public:
    event_renderer_t( float y_in, float w_in, float h_in, hue_t hue_in )
    {
        y = y_in;
        w = w_in;
        h = h_in;
        x0 = -1;
        hue = hue_in;
    }

    void add_event( float x )
    {
        if ( x0 < 0.0f )
        {
            // First event
            start( x );
        }
        else if ( x - x1 <= 1.0f )
        {
            // New event real close to last event
            x1 = x;
            num_events++;
        }
        else
        {
            // New event is away from current group, so draw.
            draw();

            // Start a new group
            start( x );
        }
    }

    void done()
    {
        if ( x0 != -1 )
        {
            draw();
            start( -1.0f );
        }
    }

protected:
    void start( float x )
    {
        num_events = 0;
        x0 = x;
        x1 = x + .0001f;
    }

    void draw()
    {
        // Try to figure out how many events per x unit.
        // Usually is around 1 (not crowded) to 40 (really crowded)
        float crowding = num_events / ceil( x1 - x0 );

        // Try to map crowding to color hue.
        uint32_t index = crowding * 6.0f / 42.0f;
        ImU32 color = get_hue( hue, index );

        imgui_drawrect( x0, x1 - x0, y, h, color );
    }

public:
    float x0, x1;
    uint32_t num_events;

    float y, w, h;
    hue_t hue;
};

void TraceWin::render_process_graphs()
{
    float scale = ImGui::GetIO().FontGlobalScale;

    float h = 40.0f * scale;
    float w = ImGui::GetContentRegionAvailWidth();

    std::vector< trace_event_t > &events = m_trace_events->m_events;
    uint32_t eventstart = m_graph_start_eventid;
    uint32_t eventend = m_graph_end_eventid;
    trace_event_t *event0 = &events[ eventstart ];
    trace_event_t *event1 = &events[ eventend ];

    unsigned long long ts0 = event0->ts;
    unsigned long long ts1 = event1->ts;
    unsigned long long tsdx = ts1 - ts0 + 1;

    double tsdxrcp = 1.0 / tsdx;

    ImVec2 mouse_pos = ImGui::GetMousePos();
    ImVec2 pos_min( FLT_MAX, FLT_MAX );
    ImVec2 pos_max( FLT_MIN, FLT_MIN );

    for ( const std::string &comm : m_graph_rows )
    {
        std::vector< uint32_t > *plocs = m_trace_events->get_comm_locs( comm.c_str() );
        if ( !plocs )
        {
            plocs = m_trace_events->get_event_locs( comm.c_str() );
            if ( !plocs )
                continue;
        }

        std::string label = string_format( "%s %lu events", comm.c_str(), plocs->size() );

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen;

        if ( ImGui::CollapsingHeader( label.c_str(), flags ) )
        {
            ImVec2 pos = ImGui::GetCursorScreenPos();

            pos_min.x = std::min( pos_min.x, pos.x );
            pos_min.y = std::min( pos_min.y, pos.y );
            pos_max.x = std::max( pos_max.x, pos.x + w );
            pos_max.y = std::max( pos_max.y, pos.y + h );

            ImGui::BeginChild( ( "g_" + label ).c_str(), ImVec2( w, h ) );

            // Draw background
            static const ImU32 col_background = IM_COL32( 255, 255, 255, 50 );
            ImGui::GetWindowDrawList()->AddRectFilled(
                        ImVec2( pos.x, pos.y ),
                        ImVec2( pos.x + w, pos.y + h ),
                        col_background );

            // Go through all event IDs for this process
            event_renderer_t event_renderer( pos.y, w, h, Hue_YlRd );
            for ( uint32_t id : *plocs )
            {
                if ( id > eventend )
                    break;
                if ( id < eventstart )
                    continue;

                trace_event_t &event = m_trace_events->m_events[ id ];
                float x = pos.x + w * ( event.ts - ts0 ) * tsdxrcp;

                event_renderer.add_event( x );
            }
            event_renderer.done();

            // Draw vblank events on every graph.
            std::vector< uint32_t > *vblank_locs = m_trace_events->get_event_locs( "drm_vblank_event" );
            if ( vblank_locs )
            {
                for ( uint32_t id : *vblank_locs )
                {
                    if ( id > eventend )
                        break;
                    if ( id < eventstart )
                        continue;

                    trace_event_t &event = m_trace_events->m_events[ id ];
                    float x = pos.x + w * ( event.ts - ts0 ) * tsdxrcp;

                    imgui_drawrect( x, 2.0f, pos.y, h, col_OrangeRed );
                }
            }

            // Draw time ticks every millisecond
            //$ TODO: Draw little ticks every quarter ms when zoomed in?
            unsigned long long msecs = ts0 / MSECS_PER_SEC;
            unsigned long long msec0 = msecs * MSECS_PER_SEC;

            for ( unsigned long long ts = msec0; ts <= ts1; ts += MSECS_PER_SEC )
            {
                float x = pos.x + w * ( ts - ts0 ) * tsdxrcp;

                imgui_drawrect( x, 2.0f, pos.y, h / 4, col_Lime );
            }

            // Draw location line for mouse if mouse is over graph
            if ( m_mouse_over_graph &&
                 mouse_pos.x >= pos.x &&
                 mouse_pos.x <= pos.x + w )
            {
                imgui_drawrect( mouse_pos.x, 2.0f, pos.y, h, col_DeepPink );
            }

            ImGui::EndChild();
        }
    }

    //$ TODO mikesart: click and drag to new zoom area

    // Check if the mouse is currently over our graph area
    m_mouse_over_graph =
        ( mouse_pos.x >= pos_min.x && mouse_pos.x <= pos_max.x &&
          mouse_pos.y >= pos_min.y && mouse_pos.y <= pos_max.y );

    if ( m_mouse_over_graph)
    {
        double x = ( mouse_pos.x - pos_min.x ) / w;
        unsigned long long event_ts = ts0 + tsdx * x;
        std::string time_buf = ts_to_timestr( event_ts, m_tsdelta );

        ImGui::SetTooltip( "%s", time_buf.c_str() );

        bool mouse_clicked = ImGui::IsMouseClicked( 0 );
        if ( mouse_clicked )
        {
            m_timegoto_buf = time_buf;
            m_do_gototime = true;
        }
    }
}

bool TraceWin::render_events()
{
    std::vector< trace_event_t > &events = m_trace_events->m_events;
    size_t event_count = events.size();

    ImGui::Begin( m_title.c_str(), &m_open );

    if ( ImGui::CollapsingHeader( "Options", ImGuiTreeNodeFlags_DefaultOpen ) )
    {
        if ( !render_options() )
        {
            ImGui::End();
            return false;
        }
    }

    if ( !m_inited )
    {
        // Initialize our graph rows first time through.
        init_graph_rows_str();
    }

    m_start_eventid = Clamp< int >( m_start_eventid, 0, event_count - 1 );
    m_end_eventid = Clamp< int >( m_end_eventid, m_start_eventid, event_count - 1 );

    m_goto_eventid = std::min< uint32_t >( m_goto_eventid, event_count - 1 );

    if ( ImGui::CollapsingHeader( "Process Graphs", ImGuiTreeNodeFlags_DefaultOpen ) )
    {
        bool graph_start = ImGui::Button( "Time Start:" );

        ImGui::SameLine();
        ImGui::PushItemWidth( 150 );
        m_graphtime_start.reserve( 32 );
        graph_start |= ImGui::InputText( "##GraphStart", &m_graphtime_start[ 0 ], m_graphtime_start.capacity(), 0, 0 );
        ImGui::PopItemWidth();

        ImGui::SameLine();
        bool graph_end = ImGui::Button( "Time Length:" );

        ImGui::SameLine();
        ImGui::PushItemWidth( 150 );
        m_graphtime_length.reserve( 32 );
        graph_end |= ImGui::InputText( "##GraphLength", &m_graphtime_length[ 0 ], m_graphtime_length.capacity(), 0, 0 );
        ImGui::PopItemWidth();

        if ( graph_start || !m_inited )
            m_graph_start_eventid = timestr_to_eventid( m_graphtime_start.c_str(), m_tsdelta );

        if ( graph_end || !m_inited )
        {
            unsigned long long ts = timestr_to_ts( m_graphtime_length.c_str() );

            m_graph_end_eventid = ts_to_eventid( events[ m_graph_start_eventid ].ts + ts );
        }

        ImGui::SameLine();
        bool do_graph_start = imgui_input_int( &m_graph_start_eventid, 75.0f, "Event Start:", "##GraphEventStart" );

        ImGui::SameLine();
        bool do_graph_end = imgui_input_int( &m_graph_end_eventid, 75.0f, "Event End:", "##GraphEventEnd" );

        m_graph_start_eventid = Clamp< int >( m_graph_start_eventid, m_start_eventid, m_end_eventid );
        m_graph_end_eventid = Clamp< int >( m_graph_end_eventid, m_graph_start_eventid, m_end_eventid );

        if ( do_graph_start )
            m_graphtime_start = ts_to_timestr( events[ m_graph_start_eventid ].ts, m_tsdelta );

        if ( do_graph_end )
        {
            unsigned long long ts = events[ m_graph_end_eventid ].ts - events[ m_graph_start_eventid ].ts;

            m_graphtime_length = TraceWin::ts_to_timestr( ts );
        }

        if ( ImGui::CollapsingHeader( "Graph Rows" ) )
        {
            if ( ImGui::Button( "Update Graph Rows" ) )
                update_graph_rows_list();

            ImGui::SameLine();
            if ( ImGui::Button( "Reset Graph Rows" ) )
                init_graph_rows_str();

            m_graph_rows_str.reserve( 8192 );
            ImGui::InputTextMultiline( "##GraphRows", &m_graph_rows_str[ 0 ], m_graph_rows_str.capacity(),
                    ImVec2( -1.0f, ImGui::GetTextLineHeight() * 16 ) );
        }

        render_process_graphs();
    }

    if ( ImGui::CollapsingHeader( "Events", ImGuiTreeNodeFlags_DefaultOpen ) )
    {
        render_events_list();
    }

    ImGui::End();

    m_inited = true;
    return m_open;
}

/*
 * TraceConsole
 */
void TraceConsole::init( CIniFile *inifile )
{
    m_clear_color = inifile->GetVec4( "clearcolor", ImColor( 114, 144, 154 ) );

    logf( "Welcome to gpuvis" );

    //$ TODO mikesart: use https://github.com/SirCmpwn/libccmd.git
    //$ TODO mikesart: add "load" command

    m_commands.insert( "clear" );
    m_commands.insert( "help" );
    m_commands.insert( "history" );
    m_commands.insert( "quit" );
    m_commands.insert( "q" );

    SDL_strlcpy( m_trace_file, "trace.dat", sizeof( m_trace_file ) );
}

void TraceConsole::shutdown( CIniFile *inifile )
{
    inifile->PutVec4( "clearcolor", m_clear_color );

    m_history.clear();
}

void TraceConsole::render( class TraceLoader *loader )
{
    ImGui::SetNextWindowSize( ImVec2( 720, 600 ), ImGuiSetCond_FirstUseEver );

    if ( !ImGui::Begin( "gpuvis console" ) )
    {
        ImGui::End();
        return;
    }

    ImGui::Text( "%.2f ms/frame (%.1f FPS)",
                 1000.0f / ImGui::GetIO().Framerate,
                 ImGui::GetIO().Framerate );

    if ( loader && ImGui::CollapsingHeader( "Trace File", ImGuiTreeNodeFlags_DefaultOpen ) )
    {
        bool is_loading = loader->is_loading();

        ImGui::Text( "File:" );
        ImGui::SameLine();

        ImVec4 &text_disabled = ImGui::GetStyle().Colors[ ImGuiCol_TextDisabled ];
        ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue;

        const char *button_text;
        if ( is_loading )
        {
            ImGui::PushStyleColor( ImGuiCol_Text, text_disabled );

            flags |= ImGuiInputTextFlags_ReadOnly;
            button_text = "Loading...";
        }
        else
        {
            button_text = "Load";
        }

        bool do_load = ImGui::InputText( "##load-trace-file", m_trace_file, sizeof( m_trace_file ), flags );

        ImGui::SameLine();
        do_load |= ImGui::Button( button_text, ImVec2( 0, 0 ) );

        if ( is_loading )
        {
            ImGui::PopStyleColor();
            ImGui::Text( "Trace events Loaded: %u", SDL_AtomicGet( &loader->m_trace_events->m_eventsloaded ) );
        }
        else if ( do_load )
        {
            loader->load_file( m_trace_file );
        }
    }

    if ( ImGui::CollapsingHeader( "Opened Event Files", ImGuiTreeNodeFlags_DefaultOpen ) )
    {
        ImGui::Columns( 2, "files" );

        ImGui::Separator();

        for ( size_t i = 0; i < loader->m_trace_events_list.size(); i++ )
        {
            TraceEvents *events = loader->m_trace_events_list[ i ];
            int eventsloaded = SDL_AtomicGet( &events->m_eventsloaded );

            if ( !eventsloaded )
            {
                ImGui::Text( "%s", events->m_title.c_str() );
                ImGui::NextColumn();

                if ( ImGui::SmallButton( string_format( "Events##%lu", i ).c_str() ) )
                    loader->new_event_window( events );

                ImGui::SameLine();
                if ( ImGui::SmallButton( string_format( "Close Windows##%lu", i ).c_str() ) )
                    loader->close_event_file( events, false );

                ImGui::SameLine();
                if ( ImGui::SmallButton( string_format( "Free##%lu", i ).c_str() ) )
                    loader->close_event_file( events, true );

                ImGui::NextColumn();
            }
        }

        ImGui::Columns( 1 );
        ImGui::Separator();
    }

    if ( ImGui::CollapsingHeader( "Options", ImGuiTreeNodeFlags_DefaultOpen ) )
    {
        ImGui::Text( "Clear Color:" );
        ImGui::SameLine();
        ImGui::ColorEdit3( "", ( float * )&m_clear_color );

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
    }

    if ( ImGui::CollapsingHeader( "Log", ImGuiTreeNodeFlags_DefaultOpen ) )
    {
        ImGui::Text( "Log Filter:" );
        ImGui::SameLine();
        ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 0, 0 ) );
        m_filter.Draw( "##log-filter", 180 );
        ImGui::PopStyleVar();

        ImGui::SameLine();
        if ( ImGui::SmallButton( "Clear" ) )
            logf_clear();

        ImGui::SameLine();
        if ( ImGui::SmallButton( "Scroll to bottom" ) )
            m_log_size = ( size_t )-1;

        ImGui::Separator();

        {
            ImGui::BeginChild( "ScrollingRegion",
                               ImVec2( 0, -ImGui::GetItemsLineHeightWithSpacing() ),
                               false, ImGuiWindowFlags_HorizontalScrollbar );

            // Log popup menu
            if ( ImGui::BeginPopupContextWindow() )
            {
                if ( ImGui::Selectable( "Clear" ) )
                    logf_clear();
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

            for ( const char *item : g_log )
            {
                if ( !m_filter.PassFilter( item ) )
                    continue;

                ImVec4 col = ImVec4( 1.0f, 1.0f, 1.0f, 1.0f );

                if ( !strncasecmp( item, "[error]", 7 ) )
                    col = ImColor( 1.0f, 0.4f, 0.4f, 1.0f );
                else if ( strncmp( item, "# ", 2 ) == 0 )
                    col = ImColor( 1.0f, 0.78f, 0.58f, 1.0f );

                ImGui::PushStyleColor( ImGuiCol_Text, col );
                ImGui::TextUnformatted( item );
                ImGui::PopStyleColor();
            }

            if ( m_log_size != g_log.size() )
            {
                ImGui::SetScrollHere();

                m_log_size = g_log.size();
            }

            ImGui::PopStyleVar();
            ImGui::EndChild();
        }

        ImGui::Separator();

        // Command-line
        ImGui::Text( "Command:" );

        ImGui::SameLine();
        m_inputbuf.reserve( 512 );
        if ( ImGui::InputText( "##log-command", &m_inputbuf[ 0 ], m_inputbuf.capacity(),
                               ImGuiInputTextFlags_EnterReturnsTrue |
                               ImGuiInputTextFlags_CallbackCompletion |
                               ImGuiInputTextFlags_CallbackHistory |
                               ImGuiInputTextFlags_CallbackCharFilter,
                               &text_edit_cb_stub, ( void * )this ) )
        {
            exec_command( m_inputbuf.c_str() );

            m_inputbuf = "";
        }

        // Keep auto focus on the input box
        if ( ImGui::IsItemHovered() ||
             ( ImGui::IsRootWindowOrAnyChildFocused() && !ImGui::IsAnyItemActive() && !ImGui::IsMouseClicked( 0 ) ) )
        {
            // Auto focus previous widget
            ImGui::SetKeyboardFocusHere( -1 );
        }
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

void TraceConsole::exec_command( const std::string &cmdlinein )
{
    std::string cmdline = string_trimmed( cmdlinein );

    if ( cmdline.empty() )
        return;

    logf( "# %s\n", cmdline.c_str() );

    // Insert into history. First find match and delete it so it can be pushed to the back. This isn't trying to be smart or optimal.
    m_history_pos = -1;

    for ( size_t i = 0; i < m_history.size(); i++ )
    {
        if ( cmdline == m_history[ i ] )
        {
            m_history.erase( m_history.begin() + i );
            break;
        }
    }
    m_history.push_back( cmdline );

    // Process command
    if ( cmdline == "clear" )
    {
        logf_clear();
    }
    else if ( ( cmdline == "quit" ) || ( cmdline == "q" ) )
    {
        m_quit = true;
    }
    else if ( cmdline == "help" )
    {
        logf( "Commands:" );

        for ( const std::string &cmd : m_commands )
            logf( "- %s", cmd.c_str() );
    }
    else if ( cmdline == "history" )
    {
        for ( size_t i = m_history.size() >= 20 ? m_history.size() - 20 : 0; i < m_history.size(); i++ )
            logf( "%3lu: %s\n", i, m_history[ i ].c_str() );
    }
    else
    {
        logf( "Unknown command: '%s'\n", cmdline.c_str() );
    }
}

int TraceConsole::text_edit_cb_completion( ImGuiTextEditCallbackData *data )
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

int TraceConsole::text_edit_cb_history( ImGuiTextEditCallbackData *data )
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

        SDL_strlcpy( data->Buf, str, data->BufSize );

        data->CursorPos = data->SelectionStart = data->SelectionEnd = data->BufTextLen = strlen( data->Buf );
        data->BufDirty = true;
    }

    return 0;
}

int TraceConsole::text_edit_cb_stub( ImGuiTextEditCallbackData *data )
{
    int ret = 0;
    TraceConsole *console = ( TraceConsole * )data->UserData;

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

struct cmdline_t
{
    std::vector< std::string > inputfiles;
};

static void parse_cmdline( cmdline_t &cmdline, int argc, char **argv )
{
    int c;
    while ( ( c = getopt( argc, argv, "i:" ) ) != -1 )
    {
        switch(c)
        {
        case 'i':
            cmdline.inputfiles.push_back( optarg );
            break;

        default:
            break;
        }
    }

    for ( ; optind < argc; optind++ )
    {
        cmdline.inputfiles.push_back( argv[ optind ] );
    }
}

static bool load_trace_file( TraceLoader &loader, TraceConsole &console, const char *filename )
{
    SDL_strlcpy( console.m_trace_file, filename, sizeof( console.m_trace_file ) );

    return loader.load_file( filename );
}

#if SDL_VERSIONNUM(SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_PATCHLEVEL) < SDL_VERSIONNUM(2, 0, 5)
int SDL_GetWindowBordersSize(SDL_Window * window, int *top, int *left, int *bottom, int *right)
{
    *top = 0;
    *left = 0;
    *bottom = 0;
    *right = 0;

    return -1;
}
#endif

int main( int argc, char **argv )
{
    CIniFile inifile;
    TraceConsole console;
    TraceLoader loader;
    SDL_Window *window = NULL;
    cmdline_t cmdline;

    parse_cmdline( cmdline, argc, argv );

    // Setup SDL
    if ( SDL_Init( SDL_INIT_VIDEO | SDL_INIT_TIMER ) != 0 )
    {
        fprintf( stderr, "Error. SDL_Init failed: %s\n", SDL_GetError() );
        return -1;
    }

    logf_init();

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

        // Check for logf() calls from background threads.
        logf_update();

        // Render console / options window
        console.render( &loader );

        // Render trace windows
        loader.render();

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

        if ( !cmdline.inputfiles.empty() && !loader.is_loading() )
        {
            const char *filename = cmdline.inputfiles[ 0 ].c_str();

            load_trace_file( loader, console, filename );

            cmdline.inputfiles.erase( cmdline.inputfiles.begin() );
        }
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

    // Shut down our trace loader
    loader.shutdown();

    // Shut down our console / option window
    console.shutdown( &inifile );

    logf_clear();

    // Cleanup
    logf_shutdown();

    ImGui_ImplSdlGL3_Shutdown();
    SDL_GL_DeleteContext( glcontext );
    SDL_DestroyWindow( window );
    SDL_Quit();

    inifile.Close();
    return 0;
}
