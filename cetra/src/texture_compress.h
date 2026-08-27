#ifndef _TEXTURE_COMPRESS_H_
#define _TEXTURE_COMPRESS_H_

#include <stdbool.h>
#include <stddef.h>

/*
 * Block compression for GPU textures (spec 11.85).
 *
 * A block format stores each 4x4 tile as 8 or 16 bytes and the texture hardware
 * decodes a tile as it samples it, so the image never expands in memory. That is
 * the whole point: an uncompressed texture costs its full texel count forever,
 * where a compressed one costs a quarter of it and moves a quarter of the
 * bandwidth per lookup.
 *
 * WHICH FORMATS EXIST HERE IS A PROPERTY OF THE PLATFORM, NOT A PREFERENCE.
 * Apple's GL 4.1 advertises one compression extension, S3TC. BPTC (BC7, the
 * format any modern plan reaches for first) is core in GL 4.2 and is exposed by
 * no extension here -- an upload returns GL_INVALID_ENUM. RGTC (BC4/BC5) reads
 * as absent for the opposite reason, being core since GL 3.0 and carrying no
 * extension string at all, and works. Verified by probe, not by reading version
 * numbers.
 *
 * Nothing here touches GL. These are pure functions over pixel buffers so the
 * encoder can be tested, threaded and moved into an offline cook (F2) without
 * dragging a context along.
 */

typedef enum TextureBlockFormat {
    TEXTURE_BLOCK_NONE = 0, // store uncompressed
    TEXTURE_BLOCK_BC4,      // one channel, 8 bytes/block  -- masks
    // Two channels, 16 bytes/block. The third is REBUILT by the shader: no block
    // format here carries three, so a compressed tangent normal always costs a
    // consumer that knows to rebuild Z.
    TEXTURE_BLOCK_BC5,
    TEXTURE_BLOCK_DXT1, // rgb, 8 bytes/block
    TEXTURE_BLOCK_DXT5, // rgb + a, 16 bytes/block
} TextureBlockFormat;

// Bytes one 4x4 block occupies. 0 for TEXTURE_BLOCK_NONE.
size_t texture_block_bytes(TextureBlockFormat format);

// Size of the encoded image, over ceil(w/4) x ceil(h/4) blocks.
size_t texture_block_image_bytes(TextureBlockFormat format, int width, int height);

/*
 * Encode a whole image. `src` is `channels` bytes per texel, row-major, tightly
 * packed; `dst` must hold texture_block_image_bytes().
 *
 * Dimensions need not be multiples of 4. A partial edge block REPLICATES its
 * last row and column rather than padding with zero, so the block's endpoints
 * are fitted to real texels -- padding with black drags an edge block's whole
 * palette toward it and prints a dark fringe along the right and bottom of every
 * non-multiple-of-4 texture.
 *
 * Deterministic: the same input gives the same bytes on any machine and at any
 * optimisation level. That is a requirement rather than a nicety, because the
 * frames these feed are compared byte for byte.
 */
void texture_block_encode(TextureBlockFormat format, const unsigned char* src, int width,
                          int height, int channels, unsigned char* dst);

#endif // _TEXTURE_COMPRESS_H_
