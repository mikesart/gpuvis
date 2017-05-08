#include "imgui_sdlttf.h"
#include <math.h>
#include <stdint.h>

#include <SDL_ttf.h>

#ifdef __GNUC__
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

#define IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_PLACEMENT_NEW
#include "imgui_internal.h"

#define STBRP_ASSERT( x ) IM_ASSERT( x )
#define STBRP_STATIC
#define STB_RECT_PACK_IMPLEMENTATION
#include "stb_rect_pack.h"

// Hinting Font hinting is the use of mathematical instructions to adjust the
// display of an outline font so that it lines up with a rasterized grid. At small
// screen sizes, with or without antialiasing, hinting is critical for producing a
// clear, legible text for human readers.
//
// Kerning Kerning is the process of spacing adjacent characters apart depending
// on the actual two adjacent characters. This allows some characters to be closer
// to each other than others. When kerning is not used, such as when using the
// glyph metrics advance value, the characters will be spaced out at a constant
// size that accomodates all pairs of adjacent characters. This would be the
// maximum space between characters needed. There's currently no method to
// retrieve the kerning for a pair of characters from SDL_ttf, However correct
// kerning will be applied when a string of text is rendered instead of individual
// glyphs.


// Font parameters and metrics.
struct FontInfo
{
    // The pixel extents above the baseline in pixels (typically positive).
    float ascent;
    // The extents below the baseline in pixels (typically negative).
    float descent;

    // This field gives the maximum horizontal cursor advance for all glyphs in the font.
    float maxAdvanceWidth;

    const char *familyName;
    const char *styleName;
};

// Glyph metrics:
//  http://freetype.sourceforge.net/freetype2/docs/tutorial/step2.html
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

// A structure that describe a glyph.
struct GlyphInfo
{
    float width;    // Glyph's width in pixels.
    float height;   // Glyph's height in pixels.
    float offsetX;  // The distance from the origin ("pen position") to the left of the glyph.
    float offsetY;  // The distance from the origin to the top of the glyph. This is usually a value < 0.
    float advanceX; // The distance from the origin to the origin of the next glyph. This is usually a value > 0.
};

// Rasterized glyph image (8-bit alpha coverage).
struct GlyphBitmap
{
    static const uint32_t MaxWidth = 256;
    static const uint32_t MaxHeight = 256;

    uint8_t grayscale[ MaxWidth * MaxHeight ];
    uint32_t width, height, pitch;
};

//  SDL ttf glyph rasterizer.
class FreeTypeFont
{
public:
    // no ctor/dtor, explicitly call Init()/Shutdown()

    // Font descriptor of the current font.
    FontInfo fontInfo;

    // Initialize from an external data buffer.
    // Doesn't copy data, and you must ensure it stays valid up to this object lifetime.
    void Init( const uint8_t *data, uint32_t dataSize, uint32_t faceIndex, uint32_t pixelHeight );

    // Cleanup.
    void Shutdown();

    // Generate glyph image.
    bool RasterizeGlyph( uint32_t codepoint, GlyphInfo &glyphInfo, GlyphBitmap &glyphBitmap, uint32_t flags );

public:
    TTF_Font *m_font = nullptr;
};

void FreeTypeFont::Init( const uint8_t *data, uint32_t dataSize, uint32_t faceIndex, uint32_t pixelHeight )
{
    SDL_RWops *src = SDL_RWFromConstMem( data, dataSize );

    m_font = TTF_OpenFontIndexRW( src, 1, pixelHeight, faceIndex );

    TTF_SetFontStyle( m_font, TTF_STYLE_NORMAL );
    TTF_SetFontHinting( m_font, TTF_HINTING_NONE );
    TTF_SetFontOutline( m_font, 0 );
    TTF_SetFontKerning( m_font, 1 );

    memset( &fontInfo, 0, sizeof( fontInfo ) );

    fontInfo.familyName = TTF_FontFaceFamilyName( m_font );
    fontInfo.styleName = TTF_FontFaceStyleName( m_font );

    printf( "FamilyName: %s\nStyleName: %s\n", fontInfo.familyName, fontInfo.styleName );
    printf( "pixelHeight Requested: %d\n", pixelHeight );

    // Get the maximum pixel height of all glyphs of the loaded font. You may
    //  use this height for rendering text as close together vertically as
    //  possible, though adding at least one pixel height to it will space it so
    //  they can't touch.
    printf( "FontHeight: %d\n", TTF_FontHeight( m_font ) );

    // Get the recommended pixel height of a rendered line of text of the
    //  loaded font. This is usually larger than the TTF_FontHeight of the font.
    printf( "FontLineSkip: %d\n", TTF_FontLineSkip( m_font ) );

    // Get the maximum pixel ascent of all glyphs of the loaded font. This can
    //  also be interpreted as the distance from the top of the font to the
    //  baseline.
    fontInfo.ascent = TTF_FontAscent( m_font );
    printf( "ascent: %d\n", TTF_FontAscent( m_font ) );

    // Get the maximum pixel descent of all glyphs of the loaded font. This can
    //  also be interpreted as the distance from the baseline to the bottom of
    //  the font.
    fontInfo.descent = TTF_FontDescent( m_font );
    printf( "descent: %d\n", TTF_FontDescent( m_font ) );

    printf( "\n" );
}

