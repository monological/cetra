#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gpu_profiler.h"
#include "ext/log.h"

// How long the displayed numbers hold still. Matches the FPS counter's bucket
// (engine.c) rather than showing a per-frame value: raw per-frame milliseconds
// are unreadable, and this is the only smoothing convention the HUD has.
#define GPU_PROFILER_LATCH_SECONDS 0.5

// Retired frames whose every scope read back exactly zero before the GL backend
// is declared broken. A driver that accepts the calls and reports nothing is a
// real failure mode of the platform this targets, and it is indistinguishable
// from a working driver at the call site -- only the results tell them apart.
#define GPU_PROFILER_ZERO_FRAMES_TO_FALL_BACK 8

typedef enum GpuProfilerBackend {
    GPU_BACKEND_QUERIES = 0,
    GPU_BACKEND_CPU_FINISH,
} GpuProfilerBackend;

typedef struct GpuScope {
    const char* name; // borrowed literal, never freed
    GLuint query[GPU_PROFILER_RING];
    unsigned char issued[GPU_PROFILER_RING]; // this slot holds a real query
    double accum_ms;                         // summed over the latch window
    int samples;                             // frames that contributed to accum
    float shown_ms;                          // latched, what the HUD reads
    unsigned char shown;                     // had a sample in the last window
} GpuScope;

struct GPUProfiler {
    GpuScope scopes[GPU_PROFILER_MAX_SCOPES];
    int scope_count;

    GpuProfilerBackend backend;
    unsigned long frame;
    int slot;    // frame % GPU_PROFILER_RING
    int active;  // scope index with an open query, -1 when none

    double cpu_scope_start; // CPU backend only

    double latch_timer;
    int rows[GPU_PROFILER_MAX_SCOPES]; // scope indices, in first-seen order
    int row_count;
    float total_ms;

    int zero_streak;
    int retired_any;
};

static double monotonic_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

GPUProfiler* create_gpu_profiler(void) {
    GPUProfiler* profiler = calloc(1, sizeof(GPUProfiler));
    if (!profiler) {
        log_error("Failed to allocate GPU profiler");
        return NULL;
    }
    profiler->active = -1;

    // Capability is asserted from GL_TIME_ELAPSED and nothing else: this
    // driver answers GL_TIMESTAMP with 0 while its scoped queries work, so a
    // probe of the neighbouring primitive picks the wrong backend. Whether the
    // queries REPORT anything is not knowable until frames have run, which is
    // what the zero streak is for.
    profiler->backend = GLEW_ARB_timer_query ? GPU_BACKEND_QUERIES : GPU_BACKEND_CPU_FINISH;
    if (profiler->backend == GPU_BACKEND_CPU_FINISH)
        log_info("GPU profiler: no ARB_timer_query, timing on the CPU with glFinish");

    return profiler;
}

void free_gpu_profiler(GPUProfiler* profiler) {
    if (!profiler)
        return;
    if (profiler->backend == GPU_BACKEND_QUERIES) {
        for (int i = 0; i < profiler->scope_count; ++i)
            glDeleteQueries(GPU_PROFILER_RING, profiler->scopes[i].query);
    }
    free(profiler);
}

// Give up on the GL backend and keep timing on the CPU. The queries are dropped
// rather than left dangling, and the switch is announced once -- silently
// changing what a number means is the failure this whole feature exists to
// avoid.
static void fall_back_to_cpu(GPUProfiler* profiler) {
    log_info("GPU profiler: timer queries returned zero for %d frames; "
             "falling back to CPU+glFinish (totals will be inflated)",
             GPU_PROFILER_ZERO_FRAMES_TO_FALL_BACK);
    for (int i = 0; i < profiler->scope_count; ++i) {
        glDeleteQueries(GPU_PROFILER_RING, profiler->scopes[i].query);
        memset(profiler->scopes[i].query, 0, sizeof(profiler->scopes[i].query));
        memset(profiler->scopes[i].issued, 0, sizeof(profiler->scopes[i].issued));
    }
    profiler->backend = GPU_BACKEND_CPU_FINISH;
}

