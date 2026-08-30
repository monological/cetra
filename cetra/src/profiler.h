#ifndef PROFILER_H
#define PROFILER_H

#include <GL/glew.h>
#include <stdbool.h>
#include <stddef.h>

// What one frame cost, three ways: GPU time per pass (spec 11.27), CPU time per
// pass, and what submission issued to get it (spec 11.28). Exists because every
// performance number in this tree before it was wall-clock measured around the
// WHOLE frame, which can say whether a feature cost anything but not which of
// thirty-odd post passes should give back two milliseconds.
//
// Scopes are FLAT -- non-overlapping, never nested. That is not a
// simplification: GL_TIME_ELAPSED permits one active query per target, and the
// primitive that does nest (GL_TIMESTAMP via glQueryCounter) returns 0 on the
// GL-over-Metal driver this engine runs on.
//
// Lives for the length of the engine and is created only when asked for, so a
// run without the flag issues no query calls and keeps no counts at all.
//
// The CPU column brackets the same scope as the GPU one, and the two are NOT
// comparable term by term. GL_TIME_ELAPSED reports what the GPU spent and
// nothing about what submitting the work cost, so a pass issuing hundreds of
// small draws can read near zero there while dominating the frame -- but
// CPU-in-scope also includes driver backpressure, so a draw that blocks on a
// full command queue charges GPU time to the CPU column. Read it at a
// resolution low enough that the frame is not pixel-bound, or it measures the
// wait rather than the submission.
//
// The counts have no such caveat, which is why they carry the claims: an
// integer has no run-to-run spread, and "553 draws became 71" is either true or
// the feature does not work.

#define PROFILER_MAX_SCOPES 64

// Frames of latency between issuing a query and reading it. Results are only
// ever taken when the driver says they are ready; a blocking read would stall
// the pipeline this exists to measure, and would report the stall as the cost
// of whichever pass happened to be wrapped.
#define PROFILER_RING 4

typedef struct Profiler Profiler;

// What one PASS's submission issued -- per scope, like the two timing columns,
// because that is the granularity every question about it is asked at: whether
// culling shrank the shadow layers, whether batching shrank the opaque pass,
// whether one traversal replaced four. A frame-wide bag can answer none of
// those, and it silently folds in work the timing columns exclude.
//
// meshes_seen counts every mesh a pass considered, so
// meshes_seen == instances + meshes_culled holds exactly. The identity is on
// instances rather than draws because batching makes one draw carry many
// meshes, and an invariant that a later phase breaks is not one.
typedef struct SubmitStats {
    size_t meshes_seen;
    size_t meshes_culled;
    size_t draws;             // glDraw* calls issued for scene meshes
    size_t instances;         // meshes those draws carried; equals draws until batching
    size_t material_switches; // material blocks uploaded, in whichever pass has one
    // Triangles actually submitted, summed over instances. The one counter LOD
    // moves: a level change leaves draws and instances alone and shows up only
    // here, so an arm that watches draws cannot see whether selection works.
    size_t triangles;
} SubmitStats;

// NULL on allocation failure, or when the driver has no timer queries at all.
Profiler* create_profiler(void);
void free_profiler(Profiler* profiler);

// The counters of the scope currently open, to increment at a draw site. NULL
// when there is no profiler, when timing is suspended, or when no scope is
// open -- so an unprofiled run keeps no counts, and a nested re-render that
// the timing columns sit out cannot inflate the counts either.
SubmitStats* profiler_submit(Profiler* profiler);

// Summed over the scopes that ran, for the whole-frame totals.
SubmitStats profiler_submit_total(const Profiler* profiler);

// Frame bracket. Every frame that calls begin must call end, including frames
// that render nothing: the ring index and the display latch both advance here,
// and a frame that skips end freezes both.
//
// The bracket is also the clock. begin stamps, end measures, and nothing is
// passed in -- a caller's delta is the duration of the frame BEFORE the one
// whose scopes it would be divided against, and it contains the swap and the
// vsync wait, neither of which any scope can be inside. See profiler_frame_ms.
void profiler_begin_frame(Profiler* profiler);
void profiler_end_frame(Profiler* profiler);

