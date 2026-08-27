#include "texture_compress.h"

#include <stdbool.h>
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

size_t texture_block_image_bytes(TextureBlockFormat format, int width, int height) {
    const size_t bytes = texture_block_bytes(format);
    if (bytes == 0 || width <= 0 || height <= 0)
        return 0;
    const size_t bw = ((size_t)width + 3) / 4;
    const size_t bh = ((size_t)height + 3) / 4;
    return bw * bh * bytes;
}

// What a format needs of its source. The gather replicates red into missing
// channels, which is silent and wrong for the two-channel formats -- see the
// header. Checked rather than assumed, because the caller that gets it wrong
// renders a plausible picture.
static int texture_block_min_channels(TextureBlockFormat format) {
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
    // At hi == lo this yields (7*hi + 3)/7 == hi, so a constant block needs no
    // case of its own.
    for (int i = 0; i < 6; i++)
        pal[2 + i] = (unsigned char)(((6 - i) * (int)hi + (1 + i) * (int)lo + 3) / 7);

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

// Rounded integer division, away from zero on both signs. The float refit
// rounded when it packed its endpoints; truncating instead biases every one of
// them toward zero and darkens the image.
static int idiv_round(int num, int den) {
    return num >= 0 ? (num + den / 2) / den : -((-num + den / 2) / den);
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

    // Endpoints as INTEGERS, and the refit below in integer arithmetic, because
    // this file's header states determinism as a requirement: the frames these
    // bytes feed are compared byte for byte, and a float refit is not that. The
    // original float form differed by 64 bytes between an -O0 and an -O2 build of
    // the same source -- so out/bin and out/release/bin would have produced
    // different textures, the "two builds is not two runs" trap this repo already
    // documents for meshoptimizer's LOD chains. BC4 and BC5 never had it, being
    // integer throughout.
    //
    // Every intermediate is bounded well inside int32: with weights in {0..3}
    // over 16 texels, sw <= 48, sww <= 144, sp <= 4080, spw <= 12240, and the
    // largest product below 600k.
    int e0[3], e1[3];
    for (int c = 0; c < 3; c++) {
        e0[c] = hi[c];
        e1[c] = lo[c];
    }

    // Least-squares refit: project each texel onto the current line, then solve
    // for the endpoints that best explain the projections.
    for (int pass = 0; pass < 2; pass++) {
        int dir[3];
        int len2 = 0;
        for (int c = 0; c < 3; c++) {
            dir[c] = e0[c] - e1[c];
            len2 += dir[c] * dir[c];
        }
        if (len2 == 0)
            break;

        // Weights are the palette's own four steps held as 0..3 rather than as
        // thirds, which is what keeps the solve exact. The factor of 3 rides in
        // the numerators instead.
        int sw = 0, sww = 0, sp[3] = {0, 0, 0}, spw[3] = {0, 0, 0};
        for (int i = 0; i < 16; i++) {
            int num = 0;
            for (int c = 0; c < 3; c++)
                num += ((int)tile[i * 4 + c] - e1[c]) * dir[c];
            // round(3 * num / len2), away from zero on both signs so the snap is
            // symmetric, then clamped to the four steps.
            int w = num >= 0 ? (3 * num + len2 / 2) / len2 : -((-3 * num + len2 / 2) / len2);
            if (w < 0)
                w = 0;
            if (w > 3)
                w = 3;
            sw += w;
            sww += w * w;
            for (int c = 0; c < 3; c++) {
                sp[c] += (int)tile[i * 4 + c];
                spw[c] += (int)tile[i * 4 + c] * w;
            }
        }
        // Nine times the float form's determinant, which cancels against the 3s
        // the weights carry. Exactly zero or at least 15, so no epsilon.
        const int den = 16 * sww - sw * sw;
        if (den == 0)
            break;
        for (int c = 0; c < 3; c++) {
            // Substituting w = 3t into the least-squares solution puts a factor
            // of 3 on the SLOPE and none on the intercept -- the 9s from sww and
            // the determinant cancel. Carrying it on both reads as symmetry and
            // is wrong by 3x on the intercept.
            const int a_num = 3 * (16 * spw[c] - sw * sp[c]);
            const int b_num = sww * sp[c] - sw * spw[c];
            int n0 = idiv_round(b_num + a_num, den);
            int n1 = idiv_round(b_num, den);
            if (n0 < 0)
                n0 = 0;
            if (n0 > 255)
                n0 = 255;
            if (n1 < 0)
                n1 = 0;
            if (n1 > 255)
                n1 = 255;
            e0[c] = n0;
            e1[c] = n1;
        }
    }

    unsigned short c0 = pack565(e0[0], e0[1], e0[2]);
    unsigned short c1 = pack565(e1[0], e1[1], e1[2]);

    // For DXT1 the ORDER of the endpoints is the mode bit, so c0 must be made the
    // greater to select the four-colour mode. The indices below are chosen
    // against the palette built after this, so the swap needs no fixup of its
    // own.
    //
    // Equal endpoints are left alone: they select the three-colour mode, whose
    // index 3 is transparent black -- but index 0 decodes to c0 under both modes
    // and the loop below picks it for every texel of what is by definition a
    // constant block. Safe rather than merely tolerated.
    if (!force_four && c0 < c1) {
        const unsigned short t = c0;
        c0 = c1;
        c1 = t;
    }

    int p0[3], p1[3];
    unpack565(c0, p0);
    unpack565(c1, p1);

    // The swap above makes c0 >= c1 whenever !force_four, so the three-colour
    // mode is reachable only at c0 == c1 -- where p0 == p1 and every palette
    // entry is the same colour under either mode. Only the INDEX LIMIT below has
    // to know, and it has to know because index 3 there is transparent black.
    int pal[4][3];
    for (int c = 0; c < 3; c++) {
        pal[0][c] = p0[c];
        pal[1][c] = p1[c];
        pal[2][c] = (2 * p0[c] + p1[c] + 1) / 3;
        pal[3][c] = (p0[c] + 2 * p1[c] + 1) / 3;
    }
    // 1 rather than 3: at collapsed endpoints index 0 is the answer, and this
    // makes transparent black structurally unreachable instead of merely losing
    // a tie to a strict less-than.
    const int limit = (!force_four && c0 == c1) ? 1 : 4;

    unsigned int bits = 0;
    for (int i = 0; i < 16; i++) {
        int best = 0, best_err = 1 << 30;
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
    if (block == 0 || !src || !dst || width <= 0 || height <= 0 ||
        channels < texture_block_min_channels(format))
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
