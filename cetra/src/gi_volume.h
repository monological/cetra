#ifndef _GI_VOLUME_H_
#define _GI_VOLUME_H_

#include <GL/glew.h>
#include <stdbool.h>
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
 * each render_current_scene rebuilds the whole clustered light grid regardless of
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

struct Engine;
struct Scene;

typedef struct GIVolume {
    bool enabled;

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

    // One RGBA16F atlas holding both tile types. Irradiance tiles occupy the top
    // rows and visibility the rest; both are addressed by probe index.
    GLuint atlas;
    GLuint atlas_fbo;
    int atlas_w, atlas_h;
    int irradiance_rows; // atlas rows consumed by the irradiance block

    // Per-probe capture scratch, reused for every probe.
    GLuint capture_color; // cubemap, GI_CAPTURE_FACE^2, RGB16F
    GLuint capture_depth; // cubemap, GI_CAPTURE_FACE^2, DEPTH_COMPONENT24
    GLuint tile_fbo;      // renders one tile at a time into the atlas
    GLuint quad_vao, quad_vbo;

    ShaderProgram* project_program;
    ShaderProgram* border_program;

    // Convergence. `dirty_count` probes remain to capture, taken from
    // `next_probe` round-robin. `rate` is probes per frame while dirty, 0 for
    // all of them; it does not apply to the opening sweep, which always runs in
    // one frame (see gi_volume_update).
    int rate;
    int next_probe;
    int dirty_count;
    bool first_pass;

    bool debug_atlas; // draw the raw atlas over the composited frame
    bool failed;      // One-shot: allocation is not retried every frame
} GIVolume;

GIVolume* create_gi_volume(int nx, int ny, int nz);
void free_gi_volume(GIVolume* gi);

// Fit the grid to a scene AABB and arm a full convergence sweep.
void gi_volume_fit(GIVolume* gi, const vec3 aabb_min, const vec3 aabb_max);

// Re-arm every probe. Call when the lighting changed under the volume -- a sun
// move, a light edit, a material change.
void gi_volume_mark_dirty(GIVolume* gi);

// Capture up to `rate` probes if any remain dirty. No-op on a converged volume,
// which is the steady state. Must run BEFORE the frame's scene pass: it renders
// the scene internally and leaves the default framebuffer bound.
void gi_volume_update(GIVolume* gi, struct Engine* engine, struct Scene* scene);

// Bind the atlas and upload the grid uniforms for a program that samples it.
// No-op on a program without the uniforms, so it is safe for every material.
void gi_volume_bind(const GIVolume* gi, ShaderProgram* program);

// Ready to be sampled: allocated, enabled, and holding at least one converged
// sweep's worth of data.
bool gi_volume_active(const GIVolume* gi);

// Draw the raw atlas over the composited frame, for acceptance.
void gi_volume_debug_blit(const GIVolume* gi, struct Engine* engine, int screen_w, int screen_h);

#endif // _GI_VOLUME_H_
