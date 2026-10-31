#ifndef _GLYPH_CACHE_H_
#define _GLYPH_CACHE_H_

/*
 * A monospaced face's glyphs as coverage bitmaps made at the exact pixel size they are drawn at
 * (spec 14.4). The UI's own text is a distance field, which scales freely
 * but goes soft and thin at the 12 to 14 pixels code is read at, most of all on a screen that is
 * not a Retina one; a bitmap made at the size it is shown is what lite and every terminal draw.
 *
 * Glyphs are rasterised by stb_truetype as they are first asked for, and packed in rows into one
 * single-channel atlas whose texture reads as white with the coverage in its alpha, so the UI's
 * textured quad draws one in its tint with no shader of its own. A cell is the face's advance
 * rounded to whole pixels, and a line's height and its baseline are whole pixels too, so every
 * glyph lands on the pixel grid and stays sharp.
 *
 * A size is a pixel height, as a Font's is: ascent to descent. Asking for another -- a zoom, or a
 * window moved to a screen of another scale -- empties the atlas, which fills again as glyphs are
 * asked for.
 */

#include <stdbool.h>
#include <stdint.h>

#include "texture.h"

// Where one glyph is in the atlas, and where it goes from the pen.
typedef struct GlyphBitmap {
    float uv[4];   // u0, v0, u1, v1 in the atlas, v0 at the bitmap's top
    int left, top; // the bitmap's top-left from the pen on the baseline, in pixels, +y down
    int width, height;
    bool missing; // the face has no glyph for the codepoint
} GlyphBitmap;

typedef struct GlyphCacheEntry GlyphCacheEntry;

typedef struct GlyphCache {
    /* ENGINE-OWNED */
    unsigned char* ttf;
    void* font_info;       // stb_truetype's stbtt_fontinfo
    float pixel_size;      // the size the bitmaps are made at
    float scale;           // stb_truetype's scale for it
    int cell_width;        // pixels from one character to the next
    int line_height;       // pixels from one baseline to the next
    int ascent;            // pixels from a line's top to its baseline
    Texture atlas;         // white, the coverage in its alpha
    unsigned char* pixels; // the atlas's bytes, a row of atlas.width after another
    int pen_x, pen_y;      // where the next glyph is packed
    int row_height;        // the tallest glyph in the row being packed
    bool stale;            // the texture is behind the pixels: uploaded whole at the next glyph
    GlyphCacheEntry* ascii[128];
    GlyphCacheEntry* others; // the rest, by codepoint
} GlyphCache;

// A face from a TrueType file, at `pixel_size`. NULL when the file cannot be read or is not a font.
GlyphCache* create_glyph_cache(const char* ttf_path, float pixel_size);
void free_glyph_cache(GlyphCache* cache);

// Makes the bitmaps at another size from now on; the same size again changes nothing.
void glyph_cache_set_size(GlyphCache* cache, float pixel_size);
// A codepoint's glyph, rasterised and uploaded on first asking. False only out of memory or room;
// a codepoint the face lacks is true with `missing` set.
bool glyph_cache_get(GlyphCache* cache, uint32_t codepoint, GlyphBitmap* out);

#endif // _GLYPH_CACHE_H_