// name must be a string literal with static lifetime: it is stored by pointer
// and never copied.
//
// Two constraints, both enforced rather than assumed. A name may open at most
// ONCE per frame -- a loop is one scope, not one per iteration -- and scopes may
// not overlap. A begin that violates either is refused AND its matching end is
// swallowed, so a caller cannot be desynchronised by the refusal; re-entering
// the renderer inside an open scope (cubemap capture does this) is therefore
// safe, it simply goes untimed.
void profiler_scope_begin(Profiler* profiler, const char* name);
void profiler_scope_end(Profiler* profiler);

// A scope with no GPU query, for work that issues no GL at all.
//
// The once-per-frame rule above is GL_TIME_ELAPSED's one-active-query-per-target
// limit wearing a contract. A scope that opens no query has no such limit, so
// these SUM across repeats within a frame instead of refusing the second -- which
// is what makes a function reachable from more than one call site per frame
// timeable at all. Its GPU row reads 0.000 by construction.
//
// The published average is still per FRAME, not per call: repeats add their time
// and the frame is counted once, so the row stays comparable with every other row
// and with profiler_frame_ms.
//
// Its own open/refusal state, so a repeat of one kind cannot be mistaken for a
// nesting of the other. It IS refused inside an open GPU scope, though: the two
// share one wall clock per scope, so overlapping them would bill the same time
// twice and cost profiler_frame_ms the identity it claims.
void profiler_cpu_scope_begin(Profiler* profiler, const char* name);
void profiler_cpu_scope_end(Profiler* profiler);

// Open a scope only when `timed`, and swallow the matching end when not. For a
// pass whose gate is known at the call site: it keeps the row absent on frames
// the pass sits out, without the caller duplicating the work call to put the
// begin/end on one arm of an if. Passing a NULL name to the plain begin above
// does NOT do this -- that returns without recording a refusal, so the end
// would close whatever scope was already open.
void profiler_scope_begin_if(Profiler* profiler, bool timed, const char* name);

// Stop and restart timing around a nested re-render of the whole scene. Without
// this the capture's passes would be refused one at a time and reported as a
// wall of errors; with it they are skipped as a block and the frame's own rows
// stay attributable.
void profiler_suspend(Profiler* profiler);
void profiler_resume(Profiler* profiler);

// Latched results, stable for half a second at a time. A pass that did not run
// during the window has no row, which is what lets the gates assert that a
// disabled pass is absent rather than present at zero.
int profiler_row_count(const Profiler* profiler);
const char* profiler_row_name(const Profiler* profiler, int row);
float profiler_row_ms(const Profiler* profiler, int row);

// Wall-clock time between the same scope's begin and end, latched over the same
// window. Rows are shared with the GPU column, so an index valid for one is
// valid for the other.
float profiler_row_cpu_ms(const Profiler* profiler, int row);

// Sum of the rows: GPU time this profiler ACCOUNTED FOR, not the frame.
float profiler_total_ms(const Profiler* profiler);
float profiler_cpu_total_ms(const Profiler* profiler);

// The submission counters as named rows, so the stdout table and the HUD walk
// one vocabulary instead of each spelling the field names out.
int profiler_submit_row_count(void);
const char* profiler_submit_row_name(int row);
size_t submit_stat_value(const SubmitStats* stats, int row);

// One pass's counters, by the same row index the timing accessors use, so a
// caller can walk scopes once and read time and submission together.
const SubmitStats* profiler_row_submit(const Profiler* profiler, int row);

