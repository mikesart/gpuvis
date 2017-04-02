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

#include <array>

#include "imgui/imgui.h"
#include "gpuvis_colors.h"

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

static const struct
{
    const char *name;
    ImU32 color;
} g_colordata[] =
{
#define _XTAG( _name, _color ) { #_name, _color },
#include "gpuvis_colors.inl"
#undef _XTAG
};

ImU32 col_get( colors_t col, ImU32 alpha )
{
    ImU32 color = g_colordata[ col ].color;

    return color | ( alpha << IM_COL32_A_SHIFT );
}

const char *col_get_name( colors_t col )
{
    return g_colordata[ col ].name;
}
