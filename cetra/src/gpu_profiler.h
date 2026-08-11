#ifndef GPU_PROFILER_H
#define GPU_PROFILER_H

#include <GL/glew.h>
#include <stdbool.h>

// Per-pass GPU timing (spec 11.27). Exists because every performance number in
// this tree before it was wall-clock measured around the WHOLE frame, which can
// say whether a feature cost anything but not which of thirty-odd post passes
// should give back two milliseconds.
//
// Scopes are FLAT -- non-overlapping, never nested. That is not a
// simplification: GL_TIME_ELAPSED permits one active query per target, and the
// primitive that does nest (GL_TIMESTAMP via glQueryCounter) returns 0 on the
// GL-over-Metal driver this engine runs on.
//
// Lives for the length of the engine and is created only when asked for, so a
// run without the flag issues no query calls at all.

#define GPU_PROFILER_MAX_SCOPES 64

// Frames of latency between issuing a query and reading it. Results are only
// ever taken when the driver says they are ready; a blocking read would stall
// the pipeline this exists to measure, and would report the stall as the cost
// of whichever pass happened to be wrapped.
#define GPU_PROFILER_RING 4

typedef struct GPUProfiler GPUProfiler;

// NULL on allocation failure, or when the driver has no timer queries at all.
GPUProfiler* create_gpu_profiler(void);
void free_gpu_profiler(GPUProfiler* profiler);

// Frame bracket. Every frame that calls begin must call end, including frames
// that render nothing: the ring index and the display latch both advance here,
// and a frame that skips end freezes both.
//
// dt is the frame's wall-clock delta in seconds. It drives the latch, and it is
// also reported alongside the pass rows as the denominator they should be read
// against -- see gpu_profiler_frame_ms.
void gpu_profiler_begin_frame(GPUProfiler* profiler);
void gpu_profiler_end_frame(GPUProfiler* profiler, double dt);

// name must be a string literal with static lifetime: it is stored by pointer
// and never copied.
//
// Two constraints, both enforced rather than assumed. A name may open at most
// ONCE per frame -- a loop is one scope, not one per iteration -- and scopes may
// not overlap. A begin that violates either is refused AND its matching end is
// swallowed, so a caller cannot be desynchronised by the refusal; re-entering
// the renderer inside an open scope (cubemap capture does this) is therefore
// safe, it simply goes untimed.
void gpu_profiler_scope_begin(GPUProfiler* profiler, const char* name);
void gpu_profiler_scope_end(GPUProfiler* profiler);

// Open a scope only when `timed`, and swallow the matching end when not. For a
// pass whose gate is known at the call site: it keeps the row absent on frames
// the pass sits out, without the caller duplicating the work call to put the
// begin/end on one arm of an if. Passing a NULL name to the plain begin above
// does NOT do this -- that returns without recording a refusal, so the end
// would close whatever scope was already open.
void gpu_profiler_scope_begin_if(GPUProfiler* profiler, bool timed, const char* name);

// Stop and restart timing around a nested re-render of the whole scene. Without
// this the capture's passes would be refused one at a time and reported as a
// wall of errors; with it they are skipped as a block and the frame's own rows
// stay attributable.
void gpu_profiler_suspend(GPUProfiler* profiler);
void gpu_profiler_resume(GPUProfiler* profiler);

// Latched results, stable for half a second at a time. A pass that did not run
// during the window has no row, which is what lets the gates assert that a
// disabled pass is absent rather than present at zero.
int gpu_profiler_row_count(const GPUProfiler* profiler);
const char* gpu_profiler_row_name(const GPUProfiler* profiler, int row);
float gpu_profiler_row_ms(const GPUProfiler* profiler, int row);

// Sum of the rows: GPU time this profiler ACCOUNTED FOR, not the frame.
float gpu_profiler_total_ms(const GPUProfiler* profiler);

// Mean wall-clock frame time over the same window, published beside the total
// as the ceiling to read it against.
//
// The two are NOT the same quantity and their difference is not a single
// thing: it is GPU work no scope covers, plus CPU time, plus whatever the GPU
// spent idle. So a gap does not prove the instrumentation is incomplete -- but
// a total that tracks the frame closely does bound how much can be missing,
// and a bare total bounds nothing at all.
float gpu_profiler_frame_ms(const GPUProfiler* profiler);

// One table on stdout. For headless runs, where there is no HUD to read.
// Non-const: a run too short to have latched a window is published here, so a
// two-second capture reports what it measured instead of an empty table.
void gpu_profiler_report(GPUProfiler* profiler);

#endif // GPU_PROFILER_H
