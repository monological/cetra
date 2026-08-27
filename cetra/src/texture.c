
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "ext/stb_image.h"
#include "ext/uthash.h"
#include "ext/log.h"

#include "texture.h"
#include "texture_compress.h"
#include "util.h"

// Bleed visible RGB into fully-transparent texels. PNGs routinely store
// garbage (often white) color behind alpha = 0; bilinear and mipmap
// filtering mix it across the alpha edge, which renders as bright dashes
// along the edges of hair/foliage cards. Runs until every transparent texel
// carries a plausible color, so even the deepest mip levels average real
// strand color instead of garbage.
void texture_dilate_transparent_rgb(unsigned char* data, int width, int height) {
    size_t count = (size_t)width * (size_t)height;
    unsigned char* solid = malloc(count);
    if (!solid)
        return;
    for (size_t i = 0; i < count; i++)
        solid[i] = data[i * 4 + 3] >= 8;

    int max_passes = width > height ? width : height;
    for (int pass = 0; pass < max_passes; pass++) {
        unsigned char* next = malloc(count);
        if (!next)
            break;
        memcpy(next, solid, count);
        bool changed = false;

        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                size_t i = (size_t)y * (size_t)width + (size_t)x;
                if (solid[i])
                    continue;

                static const int dx[4] = {1, -1, 0, 0};
                static const int dy[4] = {0, 0, 1, -1};
                int r = 0, g = 0, b = 0, n = 0;
                for (int k = 0; k < 4; k++) {
                    int nx = x + dx[k];
                    int ny = y + dy[k];
                    if (nx < 0 || ny < 0 || nx >= width || ny >= height)
                        continue;
                    size_t j = (size_t)ny * (size_t)width + (size_t)nx;
                    if (!solid[j])
                        continue;
                    r += data[j * 4];
                    g += data[j * 4 + 1];
                    b += data[j * 4 + 2];
                    n++;
                }
                if (n > 0) {
                    data[i * 4] = (unsigned char)(r / n);
                    data[i * 4 + 1] = (unsigned char)(g / n);
                    data[i * 4 + 2] = (unsigned char)(b / n);
                    next[i] = 1;
                    changed = true;
                }
            }
        }

        free(solid);
        solid = next;
        if (!changed)
            break;
    }
    free(solid);
}

// Anisotropic filtering on the currently bound texture. Without it, textures
// tiled many times and viewed at grazing angles (floors, carpets) alias into
// moire banding, and their mip average washes the surface color out --
// trilinear alone cannot handle the anisotropic footprint. Ubiquitous
// extension; a no-op where unsupported. The device limit is queried once (a
// driver round-trip otherwise paid per texture upload).
void texture_set_max_anisotropy(void) {
    static GLfloat max_aniso = -1.0f;
    if (max_aniso < 0.0f) {
        max_aniso = 0.0f;
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &max_aniso);
    }
    if (max_aniso > 1.0f)
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, fminf(8.0f, max_aniso));
}

// The default sampler state every model texture gets: tiling wrap, trilinear
// minification, anisotropy. One definition so the three upload sites cannot
// drift.
void texture_set_default_sampler_state(void) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    texture_set_max_anisotropy();
}

// Float LUT upload (first user: the LTC area-light tables, spec 9.2). Data
// textures, not model textures: LINEAR filtering, CLAMP_TO_EDGE, no mips --
// the state the IBL BRDF LUT uses, baked in so LUT call sites cannot drift
// onto the tiling/trilinear model-texture defaults.
GLuint create_texture_2d_float(int width, int height, GLenum internal_format, GLenum data_format,
                               const float* pixels) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)internal_format, width, height, 0, data_format, GL_FLOAT,
                 pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

// Float volume upload/allocation, the 3D sibling of create_texture_2d_float and
// carrying the same data-texture policy: LINEAR, CLAMP_TO_EDGE on all three
// axes, no mips. The `_float` in the name is load-bearing -- the pixel type is
// GL_FLOAT, so a byte buffer here would be read as floats. A consumer needing
// integer or tiling volumes (noise) wants its own entry point.
// glTexImage3D (not glTexStorage3D, which is GL 4.2) allocates level 0 only,
// matching material_texture_array.c / shadow.c. pixels may be NULL for a render target.
GLuint create_texture_3d_float(int width, int height, int depth, GLenum internal_format,
                               GLenum data_format, const float* pixels) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_3D, tex);
    glTexImage3D(GL_TEXTURE_3D, 0, (GLint)internal_format, width, height, depth, 0, data_format,
                 GL_FLOAT, pixels);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_3D, 0);
    return tex;
}

// A filterable float 2D array, CLAMP_TO_EDGE and bilinear WITHIN each layer --
// never across layers, which is what separates this from the 3D entry point
// above. For array-of-maps data where a layer is an independent image (the fog's
// ESM cascades) rather than a sampled volume.
GLuint create_texture_2d_array_float(int width, int height, int layers, GLenum internal_format,
                                     GLenum data_format) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, (GLint)internal_format, width, height, layers, 0,
                 data_format, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    return tex;
}

