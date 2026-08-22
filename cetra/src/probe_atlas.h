#ifndef _PROBE_ATLAS_H_
#define _PROBE_ATLAS_H_

#include <GL/glew.h>
#include <stdbool.h>

#include "probe.h"
#include "probe_set.h" // PROBE_ATLAS_ROWS_MAX, which the GPU row table is sized by

// Where N probes' prefiltered radiance lives (spec 11.70). Everything the rest
// of the engine would otherwise have to know about specular probe storage is
// behind this header and include/probe_specular.glsl: swapping the octahedral
// atlas for something else replaces these two and touches nothing further.
//
// The shape is forced rather than chosen. pbr_frag declares sixteen samplers
// and the driver counts DECLARATIONS, so N probes have to ride a declaration
// that already exists -- and the only one shaped like a pool of probe images is
// the GI volume's octahedral atlas on unit 14. Roughness is ROWS rather than
// mips because generating mips would filter across the tile gutters that make
// an octahedral tile bilinear-safe at all.

// Row 0's interior edge. Any value in range: the row geometry is published to
// the shader as a table, so nothing recomputes it on the far side of the seam
// and no power-of-two constraint follows.
//
// The near-mirror case decides the default, as it decided PROBE_PREFILTER_SIZE:
// an octahedral tile carries roughly (2/sqrt(6)) of a cube face's angular
// density per edge texel, so this is softer than the cube it resamples and
// --probe-set-res is the dial when that shows.
#define PROBE_ATLAS_ROW0_DEFAULT 512
#define PROBE_ATLAS_ROW0_MIN     64
#define PROBE_ATLAS_ROW0_MAX     2048

// One row per prefiltered roughness level, so row r carries roughness
// r/(rows-1) -- ibl_prefilter_cubemap's own convention, which is what lets the
// shader's roughness lookup be two taps and a mix with no remapping.
#define PROBE_ATLAS_ROWS PROBE_PREFILTER_MIP_LEVELS
// light_cluster.h sizes the GPU row table by PROBE_ATLAS_ROWS_MAX and cannot
// include this header (it would cycle), so the two are stated apart and pinned
// together here.
_Static_assert(PROBE_ATLAS_ROWS == PROBE_ATLAS_ROWS_MAX,
               "the GPU row table must have one entry per prefiltered roughness level");

// Rows stop halving here: past it a tile is mostly gutter, and the roughness
// levels that read it are the ones a lobe has already smeared flat.
#define PROBE_ATLAS_ROW_MIN 8

// One texel, and it is not a tuning knob: the projection pass mirrors exactly
// the outermost ring into the interior, so a wider gutter would leave its outer
// ring unwritten.
#define PROBE_ATLAS_GUTTER 1

struct Engine;
struct Scene;
struct PostFX;
struct ReflectionProbeSet;

typedef struct ProbeAtlas {
    GLuint texture;
    int width, height;

    // The specular region's left edge. Non-zero when the GI volume is a tenant:
    // it keeps columns [0, spec_x) and every coordinate it already computes,
    // because giAtlasSize is a uniform and a wider texture moves no texel of it.
    int spec_x;

    int row0;     // row-0 interior edge
    int capacity; // probes the layout was sized for

    GLuint fbo;
    GLuint quad_vao, quad_vbo;
    ShaderProgram* project_program;
} ProbeAtlas;

// Allocate for `count` probes, reserving the scene's GI volume its own columns
// when one exists. row0 <= 0 takes the default.
ProbeAtlas* create_probe_atlas(struct Engine* engine, struct Scene* scene, int count, int row0);
void free_probe_atlas(ProbeAtlas* atlas);

// Resample a captured probe's prefiltered cube into its column, one row per
// roughness level, gutters included.
bool probe_atlas_project(ProbeAtlas* atlas, const ReflectionProbe* probe, int index);

// Bind the atlas on the GI atlas unit and publish the layout uniforms.
void probe_atlas_bind(const ProbeAtlas* atlas, ShaderProgram* program);

void probe_atlas_publish_to_postfx(const ProbeAtlas* atlas, const struct ReflectionProbeSet* set,
                                   struct PostFX* fx);

// A probe column's left edge, in texels. The only per-probe fact about the
// layout -- everything else about a column is shared, which is what
// probe_atlas_fill_column publishes.
float probe_atlas_column_x(const ProbeAtlas* atlas, int index);

// The atlas-wide half of the layout: the gutter and last row index, plus each
// row's y origin and interior edge. Published to the GPU so the shader reads a
// table instead of re-deriving the halving rule -- which is what lets the row
// size be any value rather than a power of two.
void probe_atlas_fill_column(const ProbeAtlas* atlas, float out_column[4], float out_rows[][4]);

void probe_atlas_size(const ProbeAtlas* atlas, int* out_w, int* out_h);
void probe_atlas_rect(const ProbeAtlas* atlas, int index, int* out_x, int* out_y, int* out_rows);

// Draw the atlas over the composited frame (--probe-set-debug). A bad column
// and a bad lookup are the same picture from outside; this is what separates
// them, exactly as --sky-debug's shadow tile does for the cloud deck.
void probe_atlas_debug_blit(const ProbeAtlas* atlas, struct Engine* engine, int screen_w,
                            int screen_h);

#endif // _PROBE_ATLAS_H_
