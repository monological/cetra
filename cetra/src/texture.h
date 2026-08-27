
#ifndef TEXTURE_H
#define TEXTURE_H

#include <GL/glew.h>
#include <stdbool.h>

#include "ext/uthash.h"
#include "thread.h"

/*
 * Texture
 */
typedef struct Texture {
    GLuint id;              // OpenGL texture ID
    char* filepath;         // File path of the texture
    int width;              // Width of the texture
    int height;             // Height of the texture
    GLenum internal_format; // This is the format of the texture object in OpenGL (e.g., GL_RGB,
                            // GL_RGBA)
    GLenum data_format;     // This is the format of the texture data (e.g., GL_RGB, GL_RGBA)
    // Wrap as the ASSET declared it, GL_REPEAT until something says otherwise.
    // Recorded rather than write-only because the pool caches by filepath: two
    // materials can reach the same image wanting different wrap, and the
    // recorded value is what lets that be detected instead of silently
    // last-one-wins.
    GLenum wrap_s;
    GLenum wrap_t;

    // The image's mean colour, LINEAR, memoized on first read. Pixels are never
    // rewritten in place -- the async loader creates a new Texture per landed
    // load -- so once true this can never go stale.
    //
    // Memoized rather than recomputed because the read is a synchronous
    // glGetTexImage, and its one consumer turned out to be calling it per mesh
    // per frame. A cache is what makes "never per frame" a property of the
    // function instead of advice its caller has to remember.
    float mean_rgb[3];
    bool mean_valid;

    size_t ref_count; // Reference count for shared ownership

    UT_hash_handle hh; // Makes this structure hashable
} Texture;

// malloc
Texture* create_texture();
void free_texture(Texture* texture);

// reference counting
Texture* texture_retain(Texture* texture);
void texture_release(Texture* texture);

void set_texture_width(Texture* texture, int width);
void set_texture_height(Texture* texture, int height);
void set_texture_internal_format(Texture* texture, GLenum internal_format);
void set_texture_data_format(Texture* texture, GLenum data_format);

// Re-point an already-uploaded texture's wrap at what the asset asked for.
// Separate from the upload because the upload runs on a worker and this is GL
// state; a no-op when the wrap already matches, so the common REPEAT case
// costs nothing.
void texture_apply_wrap(Texture* texture, GLenum wrap_s, GLenum wrap_t);

// The texture's mean colour, LINEAR, into out_rgb. False if it cannot be had --
// no chain, no GL object -- so a caller can fall back rather than use a zero.
//
// Reads the 1x1 top mip, which is that mean already computed, and MEMOIZES it:
// the readback happens once per texture for the life of the process, however
// often this is called. The first call needs a live GL context.
bool texture_mean_color(Texture* texture, float* out_rgb);

/*
 * Texture Pool
 */
typedef struct TexturePool {
    char* directory; // Directory where texture images are stored

    Texture** textures;   // Dynamic array of Texture pointers
    size_t texture_count; // Number of textures in the pool

    Texture* texture_cache; // Hash table for cached textures

    cetra_mutex_t cache_mutex; // Protects texture_cache and textures array
} TexturePool;

TexturePool* create_texture_pool();
void free_texture_pool(TexturePool* pool);

void set_texture_pool_directory(TexturePool* pool, const char* directory);

Texture* get_texture_from_pool(TexturePool* pool, const char* filepath);
void add_texture_to_pool(TexturePool* pool, Texture* texture);

// Float LUT upload from memory (LINEAR / CLAMP_TO_EDGE / no mips baked in).
// Returns the raw GL name; the caller owns deletion. First user: the LTC
// area-light tables (spec 9.2).
GLuint create_texture_2d_float(int width, int height, GLenum internal_format, GLenum data_format,
                               const float* pixels);

// Float volume allocation (LINEAR / CLAMP_TO_EDGE on S/T/R / no mips baked in).
// pixels may be NULL for a render target. Returns the raw GL name; the caller
// owns deletion. Pixel type is GL_FLOAT: integer or tiling volumes need their
// own entry point. First user: the froxel fog volumes (spec 9.5).
GLuint create_texture_3d_float(int width, int height, int depth, GLenum internal_format,
                               GLenum data_format, const float* pixels);

// Tiling RGBA8 volume upload (REPEAT on S/T/R, trilinear mips generated from
// the level-0 pixels). For CPU-baked repeating fields whose coarser mips a
// marcher samples via textureLod. Returns the raw GL name; the caller owns
// deletion.
// Filterable float 2D array; bilinear within a layer, never across layers.
GLuint create_texture_2d_array_float(int width, int height, int layers, GLenum internal_format,
                                     GLenum data_format);

GLuint create_texture_3d_rgba8_tiling(int width, int height, int depth,
                                      const unsigned char* pixels);