// The tiling-volume entry point the note above reserves: RGBA8 pixels, REPEAT
// on all three axes, trilinear mips (glGenerateMipmap after the level-0
// upload -- still no glTexStorage3D on 4.1). For CPU-baked noise fields whose
// coarser mips a marcher samples explicitly via textureLod.
GLuint create_texture_3d_rgba8_tiling(int width, int height, int depth,
                                      const unsigned char* pixels) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_3D, tex);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, width, height, depth, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 pixels);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenerateMipmap(GL_TEXTURE_3D);
    glBindTexture(GL_TEXTURE_3D, 0);
    return tex;
}

void texture_gl_formats(int channels, bool is_srgb, GLenum* internal_format, GLenum* data_format) {
    if (channels == 1) {
        *internal_format = GL_RED;
        *data_format = GL_RED;
    } else if (channels == 2) {
        *internal_format = GL_RG;
        *data_format = GL_RG;
    } else if (channels == 3) {
        *internal_format = is_srgb ? GL_SRGB : GL_RGB;
        *data_format = GL_RGB;
    } else {
        *internal_format = is_srgb ? GL_SRGB_ALPHA : GL_RGBA;
        *data_format = GL_RGBA;
    }
}

// The sRGB electro-optical transfer, tabulated over the 256 byte codes. Filled
// into the caller's array rather than memoized in a file static: the analytic
// terrain path already carries that weakness and there is no reason to add a
// second one, and 256 powf calls per texture is nothing beside the decode that
// just ran.
static void texture_srgb_decode_lut(float lut[256]) {
    for (int i = 0; i < 256; i++) {
        const float v = (float)i / 255.0f;
        lut[i] = v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
    }
}

// The inverse, by binary search over that same table rather than by evaluating
// the forward transfer. Two reasons, and the second is the load-bearing one:
// it costs eight compares where powf costs far more on every texel of every
// level, and it is EXACT against the decode by construction -- the byte it
// returns is the one whose decoded value is nearest, so encode(decode(b)) == b
// for all 256 codes and a chain of halvings cannot drift.
static unsigned char texture_srgb_encode_byte(const float lut[256], float linear) {
    int lo = 0, hi = 255;
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (lut[mid] < linear)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo > 0 && (linear - lut[lo - 1]) < (lut[lo] - linear))
        lo--;
    return (unsigned char)lo;
}

// One 2x2 box halving.
//
// sRGB sources are averaged in LINEAR space, and that is a contract rather than
// a refinement: texture_mean_rgb reads the 1x1 top mip and decodes it to recover
// the linear mean, which is only the mean if the averaging happened there. GL
// specifies the same for its own sRGB mip generation, so this matches what the
// driver was doing rather than changing it.
//
// Alpha is never in that space. It carries a coverage or, since 11.60, a HEIGHT,
// and is averaged as stored in every format.
static void texture_box_halve(const unsigned char* src, int sw, int sh, int channels, bool is_srgb,
                              const float lut[256], unsigned char* dst) {
    const int dw = sw > 1 ? sw / 2 : 1;
    const int dh = sh > 1 ? sh / 2 : 1;
    // Colour occupies the leading three channels where there are three; a 1- or
    // 2-channel sRGB texture does not occur, but the min keeps the loop honest.
    const int colour = is_srgb ? (channels < 3 ? channels : 3) : 0;
    for (int y = 0; y < dh; y++) {
        // Clamp rather than wrap at an odd far edge, so the last row is averaged
        // with itself instead of with the opposite side of the image.
        const int y0 = (sh > 1) ? y * 2 : 0;
        const int y1 = (y0 + 1 < sh) ? y0 + 1 : y0;
        for (int x = 0; x < dw; x++) {
            const int x0 = (sw > 1) ? x * 2 : 0;
            const int x1 = (x0 + 1 < sw) ? x0 + 1 : x0;
            const size_t i00 = ((size_t)y0 * (size_t)sw + (size_t)x0) * (size_t)channels;
            const size_t i01 = ((size_t)y0 * (size_t)sw + (size_t)x1) * (size_t)channels;
            const size_t i10 = ((size_t)y1 * (size_t)sw + (size_t)x0) * (size_t)channels;
            const size_t i11 = ((size_t)y1 * (size_t)sw + (size_t)x1) * (size_t)channels;
            unsigned char* out = &dst[((size_t)y * (size_t)dw + (size_t)x) * (size_t)channels];
            for (int c = 0; c < channels; c++) {
                if (c < colour) {
                    const float sum = lut[src[i00 + c]] + lut[src[i01 + c]] + lut[src[i10 + c]] +
                                      lut[src[i11 + c]];
                    out[c] = texture_srgb_encode_byte(lut, sum * 0.25f);
                } else {
                    const int sum = (int)src[i00 + c] + (int)src[i01 + c] + (int)src[i10 + c] +
                                    (int)src[i11 + c];
                    out[c] = (unsigned char)((sum + 2) / 4);
                }
            }
        }
    }
}

