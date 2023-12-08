
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "texture.h"
#include "stb_image.h"
#include "uthash.h"
#include "util.h"

Texture* create_texture() {
    Texture* texture = (Texture*)malloc(sizeof(Texture));
    if (!texture) {
        fprintf(stderr, "Failed to allocate memory for texture\n");
        return NULL;
    }

    texture->id = 0;           // OpenGL texture id
    texture->filepath = NULL;  // No file path initially
    texture->width = 0;        // Default width
    texture->height = 0;       // Default height
    texture->format = 0;       // Default format (you can choose a specific default if applicable)

    return texture;
}

void free_texture(Texture* texture) {
    if (texture) {
        glDeleteTextures(1, &(texture->id));
        if(texture->filepath){
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

void set_texture_format(Texture* texture, GLenum format) {
    if (texture) {
        texture->format = format;
    }
}

/* 
 * Texture Pool
 *
 */


TexturePool* create_texture_pool() {
    TexturePool* pool = (TexturePool*)malloc(sizeof(TexturePool));
    if (!pool) {
        fprintf(stderr, "Failed to allocate memory for TexturePool\n");
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

void set_texture_pool_directory(TexturePool* pool, const char* directory){
    if(!pool) return;

    if (directory) {
        if(pool->directory != NULL){
            free(pool->directory);
        }

        pool->directory = strdup(directory);

        if (!pool->directory) {
            fprintf(stderr, "Failed to allocate memory for directory string\n");
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
            fprintf(stderr, "Failed to reallocate memory for textures array\n");
            return;
        }
        pool->textures[pool->texture_count++] = texture;

        // Add to cache
        Texture* existing;
        HASH_FIND_STR(pool->texture_cache, texture->filepath, existing);
        if (!existing) {
            HASH_ADD_KEYPTR(hh, pool->texture_cache, texture->filepath, strlen(texture->filepath), texture);
        }
    }
}

Texture* load_texture_path_into_pool(TexturePool* pool, const char* filepath) {
    if (!pool || !filepath) {
        fprintf(stderr, "Invalid pool or filepath\n");
        return NULL;
    }

    GLenum format;

    // Normalize and work on a copy of the filepath
    char* normalized_path = convert_and_normalize_path(filepath);
    if (!normalized_path) {
        fprintf(stderr, "Failed to normalize path: %s\n", filepath);
        return NULL;
    }

    char* subpath = strdup(normalized_path);
    if (!subpath) {
        fprintf(stderr, "Memory allocation failed for subpath.\n");
        free(normalized_path);
        return NULL;
    }

    // Use find_existing_subpath to find a valid subpath
    if (!find_existing_subpath(pool->directory, &subpath)) {
        fprintf(stderr, "No valid subpath found for texture: %s\n", subpath);
        free(normalized_path);
        free(subpath);
        return NULL;
    }

    GLuint textureID = 0;
    int width, height, nrChannels;

    Texture* cachedTexture = get_texture_from_pool(pool, subpath);
    if (cachedTexture) {
        printf("Using cached texture path %s\n", subpath);
        free(normalized_path);
        free(subpath);
        return cachedTexture;
    }

    //printf("Loading texture path %s\n", subpath);

    unsigned char* data = stbi_load(subpath, &width, &height, &nrChannels, 0);
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

        // Set texture parameters
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        // Determine format
        if (nrChannels == 1)
            format = GL_RED;
        else if (nrChannels == 3)
            format = GL_RGB;
        else if (nrChannels == 4)
            format = GL_RGBA;

        // Upload texture data
        glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, format, GL_UNSIGNED_BYTE, data);
        glGenerateMipmap(GL_TEXTURE_2D);

        // Clean up
        stbi_image_free(data);

        // Update texture properties
        newTexture->id = textureID;
        newTexture->filepath = strdup(subpath);
        newTexture->width = width;
        newTexture->height = height;
        newTexture->format = format;

        // Add texture to the pool
        add_texture_to_pool(pool, newTexture);
        glBindTexture(GL_TEXTURE_2D, 0);

        free(normalized_path);
        free(subpath);

        return newTexture;
    }

    fprintf(stderr, "Failed to load texture: %s\n", subpath);
    free(normalized_path);
    free(subpath);
    return NULL;

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
        Texture* current, *tmp;
        HASH_ITER(hh, pool->texture_cache, current, tmp) {
            if (current) {
                HASH_DEL(pool->texture_cache, current);
                free_texture(current);
            }
        }
    }
}

