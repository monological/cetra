#ifndef _GI_VOLUME_H_
#define _GI_VOLUME_H_

#include <GL/glew.h>
#include <stdbool.h>
#include <stdint.h>
#include <cglm/cglm.h>

#include "program.h"

/*
 * DDGI-style irradiance probe volume (spec 9.7).
 *
 * A uniform grid of probes over the scene AABB. Each probe is captured by
 * rasterizing the real scene into a small cubemap, then projected into two
 * octahedral tiles packed in one atlas: irradiance (what a surface receives from
 * each direction) and visibility (mean distance and mean distance squared, for
 * the Chebyshev test that stops light leaking through walls).
 *
 * Because captures run the full forward shader, probes see clustered lights, LTC
 * panels, the sky and emissive surfaces with no extra work -- that is the whole
 * argument for rasterized capture over an analytic approximation.
 *
 * CADENCE. Probes are re-captured only while the volume is dirty, and a converged
 * volume costs nothing per frame. The roadmap sketch specified a fixed budget
 * every frame forever, which would mean 12 full scene renders per frame -- and
 * each engine_render_scene rebuilds the whole clustered light grid regardless of
 * the 16^2 viewport. Every scene this engine renders is static, so it converges
 * once and then idles, exactly as the sky's LUTs do (sky.h: luts_baked plus
 * rebake-on-sun-move). gi_volume_mark_dirty re-arms it.
 */

// Interior edge of an octahedral tile, in texels. Irradiance is a smooth cosine
// convolution and 8 is plenty; visibility carries a distance discontinuity at
// every silhouette and needs the extra resolution to place it.
#define GI_IRRADIANCE_RES 8
#define GI_VISIBILITY_RES 16
// Every tile carries a 1px gutter so a bilinear tap at the edge reads the
// octahedral map's wrapped neighbour rather than the tile packed beside it.
#define GI_TILE_BORDER 1
// Cube face size for a probe capture. Small on purpose: the consumers are a
// cosine convolution and a distance moment, and the capture cost is dominated by
// scene traversal rather than by fragments.
#define GI_CAPTURE_FACE 16

// The atlas sampler unit in pbr_frag. Deliberately the same number as
// IBL_SKYBOX_TEXTURE_UNIT: sampler units are per PROGRAM, and pbr_frag has never
// sampled the skybox cube, so 14 is the only slot free to it. Reserved for this
// by the roadmap's global texture-unit ledger before either feature was built.
#define GI_ATLAS_TEXTURE_UNIT 14

// Volumes resident at once: the nearest to the camera, each holding a slot of the scene's
// lighting atlas (spec 13.24). Must match GI_SLOTS in include/gi_volume.glsl. The world holds
// any number; this caps what one frame samples, and is set by the shader's uniform table
// rather than by memory.
#define GI_RESIDENT_MAX 8

struct Engine;
struct Scene;
struct LightingAtlas;

typedef struct GIVolume {
    // SETTINGS: plain stores.
    //
    // Each probe tests whether it sits inside geometry and switches itself off if so, at the
    // price of a second, back-face capture per probe. What lets a grid be laid over a
    // building's bounds with no corner tuned by hand to keep its probes out of the walls.
    bool classify;

    // ENGINE-OWNED: read, never write.
    //
    // Grid. Probe (x,y,z) sits at grid_min + (i + 0.5) * spacing, so probes are
    // cell CENTRES -- a probe exactly on the scene AABB face would be inside the
    // wall it is meant to sample away from.
    int counts[3];
    vec3 grid_min;
    vec3 spacing;
    // Capture far plane, and the unit the visibility moments are stored in --
    // they are normalised by it so a large scene cannot overflow fp16 when the
    // squared moment is written. Derived from the grid, so it lives here rather
    // than being recomputed per sweep.
    float far_clip;

    // Atlas rows the irradiance block takes in the volume's slot. Irradiance tiles occupy the
    // top rows and visibility the rest; both are addressed by probe index.
    int irradiance_rows;
    // The scratch below is built; gi_ensure_targets returns early on it.
    bool targets_ready;

    // Per-probe capture scratch, reused for every probe.
    GLuint capture_color;  // cubemap, GI_CAPTURE_FACE^2, RGB16F
    GLuint capture_depth;  // cubemap, GI_CAPTURE_FACE^2, DEPTH_COMPONENT24
    GLuint classify_depth; // the same, drawn with back faces only; 0 unless classify
    GLuint quad_vao, quad_vbo;

    ShaderProgram* project_program;

    // Convergence. `dirty_count` probes remain to capture, taken from
    // `next_probe` round-robin. An opening sweep at load runs in one frame, one
    // started later at the world's `stream_rate` (`streamed`); after it the
    // world's `rate` paces re-convergence.
    int next_probe;
    int dirty_count;
    bool first_pass;
    bool streamed;

    // Streaming (spec 13.24). `kept` is the slot's texels as of the last convergence, RGBA half
    // floats, so a volume that leaves residency and comes back is uploaded rather than
    // captured; NULL until it first converges, and dropped when its light changes.
    // `resident_slot` is the slot holding its texels, or about to; `upload_pending` says those
    // texels are still on the CPU.
    uint16_t* kept;
    int resident_slot;
    bool upload_pending;

    // Every probe capture this volume has ever run. The converge-then-idle
    // claim is only worth making if it is checkable, and this is the check: it
    // must stop advancing the moment the volume converges.
    int captures_total;

    bool failed; // One-shot: allocation is not retried every frame
} GIVolume;

