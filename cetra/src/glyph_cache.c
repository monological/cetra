#include "glyph_cache.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "ext/log.h"
#include "ext/stb_truetype.h"
#include "ext/uthash.h"
#include "util.h"

// The atlas is this wide, and grows downward from its first height to its last.
#define ATLAS_WIDTH        512
#define ATLAS_FIRST_HEIGHT 256
#define ATLAS_LAST_HEIGHT  8192
// Clear pixels between glyphs, so a linear sample at a bitmap's edge reads none of its neighbour.
#define ATLAS_GAP 1

struct GlyphCacheEntry {
    uint32_t codepoint;
    int x, y; // its bitmap's top-left in the atlas
    int left, top, width, height;
    bool missing;
    UT_hash_handle hh;
};

static void _metrics(GlyphCache* c) {
    const stbtt_fontinfo* info = c->font_info;
    c->scale = stbtt_ScaleForPixelHeight(info, c->pixel_size);
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(info, &ascent, &descent, &gap);
    c->ascent = (int)lroundf((float)ascent * c->scale);
    c->line_height = (int)lroundf((float)(ascent - descent + gap) * c->scale);
    if (c->line_height < 1)
        c->line_height = 1;
    int advance, bearing;
    stbtt_GetCodepointHMetrics(info, 'M', &advance, &bearing);
    c->cell_width = (int)lroundf((float)advance * c->scale);
    if (c->cell_width < 1)
        c->cell_width = 1;
}

static void _upload_all(GlyphCache* c) {
    glBindTexture(GL_TEXTURE_2D, c->atlas.id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, c->atlas.width, c->atlas.height, 0, GL_RED,
                 GL_UNSIGNED_BYTE, c->pixels);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
}

static void _forget(GlyphCache* c) {
    for (int i = 0; i < 128; i++) {
        free(c->ascii[i]);
        c->ascii[i] = NULL;
    }
    GlyphCacheEntry *e, *tmp;
    HASH_ITER(hh, c->others, e, tmp) {
        HASH_DEL(c->others, e);
        free(e);
    }
}

// Every glyph let go and the atlas cleared, for a new size.
static void _empty(GlyphCache* c) {
    _forget(c);
    memset(c->pixels, 0, (size_t)c->atlas.width * (size_t)c->atlas.height);
    c->pen_x = c->pen_y = ATLAS_GAP;
    c->row_height = 0;
    _upload_all(c);
}

GlyphCache* create_glyph_cache(const char* ttf_path, float pixel_size) {
    unsigned char* ttf = (unsigned char*)read_entire_file(ttf_path, NULL);
    if (!ttf) {
        log_error("create_glyph_cache: cannot read %s", ttf_path ? ttf_path : "(null)");
        return NULL;
    }
    GlyphCache* c = calloc(1, sizeof(GlyphCache));
    stbtt_fontinfo* info = calloc(1, sizeof(stbtt_fontinfo));
    unsigned char* pixels = calloc((size_t)ATLAS_WIDTH * ATLAS_FIRST_HEIGHT, 1);
    if (!c || !info || !pixels) {
        log_error("create_glyph_cache: out of memory");
        free(c);
        free(info);
        free(pixels);
        free(ttf);
        return NULL;
    }
    if (!stbtt_InitFont(info, ttf, stbtt_GetFontOffsetForIndex(ttf, 0))) {
        log_error("create_glyph_cache: %s is not a TrueType font", ttf_path);
        free(c);
        free(info);
        free(pixels);
        free(ttf);
        return NULL;
    }
    c->ttf = ttf;
    c->font_info = info;
    c->pixels = pixels;
    c->pixel_size = pixel_size > 1.0f ? pixel_size : 1.0f;
    c->atlas.width = ATLAS_WIDTH;
    c->atlas.height = ATLAS_FIRST_HEIGHT;
    c->atlas.internal_format = GL_R8;
    c->atlas.data_format = GL_RED;
    glGenTextures(1, &c->atlas.id);
    glBindTexture(GL_TEXTURE_2D, c->atlas.id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // White, with the coverage as alpha: the UI's textured quad tints it to the text's colour.
    const GLint swizzle[4] = {GL_ONE, GL_ONE, GL_ONE, GL_RED};
    glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, swizzle);
    _metrics(c);
    _empty(c);
    return c;
}

