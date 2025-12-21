
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ext/stb_image.h"
#include "ext/log.h"

#include "async_loader.h"
#include "util.h"

/*
 * Internal: Worker thread function
 */
static void* worker_thread_func(void* arg) {
    AsyncLoader* loader = (AsyncLoader*)arg;

    while (!atomic_load(&loader->shutdown)) {
        TextureLoadRequest* req = NULL;

        // Wait for work
        pthread_mutex_lock(&loader->work_mutex);
        while (!loader->work_head && !atomic_load(&loader->shutdown)) {
            pthread_cond_wait(&loader->work_cond, &loader->work_mutex);
        }

        // Pop from work queue
        if (loader->work_head) {
            req = loader->work_head;
            loader->work_head = req->next;
            if (!loader->work_head) {
                loader->work_tail = NULL;
            }
        }
        pthread_mutex_unlock(&loader->work_mutex);

        if (!req) {
            continue;
        }

        // Process the request
        TextureLoadResult* result = calloc(1, sizeof(TextureLoadResult));
        if (!result) {
            log_error("Failed to allocate TextureLoadResult");
            free(req->filepath);
            free(req);
            atomic_fetch_sub(&loader->pending_count, 1);
            continue;
        }

        result->callback = req->callback;
        result->user_data = req->user_data;

        // Normalize path
        char* normalized_path = convert_and_normalize_path(req->filepath);
        if (!normalized_path) {
            result->success = false;
            snprintf(result->error_msg, ASYNC_LOADER_MAX_ERROR_MSG, "Failed to normalize path: %s",
                     req->filepath);
            goto enqueue_result;
        }

        char* subpath = safe_strdup(normalized_path);
        if (!subpath) {
            result->success = false;
            snprintf(result->error_msg, ASYNC_LOADER_MAX_ERROR_MSG,
                     "Memory allocation failed for subpath");
            free(normalized_path);
            goto enqueue_result;
        }

        // Find existing subpath relative to pool directory
        if (!find_existing_subpath(req->pool->directory, &subpath)) {
            result->success = false;
            snprintf(result->error_msg, ASYNC_LOADER_MAX_ERROR_MSG, "Texture file not found: %s",
                     normalized_path);
            free(normalized_path);
            free(subpath);
            goto enqueue_result;
        }

        result->filepath = safe_strdup(subpath);
        free(normalized_path);
        free(subpath);

        if (!result->filepath) {
            result->success = false;
            snprintf(result->error_msg, ASYNC_LOADER_MAX_ERROR_MSG,
                     "Memory allocation failed for filepath");
            goto enqueue_result;
        }

        // Load image data (this is the slow part we're parallelizing)
        int width, height, channels;
        unsigned char* data = stbi_load(result->filepath, &width, &height, &channels, 0);

        if (!data) {
            result->success = false;
            snprintf(result->error_msg, ASYNC_LOADER_MAX_ERROR_MSG, "stbi_load failed: %s",
                     result->filepath);
            goto enqueue_result;
        }

        result->pixel_data = data;
        result->width = width;
        result->height = height;
        result->channels = channels;
        result->success = true;

        // Determine OpenGL format
        if (channels == 1) {
            result->internal_format = GL_RED;
            result->data_format = GL_RED;
        } else if (channels == 3) {
            result->internal_format = GL_SRGB;
            result->data_format = GL_RGB;
        } else {
            result->internal_format = GL_SRGB_ALPHA;
            result->data_format = GL_RGBA;
        }

    enqueue_result:
        // Add to completion queue
        result->next = NULL;
        pthread_mutex_lock(&loader->complete_mutex);
        if (loader->complete_tail) {
            loader->complete_tail->next = result;
        } else {
            loader->complete_head = result;
        }
        loader->complete_tail = result;
        pthread_mutex_unlock(&loader->complete_mutex);

        free(req->filepath);
        free(req);
    }

    return NULL;
}

/*
 * Create async loader with thread pool
 */
