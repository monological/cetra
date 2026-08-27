#include "texture_compress.h"

#include <string.h>

size_t texture_block_bytes(TextureBlockFormat format) {
    switch (format) {
    case TEXTURE_BLOCK_BC4:
    case TEXTURE_BLOCK_DXT1:
        return 8;
    case TEXTURE_BLOCK_BC5:
    case TEXTURE_BLOCK_DXT5:
        return 16;
    default:
        return 0;
    }
}

int texture_block_channels(TextureBlockFormat format) {
    switch (format) {
    case TEXTURE_BLOCK_BC4:
        return 1;
    case TEXTURE_BLOCK_BC5:
        return 2;
    case TEXTURE_BLOCK_DXT1:
        return 3;
    case TEXTURE_BLOCK_DXT5:
        return 4;
    default:
        return 0;
    }
}

size_t texture_block_image_bytes(TextureBlockFormat format, int width, int height) {
    const size_t bytes = texture_block_bytes(format);
    if (bytes == 0 || width <= 0 || height <= 0)
        return 0;
    const size_t bw = ((size_t)width + 3) / 4;
    const size_t bh = ((size_t)height + 3) / 4;
    return bw * bh * bytes;
}

// Gather one 4x4 tile, replicating the last real row/column where the image does
// not divide by four. See the header for why replication rather than zero fill.
static void gather_block(const unsigned char* src, int width, int height, int channels, int bx,
                         int by, unsigned char tile[16 * 4]) {
    for (int y = 0; y < 4; y++) {
        int sy = by * 4 + y;
        if (sy >= height)
            sy = height - 1;
        for (int x = 0; x < 4; x++) {
            int sx = bx * 4 + x;
            if (sx >= width)
                sx = width - 1;
            const unsigned char* p =
                &src[((size_t)sy * (size_t)width + (size_t)sx) * (size_t)channels];
            unsigned char* q = &tile[(size_t)(y * 4 + x) * 4];
            q[0] = p[0];
            q[1] = channels > 1 ? p[1] : p[0];
            q[2] = channels > 2 ? p[2] : p[0];
            q[3] = channels > 3 ? p[3] : 255;
        }
    }
}

/*
 * One BC4 block: a single channel as two endpoints plus sixteen 3-bit indices.
 *
 * Always the EIGHT-value mode (a0 > a1), which spends all six interpolants on
 * the block's own range. The six-value mode reserves two indices for hard 0 and
 * 255, which only pays on data that genuinely wants those exact values -- a mask
 * with a real range loses two of its eight steps for nothing.
 *
 * A constant block takes a0 == a1 and index 0 everywhere, which both modes decode
 * to that value, so it stays exact without a special case.
 */
static void encode_bc4_channel(const unsigned char tile[16 * 4], int channel,
                               unsigned char out[8]) {
    unsigned char lo = 255, hi = 0;
    for (int i = 0; i < 16; i++) {
        const unsigned char v = tile[i * 4 + channel];
        if (v < lo)
            lo = v;
        if (v > hi)
            hi = v;
    }

    out[0] = hi;
    out[1] = lo;

    // The palette this block's indices address, in index order.
    unsigned char pal[8];
    pal[0] = hi;
    pal[1] = lo;
    if (hi > lo) {
        for (int i = 0; i < 6; i++)
            pal[2 + i] = (unsigned char)(((6 - i) * (int)hi + (1 + i) * (int)lo + 3) / 7);
    } else {
        // Constant block: every entry is the same value, so any index decodes
        // correctly and the loop below picks 0.
        for (int i = 0; i < 6; i++)
            pal[2 + i] = hi;
    }

    unsigned long long bits = 0;
    for (int i = 0; i < 16; i++) {
        const int v = tile[i * 4 + channel];
        int best = 0, best_err = 256 * 256;
        for (int p = 0; p < 8; p++) {
            const int d = v - (int)pal[p];
            const int err = d * d;
            if (err < best_err) {
                best_err = err;
                best = p;
            }
        }
        bits |= (unsigned long long)best << (3 * i);
    }
    for (int i = 0; i < 6; i++)
        out[2 + i] = (unsigned char)((bits >> (8 * i)) & 0xFF);
}

