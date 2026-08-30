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

// The longest single frame the latch window will count as one frame's worth of
// elapsed time. It bounds how far one hitch can stretch the window it lands in;
// it is NOT a bound on what a frame may cost, and using it as one silently
// reports every frame below 10 fps as exactly 10 fps.
//
// A second bound on the frame TIME was tried here and reverted before it
// shipped. Dropping a sample above a threshold leaves the scope counters still
// carrying that frame, so FRAME falls below the TIMED it is documented to bound
// -- and a window of nothing but slow frames publishes 0.000, a no-data
// sentinel printed as a measurement. Magnitude is not a proxy for stall-ness
// either: profiler_suspend is how a caller says "this is not rendering", and it
// works because the caller knows.
//
// That failure now arrives through a different door and the door is here: the
// frame is measured by this file rather than handed in, so this is the only
// clamp left anywhere near it, and clamping the accumulator instead of the
// window is the same lie in one fewer place.
#define PROFILER_LATCH_STEP_MAX 0.1

// Retired frames whose every scope read back exactly zero before saying so.
// A driver that accepts the calls and reports nothing is a real failure mode of
// this platform, and it is indistinguishable from a working one at the call
// site -- only the results tell them apart. Reported and left at zero rather
// than papered over, so the gate's nonzero arm fails instead of passing on
// numbers from somewhere else.
#define PROFILER_ZERO_FRAMES_TO_WARN 8

// How many times the latch drain polls one query before giving up on it. A bound
// on pathology, not a tuned number: a query issued up to PROFILER_RING frames
// ago is normally ready on the first poll, and the only thing this has to do is
// refuse to spin forever on a driver that never signals.
#define PROFILER_DRAIN_SPINS 1000

typedef struct ProfilerScope {
    const char* name; // borrowed literal, never freed
    GLuint query[PROFILER_RING];
    unsigned char issued[PROFILER_RING]; // this slot holds a real query
    double accum_ms; // summed over the latch window
    float shown_ms;  // latched, what the HUD reads

    // CPU wall-clock over the same bracket.
    double cpu_t0;
    double cpu_accum_ms;
    float cpu_shown_ms;

    // Frames in this window the scope was entered. NOT a denominator -- every
    // published row divides by the window's frame count, which is what makes a
    // pass that ran on some frames comparable with one that ran on all. This
    // decides only whether the scope gets a ROW: a pass that did not run has
    // none, which is what lets a gate assert a disabled pass is absent rather
    // than present at zero.
    //
    // There is deliberately no second count for GPU results. The two used to
    // disagree -- results land PROFILER_RING frames late and an unready slot
    // was dropped -- and dividing each column by its own count is what let a
    // scope entered once in a window publish a per-occurrence cost beside
    // per-frame means. The ring is drained at every latch instead, so the
    // results in accum_ms are the frames counted here.
    int frames_entered;
    // Whether this frame already counted. A CPU-only scope may open more than
    // once per frame and its time SUMS; the frame must be counted once, or a
    // row's presence starts depending on how many times it was called.
    unsigned char entered_this_frame;

    // This pass's submission, for the frame just gone. Not latched with the
    // timings: an average over a half-second window would give the one quantity
    // here with no run-to-run spread some.
    SubmitStats submit;
} ProfilerScope;

struct Profiler {
    ProfilerScope scopes[PROFILER_MAX_SCOPES];
    int scope_count;

    unsigned long frame;
    int slot;   // frame % PROFILER_RING
    int active; // scope index with an open query, -1 when none

    // The CPU-only scope's own pair. Separate from `active`/`suppressed` because
    // there is no query for the two kinds to collide over, so one must not
    // refuse the other.
    int cpu_active;
    int cpu_suppressed;

    // Begins refused because one was already open, a name repeated, or the
    // table is full. Counted rather than ignored so the matching ends can be
    // swallowed instead of closing somebody else's query.
    int suppressed;
    int suspends; // nested profiler_suspend depth

    double latch_timer;
    // When this frame's bracket opened -- and, on entry to begin_frame, when the
    // PREVIOUS one did, which is what makes the period measurable without the
    // caller passing anything in.
    double frame_t0;
    // The bracket end_frame measured, banked at the next begin. Both of a
    // frame's clocks are only complete there: its period needs the next frame's
    // start, so banking the bracket at end_frame instead would put the two in
    // different windows and cost the FRAME <= PERIOD ordering its exactness.
    double pending_bracket_ms;
    double frame_accum_ms;  // the brackets: work, no swap and no vsync wait
    double period_accum_ms; // begin-to-begin: everything the frame cost
    int frame_samples;      // the ONE denominator every published row divides by