// Scope index for a name, appending on first sight. Compared by content rather
// than pointer: identical literals in different translation units are not
// required to share an address, and two rows for one pass would be worse than
// the linear scan this costs.
static int scope_index(GPUProfiler* profiler, const char* name) {
    for (int i = 0; i < profiler->scope_count; ++i) {
        if (strcmp(profiler->scopes[i].name, name) == 0)
            return i;
    }
    if (profiler->scope_count >= GPU_PROFILER_MAX_SCOPES) {
        log_error("GPU profiler: more than %d scopes; '%s' is not timed",
                  GPU_PROFILER_MAX_SCOPES, name);
        return -1;
    }
    int index = profiler->scope_count++;
    GpuScope* scope = &profiler->scopes[index];
    memset(scope, 0, sizeof(*scope));
    scope->name = name;
    if (profiler->backend == GPU_BACKEND_QUERIES)
        glGenQueries(GPU_PROFILER_RING, scope->query);
    return index;
}

// Collect the ring slot about to be overwritten. Availability is checked, never
// waited on: a slot whose result is not ready is dropped, costing one sample out
// of a half-second window, where blocking would cost the measurement its
// meaning.
static void retire_slot(GPUProfiler* profiler, int slot) {
    if (profiler->backend != GPU_BACKEND_QUERIES)
        return;

    int saw_any = 0;
    int saw_nonzero = 0;
    for (int i = 0; i < profiler->scope_count; ++i) {
        GpuScope* scope = &profiler->scopes[i];
        if (!scope->issued[slot])
            continue;
        scope->issued[slot] = 0;

        GLuint available = 0;
        glGetQueryObjectuiv(scope->query[slot], GL_QUERY_RESULT_AVAILABLE, &available);
        if (!available)
            continue;

        GLuint64 ns = 0;
        glGetQueryObjectui64v(scope->query[slot], GL_QUERY_RESULT, &ns);
        scope->accum_ms += (double)ns * 1e-6;
        scope->samples++;
        saw_any = 1;
        if (ns != 0)
            saw_nonzero = 1;
    }

    if (!saw_any)
        return;
    profiler->retired_any = 1;
    profiler->zero_streak = saw_nonzero ? 0 : profiler->zero_streak + 1;
    if (profiler->zero_streak >= GPU_PROFILER_ZERO_FRAMES_TO_FALL_BACK)
        fall_back_to_cpu(profiler);
}

void gpu_profiler_begin_frame(GPUProfiler* profiler) {
    if (!profiler)
        return;
    profiler->slot = (int)(profiler->frame % GPU_PROFILER_RING);
    retire_slot(profiler, profiler->slot);
    profiler->active = -1;
}

// Publish the averages and start a new window. Rows are rebuilt each latch from
// the scopes that actually ran, so a pass switched off mid-run leaves the table
// rather than sitting at 0.00 forever.
static void latch_rows(GPUProfiler* profiler) {
    profiler->row_count = 0;
    profiler->total_ms = 0.0f;
    for (int i = 0; i < profiler->scope_count; ++i) {
        GpuScope* scope = &profiler->scopes[i];
        scope->shown = 0;
        if (scope->samples > 0) {
            scope->shown_ms = (float)(scope->accum_ms / scope->samples);
            scope->shown = 1;
            profiler->rows[profiler->row_count++] = i;
            profiler->total_ms += scope->shown_ms;
        }
        scope->accum_ms = 0.0;
        scope->samples = 0;
    }
}

void gpu_profiler_end_frame(GPUProfiler* profiler, double dt) {
    if (!profiler)
        return;
    if (profiler->active >= 0) {
        log_error("GPU profiler: scope '%s' was never closed",
                  profiler->scopes[profiler->active].name);
        profiler->active = -1;
    }
    profiler->frame++;

    // Clamped for the reason the FPS counter clamps: one long hitch should not
    // stretch the window it happens to land in.
    profiler->latch_timer += dt > 0.1 ? 0.1 : dt;
    if (profiler->latch_timer >= GPU_PROFILER_LATCH_SECONDS) {
        latch_rows(profiler);
        profiler->latch_timer = 0.0;
    }
}

