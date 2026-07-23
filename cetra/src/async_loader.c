
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ext/stb_image.h"
#include "ext/log.h"

#include "async_loader.h"
#include "util.h"

/*
 * A caller that asked for a key already being decoded. Rather than decoding the
 * image a second time it rides along on the in-flight load, and is invoked with
 * the same Texture when that load lands.
 */
typedef struct TextureWaiter {
    void (*callback)(Texture* tex, void* user_data);
    void* user_data;

    struct TextureWaiter* next;
} TextureWaiter;

/*
 * One decode in flight, keyed by the caller's submit key. Lives from submit
 * until async_loader_process_pending finalizes it. The texture pool cannot fill
 * this role: nothing reaches the pool until the GL upload, which is many frames
 * after the requests were queued.
 *
 * "In flight" means "this key has an owner you can join", which is very slightly
 * weaker than "exactly one decode is running for this key" -- see the allocation
 * fallbacks in inflight_join_or_claim.
 */
struct InFlightLoad {
    // Identity is (pool, key), not key alone: one AsyncLoader is owned by the
    // Engine while a TexturePool belongs to a Scene, and embedded keys ("*0",
    // "*1", ...) repeat in every glTF. Matching on the string alone would hand
    // one scene's image to another scene's material.
    TexturePool* pool;
    char* key;
    TextureWaiter* waiters;

    struct InFlightLoad* next;
};

// Fill a result from freshly decoded pixels: repair transparent texels (RGBA
// only) off the main thread, then record what the GL upload will need. Shared
// by both decode sources so file and embedded textures cannot drift.
static void finalize_decoded_result(TextureLoadResult* result, unsigned char* pixels, int width,
                                    int height, int channels, bool is_srgb) {
    if (channels == 4) {
        texture_dilate_transparent_rgb(pixels, width, height);
    }
    result->pixel_data = pixels;
    result->width = width;
    result->height = height;
    result->channels = channels;
    result->success = true;
    texture_gl_formats(channels, is_srgb, &result->internal_format, &result->data_format);
}

// Join the decode already running for `key`, or claim the key for this caller.
// Returns true if it joined: an identical decode is in flight, this caller has
// been recorded as a waiter, and it must NOT submit its own request. Returns
// false if it now owns the decode and should proceed to enqueue one.
//
// This is what keeps aliased slots from decoding the same image twice: assimp
// reports one glTF image under several texture types (baseColor as both DIFFUSE
// and BASE_COLOR, metallicRoughness as both METALNESS and DIFFUSE_ROUGHNESS),
// so a material resolves the same key repeatedly, and every one of those lands
// before the first decode has finished and reached the pool.
static bool inflight_join_or_claim(AsyncLoader* loader, TexturePool* pool, const char* key,
                                   void (*callback)(Texture* tex, void* user_data),
                                   void* user_data) {
    bool joined = false;

    cetra_mutex_lock(&loader->inflight_mutex);

    InFlightLoad* entry = loader->inflight;
    while (entry && (entry->pool != pool || strcmp(entry->key, key) != 0)) {
        entry = entry->next;
    }

    if (entry) {
        TextureWaiter* waiter = calloc(1, sizeof(TextureWaiter));
        if (waiter) {
            waiter->callback = callback;
            waiter->user_data = user_data;
            waiter->next = entry->waiters;
            entry->waiters = waiter;
            joined = true;
        }
        // On allocation failure fall through as a claim: decoding a redundant
        // copy is wasteful but correct, whereas dropping the callback is not.
    } else {
        InFlightLoad* fresh = calloc(1, sizeof(InFlightLoad));
        if (fresh) {
            fresh->key = safe_strdup(key);
            if (fresh->key) {
                fresh->pool = pool;
                fresh->next = loader->inflight;
                loader->inflight = fresh;
            } else {
                free(fresh);
                fresh = NULL;
            }
        }
        // Registration failed. Still reported as a claim, so this caller decodes
        // and is answered normally -- only dedup is lost, and concurrent
        // duplicates of this key each decode their own copy. Wasteful, never
        // wrong, and worth knowing about because it looks identical to dedup
        // simply not being hit.
        if (!fresh) {
            log_warn("Texture dedup registry allocation failed for '%s'; "
                     "duplicate requests will each decode",
                     key);
        }
    }

    cetra_mutex_unlock(&loader->inflight_mutex);
    return joined;
}

