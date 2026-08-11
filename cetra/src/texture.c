
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "ext/stb_image.h"
#include "ext/uthash.h"
#include "ext/log.h"

#include "texture.h"
#include "util.h"

// Bleed visible RGB into fully-transparent texels. PNGs routinely store
// garbage (often white) color behind alpha = 0; bilinear and mipmap
// filtering mix it across the alpha edge, which renders as bright dashes
// along the edges of hair/foliage cards. Runs until every transparent texel
// carries a plausible color, so even the deepest mip levels average real
// strand color instead of garbage.
void texture_dilate_transparent_rgb(unsigned char* data, int width, int height) {
    size_t count = (size_t)width * (size_t)height;
    unsigned char* solid = malloc(count);
    if (!solid)
        return;
    for (size_t i = 0; i < count; i++)
        solid[i] = data[i * 4 + 3] >= 8;

    int max_passes = width > height ? width : height;
    for (int pass = 0; pass < max_passes; pass++) {
        unsigned char* next = malloc(count);
        if (!next)
            break;
        memcpy(next, solid, count);
        bool changed = false;

        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                size_t i = (size_t)y * (size_t)width + (size_t)x;
                if (solid[i])
                    continue;

                static const int dx[4] = {1, -1, 0, 0};
                static const int dy[4] = {0, 0, 1, -1};
                int r = 0, g = 0, b = 0, n = 0;
                for (int k = 0; k < 4; k++) {
                    int nx = x + dx[k];
                    int ny = y + dy[k];
                    if (nx < 0 || ny < 0 || nx >= width || ny >= height)
                        continue;
                    size_t j = (size_t)ny * (size_t)width + (size_t)nx;
                    if (!solid[j])
                        continue;
                    r += data[j * 4];
                    g += data[j * 4 + 1];
                    b += data[j * 4 + 2];
                    n++;
                }
                if (n > 0) {
                    data[i * 4] = (unsigned char)(r / n);
                    data[i * 4 + 1] = (unsigned char)(g / n);
                    data[i * 4 + 2] = (unsigned char)(b / n);
                    next[i] = 1;
                    changed = true;
                }
            }
        }

        free(solid);
        solid = next;
        if (!changed)
            break;
    }
    free(solid);
}

// Anisotropic filtering on the currently bound texture. Without it, textures
// tiled many times and viewed at grazing angles (floors, carpets) alias into
// moire banding, and their mip average washes the surface color out --
// trilinear alone cannot handle the anisotropic footprint. Ubiquitous
// extension; a no-op where unsupported. The device limit is queried once (a
// driver round-trip otherwise paid per texture upload).
void texture_set_max_anisotropy(void) {
    static GLfloat max_aniso = -1.0f;
    if (max_aniso < 0.0f) {
        max_aniso = 0.0f;
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &max_aniso);
    }
    if (max_aniso > 1.0f)
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, fminf(8.0f, max_aniso));
}

// The default sampler state every model texture gets: tiling wrap, trilinear
// minification, anisotropy. One definition so the three upload sites cannot
// drift.
void texture_set_default_sampler_state(void) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    texture_set_max_anisotropy();
}

// Float LUT upload (first user: the LTC area-light tables, spec 9.2). Data
// textures, not model textures: LINEAR filtering, CLAMP_TO_EDGE, no mips --
// the state the IBL BRDF LUT uses, baked in so LUT call sites cannot drift
// onto the tiling/trilinear model-texture defaults.
GLuint create_texture_2d_float(int width, int height, GLenum internal_format, GLenum data_format,
                               const float* pixels) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)internal_format, width, height, 0, data_format, GL_FLOAT,
                 pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

// Float volume upload/allocation, the 3D sibling of create_texture_2d_float and
// carrying the same data-texture policy: LINEAR, CLAMP_TO_EDGE on all three
// axes, no mips. The `_float` in the name is load-bearing -- the pixel type is
// GL_FLOAT, so a byte buffer here would be read as floats. A consumer needing
// integer or tiling volumes (noise) wants its own entry point.
// glTexImage3D (not glTexStorage3D, which is GL 4.2) allocates level 0 only,
// matching mask_array.c / shadow.c. pixels may be NULL for a render target.
GLuint create_texture_3d_float(int width, int height, int depth, GLenum internal_format,
                               GLenum data_format, const float* pixels) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_3D, tex);
    glTexImage3D(GL_TEXTURE_3D, 0, (GLint)internal_format, width, height, depth, 0, data_format,
                 GL_FLOAT, pixels);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_3D, 0);
    return tex;
}

// A filterable float 2D array, CLAMP_TO_EDGE and bilinear WITHIN each layer --
// never across layers, which is what separates this from the 3D entry point
// above. For array-of-maps data where a layer is an independent image (the fog's
// ESM cascades) rather than a sampled volume.
GLuint create_texture_2d_array_float(int width, int height, int layers, GLenum internal_format,
                                     GLenum data_format) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, (GLint)internal_format, width, height, layers, 0,
                 data_format, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    return tex;
}

