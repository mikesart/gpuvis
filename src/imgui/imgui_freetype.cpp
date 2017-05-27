/*
 * From:
 *   https://github.com/Vuhdo/imgui_freetype.git
 */
#ifdef USE_FREETYPE

#include <ft2build.h>
#include <math.h>
#include <stdint.h>
#include <vector>

#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_SYNTHESIS_H

#include "imgui.h"
#include "imgui_freetype.h"

#ifdef __GNUC__
// Disable unused function warnings in stb_rect_packed.h
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

#define IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_PLACEMENT_NEW
#include "imgui_internal.h"

#define STBRP_ASSERT( x ) IM_ASSERT( x )
#define STBRP_STATIC
#define STB_RECT_PACK_IMPLEMENTATION
#include "stb_rect_pack.h"

// Glyph metrics:
// --------------
//
//                       xmin                     xmax
//                        |                         |
//                        |<-------- width -------->|
//                        |                         |
//              |         +-------------------------+----------------- ymax
//              |         |    ggggggggg   ggggg    |     ^        ^
//              |         |   g:::::::::ggg::::g    |     |        |
//              |         |  g:::::::::::::::::g    |     |        |
//              |         | g::::::ggggg::::::gg    |     |        |
//              |         | g:::::g     g:::::g     |     |        |
//    offsetX  -|-------->| g:::::g     g:::::g     |  offsetY     |
//              |         | g:::::g     g:::::g     |     |        |
//              |         | g::::::g    g:::::g     |     |        |
//              |         | g:::::::ggggg:::::g     |     |        |
//              |         |  g::::::::::::::::g     |     |      height
//              |         |   gg::::::::::::::g     |     |        |
//  baseline ---*---------|---- gggggggg::::::g-----*--------      |
//            / |         |             g:::::g     |              |
//     origin   |         | gggggg      g:::::g     |              |
//              |         | g:::::gg   gg:::::g     |              |
//              |         |  g::::::ggg:::::::g     |              |
//              |         |   gg:::::::::::::g      |              |
//              |         |     ggg::::::ggg        |              |
//              |         |         gggggg          |              v
//              |         +-------------------------+----------------- ymin
//              |                                   |
//              |------------- advanceX ----------->|

// From SDL_ttf: Handy routines for converting from fixed point
#define FT_FLOOR( X ) ( ( X & -64 ) / 64 )
#define FT_CEIL( X )  ( ( ( X + 63 ) & -64 ) / 64 )

// Glyph information
struct GlyphInfo
{
    float offsetX;  // The distance from the origin ("pen position") to the left of the glyph.
    float offsetY;  // The distance from the origin to the top of the glyph. This is usually a value < 0.
    float advanceX; // The distance from the origin to the origin of the next glyph. This is usually a value > 0.

    uint32_t width;    // Glyph's width in pixels.
    uint32_t height;   // Glyph's height in pixels.
};

//  FreeType glyph rasterizer
class FreeTypeFont
{
public:
    FreeTypeFont() {}
    ~FreeTypeFont();

    // Initialize from imgui font config struct.
    void Init( ImFontConfig &cfg );

    // Generate glyph image and get glyph information.
    bool GetGlyphInfo( uint32_t codepoint, GlyphInfo &glyphInfo );

    // Blit glyph bitmap (from last call to GetGlyphInfo) to dest.
    void BlitGlyph( uint8_t *dst, uint32_t dst_pitch, float brighten );

public:
    // The pixel extents above the baseline in pixels (typically positive).
    float m_ascender;
    // The extents below the baseline in pixels (typically negative).
    float m_descender;
    // This field gives the maximum horizontal cursor advance for all glyphs in the font.
    float m_maxAdvanceWidth;

    bool m_bold;
    bool m_oblique;
    FT_Int32 m_LoadGlyphFlags;

    FT_Glyph m_glyph = nullptr;
    FT_BitmapGlyph m_glyph_bitmap = nullptr;

    FT_Library m_library = nullptr;
    FT_Face m_face = nullptr;
};

FreeTypeFont::~FreeTypeFont()
{
    FT_Done_Glyph( m_glyph );
    m_glyph = NULL;
    m_glyph_bitmap = NULL;

    if ( m_face )
    {
        FT_Done_Face( m_face );
        m_face = nullptr;

        FT_Done_FreeType( m_library );
        m_library = nullptr;
    }
}