// --no-texture-compression. A file static rather than a field on anything,
// because it is a property of the RUN and every upload path has to see it
// without threading a context it otherwise does not need.
static bool g_texture_compression_enabled = true;

void texture_set_compression_enabled(bool enabled) {
    g_texture_compression_enabled = enabled;
}

bool texture_compression_enabled(void) {
    return g_texture_compression_enabled;
}

// Which block format a texture takes, or NONE.
//
// COLOUR returns NONE for now: phase 7 is where DXT is judged, and until then
// this being the only branch that declines is what keeps every albedo in the
// corpus byte-identical.
//
// The channel counts are a floor, not a match: a normal map that decoded to
// three channels still compresses as BC5 on its first two, and a mask that
// decoded to three (a grey PNG) still compresses as BC4 on its first. What
// matters is that the channels this format DROPS carry nothing -- which is a
// statement about the semantic, and the semantic is what the caller just gave.
static TextureBlockFormat texture_block_format_for(TextureUse use, int channels, bool is_srgb) {
    if (!g_texture_compression_enabled)
        return TEXTURE_BLOCK_NONE;
    switch (use) {
    case TEXTURE_USE_NORMAL:
        return channels >= 2 ? TEXTURE_BLOCK_BC5 : TEXTURE_BLOCK_NONE;
    case TEXTURE_USE_DATA:
        // Only a single-channel source. A 3- or 4-channel linear map is an ORM
        // or a packed surface map whose channels are DIFFERENT quantities, and
        // BC4 would keep one and discard the rest.
        return channels == 1 ? TEXTURE_BLOCK_BC4 : TEXTURE_BLOCK_NONE;
    case TEXTURE_USE_COLOUR:
    default:
        (void)is_srgb;
        return TEXTURE_BLOCK_NONE;
    }
}

static GLenum texture_block_gl_format(TextureBlockFormat format, bool is_srgb) {
    switch (format) {
    case TEXTURE_BLOCK_BC4:
        return GL_COMPRESSED_RED_RGTC1;
    case TEXTURE_BLOCK_BC5:
        return GL_COMPRESSED_RG_RGTC2;
    case TEXTURE_BLOCK_DXT1:
        return is_srgb ? GL_COMPRESSED_SRGB_S3TC_DXT1_EXT : GL_COMPRESSED_RGB_S3TC_DXT1_EXT;
    case TEXTURE_BLOCK_DXT5:
        return is_srgb ? GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT
                       : GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
    default:
        return 0;
    }
}

// Encode and upload one level. Split out because the mip loop needs it for every
// level and the caller needs it for level 0, and the block arithmetic is easy to
// get subtly wrong twice.
static void texture_upload_compressed_level(TextureBlockFormat format, GLenum gl_format,
                                            GLint level, int width, int height, int channels,
                                            const unsigned char* pixels, unsigned char* scratch) {
    texture_block_encode(format, pixels, width, height, channels, scratch);
    glCompressedTexImage2D(GL_TEXTURE_2D, level, gl_format, width, height, 0,
                           (GLsizei)texture_block_image_bytes(format, width, height), scratch);
}

// Upload level 0 and every level below it, building the chain on the CPU.
//
// This is what replaces glGenerateMipmap, and the reason is compression: a
// compressed chain cannot be filled by it, because the driver would have to
// decode, filter and re-encode each level and nothing here does that. The filter
// therefore has to run before an encoder ever sees a level -- so it runs on the
// CPU for the uncompressed case too, rather than leaving two filters that have
// to agree with each other and silently would not.
//
// Returns false only on allocation failure, where the caller still has a usable
// level 0.
bool texture_upload_image(GLenum internal_format, GLenum data_format, int width, int height,
                          int channels, bool is_srgb, TextureUse use, const unsigned char* pixels,
                          GLenum* out_internal_format) {
    TextureBlockFormat block = texture_block_format_for(use, channels, is_srgb);
    const GLenum gl_block = texture_block_gl_format(block, is_srgb);
    // Scratch for the encoder, sized for level 0 and reused by every level under
    // it. Allocated before anything is uploaded so a failure falls back to the
    // uncompressed path cleanly rather than half way down a chain.
    unsigned char* scratch = NULL;
    if (gl_block != 0) {
        scratch = malloc(texture_block_image_bytes(block, width, height));
        if (!scratch) {
            log_error("texture compression: out of memory at %dx%d, storing uncompressed", width,
                      height);
            block = TEXTURE_BLOCK_NONE;
        }
    }
    const bool compressed = (gl_block != 0 && block != TEXTURE_BLOCK_NONE);
    if (out_internal_format)
        *out_internal_format = compressed ? gl_block : internal_format;

    if (compressed)
        texture_upload_compressed_level(block, gl_block, 0, width, height, channels, pixels,
                                        scratch);
    else
        glTexImage2D(GL_TEXTURE_2D, 0, (GLint)internal_format, width, height, 0, data_format,
                     GL_UNSIGNED_BYTE, pixels);

    if (width <= 1 && height <= 1) {
        free(scratch);
        return true;
    }

    float lut[256];
    texture_srgb_decode_lut(lut);

    // Two buffers, each big enough for the largest level either will hold, so the
    // chain ping-pongs without reallocating per level.
    const size_t cap = ((size_t)width / 2 + 1) * ((size_t)height / 2 + 1) * (size_t)channels;
    unsigned char* a = malloc(cap);
    unsigned char* b = malloc(cap);
    if (!a || !b) {
        free(a);
        free(b);
        free(scratch);
        log_error("texture mip chain: out of memory at %dx%d", width, height);
        return false;
    }

    const unsigned char* src = pixels;
    int sw = width, sh = height;
    unsigned char* dst = a;
    for (GLint level = 1; sw > 1 || sh > 1; level++) {
        const int dw = sw > 1 ? sw / 2 : 1;
        const int dh = sh > 1 ? sh / 2 : 1;
        // Filtered from the UNCOMPRESSED parent, never from a decoded block. The
        // chain is a chain of images, not of encodings, so quantisation error
        // cannot compound down it.
        texture_box_halve(src, sw, sh, channels, is_srgb, lut, dst);
        if (compressed)
            texture_upload_compressed_level(block, gl_block, level, dw, dh, channels, dst,
                                            scratch);
        else
            glTexImage2D(GL_TEXTURE_2D, level, (GLint)internal_format, dw, dh, 0, data_format,
                         GL_UNSIGNED_BYTE, dst);
        src = dst;
        dst = (dst == a) ? b : a;
        sw = dw;
        sh = dh;
    }

    free(a);
    free(b);
    free(scratch);
    return true;
}