// The tiling-volume entry point the note above reserves: RGBA8 pixels, REPEAT
// on all three axes, trilinear mips (glGenerateMipmap after the level-0
// upload -- still no glTexStorage3D on 4.1). For CPU-baked noise fields whose
// coarser mips a marcher samples explicitly via textureLod.
GLuint create_texture_3d_rgba8_tiling(int width, int height, int depth,
                                      const unsigned char* pixels) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_3D, tex);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, width, height, depth, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 pixels);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenerateMipmap(GL_TEXTURE_3D);
    glBindTexture(GL_TEXTURE_3D, 0);
    return tex;
}

void texture_gl_formats(int channels, bool is_srgb, GLenum* internal_format, GLenum* data_format) {
    if (channels == 1) {
        *internal_format = GL_RED;
        *data_format = GL_RED;
    } else if (channels == 2) {
        *internal_format = GL_RG;
        *data_format = GL_RG;
    } else if (channels == 3) {
        *internal_format = is_srgb ? GL_SRGB : GL_RGB;
        *data_format = GL_RGB;
    } else {
        *internal_format = is_srgb ? GL_SRGB_ALPHA : GL_RGBA;
        *data_format = GL_RGBA;
    }
}

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
    // Matches texture_set_default_sampler_state, which is what every upload
    // path applies -- so the recorded value describes the texture object even
    // for the sites that never ask for anything else.
    texture->wrap_s = GL_REPEAT;
    texture->wrap_t = GL_REPEAT;
    texture->ref_count = 1;

    return texture;
}

void texture_apply_wrap(Texture* texture, GLenum wrap_s, GLenum wrap_t) {
    if (!texture || texture->id == 0)
        return;
    if (texture->wrap_s == wrap_s && texture->wrap_t == wrap_t)
        return;
    // The pool caches by filepath, so one image can be reached by materials
    // that disagree. Whoever asks last wins -- but say so, because the loser
    // gets wrap it did not ask for and the symptom (edge bleed on one model
    // and not another sharing the atlas) is not one anybody would guess at.
    if (texture->wrap_s != GL_REPEAT || texture->wrap_t != GL_REPEAT)
        log_warn("Texture %s re-wrapped (0x%x,0x%x -> 0x%x,0x%x); it is shared by materials that "
                 "declare different wrap",
                 texture->filepath ? texture->filepath : "?", texture->wrap_s, texture->wrap_t,
                 wrap_s, wrap_t);

    glBindTexture(GL_TEXTURE_2D, texture->id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (GLint)wrap_s);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (GLint)wrap_t);
    glBindTexture(GL_TEXTURE_2D, 0);
    texture->wrap_s = wrap_s;
    texture->wrap_t = wrap_t;
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

Texture* texture_retain(Texture* texture) {
    if (texture) {
        texture->ref_count++;
    }
    return texture;
}