void FreeTypeFont::Init( ImFontConfig &cfg )
{
    int faceIndex = cfg.FontNo;
    float pixelHeight  = cfg.SizePixels;
    unsigned int flags = cfg.FreetypeFlags;
    int dataSize = cfg.FontDataSize;
    const FT_Byte *data = ( const FT_Byte * )cfg.FontData;

    FT_Error error = FT_Init_FreeType( &m_library );
    IM_ASSERT( error == 0 );

    error = FT_New_Memory_Face( m_library, data, dataSize, faceIndex, &m_face );
    IM_ASSERT( error == 0 );

    error = FT_Select_Charmap( m_face, FT_ENCODING_UNICODE );
    IM_ASSERT( error == 0 );

    // I'm not sure how to deal with font sizes properly.
    // As far as I understand, currently ImGui assumes that the 'pixelHeight' is a maximum height of an any given glyph,
    // i.e. it's the sum of font's ascender and descender.
    // Seems strange to me.
    FT_Size_RequestRec req;

    req.type = FT_SIZE_REQUEST_TYPE_REAL_DIM;
    req.width = 0;
    req.height = pixelHeight * 64.0f;
    req.horiResolution = 0;
    req.vertResolution = 0;

    FT_Request_Size( m_face, &req );

    // update font info
    FT_Size_Metrics metrics = m_face->size->metrics;

    m_ascender = FT_CEIL( metrics.ascender );
    m_descender = FT_CEIL( metrics.descender );
    m_maxAdvanceWidth = FT_CEIL( metrics.max_advance );

    m_LoadGlyphFlags = FT_LOAD_NO_BITMAP;

    if ( flags & ImGuiFreeType::DisableHinting )
        m_LoadGlyphFlags |= FT_LOAD_NO_HINTING;
    if ( flags & ImGuiFreeType::ForceAutoHint )
        m_LoadGlyphFlags |= FT_LOAD_FORCE_AUTOHINT;
    if ( flags & ImGuiFreeType::NoAutoHint )
        m_LoadGlyphFlags |= FT_LOAD_NO_AUTOHINT;

    if ( flags & ImGuiFreeType::LightHinting )
        m_LoadGlyphFlags |= FT_LOAD_TARGET_LIGHT;
    else if ( flags & ImGuiFreeType::MonoHinting )
        m_LoadGlyphFlags |= FT_LOAD_TARGET_MONO;
    else
        m_LoadGlyphFlags |= FT_LOAD_TARGET_NORMAL;

    m_oblique = !!( flags & ImGuiFreeType::Oblique );
    m_bold = !!( flags & ImGuiFreeType::Bold );
}

bool FreeTypeFont::GetGlyphInfo( uint32_t codepoint, GlyphInfo &glyphInfo )
{
    bool ret = false;
    uint32_t glyphIndex = FT_Get_Char_Index( m_face, codepoint );

    memset( &glyphInfo, 0, sizeof( glyphInfo ) );

    FT_Done_Glyph( m_glyph );
    m_glyph = NULL;
    m_glyph_bitmap = NULL;

    FT_Error error = FT_Load_Glyph( m_face, glyphIndex, m_LoadGlyphFlags );
    if ( error )
        return false;

    FT_GlyphSlot slot = m_face->glyph;

    // need an outline for this to work
    IM_ASSERT( slot->format == FT_GLYPH_FORMAT_OUTLINE );

    if ( m_oblique )
        FT_GlyphSlot_Oblique( slot );

    if ( m_bold )
        FT_GlyphSlot_Embolden( slot );

    // retrieve the glyph
    error = FT_Get_Glyph( slot, &m_glyph );
    if ( !error )
    {
        FT_Bool destroy = true;
        FT_Vector *origin = NULL;

        error = FT_Glyph_To_Bitmap( &m_glyph, FT_RENDER_MODE_NORMAL, origin, destroy );
        if ( !error )
        {
            m_glyph_bitmap = ( FT_BitmapGlyph )m_glyph;

            glyphInfo.advanceX = slot->advance.x * ( 1.0f / 64.0f );
            glyphInfo.offsetX = m_glyph_bitmap->left;
            glyphInfo.offsetY = -m_glyph_bitmap->top;

            if ( m_glyph_bitmap->bitmap.buffer )
            {
                glyphInfo.width = m_glyph_bitmap->bitmap.width;
                glyphInfo.height = m_glyph_bitmap->bitmap.rows;
            }

            ret = true;
        }
    }

    return ret;
}