// GPU footprint including the chain. The 4/3 is the mip tail; a compressed level
// is counted from its own block arithmetic, which is where the saving actually
// shows up.
size_t texture_gpu_bytes(const Texture* texture) {
    if (!texture || texture->width <= 0 || texture->height <= 0)
        return 0;
    size_t bytes = 0;
    int w = texture->width, h = texture->height;
    for (;;) {
        switch (texture->internal_format) {
        case GL_COMPRESSED_RED_RGTC1:
            bytes += texture_block_image_bytes(TEXTURE_BLOCK_BC4, w, h);
            break;
        case GL_COMPRESSED_RG_RGTC2:
            bytes += texture_block_image_bytes(TEXTURE_BLOCK_BC5, w, h);
            break;
        case GL_COMPRESSED_SRGB_S3TC_DXT1_EXT:
        case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
            bytes += texture_block_image_bytes(TEXTURE_BLOCK_DXT1, w, h);
            break;
        case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT:
        case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
            bytes += texture_block_image_bytes(TEXTURE_BLOCK_DXT5, w, h);
            break;
        default:
            // Four bytes whatever the channel count: the formats this engine
            // passes are UNSIZED, so the driver picks the storage and every
            // desktop one pads RGB to RGBA. Counting three here would report a
            // saving that does not exist.
            bytes += (size_t)w * (size_t)h * 4;
            break;
        }
        if (w <= 1 && h <= 1)
            break;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    return bytes;
}

static const char* texture_format_name(GLenum internal_format) {
    switch (internal_format) {
    case GL_COMPRESSED_RED_RGTC1:
        return "BC4";
    case GL_COMPRESSED_RG_RGTC2:
        return "BC5";
    case GL_COMPRESSED_SRGB_S3TC_DXT1_EXT:
        return "DXT1-srgb";
    case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
        return "DXT1";
    case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT:
        return "DXT5-srgb";
    case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
        return "DXT5";
    case GL_SRGB:
        return "SRGB";
    case GL_SRGB_ALPHA:
        return "SRGB_ALPHA";
    case GL_RGB:
        return "RGB";
    case GL_RGBA:
        return "RGBA";
    case GL_RG:
        return "RG";
    case GL_RED:
        return "RED";
    default:
        return "?";
    }
}

void texture_pool_probe(const TexturePool* pool, const char* label) {
    if (!pool) {
        printf("texture-probe %s: no pool\n", label ? label : "");
        return;
    }
    size_t total = 0, compressed_total = 0, compressed_count = 0;
    for (size_t i = 0; i < pool->texture_count; i++) {
        const Texture* t = pool->textures[i];
        if (!t)
            continue;
        const size_t bytes = texture_gpu_bytes(t);
        total += bytes;
        // "Compressed" is read off the stored format, so a texture that ASKED
        // for a block format and did not get one is counted honestly.
        const char* name = texture_format_name(t->internal_format);
        const bool is_block = name[0] == 'B' || name[0] == 'D';
        if (is_block) {
            compressed_total += bytes;
            compressed_count++;
        }
        printf("texture-probe %s tex %s %dx%d %s %.3f MB\n", label ? label : "",
               t->filepath ? t->filepath : "?", t->width, t->height, name,
               (double)bytes / (1024.0 * 1024.0));
    }
    printf("texture-probe %s total count=%zu bytes=%zu mb=%.3f compressed_count=%zu "
           "compressed_mb=%.3f\n",
           label ? label : "", pool->texture_count, total, (double)total / (1024.0 * 1024.0),
           compressed_count, (double)compressed_total / (1024.0 * 1024.0));
}

Texture* create_texture() {
    Texture* texture = (Texture*)malloc(sizeof(Texture));
    if (!texture) {
        log_error("Failed to allocate memory for texture");
        return NULL;
    }

    texture->id = 0;
    texture->filepath = NULL;
    texture->width = 0;
    texture->height = 0;
    texture->internal_format = 0;
    texture->data_format = 0;
    // Matches texture_set_default_sampler_state, which is what every upload
    // path applies -- so the recorded value describes the texture object even
    // for the sites that never ask for anything else.
    texture->wrap_s = GL_REPEAT;
    texture->wrap_t = GL_REPEAT;
    texture->ref_count = 1;

    return texture;
}

void texture_apply_wrap(Texture* texture, GLenum wrap_s, GLenum wrap_t) {
    if (!texture || texture->id == 0)
        return;
    if (texture->wrap_s == wrap_s && texture->wrap_t == wrap_t)
        return;
    // The pool caches by filepath, so one image can be reached by materials
    // that disagree. Whoever asks last wins -- but say so, because the loser
    // gets wrap it did not ask for and the symptom (edge bleed on one model
    // and not another sharing the atlas) is not one anybody would guess at.
    if (texture->wrap_s != GL_REPEAT || texture->wrap_t != GL_REPEAT)
        log_warn("Texture %s re-wrapped (0x%x,0x%x -> 0x%x,0x%x); it is shared by materials that "
                 "declare different wrap",
                 texture->filepath ? texture->filepath : "?", texture->wrap_s, texture->wrap_t,
                 wrap_s, wrap_t);

    glBindTexture(GL_TEXTURE_2D, texture->id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (GLint)wrap_s);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (GLint)wrap_t);
    glBindTexture(GL_TEXTURE_2D, 0);
    texture->wrap_s = wrap_s;
    texture->wrap_t = wrap_t;
}