void gpu_profiler_scope_begin(GPUProfiler* profiler, const char* name) {
    if (!profiler || !name)
        return;
    if (profiler->active >= 0) {
        // Flat by construction. Nesting here would silently drop the inner
        // timing under GL_TIME_ELAPSED, so it is reported instead.
        log_error("GPU profiler: '%s' opened inside '%s'; scopes must not nest", name,
                  profiler->scopes[profiler->active].name);
        return;
    }
    int index = scope_index(profiler, name);
    if (index < 0)
        return;

    profiler->active = index;
    if (profiler->backend == GPU_BACKEND_QUERIES) {
        GpuScope* scope = &profiler->scopes[index];
        if (scope->issued[profiler->slot]) {
            // Same name twice in one frame: the ring slot is already spoken for
            // and the second begin would orphan the first. Loops are meant to be
            // one scope, not one per iteration.
            profiler->active = -1;
            return;
        }
        glBeginQuery(GL_TIME_ELAPSED, scope->query[profiler->slot]);
        scope->issued[profiler->slot] = 1;
    } else {
        glFinish();
        profiler->cpu_scope_start = monotonic_seconds();
    }
}

void gpu_profiler_scope_end(GPUProfiler* profiler) {
    if (!profiler || profiler->active < 0)
        return;
    GpuScope* scope = &profiler->scopes[profiler->active];
    if (profiler->backend == GPU_BACKEND_QUERIES) {
        glEndQuery(GL_TIME_ELAPSED);
    } else {
        glFinish();
        scope->accum_ms += (monotonic_seconds() - profiler->cpu_scope_start) * 1000.0;
        scope->samples++;
    }
    profiler->active = -1;
}

int gpu_profiler_row_count(const GPUProfiler* profiler) {
    return profiler ? profiler->row_count : 0;
}

const char* gpu_profiler_row_name(const GPUProfiler* profiler, int row) {
    if (!profiler || row < 0 || row >= profiler->row_count)
        return "";
    return profiler->scopes[profiler->rows[row]].name;
}

float gpu_profiler_row_ms(const GPUProfiler* profiler, int row) {
    if (!profiler || row < 0 || row >= profiler->row_count)
        return 0.0f;
    return profiler->scopes[profiler->rows[row]].shown_ms;
}

float gpu_profiler_total_ms(const GPUProfiler* profiler) {
    return profiler ? profiler->total_ms : 0.0f;
}

const char* gpu_profiler_backend(const GPUProfiler* profiler) {
    if (!profiler)
        return "none";
    return profiler->backend == GPU_BACKEND_QUERIES ? "GL timer queries"
                                                    : "CPU+glFinish (SERIALIZED)";
}

void gpu_profiler_report(GPUProfiler* profiler) {
    if (!profiler)
        return;
    // A run shorter than one latch window has collected samples but published
    // nothing. Reporting the empty table would say "every pass was free" about
    // a frame that plainly was not -- and short runs are exactly what a gate
    // and a CI capture do.
    if (profiler->row_count == 0)
        latch_rows(profiler);
    printf("\n===== GPU TIMING (%s) =====\n", gpu_profiler_backend(profiler));
    if (profiler->row_count == 0) {
        // Distinguished from "everything was free": no window has closed yet, or
        // no pass ever ran inside one.
        printf("no passes timed (run longer than %.1fs to latch a window)\n",
               GPU_PROFILER_LATCH_SECONDS);
    }
    for (int row = 0; row < profiler->row_count; ++row) {
        printf("%-28s %8.3f ms\n", gpu_profiler_row_name(profiler, row),
               gpu_profiler_row_ms(profiler, row));
    }
    printf("%-28s %8.3f ms\n", "TOTAL", profiler->total_ms);
    printf("===== END GPU TIMING =====\n\n");
}
