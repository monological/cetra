#include "thread.h"

#if defined(_WIN32)

#include <process.h> // _beginthreadex
#include <stdint.h>  // uintptr_t (_beginthreadex's return type)
#include <stdlib.h>

// _beginthreadex wants an `unsigned __stdcall (*)(void*)` entry, so route the
// pthread-shaped start routine through a heap thunk. The caller keeps the
// void*(void*) signature; the return value is discarded (the loader ignores it).
typedef struct {
    void* (*start)(void*);
    void* arg;
} cetra_thread_thunk;

static unsigned __stdcall cetra_thread_trampoline(void* param) {
    cetra_thread_thunk thunk = *(cetra_thread_thunk*)param;
    free(param);
    thunk.start(thunk.arg);
    return 0;
}

bool cetra_thread_create(cetra_thread_t* thread, void* (*start)(void*), void* arg) {
    cetra_thread_thunk* thunk = malloc(sizeof(*thunk));
    if (!thunk) {
        return false;
    }
    thunk->start = start;
    thunk->arg = arg;
    uintptr_t handle = _beginthreadex(NULL, 0, cetra_thread_trampoline, thunk, 0, NULL);
    if (handle == 0) {
        free(thunk);
        return false;
    }
    *thread = (HANDLE)handle;
    return true;
}

void cetra_thread_join(cetra_thread_t thread) {
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
}

// SRWLOCK / CONDITION_VARIABLE initialize in place and need no destroy.
bool cetra_mutex_init(cetra_mutex_t* mutex) {
    InitializeSRWLock(mutex);
    return true;
}
void cetra_mutex_destroy(cetra_mutex_t* mutex) {
    (void)mutex;
}
void cetra_mutex_lock(cetra_mutex_t* mutex) {
    AcquireSRWLockExclusive(mutex);
}
void cetra_mutex_unlock(cetra_mutex_t* mutex) {
    ReleaseSRWLockExclusive(mutex);
}

bool cetra_cond_init(cetra_cond_t* cond) {
    InitializeConditionVariable(cond);
    return true;
}
void cetra_cond_destroy(cetra_cond_t* cond) {
    (void)cond;
}
void cetra_cond_wait(cetra_cond_t* cond, cetra_mutex_t* mutex) {
    SleepConditionVariableSRW(cond, mutex, INFINITE, 0);
}
void cetra_cond_signal(cetra_cond_t* cond) {
    WakeConditionVariable(cond);
}
void cetra_cond_broadcast(cetra_cond_t* cond) {
    WakeAllConditionVariable(cond);
}

void cetra_sleep_ms(unsigned milliseconds) {
    Sleep(milliseconds);
}

#else // POSIX

#include <time.h>

bool cetra_thread_create(cetra_thread_t* thread, void* (*start)(void*), void* arg) {
    return pthread_create(thread, NULL, start, arg) == 0;
}
void cetra_thread_join(cetra_thread_t thread) {
    pthread_join(thread, NULL);
}

bool cetra_mutex_init(cetra_mutex_t* mutex) {
    return pthread_mutex_init(mutex, NULL) == 0;
}
void cetra_mutex_destroy(cetra_mutex_t* mutex) {
    pthread_mutex_destroy(mutex);
}
void cetra_mutex_lock(cetra_mutex_t* mutex) {
    pthread_mutex_lock(mutex);
}
void cetra_mutex_unlock(cetra_mutex_t* mutex) {
    pthread_mutex_unlock(mutex);
}

bool cetra_cond_init(cetra_cond_t* cond) {
    return pthread_cond_init(cond, NULL) == 0;
}
void cetra_cond_destroy(cetra_cond_t* cond) {
    pthread_cond_destroy(cond);
}
void cetra_cond_wait(cetra_cond_t* cond, cetra_mutex_t* mutex) {
    pthread_cond_wait(cond, mutex);
}
void cetra_cond_signal(cetra_cond_t* cond) {
    pthread_cond_signal(cond);
}
void cetra_cond_broadcast(cetra_cond_t* cond) {
    pthread_cond_broadcast(cond);
}

void cetra_sleep_ms(unsigned milliseconds) {
    struct timespec ts;
    ts.tv_sec = milliseconds / 1000u;
    ts.tv_nsec = (long)(milliseconds % 1000u) * 1000000L;
    nanosleep(&ts, NULL);
}

#endif

// --- band-parallel bake, one implementation for both backends ---------------

#include "util.h" // get_cpu_cores

typedef struct {
    void (*fn)(void*, int, int);
    void* ctx;
    int begin;
    int end;
} CetraBandJob;

static void* cetra_band_worker(void* arg) {
    CetraBandJob* job = (CetraBandJob*)arg;
    job->fn(job->ctx, job->begin, job->end);
    return NULL;
}

int cetra_bake_workers(int requested, int rows) {
    int workers = requested > 0 ? requested : get_cpu_cores();
    if (workers > CETRA_BAKE_MAX_WORKERS)
        workers = CETRA_BAKE_MAX_WORKERS;
    if (workers > rows)
        workers = rows;
    return workers < 1 ? 1 : workers;
}

void cetra_bake_bands(int rows, int workers, void (*fn)(void* ctx, int begin, int end), void* ctx) {
    if (!fn || rows <= 0)
        return;
    if (workers < 2) {
        fn(ctx, 0, rows);
        return;
    }
    if (workers > CETRA_BAKE_MAX_WORKERS)
        workers = CETRA_BAKE_MAX_WORKERS;

    CetraBandJob jobs[CETRA_BAKE_MAX_WORKERS];
    cetra_thread_t threads[CETRA_BAKE_MAX_WORKERS];
    bool running[CETRA_BAKE_MAX_WORKERS] = {false};

    for (int i = 0; i < workers; i++) {
        jobs[i].fn = fn;
        jobs[i].ctx = ctx;
        jobs[i].begin = rows * i / workers;
        jobs[i].end = rows * (i + 1) / workers;
        running[i] = cetra_thread_create(&threads[i], cetra_band_worker, &jobs[i]);
        if (!running[i])
            cetra_band_worker(&jobs[i]); // could not start: run the band inline
    }
    for (int i = 0; i < workers; i++) {
        if (running[i])
            cetra_thread_join(threads[i]);
    }
}
