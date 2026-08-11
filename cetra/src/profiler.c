#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "profiler.h"

#include <GLFW/glfw3.h>

#include "ext/log.h"

// How long the displayed numbers hold still. Matches the FPS counter's bucket
// (engine.c) rather than showing a per-frame value: raw per-frame milliseconds
// are unreadable, and this is the only smoothing convention the HUD has.
#define PROFILER_LATCH_SECONDS 0.5

// Retired frames whose every scope read back exactly zero before saying so.
// A driver that accepts the calls and reports nothing is a real failure mode of
// this platform, and it is indistinguishable from a working one at the call
// site -- only the results tell them apart. Reported and left at zero rather
// than papered over, so the gate's nonzero arm fails instead of passing on
// numbers from somewhere else.
#define PROFILER_ZERO_FRAMES_TO_WARN 8

typedef struct ProfilerScope {
    const char* name; // borrowed literal, never freed
    GLuint query[PROFILER_RING];
    unsigned char issued[PROFILER_RING]; // this slot holds a real query
    double accum_ms;                         // summed over the latch window
    int samples;                             // frames that contributed to accum
    float shown_ms;                          // latched, what the HUD reads

    // CPU wall-clock over the same bracket. Kept on its own counter rather than
    // sharing `samples`: the GPU side lands PROFILER_RING frames late and
    // drops any slot the driver has not finished, while this one closes the
    // instant the scope does, so the two advance at different rates.
    double cpu_t0;
    double cpu_accum_ms;
    int cpu_samples;
    float cpu_shown_ms;
} ProfilerScope;

struct Profiler {
    ProfilerScope scopes[PROFILER_MAX_SCOPES];
    int scope_count;

    unsigned long frame;
    int slot;   // frame % PROFILER_RING
    int active; // scope index with an open query, -1 when none

    // Begins refused because one was already open, a name repeated, or the
    // table is full. Counted rather than ignored so the matching ends can be
    // swallowed instead of closing somebody else's query.
    int suppressed;
    int suspends; // nested profiler_suspend depth

    double latch_timer;
    double frame_accum_ms;
    int frame_samples;

    int rows[PROFILER_MAX_SCOPES]; // scope indices, in first-seen order
    int row_count;
    float total_ms;
    float cpu_total_ms;
    float frame_ms;

    int zero_streak;
    int warned_zero;

    SubmitStats submit;
};

// The one place the submission vocabulary is written. Both consumers walk it,
// so a counter added here reaches the stdout table and the HUD together.
static const struct {
    const char* name;
    size_t offset;
} k_submit_fields[] = {
    {"meshes seen", offsetof(SubmitStats, meshes_seen)},
    {"meshes culled", offsetof(SubmitStats, meshes_culled)},
    {"draws", offsetof(SubmitStats, draws)},
    {"instances", offsetof(SubmitStats, instances)},
    {"material switches", offsetof(SubmitStats, material_switches)},
};

Profiler* create_profiler(void) {
    // Asserted from GL_TIME_ELAPSED and nothing else: this driver answers
    // GL_TIMESTAMP with 0 while its scoped queries work, so probing the
    // neighbouring primitive rejects a machine that is fine.
    if (!GLEW_ARB_timer_query) {
        log_error("Profiler: no ARB_timer_query on this driver");
        return NULL;
    }
    Profiler* profiler = calloc(1, sizeof(Profiler));
    if (!profiler) {
        log_error("Failed to allocate profiler");
        return NULL;
    }
    profiler->active = -1;
    return profiler;
}

void free_profiler(Profiler* profiler) {
    if (!profiler)
        return;
    for (int i = 0; i < profiler->scope_count; ++i)
        glDeleteQueries(PROFILER_RING, profiler->scopes[i].query);
    free(profiler);
}

// Scope index for a name, appending on first sight. Compared by content rather
// than pointer: identical literals in different translation units are not
// required to share an address, and two rows for one pass would be worse than
// the linear scan this costs.
static int scope_index(Profiler* profiler, const char* name) {
    for (int i = 0; i < profiler->scope_count; ++i) {
        if (strcmp(profiler->scopes[i].name, name) == 0)
            return i;
    }
    if (profiler->scope_count >= PROFILER_MAX_SCOPES) {
        log_error("Profiler: more than %d scopes; '%s' is not timed", PROFILER_MAX_SCOPES,
                  name);
        return -1;
    }
    int index = profiler->scope_count++;
    ProfilerScope* scope = &profiler->scopes[index];
    memset(scope, 0, sizeof(*scope));
    scope->name = name;
    glGenQueries(PROFILER_RING, scope->query);
    return index;
}

