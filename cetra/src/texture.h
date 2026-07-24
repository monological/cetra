
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

// Volume allocation (LINEAR / CLAMP_TO_EDGE on S/T/R / no mips baked in).
// pixels may be NULL for a render target. Returns the raw GL name; the caller
// owns deletion. First user: the froxel fog volumes (spec 9.5), which need a
// real 3D texture because their composite tap filters across slices.
GLuint create_texture_3d(int width, int height, int depth, GLenum internal_format,
                         GLenum data_format, const void* pixels);

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
Texture* load_texture_from_memory(TexturePool* pool, const char* key, const unsigned char* pixels,
                                  int width, int height, int channels, bool is_srgb);
void remove_texture_from_pool(TexturePool* pool, const char* filepath);
void clear_texture_pool(TexturePool* pool);

// Thread-safe variants for async loading
Texture* get_texture_from_pool_threadsafe(TexturePool* pool, const char* filepath);
void add_texture_to_pool_threadsafe(TexturePool* pool, Texture* texture);
void remove_texture_from_pool_threadsafe(TexturePool* pool, const char* filepath);

#endif // TEXTURE_H