bool texture_mean_color(Texture* texture, float* out_rgb) {
    if (!texture || !out_rgb || texture->id == 0 || texture->width <= 0 || texture->height <= 0)
        return false;

    // Memoized: the readback below is a pipeline stall, and its consumer
    // evaluates candidacy for every mesh every frame. Pixels are never rewritten
    // in place, so a hit can never be stale.
    if (texture->mean_valid) {
        for (int c = 0; c < 3; c++)
            out_rgb[c] = texture->mean_rgb[c];
        return true;
    }

    // The 1x1 top mip IS the mean, which is the whole trick: the chain already
    // exists (every load path calls glGenerateMipmap), so an average that would
    // otherwise need the pixels back on the CPU is one texel already sitting in
    // VRAM. Nothing here keeps a copy -- Texture deliberately holds no data
    // pointer -- so without this the mean is simply unobtainable.
    int longest = texture->width > texture->height ? texture->width : texture->height;
    GLint level = 0;
    while ((longest >> level) > 1)
        level++;

    glBindTexture(GL_TEXTURE_2D, texture->id);

    // Ask the driver rather than trusting the arithmetic: a texture whose chain
    // was never generated answers with something other than 1 and this declines,
    // where reading the level anyway would return uninitialised memory and call
    // it a colour.
    GLint w = 0, h = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_WIDTH, &w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_HEIGHT, &h);
    if (w != 1 || h != 1) {
        glBindTexture(GL_TEXTURE_2D, 0);
        return false;
    }

    unsigned char texel[4] = {0, 0, 0, 255};
    glGetTexImage(GL_TEXTURE_2D, level, GL_RGBA, GL_UNSIGNED_BYTE, texel);
    glBindTexture(GL_TEXTURE_2D, 0);

    // A colour texture is stored sRGB-encoded, and glGetTexImage hands back what
    // is STORED -- no decode, where the sampler would have done one. Filtering an
    // sRGB format is specified to happen in linear space, so the top mip is the
    // encoding of the linear mean and decoding it here recovers that mean.
    bool is_srgb =
        texture->internal_format == GL_SRGB || texture->internal_format == GL_SRGB_ALPHA;
    for (int c = 0; c < 3; c++) {
        float v = (float)texel[c] / 255.0f;
        out_rgb[c] = is_srgb ? (v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f)) : v;
        texture->mean_rgb[c] = out_rgb[c];
    }
    texture->mean_valid = true;
    return true;
}

void free_texture(Texture* texture) {
    if (texture) {
        glDeleteTextures(1, &(texture->id));
        if (texture->filepath) {
            free(texture->filepath);
            texture->filepath = NULL;
        }
        free(texture);
    }
}

Texture* texture_retain(Texture* texture) {
    if (texture) {
        texture->ref_count++;
    }
    return texture;
}

void texture_release(Texture* texture) {
    if (texture) {
        if (texture->ref_count > 0) {
            texture->ref_count--;
        }
        if (texture->ref_count == 0) {
            free_texture(texture);
        }
    }
}

void set_texture_width(Texture* texture, int width) {
    if (texture) {
        texture->width = width;
    }
}

void set_texture_height(Texture* texture, int height) {
    if (texture) {
        texture->height = height;
    }
}