// Mean time inside the frame BRACKET over the same window -- the frame's own
// work, measured from begin to end. Unbounded above: a slow frame is the thing
// this row exists to show, and a stall that is not rendering is excluded by the
// caller through profiler_suspend rather than guessed at here by magnitude.
//
// EVERY PUBLISHED ROW DIVIDES BY THE SAME COUNT, the frames in the window, so
// every row means the same thing: what this cost the average frame. A scope
// entered once in a window and a scope entered every frame are therefore
// ADDABLE, which is what lets TIMED be read against this at all. Each column
// used to divide by its own count -- GPU results the driver returned, frames
// the scope was entered, frames in the window -- and a scope gated on something
// occasional then published a per-occurrence cost beside per-frame means. A
// one-frame GI sweep printed a TIMED 21x this row with nothing wrong in the
// clock (spec 11.97).
//
// For the CPU column TIMED <= FRAME is an IDENTITY: scopes are flat and every
// one closes inside this bracket. A violation is a bug here, never a slow
// frame. For the GPU column it is a loose bound in both directions -- the
// driver runs behind, so a pass's time can land outside the bracket that
// submitted it -- and the gap there also carries whatever the GPU spent idle,
// so it bounds how much can be missing rather than measuring it.
float profiler_frame_ms(const Profiler* profiler);

// Mean time from one frame's start to the next: the whole period, including the
// swap, the vsync wait and the poll, none of which the bracket above contains.
//
// Both are published because they answer different questions and neither
// answers the other's. FRAME is the ceiling the rows are inside of. This is
// what the frame COST, so it is the one that matches an fps counter and the one
// a budget is read against -- a vsync-locked frame doing 3 ms of work in a
// 16.7 ms period has five sixths of its budget left, and the bracket alone
// reads as though it had none.
//
// FRAME <= PERIOD always, and exactly rather than approximately: both are banked
// for the same frame at the same moment, so they cannot land in different
// windows or be divided by different counts.
float profiler_period_ms(const Profiler* profiler);

// Samples that survived the depth test between these calls -- for the opaque
// pass, the samples the uber-shader actually ran for. Against the frame's sample
// budget that is mean depth complexity, which is the quantity Wall 4 of the
// roadmap is about and the one thing this instrument could not previously see:
// every counter above measures what was SUBMITTED, and overdraw is invisible to
// all of them.
//
// A single GL_SAMPLES_PASSED query rather than one per scope. It is only
// meaningful where scene geometry is shaded, and issuing it around thirty
// fullscreen post passes would spend real query traffic measuring a constant.
// GL_SAMPLES_PASSED is a different query TARGET from GL_TIME_ELAPSED, so this
// nests inside a timing scope rather than competing with it.
//
// Counts samples that PASSED, not fragments that shaded, and the two differ in
// two directions that matter:
//
//   - Early-Z rejects before the shader runs, so a rejected fragment costs
//     nothing and passes nothing. Correct bias: a prepass works by making
//     early-Z reject, so the number it is meant to move is the one counted.
//   - A fragment that DISCARDS costs a full shader invocation and passes no
//     sample. So this UNDERSTATES the work wherever alpha-masked geometry
//     dominates -- which is exactly the foliage case a prepass most wants to
//     shield. Measured: apps/forest reads 1.85 here while shading samples 4.4x
//     slower than an interior reading 2.78, and the gap is the leaf cards
//     whose fragments run and then discard.
//
// So this is a floor on depth complexity, not an estimate of it. A number above
// 1 proves redundant shading; a number near 1 does not prove its absence.
void profiler_samples_begin(Profiler* profiler);
void profiler_samples_end(Profiler* profiler);

// The frame's sample budget -- render width * height * MSAA samples -- as the
// denominator the count is read against. Published by whoever owns the scene
// target: the profiler cannot know the supersample or render-scale factors.
void profiler_set_sample_budget(Profiler* profiler, size_t samples);

// One table on stdout. For headless runs, where there is no HUD to read.
// Non-const: a run too short to have latched a window is published here, so a
// two-second capture reports what it measured instead of an empty table.
void profiler_report(Profiler* profiler);

#endif // PROFILER_H
