
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

/*
 * A caller that asked for a key already being decoded. Rather than decoding the
 * image a second time, it rides along on the in-flight load and is invoked with
 * the same Texture when it lands.
 */
typedef struct TextureWaiter {
    void (*callback)(Texture* tex, void* user_data);
    void* user_data;

    struct TextureWaiter* next;
} TextureWaiter;

/*
 * One decode in flight, keyed by the caller's submit key. Exists from submit
 * until async_loader_process_pending finalizes it. The texture pool can't serve
 * this role: nothing lands in the pool until the GL upload, which is many
 * frames after the requests were queued.
 */
typedef struct InFlightLoad {
    char* key;
    TextureWaiter* waiters;

    struct InFlightLoad* next;
} InFlightLoad;

/*
 * Texture Load Result - intermediate data between load and GPU upload
 */
typedef struct TextureLoadResult {
    char* key; // submit key; the pool path below may be resolved elsewhere
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
 * Query loading state
 */
bool async_loader_is_busy(AsyncLoader* loader);
size_t async_loader_pending_count(AsyncLoader* loader);

#endif // ASYNC_LOADER_H