void free_glyph_cache(GlyphCache* c) {
    if (!c)
        return;
    _forget(c);
    if (c->atlas.id)
        glDeleteTextures(1, &c->atlas.id);
    free(c->pixels);
    free(c->font_info);
    free(c->ttf);
    free(c);
}

void glyph_cache_set_size(GlyphCache* c, float pixel_size) {
    if (!c)
        return;
    if (pixel_size < 1.0f)
        pixel_size = 1.0f;
    if (pixel_size == c->pixel_size)
        return;
    c->pixel_size = pixel_size;
    _metrics(c);
    _empty(c);
}

// The atlas twice as tall, its glyphs where they were.
static bool _grow(GlyphCache* c) {
    if (c->atlas.height >= ATLAS_LAST_HEIGHT)
        return false;
    const size_t old = (size_t)c->atlas.width * (size_t)c->atlas.height;
    unsigned char* pixels = realloc(c->pixels, old * 2);
    if (!pixels)
        return false;
    memset(pixels + old, 0, old);
    c->pixels = pixels;
    c->atlas.height *= 2;
    _upload_all(c);
    return true;
}

static GlyphCacheEntry* _make(GlyphCache* c, uint32_t codepoint) {
    GlyphCacheEntry* e = calloc(1, sizeof(GlyphCacheEntry));
    if (!e)
        return NULL;
    e->codepoint = codepoint;
    const stbtt_fontinfo* info = c->font_info;
    const int glyph = stbtt_FindGlyphIndex(info, (int)codepoint);
    if (!glyph) {
        e->missing = true;
        return e;
    }
    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBox(info, glyph, c->scale, c->scale, &x0, &y0, &x1, &y1);
    e->left = x0;
    e->top = y0;
    e->width = x1 - x0;
    e->height = y1 - y0;
    if (e->width <= 0 || e->height <= 0) {
        e->width = e->height = 0;
        return e;
    }
    if (e->width + 2 * ATLAS_GAP > c->atlas.width) {
        free(e);
        return NULL;
    }
    if (c->pen_x + e->width + ATLAS_GAP > c->atlas.width) {
        c->pen_x = ATLAS_GAP;
        c->pen_y += c->row_height + ATLAS_GAP;
        c->row_height = 0;
    }
    while (c->pen_y + e->height + ATLAS_GAP > c->atlas.height) {
        if (!_grow(c)) {
            log_error("glyph_cache: the atlas is full at %d pixels tall", c->atlas.height);
            free(e);
            return NULL;
        }
    }
    e->x = c->pen_x;
    e->y = c->pen_y;
    unsigned char* at = c->pixels + (size_t)e->y * (size_t)c->atlas.width + (size_t)e->x;
    stbtt_MakeGlyphBitmap(info, at, e->width, e->height, c->atlas.width, c->scale, c->scale, glyph);
    glBindTexture(GL_TEXTURE_2D, c->atlas.id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, c->atlas.width);
    glTexSubImage2D(GL_TEXTURE_2D, 0, e->x, e->y, e->width, e->height, GL_RED, GL_UNSIGNED_BYTE,
                    at);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    c->pen_x += e->width + ATLAS_GAP;
    if (e->height > c->row_height)
        c->row_height = e->height;
    return e;
}

bool glyph_cache_get(GlyphCache* c, uint32_t codepoint, GlyphBitmap* out) {
    if (!c || !out)
        return false;
    GlyphCacheEntry* e = NULL;
    if (codepoint < 128)
        e = c->ascii[codepoint];
    else
        HASH_FIND(hh, c->others, &codepoint, sizeof(uint32_t), e);
    if (!e) {
        e = _make(c, codepoint);
        if (!e)
            return false;
        if (codepoint < 128)
            c->ascii[codepoint] = e;
        else
            HASH_ADD(hh, c->others, codepoint, sizeof(uint32_t), e);
    }
    const float w = (float)c->atlas.width, h = (float)c->atlas.height;
    out->uv[0] = (float)e->x / w;
    out->uv[1] = (float)e->y / h;
    out->uv[2] = (float)(e->x + e->width) / w;
    out->uv[3] = (float)(e->y + e->height) / h;
    out->left = e->left;
    out->top = e->top;
    out->width = e->width;
    out->height = e->height;
    out->missing = e->missing;
    return true;
}