void FreeTypeFont::BlitGlyph( uint8_t *dst, uint32_t dst_pitch, float brighten )
{
    if ( !m_glyph_bitmap )
        return;

    uint32_t w = m_glyph_bitmap->bitmap.width;
    uint32_t h = m_glyph_bitmap->bitmap.rows;
    uint8_t *src = m_glyph_bitmap->bitmap.buffer;
    uint32_t src_pitch = m_glyph_bitmap->bitmap.pitch;

    if ( brighten == 1.0f )
    {
        for ( uint32_t y = 0; y < h; y++ )
        {
            memcpy( dst, src, w );
            src += src_pitch;
            dst += dst_pitch;
        }
    }
    else
    {
        for ( uint32_t y = 0; y < h; y++ )
        {
            for ( uint32_t x = 0; x < w; x++ )
                dst[ x ] = std::min< uint32_t >( 255, src[ x ] * brighten );

            src += src_pitch;
            dst += dst_pitch;
        }
    }
}

bool ImGuiFreeType::BuildFontAtlas( ImFontAtlas *atlas )
{
    IM_ASSERT( atlas->ConfigData.Size > 0 );

    atlas->TexID = NULL;
    atlas->TexWidth = atlas->TexHeight = 0;
    atlas->TexUvWhitePixel = ImVec2( 0, 0 );
    atlas->ClearTexData();

    ImVec2 maxGlyphSize = { 1.0f, 1.0f };
    std::vector< FreeTypeFont > fonts( atlas->ConfigData.Size );

    // Initialize font information early (so we can error without any cleanup) + count glyphs
    int total_glyph_count = 0;
    for ( int input_i = 0; input_i < atlas->ConfigData.Size; input_i++ )
    {
        ImFontConfig &cfg = atlas->ConfigData[ input_i ];
        FreeTypeFont &fontFace = fonts[ input_i ];

        IM_ASSERT( cfg.DstFont && ( !cfg.DstFont->IsLoaded() || cfg.DstFont->ContainerAtlas == atlas ) );

        if ( !cfg.GlyphRanges )
            cfg.GlyphRanges = atlas->GetGlyphRangesDefault();

        fontFace.Init( cfg );

        maxGlyphSize.x = ImMax( maxGlyphSize.x, fontFace.m_maxAdvanceWidth );
        maxGlyphSize.y = ImMax( maxGlyphSize.y, fontFace.m_ascender - fontFace.m_descender );

        // Count glyphs
        for ( const ImWchar *in_range = cfg.GlyphRanges; in_range[ 0 ] && in_range[ 1 ]; in_range += 2 )
            total_glyph_count += ( in_range[ 1 ] - in_range[ 0 ] ) + 1;
    }

    // Start packing. We need a known width for the skyline algorithm. Using a
    //   cheap heuristic here to decide of width. User can override
    //   TexDesiredWidth if they wish.
    // After packing is done, width shouldn't matter much, but some API/GPU
    //   have texture size limitations and increasing width can decrease height.
    if ( atlas->TexDesiredWidth > 0 )
        atlas->TexWidth = atlas->TexDesiredWidth;
    else if ( total_glyph_count > 4000 )
        atlas->TexWidth = 4096;
    else if ( total_glyph_count > 2000 )
        atlas->TexWidth = 2048;
    else if ( total_glyph_count > 1000 )
        atlas->TexWidth = 1024;
    else
        atlas->TexWidth = 512;

    // Pack our extra data rectangles first, so it will be on the upper-left corner of our texture (UV will have small values).
    ImVector< stbrp_rect > extra_rects;
    atlas->RenderCustomTexData( 0, &extra_rects );
    const int TotalRects = total_glyph_count + extra_rects.size();

    // #Vuhdo: Now, I won't do the original first pass to determine texture height, but just rough estimate.
    // Looks ugly inaccurate and excessive, but AFAIK with FreeType we actually need to render glyphs to get exact sizes.
    // Alternatively, we could just render all glyphs into a big shadow buffer, get their sizes, do the rectangle
    // packing and just copy back from the shadow buffer to the texture buffer.
    // Will give us an accurate texture height, but eat a lot of temp memory.
    // Probably no one will notice.
    float MinRectsPerRow = ceilf( ( atlas->TexWidth / ( maxGlyphSize.x + 1.0f ) ) );
    float MinRectsPerColumn = ceilf( TotalRects / MinRectsPerRow );

    atlas->TexHeight = ( int )( MinRectsPerColumn * ( maxGlyphSize.y + 1.0f ) );
    atlas->TexHeight = ImUpperPowerOfTwo( atlas->TexHeight );

    stbrp_context context;
    std::vector< stbrp_node > nodes( TotalRects );

    // Initialize a rectangle packer
    stbrp_init_target( &context, atlas->TexWidth, atlas->TexHeight, &nodes[ 0 ], TotalRects );

    // Assign packed locations to rectangles.
    stbrp_pack_rects( &context, &extra_rects[ 0 ], extra_rects.Size );

    for ( int i = 0; i < extra_rects.Size; i++ )
    {
        if ( extra_rects[ i ].was_packed )
            atlas->TexHeight = ImMax( atlas->TexHeight, extra_rects[ i ].y + extra_rects[ i ].h );
    }

    // Create texture
    atlas->TexPixelsAlpha8 = ( unsigned char * )ImGui::MemAlloc( atlas->TexWidth * atlas->TexHeight );
    memset( atlas->TexPixelsAlpha8, 0, atlas->TexWidth * atlas->TexHeight );

    // render characters, setup ImFont and glyphs for runtime
    for ( int input_i = 0; input_i < atlas->ConfigData.Size; input_i++ )
    {
        ImFontConfig &cfg = atlas->ConfigData[ input_i ];
        FreeTypeFont &fontFace = fonts[ input_i ];
        ImFont *dst_font = cfg.DstFont;

        float ascent = fontFace.m_ascender;
        float descent = fontFace.m_descender;

        if ( !cfg.MergeMode )
        {
            dst_font->ContainerAtlas = atlas;
            dst_font->ConfigData = &cfg;
            dst_font->ConfigDataCount = 0;
            dst_font->FontSize = cfg.SizePixels;
            dst_font->Ascent = ascent;
            dst_font->Descent = descent;
            dst_font->Glyphs.resize( 0 );
            dst_font->MetricsTotalSurface = 0;
        }
        dst_font->ConfigDataCount++;

        float off_x = cfg.GlyphOffset.x;
        float off_y = cfg.GlyphOffset.y;

        // Always clear fallback so FindGlyph can return NULL. It will be set again in BuildLookupTable()
        dst_font->FallbackGlyph = NULL;

        for ( const ImWchar *in_range = cfg.GlyphRanges; in_range[ 0 ] && in_range[ 1 ]; in_range += 2 )
        {
            for ( uint32_t codepoint = in_range[ 0 ]; codepoint <= in_range[ 1 ]; ++codepoint )
            {
                if ( cfg.MergeMode && dst_font->FindGlyph( ( unsigned short )codepoint ) )
                    continue;

                GlyphInfo glyphInfo;
                fontFace.GetGlyphInfo( codepoint, glyphInfo );

                // blit to texture
                stbrp_rect rect;
                rect.w = ( uint16_t )glyphInfo.width + 1; // account for texture filtering;
                rect.h = ( uint16_t )glyphInfo.height + 1;

                stbrp_pack_rects( &context, &rect, 1 );

                if ( glyphInfo.height )
                {
                    uint8_t *dst = atlas->TexPixelsAlpha8 + rect.y * atlas->TexWidth + rect.x;

                    fontFace.BlitGlyph( dst, atlas->TexWidth, cfg.Brighten + 1.0f );
                }

                dst_font->Glyphs.resize( dst_font->Glyphs.Size + 1 );
                ImFont::Glyph &glyph = dst_font->Glyphs.back();

                glyph.Codepoint = ( ImWchar )codepoint;

                glyph.X0 = glyphInfo.offsetX + off_x;
                glyph.Y0 = glyphInfo.offsetY + off_y;
                glyph.X1 = glyph.X0 + glyphInfo.width + off_x;
                glyph.Y1 = glyph.Y0 + glyphInfo.height + off_y;
                glyph.U0 = rect.x / ( float )atlas->TexWidth;
                glyph.V0 = rect.y / ( float )atlas->TexHeight;
                glyph.U1 = ( rect.x + glyphInfo.width ) / ( float )atlas->TexWidth;
                glyph.V1 = ( rect.y + glyphInfo.height ) / ( float )atlas->TexHeight;
                glyph.Y0 += ( float )( int )( dst_font->Ascent + off_y + 0.5f );
                glyph.Y1 += ( float )( int )( dst_font->Ascent + off_y + 0.5f );
                glyph.XAdvance = ( glyphInfo.advanceX + cfg.GlyphExtraSpacing.x ); // Bake spacing into XAdvance

                if ( cfg.PixelSnapH )
                    glyph.XAdvance = ( float )( int )( glyph.XAdvance + 0.5f );
            }
        }

        cfg.DstFont->BuildLookupTable();
    }

    // Render into our custom data block
    atlas->RenderCustomTexData( 1, &extra_rects );

    return true;
}

#endif // USE_FREETYPE