// Detach `key`'s in-flight entry and hand back its waiter list (caller owns and
// must free it).
//
// NULL covers three distinct cases: the key was registered but nobody joined it,
// the decode failed (waiters are still delivered, with NULL), or the key was
// never registered at all. Every current caller treats those identically; a
// caller that must tell them apart should return found/not-found separately
// rather than overloading the return.
static TextureWaiter* inflight_release(AsyncLoader* loader, const TexturePool* pool,
                                       const char* key) {
    if (!key) {
        return NULL;
    }

    TextureWaiter* waiters = NULL;

    cetra_mutex_lock(&loader->inflight_mutex);
    InFlightLoad** link = &loader->inflight;
    while (*link) {
        if ((*link)->pool == pool && strcmp((*link)->key, key) == 0) {
            InFlightLoad* entry = *link;
            *link = entry->next;
            waiters = entry->waiters;
            free(entry->key);
            free(entry);
            break;
        }
        link = &(*link)->next;
    }
    cetra_mutex_unlock(&loader->inflight_mutex);

    return waiters;
}

// Invoke every waiter with the finished texture (NULL if the decode failed) and
// free the list.
static void waiters_deliver(TextureWaiter* waiter, Texture* texture) {
    while (waiter) {
        TextureWaiter* next = waiter->next;
        if (waiter->callback) {
            waiter->callback(texture, waiter->user_data);
        }
        free(waiter);
        waiter = next;
    }
}

// Publish a filled request to the work queue and wake a worker. pending_count
// counts submitted-but-not-yet-finalized work; the load-time drains that gate
// the mask-array build and the probe capture wait on it reaching zero. It falls
// again in async_loader_process_pending, or in the worker if the request has to
// be dropped there.
//
// The count rises BEFORE the request is visible to a worker: a fast-failing
// decode can otherwise reach the completion queue and be decremented before the
// increment lands, underflowing an unsigned counter that would then never read
// zero again.
static void async_loader_enqueue(AsyncLoader* loader, TextureLoadRequest* req) {
    req->next = NULL;

    atomic_fetch_add(&loader->pending_count, 1);

    cetra_mutex_lock(&loader->work_mutex);
    if (loader->work_tail) {
        loader->work_tail->next = req;
    } else {
        loader->work_head = req;
    }
    loader->work_tail = req;
    cetra_cond_signal(&loader->work_cond);
    cetra_mutex_unlock(&loader->work_mutex);
}

/*
 * Internal: Worker thread function
 */
