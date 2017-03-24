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

//$ TODO: Right click on events - popup menu
//    start graph at a specific location
//    find event in graph

// popup graph tooltip shows events around location you're at?

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

ImU32 col_w_alpha( ImU32 col, ImU32 alpha )
{
    return ( col & ~IM_COL32_A_MASK ) | IM_COL32( 0, 0, 0, alpha );
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

static bool imgui_input_int( int *val, float w, const char *label, const char *label2, ImGuiInputTextFlags flags = 0 )
{
    bool ret = ImGui::Button( label );
    float scale = ImGui::GetIO().FontGlobalScale;

    ImGui::SameLine();
    ImGui::PushItemWidth( w * scale );
    ret |= ImGui::InputInt( label2, val, 0, 0, flags );
    ImGui::PopItemWidth();

    return ret;
}

static bool imgui_input_text( const char *button_label, const char *text_label,
                       std::string &str, size_t capacity, float w )
{
    bool ret = ImGui::Button( button_label );

    ImGui::SameLine();
    ImGui::PushItemWidth( w );
    str.reserve( capacity );
    ret |= ImGui::InputText( text_label, &str[ 0 ], str.capacity(), 0, 0 );
    ImGui::PopItemWidth();

    return ret;
}

static bool imgui_key_pressed( ImGuiKey key )
{
    return ImGui::IsKeyPressed( ImGui::GetKeyIndex( key ) );
}

static void imgui_draw_text( float x, float y, const char *text, ImU32 color )
{
    ImVec2 textsize = ImGui::CalcTextSize( text );

    ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2( x, y ), ImVec2( x + textsize.x, y + textsize.y ),
                col_w_alpha( col_Black, 150 ) );

    ImGui::GetWindowDrawList()->AddText( ImVec2( x, y ), color, text );
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

    if ( event.cpu < trace_events->m_cpucount.size() )
        trace_events->m_cpucount[ event.cpu ]++;

    if ( id == 0 )
        trace_events->m_ts_min = event.ts;

    trace_events->m_events.push_back( event );
    trace_events->m_events[ id ].id = id;
    trace_events->m_events[ id ].ts -= trace_events->m_ts_min;

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
 * TraceWin
 */
void TraceWin::render_time_offset_button_init( TraceEvents &trace_events )
{
    int64_t ts = 0;
    std::vector< trace_event_t > &events = trace_events.m_events;
    const std::vector< uint32_t > &vblank_locs = trace_events.get_event_locs( "drm_vblank_event" );

    for ( uint32_t i : vblank_locs )
    {
        if ( !ts || events[ i ].pid )
        {
            m_do_gotoevent = true;
            m_goto_eventid = i;

            ts = events[ i ].ts;

            if ( events[ i ].pid )
                break;
        }
    }

    m_tsoffset = ts;
    m_timeoffset_buf = ts_to_timestr( ts );
}

int64_t TraceWin::timestr_to_ts( const char *buf, int64_t tsoffset )
{
    double val;

    if ( sscanf( buf, "%lf", &val ) != 1 )
        val = 0.0;

    return tsoffset + ( int64_t )( val * MSECS_PER_SEC );
}

std::string TraceWin::ts_to_timestr( int64_t event_ts, int64_t tsoffset, int precision )
{
    double val = ( event_ts - tsoffset ) * ( 1.0 / MSECS_PER_SEC );

    return string_format( "%.*lf", precision, val );
}

void TraceWin::init_graph_rows_str()
{
    m_graph_rows_str = "# comm and event names to graph\n\n";
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

int TraceWin::ts_to_eventid( int64_t ts )
{
    // When running a debug build w/ ASAN on, the lower_bound function is
    //  horribly slow so we cache the timestamp to event ids.
    auto i = m_ts_to_eventid_cache.find( ts );
    if ( i != m_ts_to_eventid_cache.end() )
        return m_ts_to_eventid_cache.at( ts );

    trace_event_t x;
    std::vector< trace_event_t > &events = m_trace_events->m_events;

    x.ts = ts;

    auto eventidx = std::lower_bound( events.begin(), events.end(), x,
        []( const trace_event_t &f1, const trace_event_t &f2 ){ return f1.ts < f2.ts; } );

    int id = eventidx - events.begin();

    if ( ( size_t )id >= events.size() )
        id = events.size() - 1;

    m_ts_to_eventid_cache[ ts ] = id;
    return id;
}

int TraceWin::timestr_to_eventid( const char *buf, int64_t tsoffset )
{
    int64_t ts = timestr_to_ts( buf, tsoffset );

    return ts_to_eventid( ts );
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

    ImGui::Begin( m_title.c_str(), &m_open );

    if ( ImGui::CollapsingHeader( "Trace Info" ) )
    {
        if ( !render_info() )
        {
            ImGui::End();
            return false;
        }
    }

    if ( !m_inited )
    {
        // Initialize our graph rows first time through.
        init_graph_rows_str();

        render_time_offset_button_init( *m_trace_events );
    }

    // Sanity check these aren't out of range.
    size_t event_count = m_trace_events->m_events.size();
    m_start_eventid = Clamp< int >( m_start_eventid, 0, event_count - 1 );
    m_end_eventid = Clamp< int >( m_end_eventid, m_start_eventid, event_count - 1 );

    if ( ImGui::CollapsingHeader( "Events Graph", ImGuiTreeNodeFlags_DefaultOpen ) )
    {
        if ( imgui_input_text( "Start:", "##GraphStart", m_graphtime_start_buf, 32, 150 ) )
            m_graph_start_ts = timestr_to_ts( m_graphtime_start_buf.c_str() );

        ImGui::SameLine();
        if ( imgui_input_text( "Length:", "##GraphLength", m_graphtime_length_buf, 32, 150 ) )
            m_graph_length_ts = timestr_to_ts( m_graphtime_length_buf.c_str() );

        ImGui::SameLine();
        bool zoom_in = ImGui::SmallButton( "Zoom In" );
        ImGui::SameLine();
        bool zoom_out = ImGui::SmallButton( "Zoom Out" );
        if ( zoom_in || zoom_out )
        {
            int64_t sign = zoom_in ? -1 : +1;
            int64_t amt = 1000 * sign * ( m_graph_length_ts / 2000 );

            m_graph_start_ts -= amt / 2;
            m_graph_length_ts += amt;
            m_do_graph_start_ts = true;
            m_do_graph_length_ts = true;
        }

        if ( m_do_graph_start_ts )
            m_graphtime_start_buf = ts_to_timestr( m_graph_start_ts, 0, 4 );
        if ( m_do_graph_length_ts )
            m_graphtime_length_buf = ts_to_timestr( m_graph_length_ts, 0, 4 );

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

    if ( ImGui::CollapsingHeader( "Events List", ImGuiTreeNodeFlags_DefaultOpen ) )
    {
        bool update_eventids = imgui_input_int( &m_start_eventid, 75.0f,
                "Event Start:", "##EventStart", ImGuiInputTextFlags_EnterReturnsTrue );

        ImGui::SameLine();
        update_eventids |= imgui_input_int( &m_end_eventid, 75.0f,
                "Event End:", "##EventEnd", ImGuiInputTextFlags_EnterReturnsTrue );

        if ( update_eventids )
        {
            m_start_eventid = Clamp< int >( m_start_eventid, 0, event_count - 1 );
            m_end_eventid = Clamp< int >( m_end_eventid, m_start_eventid, event_count - 1 );
        }

        ImGui::SameLine();
        if ( imgui_input_text( "Time Offset:", "##TimeOffset", m_timeoffset_buf, 32, 150 ) )
            m_tsoffset = timestr_to_ts( m_timeoffset_buf.c_str() );

        m_do_gotoevent |= imgui_input_int( &m_goto_eventid, 75.0f, "Goto Event:", "##GotoEvent" );

        ImGui::SameLine();
        if ( imgui_input_text( "Goto Time:", "##GotoTime", m_timegoto_buf, 32, 150 ) )
        {
            m_do_gotoevent = true;
            m_goto_eventid = timestr_to_eventid( m_timegoto_buf.c_str(), m_tsoffset );
        }

        render_events_list( loader->m_inifile );
    }

    ImGui::End();

    m_inited = true;
    return m_open;
}

bool TraceWin::render_info()
{
    size_t event_count = m_trace_events->m_events.size();
    ImGui::Text( "Events: %lu\n", event_count );

    if ( !event_count )
        return false;

    trace_info_t& trace_info = m_trace_events->m_trace_info;
    ImGui::Text( "Trace cpus: %u", trace_info.cpus );

    if ( !trace_info.uname.empty() )
        ImGui::Text( "Trace uname: %s", trace_info.uname.c_str() );

#if 0
    //$ TODO mikesart: figure out best way to display this info
    if ( ImGui::CollapsingHeader( "CPU Stats" ) )
    {
        static std::string blah;
        for ( const std::string &str : trace_info.cpustats )
            blah += str;
        ImGui::InputTextMultiline( "##CpuStats", &blah[ 0 ], blah.capacity(), ImVec2( 0, 0 ), ImGuiInputTextFlags_ReadOnly );
    }
#endif

    return true;
}

//$ TODO mikesart: Temporary popup menu
bool TraceWin::render_events_list_popup()
{
    if ( !ImGui::BeginPopup( "EventsListPopup" ) )
        return false;

    const char* names[] = { "Bream", "Haddock", "Mackerel", "Pollock", "Tilefish" };
    static bool toggles[] = { true, false, false, false, false };

    for (int i = 0; i < 5; i++)
        ImGui::MenuItem(names[i], "", &toggles[i]);

    if (ImGui::BeginMenu("Sub-menu"))
    {
        ImGui::MenuItem("Click me");
        ImGui::EndMenu();
    }

    ImGui::Separator();
    ImGui::Text("Tooltip here");

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("I am a tooltip over a popup");

    if (ImGui::Button("Stacked Popup"))
        ImGui::OpenPopup("another popup");

    if (ImGui::BeginPopup("another popup"))
    {
        for (int i = 0; i < 5; i++)
            ImGui::MenuItem(names[i], "", &toggles[i]);

        if (ImGui::BeginMenu("Sub-menu"))
        {
            ImGui::MenuItem("Click me");
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }
    ImGui::EndPopup();

    return true;
}

std::string get_event_field_str( std::vector< event_field_t > &fields, const char *eqstr, char sep )
{
    std::string fieldstr;

    for ( const event_field_t &field : fields )
    {
        fieldstr += string_format( "%s%s%s%c", field.key, eqstr, field.value, sep );
    }

    return fieldstr;
}

void TraceWin::render_events_list( CIniFile &inifile )
{
    float scale = ImGui::GetIO().FontGlobalScale;
    size_t event_count = m_end_eventid - m_start_eventid + 1;
    std::vector< trace_event_t > &events = m_trace_events->m_events;

    // Set focus on event list first time we open.
    if ( !m_inited && ImGui::IsWindowFocused() )
        ImGui::SetNextWindowFocus();

    // Events list
    ImVec2 avail = ImGui::GetContentRegionAvail();

    ImFontAtlas *atlas = ImGui::GetIO().Fonts;
    if ( atlas->Fonts.Size > 1 )
        ImGui::PushFont( atlas->Fonts[ 1 ] );

    {
        // Set the child window size to hold count of items + header + separator
        float lineh = ImGui::GetTextLineHeightWithSpacing();
        float y = ( avail.y < 384.0f * scale ) ? 384.0f * scale : 0.0f;

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
            m_goto_eventid = std::min< uint32_t >( m_goto_eventid, event_count - 1 );
            ImGui::SetScrollY( std::max< int >( 0, m_goto_eventid - m_start_eventid - 6 ) * lineh );

            m_do_gotoevent = false;
        }

        float scrolly = ImGui::GetScrollY();
        uint32_t start_idx = ( scrolly >= lineh ) ? ( uint32_t )( scrolly / lineh - 1 ) : 0;
        uint32_t rows = ( winh + 1 ) / lineh;
        uint32_t end_idx = std::min< uint32_t >( start_idx + 2 + rows, event_count );

        // Draw columns
        std::array< const char *, 6 > columns = { "Id", "Time Stamp", "Task", "Event", "seqno", "Info" };
        ImGui::Columns( columns.size(), "events" );
        for ( const char *str : columns )
        {
            ImGui::TextColored( ImVec4( 1, 1, 0, 1 ), "%s", str );
            ImGui::NextColumn();
        }
        ImGui::Separator();

        if ( !m_inited )
        {
            // Try to restore the column sizes from our ini file.
            for ( size_t i = 1; i < columns.size(); i++ )
            {
                float val = inifile.GetFloat( string_format( "column_offset%lu", i ).c_str(), -1.0f );
                if ( val <= 0.0f )
                    break;

                ImGui::SetColumnOffset( i, val );
            }
        }
        else if ( ImGui::IsWindowHovered() && ImGui::IsMouseReleased( 0 ) )
        {
            // Someone release the mouse - save column sizes in case they were changed.
            for ( size_t i = 1; i < columns.size(); i++ )
            {
                inifile.PutFloat( string_format( "column_offset%lu", i ).c_str(),
                                  ImGui::GetColumnOffset( i ) );
            }
        }

        if ( start_idx > 0 )
        {
            // Move cursor position down to where we've scrolled.
            ImGui::SetCursorPosY( ImGui::GetCursorPosY() + lineh * ( start_idx - 1 ) );

            // Scoot to next row (fixes top row occasionally drawing half).
            for ( size_t i = 0; i < columns.size(); i++ )
                ImGui::NextColumn();
        }

        // Reset our hovered event id
        m_hovered_eventid = ( uint32_t )-1;

        // Draw events
        for ( uint32_t i = start_idx; i < end_idx; i++ )
        {
            char label[ 32 ];
            int colors_pushed = 0;
            trace_event_t &event = events[ m_start_eventid + i ];
            bool selected = ( m_selected_eventid == event.id );
            bool is_vblank = !strcmp( event.name, "drm_vblank_event" );
            std::string ts_str = ts_to_timestr( event.ts, m_tsoffset );

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
                m_selected_eventid = event.id;

            // Check if item is hovered and we don't have a popup menu up.
            if ( ( m_events_list_popup_eventid == ( uint32_t )-1 ) &&
                 ImGui::IsItemHovered() )
            {
                // Store the hovered event id.
                m_hovered_eventid = event.id;

                if ( ImGui::IsMouseClicked( 1 ) )
                {
                    // If they right clicked, show the context menu.
                    m_events_list_popup_eventid = i;
                    ImGui::OpenPopup("EventsListPopup");
                }
                else
                {
                    // Otherwise show a tooltop.
                    std::string fieldstr = get_event_field_str( event.fields, ": ", '\n' );

                    ImGui::SetTooltip( "Id: %u\nTime: %s\nComm: %s\n%s",
                                        event.id, ts_str.c_str(), event.comm, fieldstr.c_str() );
                }
            }

            // If we've got an active popup menu, render it.
            if ( m_events_list_popup_eventid == i )
            {
                if ( !TraceWin::render_events_list_popup() )
                    m_events_list_popup_eventid = ( uint32_t )-1;
            }

            ImGui::NextColumn();

            ImGui::Text( "%s", ts_str.c_str() );
            ImGui::NextColumn();
            ImGui::Text( "%s (%u)", event.comm, event.cpu );
            ImGui::NextColumn();
            ImGui::Text( "%s", event.name );
            ImGui::NextColumn();

            if ( event.seqno )
                ImGui::Text( "%u", event.seqno );
            ImGui::NextColumn();

            std::string fieldstr = get_event_field_str( event.fields, "=", ' ' );
            ImGui::Text( "%s", fieldstr.c_str() );
            ImGui::NextColumn();

            ImGui::PopStyleColor( colors_pushed );
        }

        ImGui::Columns( 1 );
        ImGui::EndChild();
    }

    if ( atlas->Fonts.Size > 1 )
        ImGui::PopFont();
}

static void imgui_drawrect( float x, float w, float y, float h, ImU32 color )
{
    if ( w < 0.0f )
    {
        x += w;
        w = -w;
    }

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

        hue = hue_in;

        start( -1.0f );
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
        float crowding = num_events / floor( x1 - x0 + 1.0f );

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

class graph_info_t
{
public:
    void init( int64_t start_ts, int64_t length_ts )
    {
        ts0 = start_ts;
        ts1 = start_ts + length_ts;

        tsdx = ts1 - ts0 + 1;
        tsdxrcp = 1.0 / tsdx;

        scale = ImGui::GetIO().FontGlobalScale;

        mouse_pos = ImGui::GetMousePos();
    }

    void set_cursor_screen_pos( const ImVec2 &posin, const ImVec2 &size )
    {
        pos = posin;
        w = size.x;
        h = size.y;

        pos_min.x = std::min( pos_min.x, pos.x );
        pos_min.y = std::min( pos_min.y, pos.y );
        pos_max.x = std::max( pos_max.x, pos.x + w );
        pos_max.y = std::max( pos_max.y, pos.y + h );
    }

    float ts_to_x( int64_t ts )
    {
        return w * ( ts - ts0 ) * tsdxrcp;
    }

    float ts_to_screenx( int64_t ts )
    {
        return pos.x + ts_to_x( ts );
    }

    int64_t screenx_to_event_ts( float x )
    {
        double val = ( x - pos_min.x ) / w;

        return ts0 + val * tsdx;
    }
    int64_t dx_to_event_ts( float x )
    {
        return ( x / w ) * tsdx;
    }

    bool pt_in_graph( const ImVec2& posin )
    {
        return ( posin.x >= pos_min.x && posin.x <= pos_max.x &&
                 posin.y >= pos_min.y && posin.y <= pos_max.y );
    }

    bool mouse_pos_in_graph()
    {
        return pt_in_graph( mouse_pos );
    }

public:
    ImVec2 pos;

    float scale;
    float h;
    float w;

    int64_t ts0;
    int64_t ts1;
    int64_t tsdx;
    double tsdxrcp;

    ImVec2 mouse_pos;
    ImVec2 pos_min{ FLT_MAX, FLT_MAX };
    ImVec2 pos_max{ FLT_MIN, FLT_MIN };

    uint32_t eventstart;
    uint32_t eventend;
};

void TraceWin::render_graph_row( const std::string &comm, std::vector< uint32_t > &locs, class graph_info_t *pgi )
{
    graph_info_t &gi = *pgi;

    // Draw background
    ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2( gi.pos.x, gi.pos.y ),
                ImVec2( gi.pos.x + gi.w, gi.pos.y + gi.h ),
                col_DarkSlateGray );

    // Go through all event IDs for this process
    uint32_t num_events = 0;
    bool draw_selected_event = false;
    bool draw_hovered_event = false;
    event_renderer_t event_renderer( gi.pos.y, gi.w, gi.h, Hue_YlRd );

    for ( size_t idx = vec_find_eventid( locs, gi.eventstart );
          idx < locs.size();
          idx++ )
    {
        uint32_t eventid = locs[ idx ];

        if ( eventid > gi.eventend )
            break;

        if ( eventid == m_hovered_eventid )
            draw_hovered_event = true;
        else if ( eventid == m_selected_eventid )
            draw_selected_event = true;

        num_events++;
        trace_event_t &event = m_trace_events->m_events[ eventid ];
        float x = gi.ts_to_screenx( event.ts );

        event_renderer.add_event( x );
    }
    event_renderer.done();

    if ( draw_hovered_event )
    {
        trace_event_t &event = m_trace_events->m_events[ m_hovered_eventid ];
        float x = gi.ts_to_screenx( event.ts );

        imgui_drawrect( x, 3.0f, gi.pos.y, gi.h, col_Maroon );
    }
    if ( draw_selected_event )
    {
        trace_event_t &event = m_trace_events->m_events[ m_selected_eventid ];
        float x = gi.ts_to_screenx( event.ts );

        imgui_drawrect( x, 3.0f, gi.pos.y, gi.h, col_Indigo );
    }

    float x = gi.pos.x + ImGui::GetStyle().FramePadding.x;
    std::string label = string_format( "%u events", num_events );

    imgui_draw_text( x, gi.pos.y, comm.c_str(), col_LightYellow );
    imgui_draw_text( x, gi.pos.y + ImGui::GetTextLineHeight(), label.c_str(), col_LightYellow );
}

void TraceWin::render_graph_vblanks( class graph_info_t *pgi )
{
    graph_info_t &gi = *pgi;

    // Draw time ticks every millisecond
    int64_t tsstart = std::max< int64_t >( gi.ts0 / MSECS_PER_SEC - 1, 0 ) * MSECS_PER_SEC;
    float x0 = gi.ts_to_x( tsstart );
    float dx = gi.w * MSECS_PER_SEC * gi.tsdxrcp;

    for ( ; x0 <= gi.w; x0 += dx )
    {
        imgui_drawrect( gi.pos.x + x0, 1.0f, gi.pos.y, 16.0f, col_Lime );

        if ( dx >= 35.0f )
        {
            for ( int i = 1; i < 4; i++ )
                imgui_drawrect( gi.pos.x + x0 + i * dx / 4, 1.0f, gi.pos.y, 4.0f, col_Lime );
        }
    }

    // Draw vblank events on every graph.
    const std::vector< uint32_t > &vblank_locs = m_trace_events->get_event_locs( "drm_vblank_event" );

    for ( size_t idx = vec_find_eventid( vblank_locs, gi.eventstart );
          idx < vblank_locs.size();
          idx++ )
    {
        uint32_t id = vblank_locs[ idx ];

        if ( id > gi.eventend )
            break;

        trace_event_t &event = m_trace_events->m_events[ id ];
        float x = gi.ts_to_screenx( event.ts );

        imgui_drawrect( x, 1.0f, gi.pos.y, gi.h, col_OrangeRed );
    }

    // Draw location line for mouse if mouse is over graph
    if ( m_mouse_over_graph &&
         gi.mouse_pos.x >= gi.pos.x &&
         gi.mouse_pos.x <= gi.pos.x + gi.w )
    {
        imgui_drawrect( gi.mouse_pos.x, 2.0f, gi.pos.y, gi.h, col_DeepPink );
    }

    // Draw mouse selection location
    if ( m_mouse_captured == 1 )
    {
        float mousex0 = m_mouse_capture_pos.x;
        float mousex1 = gi.mouse_pos.x;
        ImU32 col = col_w_alpha( col_White, 80 );

        imgui_drawrect( mousex0, mousex1 - mousex0, gi.pos.y, gi.h, col );
    }
}

void TraceWin::render_process_graphs()
{
    graph_info_t gi;

    if ( m_graph_length_ts < 100 )
    {
        m_graph_length_ts = 100;
        m_do_graph_length_ts = true;
    }

    gi.init( m_graph_start_ts + m_tsoffset, m_graph_length_ts );
    gi.eventstart = std::max( ts_to_eventid( gi.ts0 ), m_start_eventid );
    gi.eventend = std::min( ts_to_eventid( gi.ts1 ), m_end_eventid );

    uint32_t graph_rows = 0;
    for ( const std::string &comm : m_graph_rows )
    {
        std::vector< uint32_t > &locs = m_trace_events->get_comm_locs( comm.c_str() );

        if ( locs.empty() )
        {
            locs = m_trace_events->get_event_locs( comm.c_str() );
            if ( locs.empty() )
                continue;
        }

        graph_rows++;
    }

    {
        const float graph_row_h = 50.0f * gi.scale;
        const float graph_padding = ImGui::GetStyle().FramePadding.y;
        const float graph_height = graph_rows * ( graph_row_h + graph_padding );

        ImGui::BeginChild( "EventGraph", ImVec2( 0, graph_height ), true );

        ImVec2 windowpos = ImGui::GetWindowPos();
        ImVec2 windowsize = ImGui::GetWindowSize();

        windowpos.y += graph_padding;
        windowsize.y -= 2 * graph_padding;

        // Draw graph background
        ImGui::GetWindowDrawList()->AddRectFilled(
                    ImVec2( windowpos.x, windowpos.y ),
                    ImVec2( windowpos.x + windowsize.x, windowpos.y + windowsize.y ),
                    col_Black );

        gi.set_cursor_screen_pos( ImVec2( windowpos.x, windowpos.y ),
                                  ImVec2( windowsize.x, graph_row_h ) );
        for ( const std::string &comm : m_graph_rows )
        {
            std::vector< uint32_t > &locs = m_trace_events->get_comm_locs( comm.c_str() );

            if ( locs.empty() )
            {
                locs = m_trace_events->get_event_locs( comm.c_str() );
                if ( locs.empty() )
                    continue;
            }

            render_graph_row( comm, locs, &gi );

            gi.set_cursor_screen_pos( ImVec2( gi.pos.x, gi.pos.y + graph_row_h + graph_padding ),
                                      ImVec2( gi.w, gi.h ) );
        }

        // Render full graph lines: vblanks, mouse cursors, etc...
        gi.set_cursor_screen_pos( ImVec2( windowpos.x, windowpos.y ),
                                  ImVec2( windowsize.x, windowsize.y ) );
        render_graph_vblanks( &gi );

        ImGui::EndChild();
    }

    render_mouse_graph( &gi );
}

void TraceWin::render_mouse_graph( class graph_info_t *pgi )
{
    graph_info_t &gi = *pgi;

    // Check if the mouse is currently over our graph area

    //$ TODO: this needs to check that we're the active window also?
    // still works when the debug test window is on top of us.
    m_mouse_over_graph = gi.mouse_pos_in_graph();

    if ( m_mouse_captured && imgui_key_pressed( ImGuiKey_Escape ) )
    {
        m_mouse_captured = 0;
        ImGui::CaptureMouseFromApp( false );
    }

    if ( m_mouse_captured == 1 )
    {
        // shift + click: zoom
        int64_t event_ts0 = gi.screenx_to_event_ts( m_mouse_capture_pos.x );
        int64_t event_ts1 = gi.screenx_to_event_ts( gi.mouse_pos.x );

        if ( event_ts0 > event_ts1 )
            std::swap( event_ts0, event_ts1 );

        if ( ImGui::IsMouseDown( 0 ) )
        {
            std::string time_buf0 = ts_to_timestr( event_ts0, m_tsoffset );
            std::string time_buf1 = ts_to_timestr( event_ts1 - event_ts0 );

            ImGui::SetTooltip( "%s (%s ms)", time_buf0.c_str(), time_buf1.c_str() );
        }
        else
        {
            m_mouse_captured = 0;
            ImGui::CaptureMouseFromApp( false );

            m_graph_location_stack.push_back( { m_graph_start_ts, m_graph_length_ts } );
            if ( m_graph_location_stack.size() > 64 )
                m_graph_location_stack.erase( m_graph_location_stack.begin() );

            m_graph_start_ts = event_ts0 - m_tsoffset;
            m_graph_length_ts = event_ts1 - event_ts0;
            m_do_graph_start_ts = true;
            m_do_graph_length_ts = true;
        }
    }
    else if ( m_mouse_captured == 2 )
    {
        // ctrl + click: pan
        if ( ImGui::IsMouseDown( 0 ) )
        {
            float dx = gi.mouse_pos.x - m_mouse_capture_pos.x;
            int64_t tsdiff = gi.dx_to_event_ts( dx );

            m_graph_start_ts -= tsdiff;
            m_do_graph_start_ts = true;

            m_mouse_capture_pos = gi.mouse_pos;
        }
        else
        {
            m_mouse_captured = 0;
            ImGui::CaptureMouseFromApp( false );
        }
    }
    else if ( m_mouse_over_graph && !m_mouse_captured )
    {
        bool mouse_clicked = ImGui::IsMouseClicked( 0 );
        int64_t event_ts = gi.screenx_to_event_ts( gi.mouse_pos.x );
        std::string time_buf = ts_to_timestr( event_ts, m_tsoffset );

        ImGui::SetTooltip( "%s", time_buf.c_str() );

        if ( mouse_clicked && ImGui::GetIO().KeyShift )
        {
            m_mouse_captured = 1;
            ImGui::CaptureMouseFromApp( true );
            m_mouse_capture_pos = gi.mouse_pos;
        }
        else if ( mouse_clicked && ImGui::GetIO().KeyCtrl )
        {
            m_mouse_captured = 2;
            ImGui::CaptureMouseFromApp( true );
            m_mouse_capture_pos = gi.mouse_pos;
        }
        else if ( ImGui::IsMouseClicked( 1 ) && !m_graph_location_stack.empty() )
        {
            // Right click restores previous graph location
            std::pair< int64_t, int64_t > &locs = m_graph_location_stack.back();

            m_graph_start_ts = locs.first;
            m_graph_length_ts = locs.second;

            m_do_graph_start_ts = true;
            m_do_graph_length_ts = true;

            m_graph_location_stack.pop_back();
        }
        else if ( ImGui::IsMouseDoubleClicked( 0 ) )
        {
            // Double click moves event log to time.
            m_goto_eventid = timestr_to_eventid( time_buf.c_str(), m_tsoffset );
            m_selected_eventid = m_goto_eventid;
            m_do_gotoevent = true;
        }
    }
}

/*
 * TraceConsole
 */
void TraceConsole::init( CIniFile *inifile )
{
    m_clear_color = inifile->GetVec4( "clearcolor", ImColor( 114, 144, 154 ) );

    logf( "Welcome to gpuvis\n" );

    logf( "graph shortcuts:" );
    logf( "  double click: go to location in event list" );
    logf( "  shift+click+drag: zoom to selection" );
    logf( "  right click: restore previous location before zoom" );
    logf( "  ctrl+click+drag: pan graph" );

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

static void imgui_load_fonts()
{
    ImGuiIO& io = ImGui::GetIO();

    io.Fonts->AddFontDefault();

    std::array< const char *, 3 > fontpaths =
    {
        "./fonts/ProggyTiny.ttf",
        "../fonts/ProggyTiny.ttf",
        "./ProggyTiny.ttf"
    };
    for ( const char *fontname : fontpaths )
    {
        if ( io.Fonts->AddFontFromFileTTF( fontname, 10.0f ) )
        {
            logf( "Loaded font: %s", fontname );
            break;
        }
    }
}

int main( int argc, char **argv )
{
    CIniFile inifile;
    TraceConsole console;
    TraceLoader loader( inifile );
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

    ImGuiIO& io = ImGui::GetIO();
    io.IniLoadSettingCB = std::bind( imgui_ini_load_settings_cb, &inifile, _1, _2 );
    io.IniSaveSettingCB = std::bind( imgui_ini_save_settings_cb, &inifile, _1, _2 );

    int x = inifile.GetInt( "win_x", SDL_WINDOWPOS_CENTERED );
    int y = inifile.GetInt( "win_y", SDL_WINDOWPOS_CENTERED );
    int w = inifile.GetInt( "win_w", 1280 );
    int h = inifile.GetInt( "win_h", 1024 );
    io.FontGlobalScale = inifile.GetFloat( "win_scale", 1.0f );

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

    imgui_load_fonts();

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