// Enable anisotropic filtering on the currently bound texture (no-op where
// unsupported). Tiled textures at grazing angles alias into moire without it.
void texture_set_max_anisotropy(void);

// Default sampler state for model textures on the currently bound texture:
// tiling wrap + trilinear + anisotropy.
void texture_set_default_sampler_state(void);

// Bleed visible RGB into transparent texels of an RGBA image so filtering
// and mipmaps don't mix the garbage colors stored behind alpha = 0 into
// visible edges (bright dashes on hair/foliage cards)
void texture_dilate_transparent_rgb(unsigned char* data, int width, int height);

// What an RGBA source's ALPHA means, which decides whether its transparent
// texels' rgb wants repairing before it is filtered or mipped.
//
// A named pair rather than a bool, because the call site is what has to be
// legible: `(false, true)` and `(false, false)` next to each other say nothing
// about which is which, and both needed a comment above them to explain the
// second argument.
typedef enum TextureAlpha {
    TEXTURE_ALPHA_OPACITY, // coverage: dilate, so a cutout edge does not bleed
    TEXTURE_ALPHA_DATA,    // a height, an occlusion, a mask: never touch the rgb
} TextureAlpha;

/*
 * What a texture IS, which decides how it may be block-compressed (spec 11.85).
 *
 * The same problem TextureAlpha solved one field over: the loader knows only
 * `is_srgb`, which is false for a tangent normal and false for a roughness mask
 * alike, and those two want opposite formats. So the caller states it.
 *
 * For a MODEL texture the caller is import.c, which derives this from the
 * material setter rather than tabulating it -- a row is wrong here only if it is
 * also wrong about which slot it fills, which is not a mistake that hides. For a
 * procedurally generated one there is no table and the app states it directly,
 * which is the path every one of this engine's own normal maps takes.
 *
 * TEXTURE_USE_NORMAL is the one with a consequence beyond storage: BC5 carries
 * two channels and pbr_frag RECONSTRUCTS Z, because no block format carries
 * three. Tagging something a normal that is not one loses its third channel.
 */
typedef enum TextureUse {
    TEXTURE_USE_COLOUR, // albedo, emissive, sheen: sRGB, and the last to compress
    TEXTURE_USE_NORMAL, // a tangent-space normal, and nothing else
    TEXTURE_USE_DATA,   // roughness, metalness, AO, opacity, height
} TextureUse;

/*
 * The three facts a loader needs about an image, carried together.
 *
 * They are three and not one because they genuinely disagree. Reflectance is
 * colour-ish and LINEAR; a decal is colour with a real opacity alpha, also
 * linear, because it lives in the material array; apps/tree's sand albedo is
 * baked non-sRGB on purpose so the stochastic transform operates on stored
 * codes. No two of these can be derived from the third.
 *
 * A struct rather than three parameters because they arrived one at a time and
 * cost an entry point each time -- seven of them for three facts, four of which
 * existed only to default a field. `texture_desc` states the historical
 * inference once so a caller overrides what it means and inherits the rest.
 */
typedef struct TextureDesc {
    bool is_srgb;       // stored sRGB-encoded, not linear
    TextureAlpha alpha; // what the alpha channel MEANS
    TextureUse use;     // what the image IS, which decides its block format
} TextureDesc;

// The historical inference as a starting point: a colour texture is the one with
// an opacity in alpha, and an unstated use compresses nothing.
//
// NOT a zero initialiser, and that is the reason this exists rather than a
// `(TextureDesc){0}` at each site: TEXTURE_ALPHA_OPACITY is the zero of its
// enum, so a zeroed desc claims a LINEAR image has an opacity in alpha -- which
// dilates a height or an occlusion and overwrites the three channels beside it.
TextureDesc texture_desc(bool is_srgb);

// Whether this image's transparent texels want their rgb repaired before it is
// filtered or mipped. One function because the answer is needed on three
// threads at three moments, and it is the same question every time.
bool texture_wants_dilate(int channels, TextureDesc desc);

// Global switch behind --no-texture-compression: off stores every texture
// uncompressed, and pbr_frag then reads a stored Z rather than rebuilding one,
// so a normal-mapped frame returns to what it was.
//
// It is a lever on STORAGE, and the CPU mip chain is not behind it -- that
// replaced glGenerateMipmap unconditionally and moves 59,927 px of 480,000 on
// forest by itself. Bisecting across this spec means the commits, not the flag.
void texture_set_compression_enabled(bool enabled);

// Colour is its own switch and defaults OFF. DXT quantises endpoints to RGB565,
// which is visible on a smooth gradient where BC4 and BC5 are not, so the two
// halves of this feature are not one decision and are not one flag.
// --texture-compress-colour turns it on.
void texture_set_colour_compression_enabled(bool enabled);