AsyncLoader* create_async_loader(void) {
    AsyncLoader* loader = calloc(1, sizeof(AsyncLoader));
    if (!loader) {
        log_error("Failed to allocate AsyncLoader");
        return NULL;
    }

    atomic_store(&loader->shutdown, false);
    atomic_store(&loader->pending_count, 0);
    atomic_store(&loader->completed_count, 0);

    loader->work_head = NULL;
    loader->work_tail = NULL;
    loader->complete_head = NULL;
    loader->complete_tail = NULL;

    if (pthread_mutex_init(&loader->work_mutex, NULL) != 0) {
        log_error("Failed to init work_mutex");
        free(loader);
        return NULL;
    }

    if (pthread_cond_init(&loader->work_cond, NULL) != 0) {
        log_error("Failed to init work_cond");
        pthread_mutex_destroy(&loader->work_mutex);
        free(loader);
        return NULL;
    }

    if (pthread_mutex_init(&loader->complete_mutex, NULL) != 0) {
        log_error("Failed to init complete_mutex");
        pthread_cond_destroy(&loader->work_cond);
        pthread_mutex_destroy(&loader->work_mutex);
        free(loader);
        return NULL;
    }

    // Start worker threads
    for (int i = 0; i < ASYNC_LOADER_WORKER_COUNT; i++) {
        if (pthread_create(&loader->workers[i], NULL, worker_thread_func, loader) != 0) {
            log_error("Failed to create worker thread %d", i);
            // Shutdown already-created threads
            atomic_store(&loader->shutdown, true);
            pthread_cond_broadcast(&loader->work_cond);
            for (int j = 0; j < i; j++) {
                pthread_join(loader->workers[j], NULL);
            }
            pthread_mutex_destroy(&loader->complete_mutex);
            pthread_cond_destroy(&loader->work_cond);
            pthread_mutex_destroy(&loader->work_mutex);
            free(loader);
            return NULL;
        }
    }

    log_info("Created async loader with %d worker threads", ASYNC_LOADER_WORKER_COUNT);
    return loader;
}

/*
 * Free async loader and join threads
 */
void free_async_loader(AsyncLoader* loader) {
    if (!loader) {
        return;
    }

    // Signal shutdown
    atomic_store(&loader->shutdown, true);

    // Wake all workers
    pthread_mutex_lock(&loader->work_mutex);
    pthread_cond_broadcast(&loader->work_cond);
    pthread_mutex_unlock(&loader->work_mutex);

    // Join all workers
    for (int i = 0; i < ASYNC_LOADER_WORKER_COUNT; i++) {
        pthread_join(loader->workers[i], NULL);
    }

    // Free remaining work queue items
    TextureLoadRequest* req = loader->work_head;
    while (req) {
        TextureLoadRequest* next = req->next;
        free(req->filepath);
        free(req);
        req = next;
    }

    // Free remaining completion queue items
    TextureLoadResult* result = loader->complete_head;
    while (result) {
        TextureLoadResult* next = result->next;
        if (result->pixel_data) {
            stbi_image_free(result->pixel_data);
        }
        free(result->filepath);
        free(result);
        result = next;
    }

    pthread_mutex_destroy(&loader->complete_mutex);
    pthread_cond_destroy(&loader->work_cond);
    pthread_mutex_destroy(&loader->work_mutex);

    free(loader);
    log_info("Freed async loader");
}

/*
 * Submit texture load request to worker queue
 */