// Collect the ring slot about to be overwritten. Availability is checked, never
// waited on: a slot whose result is not ready is dropped, costing one sample out
// of a half-second window, where blocking would cost the measurement its
// meaning.
static void retire_slot(Profiler* profiler, int slot) {
    int saw_any = 0;
    int saw_nonzero = 0;
    for (int i = 0; i < profiler->scope_count; ++i) {
        ProfilerScope* scope = &profiler->scopes[i];
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
    profiler->zero_streak = saw_nonzero ? 0 : profiler->zero_streak + 1;
    if (profiler->zero_streak >= PROFILER_ZERO_FRAMES_TO_WARN && !profiler->warned_zero) {
        log_error("Profiler: timer queries have returned zero for %d frames; this driver "
                  "accepts them without measuring. The table below is not GPU time.",
                  profiler->zero_streak);
        profiler->warned_zero = 1;
    }
}

void profiler_begin_frame(Profiler* profiler) {
    if (!profiler)
        return;
    profiler->slot = (int)(profiler->frame % PROFILER_RING);
    retire_slot(profiler, profiler->slot);
    profiler->active = -1;
    profiler->suppressed = 0;
    profiler->suspends = 0;
    // Must precede the shadow depth pass, which draws.
    profiler->submit = (SubmitStats){0};
}

SubmitStats* profiler_submit(Profiler* profiler) {
    return profiler ? &profiler->submit : NULL;
}

int profiler_submit_row_count(void) {
    return (int)(sizeof(k_submit_fields) / sizeof(k_submit_fields[0]));
}

const char* profiler_submit_row_name(int row) {
    if (row < 0 || row >= profiler_submit_row_count())
        return "";
    return k_submit_fields[row].name;
}

size_t profiler_submit_row_value(const Profiler* profiler, int row) {
    if (!profiler || row < 0 || row >= profiler_submit_row_count())
        return 0;
    return *(const size_t*)((const char*)&profiler->submit + k_submit_fields[row].offset);
}

// Publish the averages and start a new window. Rows are rebuilt each latch from
// the scopes that actually ran, so a pass switched off mid-run leaves the table
// rather than sitting at 0.00 forever.
static void latch_rows(Profiler* profiler) {
    profiler->row_count = 0;
    profiler->total_ms = 0.0f;
    profiler->cpu_total_ms = 0.0f;
    for (int i = 0; i < profiler->scope_count; ++i) {
        ProfilerScope* scope = &profiler->scopes[i];
        // Row membership is keyed on the GPU sample count alone; the CPU column
        // rides along on whatever rows that produces.
        if (scope->samples > 0) {
            scope->shown_ms = (float)(scope->accum_ms / scope->samples);
            scope->cpu_shown_ms =
                scope->cpu_samples > 0 ? (float)(scope->cpu_accum_ms / scope->cpu_samples) : 0.0f;
            profiler->rows[profiler->row_count++] = i;
            profiler->total_ms += scope->shown_ms;
            profiler->cpu_total_ms += scope->cpu_shown_ms;
        }
        scope->accum_ms = 0.0;
        scope->samples = 0;
        scope->cpu_accum_ms = 0.0;
        scope->cpu_samples = 0;
    }
    profiler->frame_ms =
        profiler->frame_samples > 0
            ? (float)(profiler->frame_accum_ms / profiler->frame_samples)
            : 0.0f;
    profiler->frame_accum_ms = 0.0;
    profiler->frame_samples = 0;
}

void profiler_end_frame(Profiler* profiler, double dt) {
    if (!profiler)
        return;
    if (profiler->active >= 0) {
        log_error("Profiler: scope '%s' was never closed",
                  profiler->scopes[profiler->active].name);
        profiler->active = -1;
    }
    profiler->frame++;

    // Clamped for the reason the FPS counter clamps: one long hitch should not
    // stretch the window it happens to land in.
    double clamped = dt > 0.1 ? 0.1 : dt;
    profiler->latch_timer += clamped;
    profiler->frame_accum_ms += clamped * 1000.0;
    profiler->frame_samples++;
    if (profiler->latch_timer >= PROFILER_LATCH_SECONDS) {
        latch_rows(profiler);
        profiler->latch_timer = 0.0;
    }
}

void profiler_suspend(Profiler* profiler) {
    if (profiler)
        profiler->suspends++;
}

void profiler_resume(Profiler* profiler) {
    if (profiler && profiler->suspends > 0)
        profiler->suspends--;
}

void profiler_scope_begin(Profiler* profiler, const char* name) {
    if (!profiler || !name)
        return;

    // Every refusal below increments `suppressed` so the matching end is
    // swallowed. Without that the end closes whatever query IS open, which
    // silently truncates the outer pass and hands its remainder to the next
    // row -- the failure this instrument exists to detect, inside the
    // instrument.
    if (profiler->suspends > 0 || profiler->active >= 0) {
        profiler->suppressed++;
        return;
    }
    int index = scope_index(profiler, name);
    if (index < 0) {
        profiler->suppressed++;
        return;
    }
    ProfilerScope* scope = &profiler->scopes[index];
    if (scope->issued[profiler->slot]) {
        log_error("Profiler: '%s' opened twice in one frame; it is timed once", name);
        profiler->suppressed++;
        return;
    }

    glBeginQuery(GL_TIME_ELAPSED, scope->query[profiler->slot]);
    scope->issued[profiler->slot] = 1;
    scope->cpu_t0 = glfwGetTime();
    profiler->active = index;
}

void profiler_scope_begin_if(Profiler* profiler, bool timed, const char* name) {
    if (!profiler)
        return;
    if (!timed) {
        profiler->suppressed++;
        return;
    }
    profiler_scope_begin(profiler, name);
}

void profiler_scope_end(Profiler* profiler) {
    if (!profiler)
        return;
    if (profiler->suppressed > 0) {
        profiler->suppressed--;
        return;
    }
    if (profiler->active < 0)
        return;
    glEndQuery(GL_TIME_ELAPSED);
    ProfilerScope* scope = &profiler->scopes[profiler->active];
    scope->cpu_accum_ms += (glfwGetTime() - scope->cpu_t0) * 1000.0;
    scope->cpu_samples++;
    profiler->active = -1;
}

int profiler_row_count(const Profiler* profiler) {
    return profiler ? profiler->row_count : 0;
}

const char* profiler_row_name(const Profiler* profiler, int row) {
    if (!profiler || row < 0 || row >= profiler->row_count)
        return "";
    return profiler->scopes[profiler->rows[row]].name;
}

float profiler_row_ms(const Profiler* profiler, int row) {
    if (!profiler || row < 0 || row >= profiler->row_count)
        return 0.0f;
    return profiler->scopes[profiler->rows[row]].shown_ms;
}

float profiler_row_cpu_ms(const Profiler* profiler, int row) {
    if (!profiler || row < 0 || row >= profiler->row_count)
        return 0.0f;
    return profiler->scopes[profiler->rows[row]].cpu_shown_ms;
}

float profiler_total_ms(const Profiler* profiler) {
    return profiler ? profiler->total_ms : 0.0f;
}

float profiler_cpu_total_ms(const Profiler* profiler) {
    return profiler ? profiler->cpu_total_ms : 0.0f;
}

float profiler_frame_ms(const Profiler* profiler) {
    return profiler ? profiler->frame_ms : 0.0f;
}

// One timing table. Two callers rather than two copies, so the row format the
// gates parse cannot differ between the columns.
static void print_timing_table(const Profiler* profiler, const char* banner, bool cpu) {
    printf("\n===== %s TIMING =====\n", banner);
    if (profiler->row_count == 0)
        printf("no passes timed\n");
    for (int row = 0; row < profiler->row_count; ++row) {
        printf("%-28s %8.3f ms\n", profiler_row_name(profiler, row),
               cpu ? profiler_row_cpu_ms(profiler, row) : profiler_row_ms(profiler, row));
    }
    printf("%-28s %8.3f ms\n", "TIMED", cpu ? profiler->cpu_total_ms : profiler->total_ms);
    // The ceiling TIMED should be read against. Their difference mixes
    // uninstrumented work with whatever the GPU spent idle, so it is a bound on
    // what can be missing rather than a measurement of it -- which is still
    // more than a bare total says.
    printf("%-28s %8.3f ms\n", "FRAME (wall)", profiler->frame_ms);
    printf("===== END %s TIMING =====\n", banner);
}

void profiler_report(Profiler* profiler) {
    if (!profiler)
        return;
    // A run shorter than one latch window has collected samples but published
    // nothing. Reporting the empty table would say "every pass was free" about
    // a frame that plainly was not.
    if (profiler->row_count == 0)
        latch_rows(profiler);

    // Timings in two tables rather than one two-column table, because the GPU
    // table's bytes are a gate assertion surface.
    print_timing_table(profiler, "GPU", false);
    print_timing_table(profiler, "CPU", true);

    // Counts, from the last completed frame rather than the latch window: they
    // are exact per frame, and averaging them would turn the one quantity here
    // with no run-to-run spread into one that has some.
    printf("\n===== SUBMISSION =====\n");
    for (int row = 0; row < profiler_submit_row_count(); ++row) {
        printf("%-28s %8zu\n", profiler_submit_row_name(row),
               profiler_submit_row_value(profiler, row));
    }
    printf("===== END SUBMISSION =====\n\n");
}