void FreeTypeFont::Shutdown()
{
    TTF_CloseFont( m_font );
    m_font = NULL;
}

bool FreeTypeFont::RasterizeGlyph( uint32_t codepoint, GlyphInfo &glyphInfo, GlyphBitmap &glyphBitmap, uint32_t flags )
{
    int minx, maxx, miny, maxy, advance;
    SDL_Color white = { 0xFF, 0xFF, 0xFF, 0 };

    // The glyph is rendered without any padding or centering in the X
    // direction, and aligned normally in the Y direction.
    SDL_Surface *glyph = TTF_RenderGlyph_Blended( m_font, ( Uint16 )codepoint, white );

    TTF_GlyphMetrics( m_font, ( Uint16 )codepoint, &minx, &maxx, &miny, &maxy, &advance );

    int ascent = TTF_FontAscent( m_font );
    int descent = TTF_FontDescent( m_font );
    int height = TTF_FontHeight( m_font );
    int pixelheight = TTF_FontHeight( m_font );

    glyphInfo.offsetX = minx;
    glyphInfo.offsetY = -( fontInfo.ascent );
    glyphInfo.width = glyph->w;
    glyphInfo.height = glyph->h;
    glyphInfo.advanceX = advance;

    glyphBitmap.width = glyph->w;
    glyphBitmap.height = glyph->h;
    glyphBitmap.pitch = glyph->w;

    if ( codepoint == 'Y' || codepoint == 'y' )
    {
        printf( "codepoint: %c\n", codepoint );
        printf( "  ascent: %d\n", ascent );
        printf( "  descent: %d\n", descent );
        printf( "  height: %d\n", height );
        printf( "  pixelheight: %d\n", pixelheight );

        printf( "  minx: %d\n", minx );
        printf( "  maxx: %d\n", maxx );
        printf( "  miny: %d\n", miny );
        printf( "  maxy: %d\n", maxy );
        printf( "  advance: %d\n", advance );

        printf( "  glyph->h: %d\n", glyph->h );
        printf( "  glyph->w: %d\n", glyph->w );
        printf( "  glyph->pitch: %d\n", glyph->pitch );

        printf( "\n" );
        fflush( stdout );
    }

    IM_ASSERT( glyphBitmap.pitch <= GlyphBitmap::MaxWidth );

    if ( glyphBitmap.width > 0 )
    {
        uint8_t *dest = glyphBitmap.grayscale;

        for ( int h = 0; h < glyph->h; h++ )
        {
            uint32_t *pixels = ( uint32_t * )( ( char * )glyph->pixels + h * glyph->pitch );

            for ( int w = 0; w < glyph->w; w++ )
            {
                *dest++ = ( uint8_t )( *pixels++ >> 24 );
            }
        }
    }

    SDL_FreeSurface( glyph );

    return true;
}

