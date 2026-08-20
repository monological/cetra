
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

// Pick GL formats for an 8-bit image. is_srgb marks color data (albedo,
// emissive) so the hardware decodes it to linear exactly once on sample;
// data textures (normals, roughness/metalness, AO, ...) stay linear.
void texture_gl_formats(int channels, bool is_srgb, GLenum* internal_format, GLenum* data_format);

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

Texture* load_texture_path_into_pool(TexturePool* pool, const char* filepath, bool is_srgb);

// The pool does NOT keep `pixels`. glTexImage2D takes GL's own copy and Texture
// stores no pixel pointer, so the caller's buffer is dead the moment this
// returns and the caller frees it. Stated here because the only place that said
// so was an implementation comment, and a caller reading the header could not
// learn it.
//
// Cached by `key`: a repeat key returns the existing Texture WITHOUT looking at
// `pixels` at all, so a duplicate key silently discards a freshly generated
// image.
Texture* load_texture_from_memory(TexturePool* pool, const char* key, const unsigned char* pixels,
                                  int width, int height, int channels, bool is_srgb);

// The same, but TAKING OWNERSHIP: uploads and then frees `pixels`.
//
// For a procedural bake, which is every caller that generates into a malloc'd
// buffer and has no use for it afterwards. Both apps that do this had grown
// their own identical copy of it -- what the helper encodes is a fact about the
// pool above, not about either app.
//
// A NULL `pixels` returns NULL, so a generator that failed needs no guard at the
// call site.
Texture* load_texture_from_memory_owned(TexturePool* pool, const char* key, unsigned char* pixels,
                                        int width, int height, int channels, bool is_srgb);

void remove_texture_from_pool(TexturePool* pool, const char* filepath);
void clear_texture_pool(TexturePool* pool);

// Thread-safe variants for async loading
Texture* get_texture_from_pool_threadsafe(TexturePool* pool, const char* filepath);
void add_texture_to_pool_threadsafe(TexturePool* pool, Texture* texture);
void remove_texture_from_pool_threadsafe(TexturePool* pool, const char* filepath);

#endif // TEXTURE_H
