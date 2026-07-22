
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
    char* filepath;               // file path, or the cache key ("*N") for embedded
    bool is_srgb;                 // Color data (albedo/emissive) vs linear data textures
    unsigned char* embedded_data; // owned copy of the compressed bytes; NULL = load from file
    int embedded_size;            // byte count of embedded_data
    void* user_data;
    void (*callback)(Texture* tex, void* user_data);

    struct TextureLoadRequest* next;
} TextureLoadRequest;

// The registry of decodes currently running, and the callers riding along on
// them. Purely internal to async_loader.c -- callers never name these.
typedef struct InFlightLoad InFlightLoad;

/*
 * Texture Load Result - intermediate data between load and GPU upload
 */
typedef struct TextureLoadResult {
    // The caller's submit string, and the handle back into the in-flight
    // registry. Always non-NULL, including for a failed decode -- pool_key
    // may not be.
    char* submit_key;
    // Key the finished Texture is stored in the pool under. For a file texture
    // this is submit_key resolved against pool->directory; for an embedded one
    // it is a copy of submit_key. NULL if the decode failed before resolution.
    char* pool_key;
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

    // Keys currently being decoded, so a repeat request joins the running load
    // instead of decoding the same image again
    InFlightLoad* inflight;
    pthread_mutex_t inflight_mutex;

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
 *
 * Both submit functions, and async_loader_process_pending, must be called on the
 * main (GL) thread: pending_count, the in-flight registry and callback delivery
 * are only consistent under that assumption.
 *
 * `callback` runs at most once per call, with the decoded Texture or NULL on
 * failure, and always on the main thread. It may run SYNCHRONOUSLY inside the
 * submit call -- on a pool hit or on bad arguments -- otherwise it runs later,
 * from async_loader_process_pending. It does NOT run if the loader is freed
 * while the load is still in flight, so it is not a safe place to hang the sole
 * ownership of `user_data`.
 *
 * Submissions are deduplicated by key: a request for a key already decoding
 * attaches to that decode and receives ITS Texture rather than starting a second
 * one. The key therefore decides the result on its own -- `is_srgb` is not part
 * of the identity, so two requests for one key that disagree about it both get
 * the first submitter's choice. A caller that needs the same image in two
 * colorspaces must submit it under two keys. (The texture pool has always been
 * keyed this way; dedup makes it bite sooner and more deterministically.)
 *
 * Keys are matched across the whole loader, not per pool. One AsyncLoader must
 * therefore serve one TexturePool at a time -- embedded keys ("*0", "*1", ...)
 * collide between any two glTF scenes.
 */
void load_texture_async(AsyncLoader* loader, TexturePool* pool, const char* filepath, bool is_srgb,
                        void (*callback)(Texture* tex, void* user_data), void* user_data);

// Decode a compressed image already in memory (e.g. a glTF-embedded PNG) on a
// worker thread. `key` is the pool cache key ("*N"); `data`/`data_size` are the
// compressed bytes, copied internally so the caller may free/release them (and
// the source aiScene) immediately after this returns.
void load_texture_from_memory_async(AsyncLoader* loader, TexturePool* pool, const char* key,
                                    const unsigned char* data, int data_size, bool is_srgb,
                                    void (*callback)(Texture* tex, void* user_data),
                                    void* user_data);

/*
 * Process completed texture loads on main thread (call each frame)
 * Returns number of textures finalized
 */
size_t async_loader_process_pending(AsyncLoader* loader, TexturePool* pool, size_t max_per_frame);

/*
 * True while any submitted decode has not yet been finalized by
 * async_loader_process_pending. Consumers use this as a load-completion gate
 * (the probe capture, the mask-array build, the height-map resolve), so it
 * carries a stronger promise than a statistic: once it reads false, every
 * callback that is going to fire has fired -- including callbacks that joined
 * an in-flight decode instead of submitting one of their own.
 *
 * Two invariants hold that promise up; do not break them:
 *   - waiters are delivered BEFORE pending_count is decremented, in the same
 *     async_loader_process_pending iteration as the decode they joined;
 *   - all submits and all process_pending calls happen on the main thread.
 *
 * It says nothing about success: a failed decode invokes its callback with NULL
 * and is counted as finished either way.
 */
bool async_loader_is_busy(AsyncLoader* loader);
size_t async_loader_pending_count(AsyncLoader* loader);

#endif // ASYNC_LOADER_H