bool ImGuiFreeType::BuildFontAtlas( ImFontAtlas *atlas, unsigned int flags )
{
    IM_ASSERT( atlas->ConfigData.Size > 0 );

    TTF_Init();

    atlas->TexID = NULL;
    atlas->TexWidth = atlas->TexHeight = 0;
    atlas->TexUvWhitePixel = ImVec2( 0, 0 );
    atlas->ClearTexData();

    ImVec2 maxGlyphSize = { 1.0f, 1.0f };
    FreeTypeFont *tmp_array = ( FreeTypeFont * )ImGui::MemAlloc( ( size_t )atlas->ConfigData.Size * sizeof( FreeTypeFont ) );

    // Initialize font information early (so we can error without any cleanup) + count glyphs
    int total_glyph_count = 0;
    int total_glyph_range_count = 0;
    for ( int input_i = 0; input_i < atlas->ConfigData.Size; input_i++ )
    {
        ImFontConfig &cfg = atlas->ConfigData[ input_i ];
        FreeTypeFont &fontFace = tmp_array[ input_i ];

        IM_ASSERT( cfg.DstFont && ( !cfg.DstFont->IsLoaded() || cfg.DstFont->ContainerAtlas == atlas ) );

        if ( !cfg.GlyphRanges )
            cfg.GlyphRanges = atlas->GetGlyphRangesDefault();

        fontFace.Init( ( uint8_t * )cfg.FontData, ( uint32_t )cfg.FontDataSize, cfg.FontNo, ( uint32_t )cfg.SizePixels );

        for ( const ImWchar *in_range = cfg.GlyphRanges; in_range[ 0 ] && in_range[ 1 ]; in_range += 2 )
        {
            ImFont *dst_font = cfg.DstFont;

            for ( uint32_t codepoint = in_range[ 0 ]; codepoint <= in_range[ 1 ]; ++codepoint )
            {
                if ( cfg.MergeMode && dst_font->FindGlyph( ( ImWchar )codepoint ) )
                    continue;

                int minx, maxx, miny, maxy, advance;
                TTF_GlyphMetrics( fontFace.m_font, ( Uint16 )codepoint, &minx, &maxx, &miny, &maxy, &advance );

                fontFace.fontInfo.maxAdvanceWidth = std::max< float >( fontFace.fontInfo.maxAdvanceWidth, advance );
            }

            // Count glyphs
            total_glyph_count += ( in_range[ 1 ] - in_range[ 0 ] ) + 1;
            total_glyph_range_count++;
        }

        maxGlyphSize.x = ImMax( maxGlyphSize.x, fontFace.fontInfo.maxAdvanceWidth );
        maxGlyphSize.y = ImMax( maxGlyphSize.y, fontFace.fontInfo.ascent - fontFace.fontInfo.descent );
    }

    // Start packing. We need a known width for the skyline algorithm. Using a cheap heuristic here to decide of width. User can override TexDesiredWidth if they wish.
    // After packing is done, width shouldn't matter much, but some API/GPU have texture size limitations and increasing width can decrease height.
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
    stbrp_node *nodes = ( stbrp_node * )ImGui::MemAlloc( TotalRects * sizeof( stbrp_node ) );
    stbrp_init_target( &context, atlas->TexWidth, atlas->TexHeight, nodes, TotalRects );

    stbrp_pack_rects( &context, &extra_rects[ 0 ], extra_rects.Size );
    for ( int i = 0; i < extra_rects.Size; i++ )
        if ( extra_rects[ i ].was_packed )
            atlas->TexHeight = ImMax( atlas->TexHeight, extra_rects[ i ].y + extra_rects[ i ].h );

    // Create texture
    atlas->TexPixelsAlpha8 = ( unsigned char * )ImGui::MemAlloc( atlas->TexWidth * atlas->TexHeight );
    memset( atlas->TexPixelsAlpha8, 0, atlas->TexWidth * atlas->TexHeight );

    // render characters, setup ImFont and glyphs for runtime
    for ( int input_i = 0; input_i < atlas->ConfigData.Size; input_i++ )
    {
        ImFontConfig &cfg = atlas->ConfigData[ input_i ];
        FreeTypeFont &fontFace = tmp_array[ input_i ];
        ImFont *dst_font = cfg.DstFont;

        float ascent = fontFace.fontInfo.ascent;
        float descent = fontFace.fontInfo.descent;
        if ( !cfg.MergeMode )
        {
            dst_font->ContainerAtlas = atlas;
            dst_font->ConfigData = &cfg;
            dst_font->ConfigDataCount = 0;
            dst_font->FontSize = cfg.SizePixels;
            dst_font->Ascent = ascent;
            dst_font->Descent = descent;
            dst_font->Glyphs.resize( 0 );
        }
        dst_font->ConfigDataCount++;
        float off_y = ( cfg.MergeMode && cfg.MergeGlyphCenterV ) ? ( ascent - dst_font->Ascent ) * 0.5f : 0.0f;

        // Always clear fallback so FindGlyph can return NULL. It will be set again in BuildLookupTable()
        dst_font->FallbackGlyph = NULL;

        for ( const ImWchar *in_range = cfg.GlyphRanges; in_range[ 0 ] && in_range[ 1 ]; in_range += 2 )
        {
            for ( uint32_t codepoint = in_range[ 0 ]; codepoint <= in_range[ 1 ]; ++codepoint )
            {
                if ( cfg.MergeMode && dst_font->FindGlyph( ( ImWchar )codepoint ) )
                    continue;

                GlyphInfo glyphInfo;
                GlyphBitmap glyphBitmap;

                fontFace.RasterizeGlyph( codepoint, glyphInfo, glyphBitmap, flags );

                // blit to texture
                stbrp_rect rect;
                rect.w = ( uint16_t )glyphBitmap.width + 1; // account for texture filtering
                rect.h = ( uint16_t )glyphBitmap.height + 1;
                stbrp_pack_rects( &context, &rect, 1 );

                const uint8_t *src = glyphBitmap.grayscale;
                uint8_t *dst = atlas->TexPixelsAlpha8 + rect.y * atlas->TexWidth + rect.x;
                for ( uint32_t yy = 0; yy < glyphBitmap.height; ++yy )
                {
                    memcpy( dst, src, glyphBitmap.width );

                    src += glyphBitmap.pitch;
                    dst += atlas->TexWidth;
                }

                dst_font->Glyphs.resize( dst_font->Glyphs.Size + 1 );
                ImFont::Glyph &glyph = dst_font->Glyphs.back();

                glyph.Codepoint = ( ImWchar )codepoint;
                glyph.X0 = glyphInfo.offsetX;
                glyph.Y0 = glyphInfo.offsetY;
                glyph.X1 = glyph.X0 + glyphInfo.width;
                glyph.Y1 = glyph.Y0 + glyphInfo.height;
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

    // Cleanup temporaries
    ImGui::MemFree( nodes );
    for ( int n = 0; n < atlas->ConfigData.Size; ++n )
        tmp_array[ n ].Shutdown();

    ImGui::MemFree( tmp_array );

    // Render into our custom data block
    atlas->RenderCustomTexData( 1, &extra_rects );

    TTF_Quit();
    return true;
}
