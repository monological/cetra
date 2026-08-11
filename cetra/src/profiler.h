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

// What one frame's submission issued. meshes_seen counts every mesh a pass
// considered, so meshes_seen == instances + meshes_culled holds exactly; the
// identity is on instances rather than draws because batching makes one draw
// carry many meshes, and an invariant that a later phase breaks is not one.
typedef struct SubmitStats {
    size_t meshes_seen;
    size_t meshes_culled;
    size_t draws;             // glDraw* calls issued for scene meshes
    size_t instances;         // meshes those draws carried; equals draws until batching
    size_t material_switches; // shading-pass re-uploads; the depth path has no material state
} SubmitStats;

// NULL on allocation failure, or when the driver has no timer queries at all.
Profiler* create_profiler(void);
void free_profiler(Profiler* profiler);

// The frame's counters, to increment at a draw site. NULL when there is no
// profiler, which is what keeps an unprofiled run free of the counting: the
// call sites test it once and skip.
SubmitStats* profiler_submit(Profiler* profiler);

// Frame bracket. Every frame that calls begin must call end, including frames
// that render nothing: the ring index and the display latch both advance here,
// and a frame that skips end freezes both.
//
// dt is the frame's wall-clock delta in seconds. It drives the latch, and it is
// also reported alongside the pass rows as the denominator they should be read
// against -- see profiler_frame_ms.
void profiler_begin_frame(Profiler* profiler);
void profiler_end_frame(Profiler* profiler, double dt);

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
size_t profiler_submit_row_value(const Profiler* profiler, int row);

// Mean wall-clock frame time over the same window, published beside the total
// as the ceiling to read it against.
//
// The two are NOT the same quantity and their difference is not a single
// thing: it is GPU work no scope covers, plus CPU time, plus whatever the GPU
// spent idle. So a gap does not prove the instrumentation is incomplete -- but
// a total that tracks the frame closely does bound how much can be missing,
// and a bare total bounds nothing at all.
float profiler_frame_ms(const Profiler* profiler);

// One table on stdout. For headless runs, where there is no HUD to read.
// Non-const: a run too short to have latched a window is published here, so a
// two-second capture reports what it measured instead of an empty table.
void profiler_report(Profiler* profiler);

#endif // PROFILER_H