    int rows[PROFILER_MAX_SCOPES]; // scope indices, in first-seen order
    int row_count;
    float total_ms;
    float cpu_total_ms;
    float frame_ms;
    float period_ms;

    // Depth complexity. One query on its own ring, generated on first use so a
    // caller that never asks pays nothing.
    GLuint samples_query[PROFILER_RING];
    unsigned char samples_issued[PROFILER_RING];
    int samples_generated;
    int samples_open; // a query is live; guards an unmatched end
    size_t samples_shaded;
    size_t sample_budget;

    int zero_streak;
    int warned_zero;
    int warned_drain;
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
    {"triangles", offsetof(SubmitStats, triangles)},
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
    profiler->cpu_active = -1;
    return profiler;
}

void free_profiler(Profiler* profiler) {
    if (!profiler)
        return;
    for (int i = 0; i < profiler->scope_count; ++i)
        glDeleteQueries(PROFILER_RING, profiler->scopes[i].query);
    if (profiler->samples_generated)
        glDeleteQueries(PROFILER_RING, profiler->samples_query);
    free(profiler);
}

// This frame counts once for this scope, however many times it closed. Shared
// by both scope kinds so the guard cannot be right on one path and absent on the
// other: the GPU path used to increment unguarded and lean on the
// once-per-frame query rule instead, which is the same answer reached by a
// different mechanism and only one of them was written down.
static void mark_entered(ProfilerScope* scope) {
    if (scope->entered_this_frame)
        return;
    scope->entered_this_frame = 1;
    scope->frames_entered++;
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
        saw_any = 1;
        if (ns != 0)
            saw_nonzero = 1;
    }

    // The depth-complexity query rides the same slot and the same
    // check-never-wait rule. Kept out of the zero-streak accounting below: zero
    // samples passed is a legitimate answer (a frame aimed at empty sky), where
    // zero nanoseconds is not.
    if (profiler->samples_generated && profiler->samples_issued[slot]) {
        GLuint available = 0;
        glGetQueryObjectuiv(profiler->samples_query[slot], GL_QUERY_RESULT_AVAILABLE, &available);
        // The flag clears only once the result has actually been taken. Clearing
        // it first -- as this did -- drops an unready result permanently and
        // leaves samples_shaded holding an older frame's number with nothing to
        // say so. The slot is about to be reissued either way, so the worst case
        // is one lost sample rather than a stale reading that looks live.
        if (available) {
            GLuint64 passed = 0;
            glGetQueryObjectui64v(profiler->samples_query[slot], GL_QUERY_RESULT, &passed);
            profiler->samples_shaded = (size_t)passed;
            profiler->samples_issued[slot] = 0;
        }
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

// Take every outstanding result before publishing a window, waiting for the ones
// the GPU has not finished. Called once per latch and nowhere else.
//
// This is the ONE place the never-wait rule is suspended, and the rule survives
// intact where it was written: a per-frame block would stall the pipeline this
// exists to measure and would bill the stall to whichever pass was wrapped. This
// blocks once every PROFILER_LATCH_SECONDS, outside every scope and outside the
// frame bracket, so it can lengthen the next period and charge nothing to any
// published number.
//
// It is what buys the single denominator. Without it the results in accum_ms are
// a SAMPLE of the window's frames -- the newest queries are still in flight, and
// at the start of a run the ring has not filled at all -- so dividing by the
// frame count understates by however many are outstanding. Measured at the run
// lengths the gates use, that is 8-11%, and it is worst in the first window,
// which is the one profiler_report publishes.
//
// The wait is bounded and the give-up is reported. A driver that never signals
// would otherwise turn a lost sample into a hang, which is a worse failure than
// the one being fixed.
static void drain_ring(Profiler* profiler) {
    int stuck = 0;
    for (int slot = 0; slot < PROFILER_RING; ++slot) {
        for (int i = 0; i < profiler->scope_count; ++i) {
            ProfilerScope* scope = &profiler->scopes[i];
            if (!scope->issued[slot])
                continue;
            GLuint available = 0;
            for (int spin = 0; spin < PROFILER_DRAIN_SPINS && !available; ++spin)
                glGetQueryObjectuiv(scope->query[slot], GL_QUERY_RESULT_AVAILABLE,
                                    &available);
            if (!available)
                stuck++;
        }
        retire_slot(profiler, slot);
    }
    if (stuck > 0 && !profiler->warned_drain) {
        log_error("Profiler: %d timer queries did not land within %d polls; the rows "
                  "below are short by that many frames of GPU time.",
                  stuck, PROFILER_DRAIN_SPINS);
        profiler->warned_drain = 1;
    }
}

// Defined below, beside the accessors that read what it publishes; the frame
// bracket is the only caller and it reads better next to the drain.
static void latch_rows(Profiler* profiler);

void profiler_begin_frame(Profiler* profiler) {
    if (!profiler)
        return;
    profiler->slot = (int)(profiler->frame % PROFILER_RING);
    retire_slot(profiler, profiler->slot);

    // Close the books on the frame that just ended, and latch here rather than
    // at its end. This is the first moment BOTH of its clocks are complete: the
    // bracket end_frame measured, and the period that needs this frame's start
    // to exist at all. Banked together, they land in the same window and share
    // one count, which is what makes FRAME <= PERIOD exact rather than off by a
    // frame at every window boundary.
    //
    // Frame 0 banks nothing because it has no predecessor to measure a period
    // against. That is the same sample the old caller-supplied dt had to skip,
    // arrived at from the geometry of the bracket rather than from a special
    // case about a first frame.
    const double now = glfwGetTime();
    if (profiler->frame > 0) {
        const double period = now - profiler->frame_t0;
        profiler->period_accum_ms += period * 1000.0;
        profiler->frame_accum_ms += profiler->pending_bracket_ms;
        profiler->frame_samples++;
        // The window is clamped; the measurement is not. One long hitch should
        // not stretch the window it lands in -- and with every row now divided
        // by the window's frame count, that clamp is also what stops a single
        // expensive frame becoming a window that is nothing but itself.
        profiler->latch_timer +=
            period > PROFILER_LATCH_STEP_MAX ? PROFILER_LATCH_STEP_MAX : period;
        if (profiler->latch_timer >= PROFILER_LATCH_SECONDS) {
            drain_ring(profiler);
            latch_rows(profiler);
            profiler->latch_timer = 0.0;
        }
    }
    profiler->frame_t0 = now;

    profiler->active = -1;
    profiler->cpu_active = -1;
    profiler->suppressed = 0;
    profiler->cpu_suppressed = 0;
    profiler->suspends = 0;
    // Must precede the shadow depth pass, which draws.
    for (int i = 0; i < profiler->scope_count; ++i) {
        profiler->scopes[i].submit = (SubmitStats){0};
        profiler->scopes[i].entered_this_frame = 0;
    }
}

SubmitStats* profiler_submit(Profiler* profiler) {
    // No open scope means either a suspended re-render or work outside any
    // pass, and neither belongs in a per-pass count.
    if (!profiler || profiler->active < 0)
        return NULL;
    return &profiler->scopes[profiler->active].submit;
}

SubmitStats profiler_submit_total(const Profiler* profiler) {
    SubmitStats total = {0};
    if (!profiler)
        return total;
    for (int i = 0; i < profiler->scope_count; ++i) {
        const SubmitStats* s = &profiler->scopes[i].submit;
        total.meshes_seen += s->meshes_seen;
        total.meshes_culled += s->meshes_culled;
        total.draws += s->draws;
        total.instances += s->instances;
        total.material_switches += s->material_switches;
        total.triangles += s->triangles;
    }
    return total;
}

const SubmitStats* profiler_row_submit(const Profiler* profiler, int row) {
    if (!profiler || row < 0 || row >= profiler->row_count)
        return NULL;
    return &profiler->scopes[profiler->rows[row]].submit;
}

int profiler_submit_row_count(void) {
    return (int)(sizeof(k_submit_fields) / sizeof(k_submit_fields[0]));
}

const char* profiler_submit_row_name(int row) {
    if (row < 0 || row >= profiler_submit_row_count())
        return "";
    return k_submit_fields[row].name;
}

size_t submit_stat_value(const SubmitStats* stats, int row) {
    if (!stats || row < 0 || row >= profiler_submit_row_count())
        return 0;
    return *(const size_t*)((const char*)stats + k_submit_fields[row].offset);
}

// Publish the averages and start a new window. Rows are rebuilt each latch from
// the scopes that actually ran, so a pass switched off mid-run leaves the table
// rather than sitting at 0.00 forever.
static void latch_rows(Profiler* profiler) {
    profiler->row_count = 0;
    profiler->total_ms = 0.0f;
    profiler->cpu_total_ms = 0.0f;
    const int frames = profiler->frame_samples;
    for (int i = 0; i < profiler->scope_count; ++i) {
        ProfilerScope* scope = &profiler->scopes[i];
        // ONE denominator, and it is the window's frame count rather than
        // anything about this scope. That is what makes the rows addable: a
        // pass entered on every frame and a pass entered once both publish what
        // they cost the AVERAGE frame, so their sum is what the average frame
        // spent and TIMED can be read against FRAME.
        //
        // Whether the scope RAN is a separate question from what it is divided
        // by, and it is the only thing frames_entered decides.
        if (scope->frames_entered > 0 && frames > 0) {
            scope->shown_ms = (float)(scope->accum_ms / frames);
            scope->cpu_shown_ms = (float)(scope->cpu_accum_ms / frames);
            profiler->rows[profiler->row_count++] = i;
            profiler->total_ms += scope->shown_ms;
            profiler->cpu_total_ms += scope->cpu_shown_ms;
        }
        scope->accum_ms = 0.0;
        scope->cpu_accum_ms = 0.0;
        scope->frames_entered = 0;
    }
    profiler->frame_ms = frames > 0 ? (float)(profiler->frame_accum_ms / frames) : 0.0f;
    profiler->period_ms = frames > 0 ? (float)(profiler->period_accum_ms / frames) : 0.0f;
    profiler->frame_accum_ms = 0.0;
    profiler->period_accum_ms = 0.0;
    profiler->frame_samples = 0;
}

void profiler_end_frame(Profiler* profiler) {
    if (!profiler)
        return;
    if (profiler->active >= 0) {
        log_error("Profiler: scope '%s' was never closed",
                  profiler->scopes[profiler->active].name);
        // Closing it matters as much as reporting it, and this is the recovery
        // the samples query below already had. A query left active fails the
        // next frame's glBeginQuery with INVALID_OPERATION while `issued` and
        // `active` are set regardless, so the NEXT scope's glEndQuery closes
        // this one's query instead: from then on every GPU row is some other
        // pass's time under this pass's name, and nothing says so.
        glEndQuery(GL_TIME_ELAPSED);
        profiler->active = -1;
    }
    // Same recovery for the samples query, which had none: an early return
    // between begin and end would otherwise leave it open forever, so no query
    // is ever issued again and the depth-complexity reading freezes at its last
    // value -- nonzero, so the arm that reads it keeps passing.
    if (profiler->samples_open) {
        log_error("Profiler: the samples query was never closed");
        glEndQuery(GL_SAMPLES_PASSED);
        profiler->samples_open = 0;
    }
    // What this frame's work cost, measured rather than taken from the caller.
    // Banked at the next begin, where the period that contains it is also
    // known -- see profiler_begin_frame.
    //
    // This is the bracket and not the wall period: the swap, the vsync wait and
    // the poll are all outside it. That is what makes it a ceiling the CPU rows
    // are actually inside of, and it is why PERIOD is published beside it for
    // anything asking what the frame cost end to end.
    profiler->pending_bracket_ms = (glfwGetTime() - profiler->frame_t0) * 1000.0;
    profiler->frame++;
}

void profiler_samples_begin(Profiler* profiler) {
    // Suspended means a nested re-render (cubemap capture); its coverage is not
    // this frame's and would overwrite the number with somebody else's viewport.
    if (!profiler || profiler->suspends > 0 || profiler->samples_open)
        return;
    if (!profiler->samples_generated) {
        glGenQueries(PROFILER_RING, profiler->samples_query);
        profiler->samples_generated = 1;
    }
    if (profiler->samples_issued[profiler->slot])
        return; // already measured this frame; a second pass would replace it
    glBeginQuery(GL_SAMPLES_PASSED, profiler->samples_query[profiler->slot]);
    profiler->samples_issued[profiler->slot] = 1;
    profiler->samples_open = 1;
}

void profiler_samples_end(Profiler* profiler) {
    if (!profiler || !profiler->samples_open)
        return;
    glEndQuery(GL_SAMPLES_PASSED);
    profiler->samples_open = 0;
}

void profiler_set_sample_budget(Profiler* profiler, size_t samples) {
    if (profiler)
        profiler->sample_budget = samples;
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
    mark_entered(scope);
    profiler->active = -1;
}

void profiler_cpu_scope_begin(Profiler* profiler, const char* name) {
    if (!profiler)
        return;
    // Nesting is still refused -- one open at a time keeps the accumulate
    // unambiguous -- but a REPEAT is not, which is the whole point of this pair.
    //
    // Refused inside an open GPU scope too, and that is not symmetry for its own
    // sake. Both kinds accumulate through cpu_accum_ms off one cpu_t0 per scope,
    // so overlapping them bills the same wall time twice and TIMED stops being
    // bounded by the frame that contains it -- which profiler_frame_ms now calls
    // an identity rather than a convention. Flatness was true of every call site
    // and enforced at none of them.
    if (profiler->suspends > 0 || profiler->cpu_active >= 0 || profiler->active >= 0) {
        profiler->cpu_suppressed++;
        return;
    }
    int index = scope_index(profiler, name);
    if (index < 0) {
        profiler->cpu_suppressed++;
        return;
    }
    profiler->scopes[index].cpu_t0 = glfwGetTime();
    profiler->cpu_active = index;
}

void profiler_cpu_scope_end(Profiler* profiler) {
    if (!profiler)
        return;
    if (profiler->cpu_suppressed > 0) {
        profiler->cpu_suppressed--;
        return;
    }
    if (profiler->cpu_active < 0)
        return;
    ProfilerScope* scope = &profiler->scopes[profiler->cpu_active];
    scope->cpu_accum_ms += (glfwGetTime() - scope->cpu_t0) * 1000.0;
    mark_entered(scope);
    profiler->cpu_active = -1;
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

float profiler_period_ms(const Profiler* profiler) {
    return profiler ? profiler->period_ms : 0.0f;
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
    // The ceiling TIMED is read against, and it means something different in the
    // two columns. Every CPU scope closes inside this bracket and they do not
    // overlap, so for that column TIMED <= FRAME is an IDENTITY -- a violation
    // is a bug in the profiler, not a slow frame. For the GPU column it stays a
    // bound and a loose one: the driver runs behind, so a pass's time can land
    // outside the bracket that submitted it, and the difference also carries
    // whatever the GPU spent idle.
    printf("%-28s %8.3f ms\n", "FRAME (wall)", profiler->frame_ms);
    // What the frame cost end to end, which the bracket above deliberately is
    // not: the swap, the vsync wait and the poll all sit outside it. Published
    // because that is the number a budget is read against -- a vsync-locked
    // frame doing 3 ms of work in a 16.7 ms period has headroom the bracket
    // alone cannot show.
    printf("%-28s %8.3f ms\n", "PERIOD", profiler->period_ms);
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
    // Out here rather than inside the printer: every line between the banners
    // must parse as a row, and this belongs to one column only -- passing that
    // through a function whose flag means "which column" would make the flag
    // mean two things.
    if (profiler->row_count > 0)
        printf("(CPU rows include driver backpressure: a blocked pass bills the wait, so a row"
               " late in the frame is a ceiling and not a cost.)\n");

    // Counts, per pass and from the last completed frame rather than the latch
    // window: they are exact per frame, and averaging them would turn the one
    // quantity here with no run-to-run spread into one that has some.
    //
    // Per pass because that is the granularity the questions are asked at --
    // whether culling shrank the shadow layers, whether batching shrank the
    // opaque pass. TOTAL is the sum, for the identities that hold frame-wide.
    printf("\n===== SUBMISSION =====\n");
    printf("%-28s", "pass");
    for (int col = 0; col < profiler_submit_row_count(); ++col)
        printf(" %10s", profiler_submit_row_name(col));
    printf("\n");
    for (int row = 0; row < profiler->row_count; ++row) {
        const SubmitStats* s = profiler_row_submit(profiler, row);
        if (!s || s->meshes_seen == 0)
            continue; // a pass that submits no scene mesh has nothing to say here
        printf("%-28s", profiler_row_name(profiler, row));
        for (int col = 0; col < profiler_submit_row_count(); ++col)
            printf(" %10zu", submit_stat_value(s, col));
        printf("\n");
    }
    SubmitStats total = profiler_submit_total(profiler);
    printf("%-28s", "TOTAL");
    for (int col = 0; col < profiler_submit_row_count(); ++col)
        printf(" %10zu", submit_stat_value(&total, col));
    printf("\n===== END SUBMISSION =====\n");

    // Its own block rather than a SUBMISSION column: every column there is a
    // count of what was submitted, and this is a count of what survived, which
    // is a different question measured by a different instrument. Folding it in
    // would also change the row format four gates parse.
    //
    // Printed even at zero, because zero is a result -- a frame aimed at empty
    // sky shades nothing, and an absent block would read as "not measured".
    printf("\n===== SHADING =====\n");
    printf("%-28s %12zu\n", "samples shaded", profiler->samples_shaded);
    printf("%-28s %12zu\n", "sample budget", profiler->sample_budget);
    printf("%-28s %12.2f\n", "depth complexity",
           profiler->sample_budget > 0
               ? (double)profiler->samples_shaded / (double)profiler->sample_budget
               : 0.0);
    printf("===== END SHADING =====\n\n");
}