void set_texture_internal_format(Texture* texture, GLenum internal_format) {
    if (texture) {
        texture->internal_format = internal_format;
    }
}

void set_texture_data_format(Texture* texture, GLenum data_format) {
    if (texture) {
        texture->data_format = data_format;
    }
}

/*
 * Texture Pool
 *
 */

TexturePool* create_texture_pool() {
    TexturePool* pool = (TexturePool*)malloc(sizeof(TexturePool));
    if (!pool) {
        log_error("Failed to allocate memory for TexturePool");
        return NULL;
    }

    pool->directory = NULL;
    pool->textures = NULL;
    pool->texture_count = 0;
    pool->texture_cache = NULL;

    if (!cetra_mutex_init(&pool->cache_mutex)) {
        log_error("Failed to init cache_mutex");
        free(pool);
        return NULL;
    }

    return pool;
}

void free_texture_pool(TexturePool* pool) {
    if (pool) {
        if (pool->directory) {
            free(pool->directory);
        }

        // Only free the array of pointers, not the textures themselves
        free(pool->textures);

        // This will handle freeing of all Texture objects
        clear_texture_pool(pool);

        cetra_mutex_destroy(&pool->cache_mutex);

        free(pool);
    }
}

void set_texture_pool_directory(TexturePool* pool, const char* directory) {
    if (!pool)
        return;

    if (directory) {
        log_info("Setting texture directory to: '%s'", directory);

        if (pool->directory != NULL) {
            free(pool->directory);
        }

        pool->directory = safe_strdup(directory);

        if (!pool->directory) {
            log_error("Failed to allocate memory for directory string");
        }
    } else {
        pool->directory = NULL;
    }
}

Texture* get_texture_from_pool(TexturePool* pool, const char* filepath) {
    if (pool && filepath) {
        Texture* found;
        HASH_FIND_STR(pool->texture_cache, filepath, found);
        return found;
    }
    return NULL;
}

void add_texture_to_pool(TexturePool* pool, Texture* texture) {
    if (pool && texture && texture->filepath) {
        // Add to dynamic array
        pool->textures = realloc(pool->textures, (pool->texture_count + 1) * sizeof(Texture*));
        if (!pool->textures) {
            log_error("Failed to reallocate memory for textures array");
            return;
        }
        pool->textures[pool->texture_count++] = texture;

        // Add to cache
        Texture* existing;
        HASH_FIND_STR(pool->texture_cache, texture->filepath, existing);
        if (!existing) {
            HASH_ADD_KEYPTR(hh, pool->texture_cache, texture->filepath, strlen(texture->filepath),
                            texture);
        }
    }
}

Texture* load_texture_path_into_pool(TexturePool* pool, const char* filepath, bool is_srgb) {
    // The historical correlation, kept as the default: a colour texture is the
    // one with an opacity in alpha.
    return load_texture_path_into_pool_ex(pool, filepath, is_srgb,
                                          is_srgb ? TEXTURE_ALPHA_OPACITY : TEXTURE_ALPHA_DATA);
}

Texture* load_texture_path_into_pool_ex(TexturePool* pool, const char* filepath, bool is_srgb,
                                        TextureAlpha alpha) {
    // Unstated use means COLOUR, which compresses nothing -- so a caller that has
    // not been taught the distinction keeps exactly the storage it had.
    return load_texture_path_into_pool_used(pool, filepath, is_srgb, alpha, TEXTURE_USE_COLOUR);
}

