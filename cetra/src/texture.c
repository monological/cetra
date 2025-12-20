
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ext/stb_image.h"
#include "ext/uthash.h"
#include "ext/log.h"

#include "texture.h"
#include "util.h"

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

    return texture;
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

Texture* load_texture_path_into_pool(TexturePool* pool, const char* filepath) {
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

    // Set texture parameters
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // Determine format
    GLenum internal_format;
    GLenum data_format;

    if (nrChannels == 1) {
        internal_format = GL_RED;
        data_format = GL_RED;
    } else if (nrChannels == 3) {
        internal_format = GL_SRGB;
        data_format = GL_RGB;
    } else {
        internal_format = GL_SRGB_ALPHA;
        data_format = GL_RGBA;
    }

    // Upload texture data
    glTexImage2D(GL_TEXTURE_2D, 0, internal_format, width, height, 0, data_format, GL_UNSIGNED_BYTE,
                 data);
    glGenerateMipmap(GL_TEXTURE_2D);

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

void remove_texture_from_pool(TexturePool* pool, const char* filepath) {
    if (pool && filepath) {
        // Remove from cache
        Texture* to_remove;
        HASH_FIND_STR(pool->texture_cache, filepath, to_remove);
        if (to_remove) {
            HASH_DEL(pool->texture_cache, to_remove);
            free_texture(to_remove);
        }

        // Remove from dynamic array
        for (size_t i = 0; i < pool->texture_count; i++) {
            if (strcmp(pool->textures[i]->filepath, filepath) == 0) {
                free_texture(pool->textures[i]);
                pool->textures[i] = pool->textures[pool->texture_count - 1];
                pool->texture_count--;
                break;
            }
        }
    }
}

void clear_texture_pool(TexturePool* pool) {
    if (pool && pool->texture_cache) {
        Texture *current, *tmp;
        HASH_ITER(hh, pool->texture_cache, current, tmp) {
            Texture* to_free = current;
            // NOLINTNEXTLINE(clang-analyzer-unix.Malloc) - uthash pattern, tmp holds next before delete
            HASH_DEL(pool->texture_cache, current);
            free_texture(to_free);
        }
        pool->texture_cache = NULL;
    }
}
