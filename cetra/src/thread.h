#ifndef CETRA_THREAD_H
#define CETRA_THREAD_H

// Minimal threading shim: exactly the intersection of pthreads and Win32 that
// this engine uses -- a worker pool plus mutex/condvar work queues, nothing
// exotic (no cancel, detach, custom attrs, recursive/timed locks, or TLS).
// POSIX maps to pthreads; Windows to SRWLOCK + CONDITION_VARIABLE (lightweight,
// no teardown) and _beginthreadex. The types live here so the pthread/Win32 API
// stays out of the public async_loader.h / texture.h -- callers see only cetra_*
// and cannot reach a pthread_* by accident.

#include <stdbool.h>

#if defined(_WIN32)
// This header is included engine-wide (via async_loader.h/texture.h), so bound
// <windows.h> here rather than trusting a build-level define: NOMINMAX keeps its
// min/max macros from colliding with C++ std::min/max.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
typedef HANDLE cetra_thread_t;
typedef SRWLOCK cetra_mutex_t;
typedef CONDITION_VARIABLE cetra_cond_t;
#else
#include <pthread.h>
typedef pthread_t cetra_thread_t;
typedef pthread_mutex_t cetra_mutex_t;
typedef pthread_cond_t cetra_cond_t;
#endif

// Entry point matches pthread's void*(void*); the return value is unused.
// Returns false if the thread could not be started.
bool cetra_thread_create(cetra_thread_t* thread, void* (*start)(void*), void* arg);
void cetra_thread_join(cetra_thread_t thread);

// Ceiling on a bake's worker count. A constant because the callers below size
// their slab arrays on the stack.
#define CETRA_BAKE_MAX_WORKERS 8

// How many workers to split `rows` across. `requested` of 0 sizes from the
// machine; the result is clamped to [1, min(CETRA_BAKE_MAX_WORKERS, rows)].
int cetra_bake_workers(int requested, int rows);

// Split [0, rows) into `workers` disjoint contiguous bands and run `fn` on each,
// joining before returning.
//
// The split is remainder-free -- band i is [rows*i/workers, rows*(i+1)/workers) --
// so it never overshoots and every worker gets a band whatever the two numbers
// are. A worker that fails to start runs its band inline rather than being
// dropped, which is what makes the result independent of whether the OS could
// spawn anything.
//
// THE POINT IS THAT THE BAND SPLIT IS ONE PIECE OF CODE. Two callers rely on it
// for bitwise-identical output at any worker count -- the cloud noise bake and
// the erosion sim -- and both had their own copy, so a fix to one would silently
// have missed the other. `fn` must write only within its own band; nothing here
// synchronises, and nothing needs to when that holds.
void cetra_bake_bands(int rows, int workers, void (*fn)(void* ctx, int begin, int end), void* ctx);

// init returns false on failure; the rest cannot fail on either backend.
bool cetra_mutex_init(cetra_mutex_t* mutex);
void cetra_mutex_destroy(cetra_mutex_t* mutex);
void cetra_mutex_lock(cetra_mutex_t* mutex);
void cetra_mutex_unlock(cetra_mutex_t* mutex);

bool cetra_cond_init(cetra_cond_t* cond);
void cetra_cond_destroy(cetra_cond_t* cond);
void cetra_cond_wait(cetra_cond_t* cond, cetra_mutex_t* mutex);
void cetra_cond_signal(cetra_cond_t* cond);
void cetra_cond_broadcast(cetra_cond_t* cond);

void cetra_sleep_ms(unsigned milliseconds);

#endif // CETRA_THREAD_H