Texture* load_texture_path_into_pool_used(TexturePool* pool, const char* filepath, bool is_srgb,
                                          TextureAlpha alpha, TextureUse use) {
    if (!pool || !filepath) {
        log_error("Invalid pool or filepath");
        return NULL;
    }

    if (pool->directory == NULL) {
        log_error("Texture pool directory not set");
        return NULL;
    }

    // Normalize and work on a copy of the filepath
    char* normalized_path = convert_and_normalize_path(filepath);
    if (!normalized_path) {
        log_error("Failed to normalize path: '%s'", filepath);
        return NULL;
    }

    char* subpath = safe_strdup(normalized_path);
    if (!subpath) {
        log_error("Memory allocation failed for subpath.");
        free(normalized_path);
        return NULL;
    }

    // Use find_existing_subpath to find a valid subpath
    if (!find_existing_subpath(pool->directory, &subpath)) {
        log_error("No valid subpath found for texture: '%s'", subpath);
        free(normalized_path);
        free(subpath);
        return NULL;
    }

    GLuint textureID = 0;
    int width, height, nrChannels;

    Texture* cached_texture = get_texture_from_pool(pool, subpath);
    if (cached_texture) {
        /*
         * The pool keys on PATH, so a second consumer wanting the same file in a
         * different colour space gets the first one's decision silently. That
         * was harmless while `is_srgb` tracked what a slot was FOR; a decal
         * broke it, being colour data that loads LINEAR because it lives in the
         * material array.
         *
         * Warned by name rather than re-keyed: two entries would double the
         * VRAM for one image, and the honest answer is that a scene wanting one
         * file as both a lit albedo and a decal wants two files. Silence here
         * renders one of the two consumers a full sRGB decode wrong -- markedly
         * too dark or too bright -- with nothing to read.
         */
        const bool cached_srgb = cached_texture->internal_format == GL_SRGB ||
                                 cached_texture->internal_format == GL_SRGB_ALPHA;
        if (cached_srgb != is_srgb)
            log_warn("texture '%s' is already loaded as %s and is now wanted as %s; "
                     "the pool keys on path, so the first load wins",
                     subpath, cached_srgb ? "sRGB" : "linear", is_srgb ? "sRGB" : "linear");
        free(normalized_path);
        free(subpath);
        return cached_texture;
    }

    unsigned char* data = stbi_load(subpath, &width, &height, &nrChannels, 0);
    if (!data) {
        log_error("Failed to load texture: %s", subpath);
        free(normalized_path);
        free(subpath);
        return NULL;
    }

    // Only where alpha is an OPACITY. The dilate repairs the rgb of transparent
    // texels so a cutout's edge does not bleed the atlas background, and callers
    // that do not say default this to `is_srgb`, which is exactly "this is colour
    // data" (import.c). A linear RGBA texture's alpha usually means something
    // else: spec 11.60 stores a layer's HEIGHT there and its ambient occlusion in
    // the surface map's, and dilating those overwrites the albedo, the packed
    // normal and the roughness of every texel whose relief dips below 3.1%.
    if (nrChannels == 4 && alpha == TEXTURE_ALPHA_OPACITY) {
        texture_dilate_transparent_rgb(data, width, height);
    }

    Texture* new_texture = create_texture();

    if (!new_texture) {
        stbi_image_free(data);
        free(normalized_path);
        free(subpath);
        return NULL;
    }

    // Generate texture
    glGenTextures(1, &textureID);
    glBindTexture(GL_TEXTURE_2D, textureID);

    texture_set_default_sampler_state();

    // Determine format
    GLenum internal_format;
    GLenum data_format;
    texture_gl_formats(nrChannels, is_srgb, &internal_format, &data_format);

    // Upload texture data
    texture_upload_image(internal_format, data_format, width, height, nrChannels, is_srgb, use,
                         data, &internal_format);
    check_gl_error("texture upload");

    // Clean up
    stbi_image_free(data);

    // Update texture properties
    new_texture->id = textureID;
    new_texture->filepath = safe_strdup(subpath);
    new_texture->width = width;
    new_texture->height = height;
    new_texture->internal_format = internal_format;
    new_texture->data_format = data_format;

    // Add texture to the pool
    add_texture_to_pool(pool, new_texture);
    glBindTexture(GL_TEXTURE_2D, 0);

    free(normalized_path);
    free(subpath);

    return new_texture;
}

Texture* load_texture_from_memory(TexturePool* pool, const char* key, const unsigned char* pixels,
                                  int width, int height, int channels, bool is_srgb) {
    return load_texture_from_memory_used(pool, key, pixels, width, height, channels, is_srgb,
                                        TEXTURE_USE_COLOUR);
}

Texture* load_texture_from_memory_used(TexturePool* pool, const char* key,
                                       const unsigned char* pixels, int width, int height,
                                       int channels, bool is_srgb, TextureUse use) {
    if (!pool || !key || !pixels) {
        log_error("Invalid pool, key, or pixel data");
        return NULL;
    }

    // Check cache first
    Texture* cached_texture = get_texture_from_pool(pool, key);
    if (cached_texture) {
        return cached_texture;
    }

    Texture* new_texture = create_texture();
    if (!new_texture) {
        return NULL;
    }

    // RGBA COLOUR sources need their transparent texels' color repaired; work on
    // a mutable copy since the caller owns the pixel data.
    //
    // Still INFERRED from is_srgb here, where the file loader above takes the
    // decision as a parameter. That is a real gap rather than a considered
    // split: an app building a decal image procedurally -- which is how
    // apps/tree and apps/forest make all of theirs -- gets no dilate and no way
    // to ask for one. It wants the same `_ex` treatment the moment something
    // needs it.
    unsigned char* dilated = NULL;
    if (channels == 4 && is_srgb) {
        size_t size = (size_t)width * (size_t)height * 4;
        dilated = malloc(size);
        if (dilated) {
            memcpy(dilated, pixels, size);
            texture_dilate_transparent_rgb(dilated, width, height);
        }
    }

    // Generate texture
    GLuint textureID = 0;
    glGenTextures(1, &textureID);
    glBindTexture(GL_TEXTURE_2D, textureID);

    texture_set_default_sampler_state();

    // Determine format
    GLenum internal_format;
    GLenum data_format;
    texture_gl_formats(channels, is_srgb, &internal_format, &data_format);

    // Upload texture data.
    //
    // TEXTURE_USE_COLOUR, i.e. uncompressed, and for the same reason this path
    // still INFERS its dilate above: it takes no statement of what the image is.
    // Every embedded and procedurally-built texture therefore keeps the storage
    // it had. It wants the `_used` treatment the moment a procedural normal map
    // is worth compressing -- apps/forest bakes several.
    texture_upload_image(internal_format, data_format, width, height, channels, is_srgb, use,
                         dilated ? dilated : pixels, &internal_format);
    free(dilated);
    check_gl_error("embedded texture upload");

    // Update texture properties
    new_texture->id = textureID;
    new_texture->filepath = safe_strdup(key);
    new_texture->width = width;
    new_texture->height = height;
    new_texture->internal_format = internal_format;
    new_texture->data_format = data_format;

    // Add texture to the pool
    add_texture_to_pool(pool, new_texture);
    glBindTexture(GL_TEXTURE_2D, 0);

    log_info("Loaded embedded texture '%s' (%dx%d, %d channels)", key, width, height, channels);

    return new_texture;
}

