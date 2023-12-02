
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "texture.h"
#include "stb_image.h"
#include "uthash.h"
#include "util.h"


static TextureCache *cache = NULL;

Texture* load_texture(const char* path, const char* directory) {
    char filename[1000];
    GLenum format;

    // Check cache first
    Texture* cachedTexture = find_texture_in_cache(path);
    if (cachedTexture) {
        return cachedTexture;
    }

    // Normalize and work on a copy of the path
    char* normalized_path = convert_and_normalize_path(path);
    if (!normalized_path) {
        fprintf(stderr, "Failed to normalize path: %s\n", path);
        return 0;
    }

    char* subpath = strdup(normalized_path);
    if (!subpath) {
        fprintf(stderr, "Memory allocation failed for subpath.\n");
        free(normalized_path);
        return 0;
    }

    bool loaded = false;
    GLuint textureID = 0;
    int width, height, nrChannels;

    do {
        snprintf(filename, sizeof(filename), "%s/%s", directory, subpath);
        printf("trying texture path %s\n", filename);

        unsigned char* data = stbi_load(filename, &width, &height, &nrChannels, 0);
        if (data) {
            // Create new texture object
            Texture* newTexture = create_texture();
            if (!newTexture) {
                stbi_image_free(data);
                free(normalized_path);
                free(subpath);
                return NULL;
            }

            glGenTextures(1, &textureID);
            glBindTexture(GL_TEXTURE_2D, textureID);

            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

            if (nrChannels == 1)
                format = GL_RED;
            else if (nrChannels == 3)
                format = GL_RGB;
            else if (nrChannels == 4)
                format = GL_RGBA;

            glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, format, GL_UNSIGNED_BYTE, data);
            glGenerateMipmap(GL_TEXTURE_2D);

            stbi_image_free(data);
            loaded = true;

            newTexture->id = textureID; // Correctly assign the texture ID
            newTexture->filepath = strdup(filename);
            newTexture->width = width;
            newTexture->height = height;
            newTexture->format = format;

            add_texture_to_cache(newTexture);
            glBindTexture(GL_TEXTURE_2D, 0);
            free(normalized_path);
            free(subpath);
            return newTexture;
        }

        char* slash_pos = strchr(subpath, '/');
        if (slash_pos != NULL) {
            memmove(subpath, slash_pos + 1, strlen(slash_pos));
        } else {
            break;
        }
    } while (!loaded);

    free(normalized_path);
    free(subpath);

    if (!loaded) {
        fprintf(stderr, "Failed to load texture after trying all sub-paths.\n");
        glBindTexture(GL_TEXTURE_2D, 0);
        return NULL;
    }
}


Texture* create_texture() {
    Texture* texture = (Texture*)malloc(sizeof(Texture));
    if (!texture) {
        fprintf(stderr, "Failed to allocate memory for texture\n");
        return NULL;
    }

    // Initialize texture properties with default values
    texture->id = 0;        // 0 indicates that the texture is not yet loaded into OpenGL
    texture->filepath = NULL;  // No file path initially
    texture->width = 0;     // Default width
    texture->height = 0;    // Default height
    texture->format = 0;    // Default format (you can choose a specific default if applicable)

    return texture;
}

void free_texture(Texture* texture) {
    if (texture) {
        glDeleteTextures(1, &(texture->id));
        free(texture->filepath);
        free(texture);
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

void set_texture_format(Texture* texture, GLenum format) {
    if (texture) {
        texture->format = format;
    }
}

/* 
 * Texture Cache
 *
 */

TextureCache* create_texture_cache(const char* filepath, Texture* texture) {
    TextureCache *cache = malloc(sizeof(TextureCache));
    if (!cache) {
        fprintf(stderr, "Failed to allocate memory for TextureCache\n");
        return NULL;
    }

    if (filepath) {
        strncpy(cache->filepath, filepath, sizeof(cache->filepath));
        cache->filepath[sizeof(cache->filepath) - 1] = '\0'; // Ensure null-termination
    } else {
        cache->filepath[0] = '\0'; // Empty string for no filepath
    }

    cache->texture = texture; // Can be NULL if just reserving the cache
    return cache;
}

void free_texture_cache(TextureCache* cache) {
    if (cache) {
        // Note: This does NOT free the associated texture, only the cache
        free(cache);
    }
}

void set_texture_cache_filepath(TextureCache* cache, const char* filepath) {
    if (cache && filepath) {
        strncpy(cache->filepath, filepath, sizeof(cache->filepath));
        cache->filepath[sizeof(cache->filepath) - 1] = '\0'; // Ensure null-termination
    }
}

void set_texture_cache_texture(TextureCache* cache, Texture* texture) {
    if (cache) {
        cache->texture = texture;
    }
}

Texture* find_texture_in_cache(const char* path) {
    TextureCache *cache;
    HASH_FIND_STR(cache, path, cache);
    return (cache) ? cache->texture : NULL;
}

void add_texture_to_cache(Texture* texture) {
    if (!texture || !texture->filepath) {
        fprintf(stderr, "Invalid texture or filepath for caching\n");
        return;
    }

    TextureCache *new_cache_entry = create_texture_cache(texture->filepath, texture);
    if (!new_cache_entry) {
        fprintf(stderr, "Failed to create TextureCache for: %s\n", texture->filepath);
        return;
    }

    if (strlen(texture->filepath) >= sizeof(new_cache_entry->filepath)) {
        fprintf(stderr, "Filepath too long for cache: %s\n", texture->filepath);
        return;
    }

    HASH_ADD_STR(cache, filepath, new_cache_entry);
}

void remove_texture_from_cache(const char* path) {
    TextureCache *cache;
    HASH_FIND_STR(cache, path, cache);
    if (cache) {
        HASH_DEL(cache, cache);
        free_texture_cache(cache);
    }
}

void clear_texture_cache(void) {
    TextureCache *current, *tmp;
    HASH_ITER(hh, cache, current, tmp) {
        HASH_DEL(cache, current);
        free_texture_cache(current);
    }
}


