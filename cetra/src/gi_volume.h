#ifndef _GI_VOLUME_H_
#define _GI_VOLUME_H_

#include <GL/glew.h>
#include <stdbool.h>
#include <stdint.h>
#include <cglm/cglm.h>

#include "mesh.h" // AABB
#include "program.h"
#include "residency.h"

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
 * rebake-on-sun-move). gi_world_mark_dirty re-arms it.
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

// GI_RESIDENT_MAX, the volumes resident at once.
#include "../shaders/include/gi_constants.glsl"

struct Engine;
struct Scene;
struct LightingAtlas;
struct LightingAtlasLayout;

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

    // Convergence. `dirty_count` probes remain to capture, taken from `next_probe` round-robin.
    // The OPENING sweep, into a slot nothing was captured in, runs in one frame at load and at
    // the world's `stream_rate` when begun later (`streamed`); a re-convergence over texels
    // already there is paced by the world's `rate`.
    int next_probe;
    int dirty_count;
    bool streamed;

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
    GIVolume** volumes; // owned; scene_add_gi_volume. One per residency item, in its order
    size_t volume_capacity;
    Residency residency; // which volumes hold the atlas's GI slots, and their texels' state
    // Some volume has swept. Before it, every opening sweep is the load's and runs in one frame.
    bool opened;
} GIWorld;

// `nx * ny * nz` probes over the box. A grid fixed at creation, so no volume exists unfitted.
GIVolume* create_gi_volume(int nx, int ny, int nz, const vec3 box_min, const vec3 box_max);
// A grid laid over a box at a cell size: as many probes per axis as the box holds cells, the
// grid centred on the box, and `classify` on, since nothing kept its probes out of the walls.
GIVolume* create_gi_volume_spaced(const vec3 box_min, const vec3 box_max, float spacing);
void free_gi_volume(GIVolume* gi);

// The atlas region this volume needs, in texels. The scene's lighting atlas sizes its GI slots
// from the largest of these.
void gi_volume_atlas_extent(const GIVolume* gi, int* out_w, int* out_h);

GIWorld* create_gi_world(void);
void free_gi_world(GIWorld* world);

// Takes ownership. False (and the volume freed) on NULL or out of memory.
bool gi_world_add(GIWorld* world, GIVolume* gi);

// Decide which volumes are resident, from the camera, then grow the scene's lighting atlas to
// hold them, put back the kept tiles of those that returned, and capture up to the world's rate
// of probes in each one still dirty whose capture may be taken now (scene_capture_ready). Once
// a frame, before anything else captures, so every pass of the frame agrees on the residency.
// Must run BEFORE the frame's scene pass: it renders the scene internally.
void gi_world_update(GIWorld* world, struct Engine* engine, struct Scene* scene);

// The atlas slots the world needs: one per volume that can be resident at once, each as large as
// the largest volume in the world.
void gi_world_atlas_needs(const GIWorld* world, struct LightingAtlasLayout* layout);

// Upload the resident volumes' grids for a program that samples them; the atlas they sit in is
// bound by lighting_atlas_bind. No-op on a program without the uniforms, so it is safe for every
// material.
void gi_world_bind(const GIWorld* world, const struct LightingAtlas* atlas, ShaderProgram* program);

// A resident volume has its opening sweep still to run.
bool gi_world_pending(const GIWorld* world);

// Every volume the box touches is resident and swept, so a capture inside it is lit by its own
// bounce light rather than the environment's or a neighbour's edge. True when none touches it.
bool gi_world_ready_in(const GIWorld* world, const AABB* box);

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
// captures, what it is doing, whether its tiles wait on the CPU, and a digest of its tiles
// wherever they are (--stream-probe).
void gi_world_probe_print(const GIWorld* world, const struct LightingAtlas* atlas, int frame);

#endif // _GI_VOLUME_H_