// Every GI volume in the world (spec 13.24), owned by the scene. Each is a grid over one place
// -- a building, a cave -- and the nearest GI_RESIDENT_MAX of them hold a slot of the scene's
// lighting atlas and are what a frame samples.
typedef struct GIWorld {
    // SETTINGS: plain stores.
    bool enabled;     // false = no volume is captured or sampled
    int rate;         // probes per frame while a swept volume re-converges; 0 = all at once
    int stream_rate;  // probes per frame in an opening sweep begun after load; 0 = all at once
    bool debug_atlas; // draw the lighting atlas over the composited frame

    // ENGINE-OWNED: read, never write.
    GIVolume** volumes; // owned; scene_add_gi_volume
    int count;
    int capacity;
    int* slot_of;                // each volume's atlas slot, or -1 while not resident
    int* home;                   // the slot each volume last held, or -1
    int holder[GI_RESIDENT_MAX]; // each slot's volume, or -1
    float* distance;             // each volume's distance from the camera, this frame's ranking
    // Some volume has swept. Before it, every opening sweep is the load's and runs in one frame.
    bool opened;
} GIWorld;

GIVolume* create_gi_volume(int nx, int ny, int nz);
// A grid laid over a box at a cell size: as many probes per axis as the box holds cells, the
// grid centred on the box, and `classify` on, since nothing kept its probes out of the walls.
GIVolume* create_gi_volume_spaced(const vec3 box_min, const vec3 box_max, float spacing);
void free_gi_volume(GIVolume* gi);

// The atlas region this volume needs, in texels. The scene's lighting atlas sizes its GI slots
// from the largest of these.
void gi_volume_atlas_extent(const GIVolume* gi, int* out_w, int* out_h);

// Fit the grid to a scene AABB and arm a full convergence sweep.
void gi_volume_fit(GIVolume* gi, const vec3 aabb_min, const vec3 aabb_max);

// Re-arm every probe. Call when the lighting changed under the volume -- a sun
// move, a light edit, a material change.
void gi_volume_mark_dirty(GIVolume* gi);

// Ready to be sampled from the slot it holds: at least one converged sweep's worth of data.
bool gi_volume_active(const GIVolume* gi);

// Its opening sweep is still to run: a capture now would see the scene without
// the light this volume will give it. False for NULL and for a volume failed or
// never fitted, neither of which will ever have an answer to wait for.
bool gi_volume_pending(const GIVolume* gi);

GIWorld* create_gi_world(void);
void free_gi_world(GIWorld* world);

// Takes ownership. False (and the volume freed) on NULL or out of memory.
bool gi_world_add(GIWorld* world, GIVolume* gi);

// Decide which volumes are resident, from the camera: once a frame, before anything captures,
// so every pass of the frame agrees on it. A volume that loses its slot sweeps again when it
// is next admitted.
void gi_world_rank(GIWorld* world, const struct Engine* engine);

// Grow the scene's lighting atlas to hold the resident volumes and capture up to the world's
// rate of probes in each one still dirty. No-op on a converged world. Must run BEFORE the
// frame's scene pass: it renders the scene internally and leaves the default framebuffer bound.
void gi_world_update(GIWorld* world, struct Engine* engine, struct Scene* scene);

// Bind the atlas and upload the resident volumes' grids for a program that samples them.
// No-op on a program without the uniforms, so it is safe for every material.
void gi_world_bind(const GIWorld* world, const struct LightingAtlas* atlas, ShaderProgram* program);

// A resident volume has its opening sweep still to run.
bool gi_world_pending(const GIWorld* world);

// Some resident volume has probes still to capture.
bool gi_world_dirty(const GIWorld* world);

// Re-arm every volume, for a change in the light every one of them saw.
void gi_world_mark_dirty(GIWorld* world);

// Re-express every grid after a world-origin shift (spec 11.62).
//
// The ATLAS is deliberately kept: a rigid translation moves every probe by the
// same delta as everything it sees, so the irradiance each one recorded is still
// correct. Only where the grids SIT changed. Re-arming instead would re-capture
// every probe -- six scene renders each -- to reproduce what is already stored,
// and would do it at the un-shifted positions unless this ran first anyway.
void gi_world_shift_origin(GIWorld* world, const vec3 delta);

// One line naming each slot's volume, then one per volume: its slot, its distance, its
// captures, what it is doing and whether its tiles are kept (--stream-probe).
void gi_world_probe_print(const GIWorld* world, int frame);

#endif // _GI_VOLUME_H_
