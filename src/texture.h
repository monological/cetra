
#ifndef TEXTURE_H
#define TEXTURE_H

#include <GL/glew.h>
#include <stdbool.h>
#include "uthash.h"

// Texture structure
typedef struct Texture {
    GLuint id;           // OpenGL texture ID
    char* filepath;      // File path of the texture
    int width;           // Width of the texture
    int height;          // Height of the texture
    GLenum format;       // Format of the texture (e.g., GL_RGB, GL_RGBA)
} Texture;

typedef struct TextureCache {
    char filepath[1000];   // key
    Texture *texture;
    UT_hash_handle hh;     // makes this structure hashable
} TextureCache;


// texture functions
Texture* load_texture(const char* path, const char* directory);
Texture* create_texture();
void free_texture(Texture* texture);
void set_texture_width(Texture* texture, int width);
void set_texture_height(Texture* texture, int height);
void set_texture_format(Texture* texture, GLenum format);

// texture cache functions
TextureCache* create_texture_cache(const char* filepath, Texture* texture);
void free_texture_cache(TextureCache* cache);
void set_texture_cache_filepath(TextureCache* cache, const char* filepath);
void set_texture_cache_texture(TextureCache* cache, Texture* texture);

// Caching functions
Texture* find_texture_in_cache(const char* path);
void add_texture_to_cache(Texture* texture);
void remove_texture_from_cache(const char* path);
void clear_texture_cache(void);


#endif // TEXTURE_H


