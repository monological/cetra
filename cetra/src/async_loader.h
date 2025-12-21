
#ifndef ASYNC_LOADER_H
#define ASYNC_LOADER_H

#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stddef.h>

#include <GL/glew.h>

#include "texture.h"

#define ASYNC_LOADER_WORKER_COUNT  4
#define ASYNC_LOADER_MAX_ERROR_MSG 256

/*
 * Texture Load Request - submitted to work queue
 */
typedef struct TextureLoadRequest {
    TexturePool* pool;
    char* filepath;
    void* user_data;
    void (*callback)(Texture* tex, void* user_data);

    struct TextureLoadRequest* next;
} TextureLoadRequest;

/*
 * Texture Load Result - intermediate data between load and GPU upload
 */
typedef struct TextureLoadResult {
    char* filepath;
    unsigned char* pixel_data;
    int width;
    int height;
    int channels;
    GLenum internal_format;
    GLenum data_format;

    void* user_data;
    void (*callback)(Texture* tex, void* user_data);

    bool success;
    char error_msg[ASYNC_LOADER_MAX_ERROR_MSG];

    struct TextureLoadResult* next;
} TextureLoadResult;

/*
 * Async Loader - thread pool for parallel texture loading
 */
typedef struct AsyncLoader {
    pthread_t workers[ASYNC_LOADER_WORKER_COUNT];
    atomic_bool shutdown;

    // Work queue (main thread -> workers)
    TextureLoadRequest* work_head;
    TextureLoadRequest* work_tail;
    pthread_mutex_t work_mutex;
    pthread_cond_t work_cond;

    // Completion queue (workers -> main thread)
    TextureLoadResult* complete_head;
    TextureLoadResult* complete_tail;
    pthread_mutex_t complete_mutex;

    // Statistics
    atomic_size_t pending_count;
    atomic_size_t completed_count;
} AsyncLoader;

/*
 * Lifecycle
 */
AsyncLoader* create_async_loader(void);
void free_async_loader(AsyncLoader* loader);

/*
 * Async texture loading
 */
void load_texture_async(AsyncLoader* loader, TexturePool* pool, const char* filepath,
                        void (*callback)(Texture* tex, void* user_data), void* user_data);

/*
 * Process completed texture loads on main thread (call each frame)
 * Returns number of textures finalized
 */
size_t async_loader_process_pending(AsyncLoader* loader, TexturePool* pool, size_t max_per_frame);

/*
 * Query loading state
 */
bool async_loader_is_busy(AsyncLoader* loader);
size_t async_loader_pending_count(AsyncLoader* loader);

#endif // ASYNC_LOADER_H
