
#ifndef TEXTURE_H
#define TEXTURE_H

#include <GL/glew.h>
#include <stdbool.h>

#include "ext/uthash.h"

/*
 * Texture
 */
typedef struct Texture {
    GLuint id;           // OpenGL texture ID
    char* filepath;      // File path of the texture
    int width;           // Width of the texture
    int height;          // Height of the texture
    GLenum format;       // Format of the texture (e.g., GL_RGB, GL_RGBA)

    UT_hash_handle hh;   // Makes this structure hashable
} Texture;

// malloc
Texture* create_texture();
void free_texture(Texture* texture);

void set_texture_width(Texture* texture, int width);
void set_texture_height(Texture* texture, int height);
void set_texture_format(Texture* texture, GLenum format);

/*
 * Texture Pool
 */
typedef struct TexturePool {
    char* directory;           // Directory where texture images are stored

    Texture** textures;        // Dynamic array of Texture pointers
    size_t texture_count;      // Number of textures in the pool

    Texture* texture_cache;    // Hash table for cached textures
} TexturePool;

TexturePool* create_texture_pool();
void free_texture_pool(TexturePool* pool);

void set_texture_pool_directory(TexturePool* pool, const char* directory);

Texture* get_texture_from_pool(TexturePool* pool, const char* filepath);
void add_texture_to_pool(TexturePool* pool, Texture* texture);
Texture* load_texture_path_into_pool(TexturePool* pool, const char* filepath);
void remove_texture_from_pool(TexturePool* pool, const char* filepath);
void clear_texture_pool(TexturePool* pool);


#endif // TEXTURE_H


