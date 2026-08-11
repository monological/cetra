#ifndef GPU_PROFILER_H
#define GPU_PROFILER_H

#include <GL/glew.h>

// Per-pass GPU timing (spec 11.27). Exists because every performance number in
// this tree before it was wall-clock measured around the WHOLE frame, which can
// say whether a feature cost anything but not which of thirty-odd post passes
// should give back two milliseconds.
//
// Scopes are FLAT -- non-overlapping, never nested. That is not a
// simplification, it is the only thing available: GL_TIME_ELAPSED permits one
// active query per target, and the primitive that does nest (GL_TIMESTAMP via
// glQueryCounter) returns 0 on the GL-over-Metal driver this engine runs on.
// Measured, in spec 11.27, before the design was fixed.
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

// NULL on allocation failure. Picks its backend here: GL timer queries when the
// driver has them, CPU timestamps with a glFinish per scope when it does not.
GPUProfiler* create_gpu_profiler(void);
void free_gpu_profiler(GPUProfiler* profiler);

// Frame bracket. dt is the frame's wall-clock delta in seconds, used only to
// drive the display latch -- the timings themselves come from the GPU.
void gpu_profiler_begin_frame(GPUProfiler* profiler);
void gpu_profiler_end_frame(GPUProfiler* profiler, double dt);

// name must be a string literal with static lifetime: it is stored by pointer
// and never copied. Ending without a matching begin, or beginning twice, is
// ignored rather than fatal -- a profiler must not be able to crash a frame.
void gpu_profiler_scope_begin(GPUProfiler* profiler, const char* name);
void gpu_profiler_scope_end(GPUProfiler* profiler);

// Latched results, stable for half a second at a time. A pass that did not run
// during the window has no row, which is what lets the gates assert that a
// disabled pass is absent rather than present at zero.
int gpu_profiler_row_count(const GPUProfiler* profiler);
const char* gpu_profiler_row_name(const GPUProfiler* profiler, int row);
float gpu_profiler_row_ms(const GPUProfiler* profiler, int row);
float gpu_profiler_total_ms(const GPUProfiler* profiler);

// "GL timer queries" or "CPU+glFinish (SERIALIZED)". Printed everywhere the
// numbers are: the fallback inflates totals by draining the pipeline at every
// scope edge, so a reader who cannot tell which one produced a table can draw
// the wrong conclusion from it.
const char* gpu_profiler_backend(const GPUProfiler* profiler);

// One table on stdout. For headless runs, where there is no HUD to read.
// Non-const: a run too short to have latched a window is published here, so a
// two-second capture reports what it measured instead of an empty table.
void gpu_profiler_report(GPUProfiler* profiler);

#endif // GPU_PROFILER_H