void load_texture_async(AsyncLoader* loader, TexturePool* pool, const char* filepath,
                        void (*callback)(Texture* tex, void* user_data), void* user_data) {
    if (!loader || !pool || !filepath) {
        log_error("Invalid arguments to load_texture_async");
        if (callback) {
            callback(NULL, user_data);
        }
        return;
    }

    if (!pool->directory) {
        log_error("Texture pool directory not set");
        if (callback) {
            callback(NULL, user_data);
        }
        return;
    }

    // Check if already cached (thread-safe lookup)
    Texture* cached = get_texture_from_pool_threadsafe(pool, filepath);
    if (cached) {
        if (callback) {
            callback(cached, user_data);
        }
        return;
    }

    // Create request
    TextureLoadRequest* req = calloc(1, sizeof(TextureLoadRequest));
    if (!req) {
        log_error("Failed to allocate TextureLoadRequest");
        if (callback) {
            callback(NULL, user_data);
        }
        return;
    }

    req->pool = pool;
    req->filepath = safe_strdup(filepath);
    req->callback = callback;
    req->user_data = user_data;
    req->next = NULL;

    if (!req->filepath) {
        log_error("Failed to duplicate filepath");
        free(req);
        if (callback) {
            callback(NULL, user_data);
        }
        return;
    }

    // Add to work queue
    pthread_mutex_lock(&loader->work_mutex);
    if (loader->work_tail) {
        loader->work_tail->next = req;
    } else {
        loader->work_head = req;
    }
    loader->work_tail = req;
    pthread_cond_signal(&loader->work_cond);
    pthread_mutex_unlock(&loader->work_mutex);

    atomic_fetch_add(&loader->pending_count, 1);
}

/*
 * Process completed texture loads on main thread
 * This is where GL calls happen (must be on main thread with GL context)
 */
size_t async_loader_process_pending(AsyncLoader* loader, TexturePool* pool, size_t max_per_frame) {
    if (!loader || !pool) {
        return 0;
    }

    size_t processed = 0;

    while (processed < max_per_frame) {
        // Pop from completion queue
        TextureLoadResult* result = NULL;

        pthread_mutex_lock(&loader->complete_mutex);
        if (loader->complete_head) {
            result = loader->complete_head;
            loader->complete_head = result->next;
            if (!loader->complete_head) {
                loader->complete_tail = NULL;
            }
        }
        pthread_mutex_unlock(&loader->complete_mutex);

        if (!result) {
            break;
        }

        Texture* texture = NULL;

        if (result->success) {
            // Check cache again (another thread may have loaded same texture)
            texture = get_texture_from_pool_threadsafe(pool, result->filepath);

            if (!texture) {
                // Create texture and upload to GPU
                texture = create_texture();
                if (texture) {
                    GLuint textureID;
                    glGenTextures(1, &textureID);
                    glBindTexture(GL_TEXTURE_2D, textureID);

                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

                    glTexImage2D(GL_TEXTURE_2D, 0, result->internal_format, result->width,
                                 result->height, 0, result->data_format, GL_UNSIGNED_BYTE,
                                 result->pixel_data);
                    glGenerateMipmap(GL_TEXTURE_2D);

                    texture->id = textureID;
                    texture->filepath = safe_strdup(result->filepath);
                    texture->width = result->width;
                    texture->height = result->height;
                    texture->internal_format = result->internal_format;
                    texture->data_format = result->data_format;

                    add_texture_to_pool_threadsafe(pool, texture);

                    glBindTexture(GL_TEXTURE_2D, 0);
                }
            }

            // Free pixel data now that it's on GPU
            stbi_image_free(result->pixel_data);
            result->pixel_data = NULL;
        } else {
            log_error("Async texture load failed: %s", result->error_msg);
        }

        // Invoke callback
        if (result->callback) {
            result->callback(texture, result->user_data);
        }

        atomic_fetch_sub(&loader->pending_count, 1);
        atomic_fetch_add(&loader->completed_count, 1);

        free(result->filepath);
        free(result);
        processed++;
    }

    return processed;
}

/*
 * Check if any work is pending
 */
bool async_loader_is_busy(AsyncLoader* loader) {
    if (!loader) {
        return false;
    }
    return atomic_load(&loader->pending_count) > 0;
}

/*
 * Get number of pending texture loads
 */
size_t async_loader_pending_count(AsyncLoader* loader) {
    if (!loader) {
        return 0;
    }
    return atomic_load(&loader->pending_count);
}