static unsigned short pack565(int r, int g, int b) {
    return (unsigned short)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static void unpack565(unsigned short c, int rgb[3]) {
    // Replicate the high bits into the low ones, which is what the hardware does
    // when it expands 5/6 bits back to 8. Truncating instead biases every decoded
    // endpoint dark and the whole image with it.
    const int r = (c >> 11) & 0x1F;
    const int g = (c >> 5) & 0x3F;
    const int b = c & 0x1F;
    rgb[0] = (r << 3) | (r >> 2);
    rgb[1] = (g << 2) | (g >> 4);
    rgb[2] = (b << 3) | (b >> 2);
}

/*
 * The colour half of a DXT block: two RGB565 endpoints and sixteen 2-bit indices.
 *
 * Endpoints come from the bounding box of the block's colours, then one
 * least-squares refinement along the resulting line. Bounding box alone is the
 * classic fast encoder and is visibly worse on any block whose colours do not
 * run corner to corner; the refit costs one extra pass over sixteen texels and
 * recovers most of what a full principal-axis search would.
 *
 * `force_four` is set for DXT5, whose colour block is always four-colour. For
 * DXT1 the ordering of the endpoints IS the mode bit, so c0 must be made greater
 * than c1 or the decoder reads a three-colour block with a transparent index.
 */
static void encode_dxt_colour(const unsigned char tile[16 * 4], bool force_four,
                              unsigned char out[8]) {
    int lo[3] = {255, 255, 255};
    int hi[3] = {0, 0, 0};
    for (int i = 0; i < 16; i++) {
        for (int c = 0; c < 3; c++) {
            const int v = tile[i * 4 + c];
            if (v < lo[c])
                lo[c] = v;
            if (v > hi[c])
                hi[c] = v;
        }
    }

    float e0[3], e1[3];
    for (int c = 0; c < 3; c++) {
        e0[c] = (float)hi[c];
        e1[c] = (float)lo[c];
    }

    // Least-squares refit: project each texel onto the current line, then solve
    // for the endpoints that best explain the projections.
    for (int pass = 0; pass < 2; pass++) {
        float dir[3];
        float len2 = 0.0f;
        for (int c = 0; c < 3; c++) {
            dir[c] = e0[c] - e1[c];
            len2 += dir[c] * dir[c];
        }
        if (len2 < 1e-6f)
            break;

        float sw = 0.0f, sww = 0.0f, sp[3] = {0.0f, 0.0f, 0.0f}, spw[3] = {0.0f, 0.0f, 0.0f};
        for (int i = 0; i < 16; i++) {
            float t = 0.0f;
            for (int c = 0; c < 3; c++)
                t += ((float)tile[i * 4 + c] - e1[c]) * dir[c];
            t /= len2;
            if (t < 0.0f)
                t = 0.0f;
            if (t > 1.0f)
                t = 1.0f;
            // Snap to the four representable steps so the fit solves for the
            // palette that will actually be used rather than a continuous line.
            t = (float)((int)(t * 3.0f + 0.5f)) / 3.0f;
            sw += t;
            sww += t * t;
            for (int c = 0; c < 3; c++) {
                sp[c] += (float)tile[i * 4 + c];
                spw[c] += (float)tile[i * 4 + c] * t;
            }
        }
        const float det = 16.0f * sww - sw * sw;
        if (det > -1e-6f && det < 1e-6f)
            break;
        for (int c = 0; c < 3; c++) {
            float a = (16.0f * spw[c] - sw * sp[c]) / det;
            float b = (sww * sp[c] - sw * spw[c]) / det;
            float n0 = b + a;
            float n1 = b;
            if (n0 < 0.0f)
                n0 = 0.0f;
            if (n0 > 255.0f)
                n0 = 255.0f;
            if (n1 < 0.0f)
                n1 = 0.0f;
            if (n1 > 255.0f)
                n1 = 255.0f;
            e0[c] = n0;
            e1[c] = n1;
        }
    }

    unsigned short c0 = pack565((int)(e0[0] + 0.5f), (int)(e0[1] + 0.5f), (int)(e0[2] + 0.5f));
    unsigned short c1 = pack565((int)(e1[0] + 0.5f), (int)(e1[1] + 0.5f), (int)(e1[2] + 0.5f));

    bool swapped = false;
    if (!force_four && c0 < c1) {
        const unsigned short t = c0;
        c0 = c1;
        c1 = t;
        swapped = true;
    }
    if (!force_four && c0 == c1) {
        // Equal endpoints select the three-colour mode, whose index 3 is
        // transparent black. Index 0 decodes to c0 for both modes, and the loop
        // below will choose it for every texel of what is by definition a
        // constant block, so this is safe rather than merely tolerated.
    }

    int p0[3], p1[3];
    unpack565(c0, p0);
    unpack565(c1, p1);

    int pal[4][3];
    for (int c = 0; c < 3; c++) {
        pal[0][c] = p0[c];
        pal[1][c] = p1[c];
        if (force_four || c0 > c1) {
            pal[2][c] = (2 * p0[c] + p1[c] + 1) / 3;
            pal[3][c] = (p0[c] + 2 * p1[c] + 1) / 3;
        } else {
            pal[2][c] = (p0[c] + p1[c]) / 2;
            pal[3][c] = 0;
        }
    }
    (void)swapped;

    unsigned int bits = 0;
    for (int i = 0; i < 16; i++) {
        int best = 0, best_err = 1 << 30;
        const int limit = (force_four || c0 > c1) ? 4 : 3;
        for (int p = 0; p < limit; p++) {
            int err = 0;
            for (int c = 0; c < 3; c++) {
                const int d = (int)tile[i * 4 + c] - pal[p][c];
                err += d * d;
            }
            if (err < best_err) {
                best_err = err;
                best = p;
            }
        }
        bits |= (unsigned int)best << (2 * i);
    }

    out[0] = (unsigned char)(c0 & 0xFF);
    out[1] = (unsigned char)(c0 >> 8);
    out[2] = (unsigned char)(c1 & 0xFF);
    out[3] = (unsigned char)(c1 >> 8);
    for (int i = 0; i < 4; i++)
        out[4 + i] = (unsigned char)((bits >> (8 * i)) & 0xFF);
}

void texture_block_encode(TextureBlockFormat format, const unsigned char* src, int width,
                          int height, int channels, unsigned char* dst) {
    const size_t block = texture_block_bytes(format);
    if (block == 0 || !src || !dst || width <= 0 || height <= 0)
        return;

    const int bw = (width + 3) / 4;
    const int bh = (height + 3) / 4;
    unsigned char tile[16 * 4];

    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            gather_block(src, width, height, channels, bx, by, tile);
            unsigned char* out = &dst[((size_t)by * (size_t)bw + (size_t)bx) * block];
            switch (format) {
            case TEXTURE_BLOCK_BC4:
                encode_bc4_channel(tile, 0, out);
                break;
            case TEXTURE_BLOCK_BC5:
                // Two independent BC4 blocks, red then green. Independence is the
                // point: a normal's X and Y have no shared range to exploit, and
                // giving each its own endpoints is what makes this the format for
                // tangent normals rather than a colour format pressed into it.
                encode_bc4_channel(tile, 0, out);
                encode_bc4_channel(tile, 1, out + 8);
                break;
            case TEXTURE_BLOCK_DXT1:
                encode_dxt_colour(tile, false, out);
                break;
            case TEXTURE_BLOCK_DXT5:
                encode_bc4_channel(tile, 3, out);
                encode_dxt_colour(tile, true, out + 8);
                break;
            default:
                break;
            }
        }
    }
}