void texture_release(Texture* texture) {
    if (texture) {
        if (texture->ref_count > 0) {
            texture->ref_count--;
        }
        if (texture->ref_count == 0) {
            free_texture(texture);
        }
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

    if (!cetra_mutex_init(&pool->cache_mutex)) {
        log_error("Failed to init cache_mutex");
        free(pool);
        return NULL;
    }

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

        cetra_mutex_destroy(&pool->cache_mutex);

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

Texture* load_texture_path_into_pool(TexturePool* pool, const char* filepath, bool is_srgb) {
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

    if (nrChannels == 4) {
        texture_dilate_transparent_rgb(data, width, height);
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

    texture_set_default_sampler_state();

    // Determine format
    GLenum internal_format;
    GLenum data_format;
    texture_gl_formats(nrChannels, is_srgb, &internal_format, &data_format);

    // Upload texture data
    glTexImage2D(GL_TEXTURE_2D, 0, internal_format, width, height, 0, data_format, GL_UNSIGNED_BYTE,
                 data);
    check_gl_error("texture upload");
    glGenerateMipmap(GL_TEXTURE_2D);
    check_gl_error("mipmap generation");

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

Texture* load_texture_from_memory(TexturePool* pool, const char* key, const unsigned char* pixels,
                                  int width, int height, int channels, bool is_srgb) {
    if (!pool || !key || !pixels) {
        log_error("Invalid pool, key, or pixel data");
        return NULL;
    }

    // Check cache first
    Texture* cached_texture = get_texture_from_pool(pool, key);
    if (cached_texture) {
        return cached_texture;
    }

    Texture* new_texture = create_texture();
    if (!new_texture) {
        return NULL;
    }

    // RGBA sources need their transparent texels' color repaired; work on a
    // mutable copy since the caller owns the pixel data
    unsigned char* dilated = NULL;
    if (channels == 4) {
        size_t size = (size_t)width * (size_t)height * 4;
        dilated = malloc(size);
        if (dilated) {
            memcpy(dilated, pixels, size);
            texture_dilate_transparent_rgb(dilated, width, height);
        }
    }

    // Generate texture
    GLuint textureID = 0;
    glGenTextures(1, &textureID);
    glBindTexture(GL_TEXTURE_2D, textureID);

    texture_set_default_sampler_state();

    // Determine format
    GLenum internal_format;
    GLenum data_format;
    texture_gl_formats(channels, is_srgb, &internal_format, &data_format);

    // Upload texture data
    glTexImage2D(GL_TEXTURE_2D, 0, internal_format, width, height, 0, data_format, GL_UNSIGNED_BYTE,
                 dilated ? dilated : pixels);
    free(dilated);
    check_gl_error("embedded texture upload");
    glGenerateMipmap(GL_TEXTURE_2D);
    check_gl_error("embedded mipmap generation");

    // Update texture properties
    new_texture->id = textureID;
    new_texture->filepath = safe_strdup(key);
    new_texture->width = width;
    new_texture->height = height;
    new_texture->internal_format = internal_format;
    new_texture->data_format = data_format;

    // Add texture to the pool
    add_texture_to_pool(pool, new_texture);
    glBindTexture(GL_TEXTURE_2D, 0);

    log_info("Loaded embedded texture '%s' (%dx%d, %d channels)", key, width, height, channels);

    return new_texture;
}

void remove_texture_from_pool(TexturePool* pool, const char* filepath) {
    if (pool && filepath) {
        Texture* to_remove = NULL;

        // Find and remove from cache (don't free yet)
        HASH_FIND_STR(pool->texture_cache, filepath, to_remove);
        if (to_remove) {
            HASH_DEL(pool->texture_cache, to_remove);
        }

        // Remove from dynamic array (swap with last, don't free yet)
        for (size_t i = 0; i < pool->texture_count; i++) {
            if (strcmp(pool->textures[i]->filepath, filepath) == 0) {
                pool->textures[i] = pool->textures[pool->texture_count - 1];
                pool->texture_count--;
                break;
            }
        }

        // Release the pool's reference (frees if ref_count reaches 0)
        if (to_remove) {
            texture_release(to_remove);
        }
    }
}

void clear_texture_pool(TexturePool* pool) {
    if (pool && pool->texture_cache) {
        Texture *current, *tmp;
        HASH_ITER(hh, pool->texture_cache, current, tmp) {
            Texture* to_release = current;
            // NOLINTNEXTLINE(clang-analyzer-unix.Malloc) - uthash pattern, tmp holds next before
            // delete
            HASH_DEL(pool->texture_cache, current);
            texture_release(to_release);
        }
        pool->texture_cache = NULL;
    }
}

/*
 * Thread-safe variants for async loading
 */

Texture* get_texture_from_pool_threadsafe(TexturePool* pool, const char* filepath) {
    if (!pool || !filepath) {
        return NULL;
    }

    cetra_mutex_lock(&pool->cache_mutex);
    Texture* found;
    HASH_FIND_STR(pool->texture_cache, filepath, found);
    cetra_mutex_unlock(&pool->cache_mutex);

    return found;
}

void add_texture_to_pool_threadsafe(TexturePool* pool, Texture* texture) {
    if (!pool || !texture || !texture->filepath) {
        return;
    }

    cetra_mutex_lock(&pool->cache_mutex);

    // Check if already exists
    Texture* existing;
    HASH_FIND_STR(pool->texture_cache, texture->filepath, existing);
    if (!existing) {
        // Add to dynamic array
        pool->textures = realloc(pool->textures, (pool->texture_count + 1) * sizeof(Texture*));
        if (pool->textures) {
            pool->textures[pool->texture_count++] = texture;

            // Add to cache
            HASH_ADD_KEYPTR(hh, pool->texture_cache, texture->filepath, strlen(texture->filepath),
                            texture);
        } else {
            log_error("Failed to reallocate memory for textures array");
        }
    }

    cetra_mutex_unlock(&pool->cache_mutex);
}

void remove_texture_from_pool_threadsafe(TexturePool* pool, const char* filepath) {
    if (!pool || !filepath) {
        return;
    }

    cetra_mutex_lock(&pool->cache_mutex);

    Texture* to_remove = NULL;

    // Find and remove from cache
    HASH_FIND_STR(pool->texture_cache, filepath, to_remove);
    if (to_remove) {
        HASH_DEL(pool->texture_cache, to_remove);
    }

    // Remove from dynamic array (swap with last)
    for (size_t i = 0; i < pool->texture_count; i++) {
        if (strcmp(pool->textures[i]->filepath, filepath) == 0) {
            pool->textures[i] = pool->textures[pool->texture_count - 1];
            pool->texture_count--;
            break;
        }
    }

    cetra_mutex_unlock(&pool->cache_mutex);

    // Release reference outside the lock to avoid potential deadlock
    if (to_remove) {
        texture_release(to_remove);
    }
}