static void* worker_thread_func(void* arg) {
    AsyncLoader* loader = (AsyncLoader*)arg;

    // stb_image's vertical-flip setting is process-global unless a thread pins
    // its own: decodes here must never inherit a flip another thread set for an
    // unrelated image (the IBL equirect load sets it). Model textures are
    // uploaded unflipped -- UV orientation is handled per-format at import.
    stbi_set_flip_vertically_on_load_thread(0);

    while (!atomic_load(&loader->shutdown)) {
        TextureLoadRequest* req = NULL;

        // Wait for work
        cetra_mutex_lock(&loader->work_mutex);
        while (!loader->work_head && !atomic_load(&loader->shutdown)) {
            cetra_cond_wait(&loader->work_cond, &loader->work_mutex);
        }

        // Pop from work queue
        if (loader->work_head) {
            req = loader->work_head;
            loader->work_head = req->next;
            if (!loader->work_head) {
                loader->work_tail = NULL;
            }
        }
        cetra_mutex_unlock(&loader->work_mutex);

        if (!req) {
            continue;
        }

        // Process the request
        TextureLoadResult* result = calloc(1, sizeof(TextureLoadResult));
        if (!result) {
            log_error("Failed to allocate TextureLoadResult");
            // Nothing will finalize this key now, so drop the claim -- a later
            // request must be free to start its own decode instead of waiting
            // forever. The waiters are discarded rather than invoked: their
            // callbacks touch materials and may only run on the main thread.
            TextureWaiter* orphaned = inflight_release(loader, req->pool, req->filepath);
            while (orphaned) {
                TextureWaiter* next_orphan = orphaned->next;
                free(orphaned);
                orphaned = next_orphan;
            }
            free(req->filepath);
            free(req->embedded_data);
            free(req);
            atomic_fetch_sub(&loader->pending_count, 1);
            continue;
        }

        result->callback = req->callback;
        result->user_data = req->user_data;
        // Moved, not copied. submit_key is the only handle back to the in-flight
        // registry, so it must exist for every result -- a copy here could fail
        // and strand the key claimed forever.
        result->submit_key = req->filepath;
        req->filepath = NULL;

        // Embedded texture: decode the compressed bytes we copied from the
        // aiScene (which may already be released). The submit key is the pool
        // key too -- there is no path to resolve.
        if (req->embedded_data) {
            result->pool_key = safe_strdup(result->submit_key);
            int ew, eh, ec;
            unsigned char* edata =
                result->pool_key ? stbi_load_from_memory(req->embedded_data, req->embedded_size,
                                                         &ew, &eh, &ec, 0)
                                 : NULL;
            if (edata) {
                finalize_decoded_result(result, edata, ew, eh, ec, req->is_srgb);
            } else {
                result->success = false;
                snprintf(result->error_msg, ASYNC_LOADER_MAX_ERROR_MSG,
                         "Failed to decode embedded texture: %s",
                         result->pool_key ? result->submit_key : "(alloc failed)");
            }
            goto enqueue_result;
        }

        // Normalize path
        char* normalized_path = convert_and_normalize_path(result->submit_key);
        if (!normalized_path) {
            result->success = false;
            snprintf(result->error_msg, ASYNC_LOADER_MAX_ERROR_MSG, "Failed to normalize path: %s",
                     result->submit_key);
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

        // The resolved path, which is what the finished texture is pooled under
        // -- deliberately not the submit key, which stays as handed in.
        result->pool_key = safe_strdup(subpath);
        free(normalized_path);
        free(subpath);

        if (!result->pool_key) {
            result->success = false;
            snprintf(result->error_msg, ASYNC_LOADER_MAX_ERROR_MSG,
                     "Memory allocation failed for resolved texture path");
            goto enqueue_result;
        }

        // Load image data (this is the slow part we're parallelizing)
        int width, height, channels;
        unsigned char* data = stbi_load(result->pool_key, &width, &height, &channels, 0);

        if (!data) {
            result->success = false;
            snprintf(result->error_msg, ASYNC_LOADER_MAX_ERROR_MSG, "stbi_load failed: %s",
                     result->pool_key);
            goto enqueue_result;
        }

        finalize_decoded_result(result, data, width, height, channels, req->is_srgb);

    enqueue_result:
        // Add to completion queue
        result->next = NULL;
        cetra_mutex_lock(&loader->complete_mutex);
        if (loader->complete_tail) {
            loader->complete_tail->next = result;
        } else {
            loader->complete_head = result;
        }
        loader->complete_tail = result;
        cetra_mutex_unlock(&loader->complete_mutex);

        free(req->filepath);
        free(req->embedded_data);
        free(req);
    }

    return NULL;
}

/*
 * Size the decode pool to the machine.
 *
 * Two cores are held back rather than one: while these workers decode, the main
 * thread is still walking the scene graph, uploading finished textures and
 * driving GL, and the driver has threads of its own. Oversubscribing slows that
 * critical path down without decoding any faster.
 *
 * The ceiling matters as much as the count. A scene has tens of distinct
 * textures and the tail is one or two large images, so past a handful of workers
 * the wall clock is bounded by the single slowest decode, not by how many run
 * alongside it -- extra threads just cost memory and scheduling.
 */
static int async_loader_worker_count(void) {
    int workers = get_cpu_cores() - 2;
    if (workers < ASYNC_LOADER_MIN_WORKERS) {
        workers = ASYNC_LOADER_MIN_WORKERS;
    }
    if (workers > ASYNC_LOADER_MAX_WORKERS) {
        workers = ASYNC_LOADER_MAX_WORKERS;
    }
    return (int)workers;
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
    loader->inflight = NULL;

    if (!cetra_mutex_init(&loader->work_mutex)) {
        log_error("Failed to init work_mutex");
        free(loader);
        return NULL;
    }

    if (!cetra_cond_init(&loader->work_cond)) {
        log_error("Failed to init work_cond");
        cetra_mutex_destroy(&loader->work_mutex);
        free(loader);
        return NULL;
    }

    if (!cetra_mutex_init(&loader->complete_mutex)) {
        log_error("Failed to init complete_mutex");
        cetra_cond_destroy(&loader->work_cond);
        cetra_mutex_destroy(&loader->work_mutex);
        free(loader);
        return NULL;
    }

    if (!cetra_mutex_init(&loader->inflight_mutex)) {
        log_error("Failed to init inflight_mutex");
        cetra_mutex_destroy(&loader->complete_mutex);
        cetra_cond_destroy(&loader->work_cond);
        cetra_mutex_destroy(&loader->work_mutex);
        free(loader);
        return NULL;
    }

    loader->worker_count = async_loader_worker_count();
    loader->workers = calloc((size_t)loader->worker_count, sizeof(cetra_thread_t));
    if (!loader->workers) {
        log_error("Failed to allocate %d worker threads", loader->worker_count);
        cetra_mutex_destroy(&loader->inflight_mutex);
        cetra_mutex_destroy(&loader->complete_mutex);
        cetra_cond_destroy(&loader->work_cond);
        cetra_mutex_destroy(&loader->work_mutex);
        free(loader);
        return NULL;
    }

    // Start worker threads
    for (int i = 0; i < loader->worker_count; i++) {
        if (!cetra_thread_create(&loader->workers[i], worker_thread_func, loader)) {
            log_error("Failed to create worker thread %d", i);
            // Carry on with the workers that did start -- a smaller pool only
            // decodes more slowly, whereas failing the loader fails the load.
            if (i >= ASYNC_LOADER_MIN_WORKERS) {
                loader->worker_count = i;
                break;
            }
            // Shutdown already-created threads
            atomic_store(&loader->shutdown, true);
            cetra_cond_broadcast(&loader->work_cond);
            for (int j = 0; j < i; j++) {
                cetra_thread_join(loader->workers[j]);
            }
            free(loader->workers);
            cetra_mutex_destroy(&loader->inflight_mutex);
            cetra_mutex_destroy(&loader->complete_mutex);
            cetra_cond_destroy(&loader->work_cond);
            cetra_mutex_destroy(&loader->work_mutex);
            free(loader);
            return NULL;
        }
    }

    log_info("Created async loader with %d worker threads (%d cores)", loader->worker_count,
             get_cpu_cores());
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
    cetra_mutex_lock(&loader->work_mutex);
    cetra_cond_broadcast(&loader->work_cond);
    cetra_mutex_unlock(&loader->work_mutex);

    // Join all workers
    for (int i = 0; i < loader->worker_count; i++) {
        cetra_thread_join(loader->workers[i]);
    }
    free(loader->workers);

    // Free remaining work queue items
    TextureLoadRequest* req = loader->work_head;
    while (req) {
        TextureLoadRequest* next = req->next;
        free(req->filepath);
        free(req->embedded_data);
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
        free(result->submit_key);
        free(result->pool_key);
        free(result);
        result = next;
    }

    // Free any in-flight entries whose decode never got finalized
    InFlightLoad* entry = loader->inflight;
    while (entry) {
        InFlightLoad* next_entry = entry->next;
        TextureWaiter* waiter = entry->waiters;
        while (waiter) {
            TextureWaiter* next_waiter = waiter->next;
            free(waiter);
            waiter = next_waiter;
        }
        free(entry->key);
        free(entry);
        entry = next_entry;
    }

    cetra_mutex_destroy(&loader->inflight_mutex);
    cetra_mutex_destroy(&loader->complete_mutex);
    cetra_cond_destroy(&loader->work_cond);
    cetra_mutex_destroy(&loader->work_mutex);

    free(loader);
    log_info("Freed async loader");
}

/*
 * Shared submit path. `bytes` non-NULL means the compressed image is already in
 * memory (a glTF-embedded PNG); NULL means load `key` from disk.
 *
 * Ordering matters here. Everything that can fail is allocated FIRST, and the
 * key is claimed last, immediately before the request is published. A claim
 * therefore always reaches the queue, so there is no window in which a key is
 * claimed by a load that will never run -- the failure that would strand it
 * cannot be expressed.
 */
static void submit_load(AsyncLoader* loader, TexturePool* pool, const char* key,
                        const unsigned char* bytes, int nbytes, bool is_srgb,
                        void (*callback)(Texture* tex, void* user_data), void* user_data) {
    // Already decoded and uploaded under this key?
    Texture* cached = get_texture_from_pool_threadsafe(pool, key);
    if (cached) {
        if (callback) {
            callback(cached, user_data);
        }
        return;
    }

    TextureLoadRequest* req = calloc(1, sizeof(TextureLoadRequest));
    char* key_copy = safe_strdup(key);
    // Allocated but not filled yet: if this turns out to be a duplicate we drop
    // it below without ever paying for the copy, which for an embedded image is
    // several megabytes.
    unsigned char* copy = bytes ? malloc((size_t)nbytes) : NULL;

    if (!req || !key_copy || (bytes && !copy)) {
        log_error("Failed to allocate texture load request for '%s'", key);
        free(req);
        free(key_copy);
        free(copy);
        if (callback) {
            callback(NULL, user_data);
        }
        return;
    }

    // Nothing above touched shared state, so this is the first and only point
    // of no return.
    if (inflight_join_or_claim(loader, pool, key, callback, user_data)) {
        free(req);
        free(key_copy);
        free(copy);
        return;
    }

    if (bytes) {
        memcpy(copy, bytes, (size_t)nbytes);
    }

    req->pool = pool;
    req->filepath = key_copy;
    req->is_srgb = is_srgb;
    req->embedded_data = copy;
    req->embedded_size = bytes ? nbytes : 0;
    req->callback = callback;
    req->user_data = user_data;

    async_loader_enqueue(loader, req);
}

/*
 * Submit texture load request to worker queue
 */
void load_texture_async(AsyncLoader* loader, TexturePool* pool, const char* filepath, bool is_srgb,
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

    submit_load(loader, pool, filepath, NULL, 0, is_srgb, callback, user_data);
}

void load_texture_from_memory_async(AsyncLoader* loader, TexturePool* pool, const char* key,
                                    const unsigned char* data, int data_size, bool is_srgb,
                                    void (*callback)(Texture* tex, void* user_data),
                                    void* user_data) {
    if (!loader || !pool || !key || !data || data_size <= 0) {
        log_error("Invalid arguments to load_texture_from_memory_async");
        if (callback) {
            callback(NULL, user_data);
        }
        return;
    }

    submit_load(loader, pool, key, data, data_size, is_srgb, callback, user_data);
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

        cetra_mutex_lock(&loader->complete_mutex);
        if (loader->complete_head) {
            result = loader->complete_head;
            loader->complete_head = result->next;
            if (!loader->complete_head) {
                loader->complete_tail = NULL;
            }
        }
        cetra_mutex_unlock(&loader->complete_mutex);

        if (!result) {
            break;
        }

        Texture* texture = NULL;

        if (result->success) {
            // Check cache again (another thread may have loaded same texture)
            texture = get_texture_from_pool_threadsafe(pool, result->pool_key);

            if (!texture) {
                // Create texture and upload to GPU
                texture = create_texture();
                if (texture) {
                    GLuint textureID;
                    glGenTextures(1, &textureID);
                    glBindTexture(GL_TEXTURE_2D, textureID);

                    texture_set_default_sampler_state();

                    glTexImage2D(GL_TEXTURE_2D, 0, result->internal_format, result->width,
                                 result->height, 0, result->data_format, GL_UNSIGNED_BYTE,
                                 result->pixel_data);
                    glGenerateMipmap(GL_TEXTURE_2D);

                    texture->id = textureID;
                    texture->filepath = safe_strdup(result->pool_key);
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

        // Retire the key first: this decode is done, so a request arriving
        // afterwards must start a fresh one rather than join a finished load.
        TextureWaiter* waiters = inflight_release(loader, pool, result->submit_key);

        // Invoke callback
        if (result->callback) {
            result->callback(texture, result->user_data);
        }

        // ...then everyone who asked for the same key while it was decoding.
        // Must happen before the decrement below: async_loader_is_busy promises
        // that once it reads false, every owed callback has run, and a joined
        // waiter is only covered by its owner's count.
        waiters_deliver(waiters, texture);

        atomic_fetch_sub(&loader->pending_count, 1);
        atomic_fetch_add(&loader->completed_count, 1);

        free(result->submit_key);
        free(result->pool_key);
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