// Bytes this texture occupies on the GPU including its mip chain, derived from
// the internal format the driver actually chose rather than from the channel
// count the loader passed. Unsized formats mean those two can disagree, which is
// exactly why the probe asks.
size_t texture_gpu_bytes(const Texture* texture);

// The `normalTexExists` / `clearcoatNormalExists` gate: 0 none, 1 stored with a
// Z, 2 two-channel and the shader must rebuild it.
//
// One function because two call sites is how the base normal and the coat normal
// came to disagree in the first place -- 11.85 taught the base path to rebuild
// and left the coat path reading a blue channel BC5 does not store, which is a
// normal pointing into the surface.
int texture_normal_gate(const Texture* texture);

/*
 * THE path from decoded pixels to a pooled Texture: pick the GL formats, create
 * and bind the object, set the sampler state, upload level 0 and the CPU mip
 * chain under it, block-encode where `use` asks for one, record the format the
 * driver actually took, and insert into `pool` under `key`.
 *
 * Public because there are three producers -- the file loader, the in-memory
 * loader and the async loader's completion -- and this body used to be written
 * out three times. It had already drifted: the async copy re-derived `is_srgb`
 * by sniffing the internal format it was about to pass, which disagrees with the
 * caller below three channels, where texture_gl_formats has no sRGB variant to
 * return. A mip filter or a block policy reaching only some producers is the
 * same class of defect as the glGenerateMipmap that survived here for a commit.
 *
 * `pixels` is NOT retained -- GL takes its own copy and Texture stores no pixel
 * pointer, so the caller's buffer is dead the moment this returns. Dilation is
 * the caller's, because the three do it at three different moments and only one
 * of them is on this thread; the DECISION is texture_wants_dilate above.
 *
 * Returns the pooled Texture, or NULL. Does not check the cache: a caller that
 * can answer from the pool must do so before decoding, not after.
 */
Texture* texture_pool_publish(TexturePool* pool, const char* key, const unsigned char* pixels,
                              int width, int height, int channels, TextureDesc desc);

/*
 * --texture-probe: one line per texture and a total, in the --water-probe idiom.
 *
 * The instrument exists because texture memory is otherwise invisible from
 * outside the process, and a saving is a NUMERIC claim no frame can make: a
 * scene whose normals silently failed to compress renders exactly like one whose
 * normals compressed, which is the failure this is here to catch. It prints the
 * internal format the driver holds, not the one the loader asked for.
 */
void texture_pool_probe(const TexturePool* pool, const char* label);

/*
 * Load a texture FILE into the pool, resolving `filepath` against the pool's
 * directory. Cached by the resolved path.
 *
 * The pool keys on PATH, so a second consumer wanting the same file under a
 * different desc gets the first one's decision. That is warned about by name
 * rather than re-keyed: two entries would double the VRAM for one image, and a
 * scene wanting one file as both a lit albedo and a decal wants two files.
 */
Texture* texture_load_file(TexturePool* pool, const char* filepath, TextureDesc desc);

/*
 * The same from pixels already in memory, cached by `key`.
 *
 * The pool does NOT keep `pixels`. GL takes its own copy and Texture stores no
 * pixel pointer, so the caller's buffer is dead the moment this returns.
 *
 * A repeat key returns the existing Texture WITHOUT looking at `pixels` at all,
 * so a duplicate key silently discards a freshly generated image.
 */
Texture* texture_load_memory(TexturePool* pool, const char* key, const unsigned char* pixels,
                             int width, int height, int channels, TextureDesc desc);

/*
 * The same, but TAKING OWNERSHIP: uploads and then frees `pixels`.
 *
 * For a procedural bake, which is every caller that generates into a malloc'd
 * buffer and has no use for it afterwards. Ownership is its own function rather
 * than a field on TextureDesc, because it is a fact about the BUFFER and the
 * desc describes the IMAGE -- one desc is meant to be reusable across calls, and
 * a lifetime bolted into it would not be.
 *
 * A NULL `pixels` returns NULL, so a generator that failed needs no guard at the
 * call site.
 */
Texture* texture_load_memory_owned(TexturePool* pool, const char* key, unsigned char* pixels,
                                   int width, int height, int channels, TextureDesc desc);

void remove_texture_from_pool(TexturePool* pool, const char* filepath);
void clear_texture_pool(TexturePool* pool);

// Thread-safe variants for async loading
Texture* get_texture_from_pool_threadsafe(TexturePool* pool, const char* filepath);
void add_texture_to_pool_threadsafe(TexturePool* pool, Texture* texture);
void remove_texture_from_pool_threadsafe(TexturePool* pool, const char* filepath);

#endif // TEXTURE_H