Texture* load_texture_from_memory_owned(TexturePool* pool, const char* key, unsigned char* pixels,
                                        int width, int height, int channels, bool is_srgb) {
    return load_texture_from_memory_owned_used(pool, key, pixels, width, height, channels, is_srgb,
                                               TEXTURE_USE_COLOUR);
}

Texture* load_texture_from_memory_owned_used(TexturePool* pool, const char* key,
                                             unsigned char* pixels, int width, int height,
                                             int channels, bool is_srgb, TextureUse use) {
    if (!pixels)
        return NULL;
    Texture* tex =
        load_texture_from_memory_used(pool, key, pixels, width, height, channels, is_srgb, use);
    free(pixels);
    return tex;
}

void remove_texture_from_pool(TexturePool* pool, const char* filepath) {
    if (pool && filepath) {
        Texture* to_remove = NULL;

        // Find and remove from cache (don't free yet)
        HASH_FIND_STR(pool->texture_cache, filepath, to_remove);
        if (to_remove) {
            HASH_DEL(pool->texture_cache, to_remove);
        }

        // Remove from dynamic array (swap with last, don't free yet)
        for (size_t i = 0; i < pool->texture_count; i++) {
            if (strcmp(pool->textures[i]->filepath, filepath) == 0) {
                pool->textures[i] = pool->textures[pool->texture_count - 1];
                pool->texture_count--;
                break;
            }
        }

        // Release the pool's reference (frees if ref_count reaches 0)
        if (to_remove) {
            texture_release(to_remove);
        }
    }
}

void clear_texture_pool(TexturePool* pool) {
    if (pool && pool->texture_cache) {
        Texture *current, *tmp;
        HASH_ITER(hh, pool->texture_cache, current, tmp) {
            Texture* to_release = current;
            // NOLINTNEXTLINE(clang-analyzer-unix.Malloc) - uthash pattern, tmp holds next before
            // delete
            HASH_DEL(pool->texture_cache, current);
            texture_release(to_release);
        }
        pool->texture_cache = NULL;
    }
}

/*
 * Thread-safe variants for async loading
 */

Texture* get_texture_from_pool_threadsafe(TexturePool* pool, const char* filepath) {
    if (!pool || !filepath) {
        return NULL;
    }

    cetra_mutex_lock(&pool->cache_mutex);
    Texture* found;
    HASH_FIND_STR(pool->texture_cache, filepath, found);
    cetra_mutex_unlock(&pool->cache_mutex);

    return found;
}

void add_texture_to_pool_threadsafe(TexturePool* pool, Texture* texture) {
    if (!pool || !texture || !texture->filepath) {
        return;
    }

    cetra_mutex_lock(&pool->cache_mutex);

    // Check if already exists
    Texture* existing;
    HASH_FIND_STR(pool->texture_cache, texture->filepath, existing);
    if (!existing) {
        // Add to dynamic array
        pool->textures = realloc(pool->textures, (pool->texture_count + 1) * sizeof(Texture*));
        if (pool->textures) {
            pool->textures[pool->texture_count++] = texture;

            // Add to cache
            HASH_ADD_KEYPTR(hh, pool->texture_cache, texture->filepath, strlen(texture->filepath),
                            texture);
        } else {
            log_error("Failed to reallocate memory for textures array");
        }
    }

    cetra_mutex_unlock(&pool->cache_mutex);
}

void remove_texture_from_pool_threadsafe(TexturePool* pool, const char* filepath) {
    if (!pool || !filepath) {
        return;
    }

    cetra_mutex_lock(&pool->cache_mutex);

    Texture* to_remove = NULL;

    // Find and remove from cache
    HASH_FIND_STR(pool->texture_cache, filepath, to_remove);
    if (to_remove) {
        HASH_DEL(pool->texture_cache, to_remove);
    }

    // Remove from dynamic array (swap with last)
    for (size_t i = 0; i < pool->texture_count; i++) {
        if (strcmp(pool->textures[i]->filepath, filepath) == 0) {
            pool->textures[i] = pool->textures[pool->texture_count - 1];
            pool->texture_count--;
            break;
        }
    }

    cetra_mutex_unlock(&pool->cache_mutex);

    // Release reference outside the lock to avoid potential deadlock
    if (to_remove) {
        texture_release(to_remove);
    }
}
