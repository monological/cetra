#ifndef _LIGHTING_ATLAS_H_
#define _LIGHTING_ATLAS_H_

#include <GL/glew.h>
#include <stdbool.h>

#include "probe.h"
#include "probe_set.h" // PROBE_ATLAS_ROWS_MAX, which the GPU row table is sized by

// The one texture every resident GI volume and reflection probe lives in (specs 11.70 and
// 13.24), owned by the scene. Everything the rest of the engine would otherwise have to know
// about where those tiles sit is behind this header, include/gi_volume.glsl and
// include/probe_specular.glsl.
//
// The shape is forced rather than chosen. pbr_frag declares sixteen samplers and the driver
// counts DECLARATIONS, so the volumes and the probes all ride the one declaration on unit 14:
// N of each cost regions of a texture, not units. Roughness is ROWS rather than mips because
// generating mips would filter across the tile gutters that make an octahedral tile
// bilinear-safe at all.
//
// The layout is SLOTS, and a slot is a resident item's place rather than the item: GI slots
// side by side from the left, each as large as the largest volume in the world, then one
// column per probe slot. The atlas only ever grows, carrying every slot across, so a resident
// item's texels never move under it except in a regrow -- and a regrow copies them.

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

typedef struct LightingAtlas {
    GLuint texture;
    int width, height;

    // The texture as a render target: every projection into it, GI tiles and probe rows.
    GLuint fbo;

    // GI slots, side by side from x = 0.
    int gi_slots;
    int gi_slot_w, gi_slot_h;

    // Probe columns, side by side from spec_x, which is where the GI slots end.
    int spec_x;
    int row0;     // row-0 interior edge
    int capacity; // probe columns

    GLuint quad_vao, quad_vbo;
    ShaderProgram* project_program; // the probe projection
} LightingAtlas;

// The scene's atlas, grown to hold what its GI world and probe set need now: a slot for each
// resident volume as large as the largest volume in the world, and a column for each resident
// probe of a set of two or more. Allocates on first need and regrows when the world outgrows
// it, copying every slot and column across. NULL while nothing needs one, or when the layout
// is past the driver's texture limit (refused by name).
LightingAtlas* lighting_atlas_sync(struct Scene* scene, struct Engine* engine);
void free_lighting_atlas(LightingAtlas* atlas);

// A GI slot's lower-left texel.
void lighting_atlas_gi_region(const LightingAtlas* atlas, int slot, int* out_x, int* out_y);

// Resample a captured probe's prefiltered cube into its column, one row per
// roughness level, gutters included.
bool lighting_atlas_project_probe(LightingAtlas* atlas, const ReflectionProbe* probe, int index);

// Bind the atlas on its unit for a program that samples it.
void lighting_atlas_bind(const LightingAtlas* atlas, ShaderProgram* program);

// The GL name, for the one consumer that binds it outside a draw (SSR's
// per-frame publish). The only thing about the atlas postfx needs.
GLuint lighting_atlas_texture(const LightingAtlas* atlas);

// A probe column's left edge, in texels. The only per-probe fact about the
// layout -- everything else about a column is shared, which is what
// lighting_atlas_fill_column publishes.
float lighting_atlas_probe_column_x(const LightingAtlas* atlas, int index);

// The atlas-wide half of the probe layout: the gutter and last row index, plus
// each row's y origin and interior edge. Published to the GPU so the shader
// reads a table instead of re-deriving the halving rule -- which is what lets
// the row size be any value rather than a power of two.
void lighting_atlas_fill_column(const LightingAtlas* atlas, float out_column[4],
                                float out_rows[][4]);

void lighting_atlas_size(const LightingAtlas* atlas, int* out_w, int* out_h);
void lighting_atlas_probe_rect(const LightingAtlas* atlas, int index, int* out_x, int* out_y,
                               int* out_rows);

// Draw the atlas over the composited frame (--gi-debug, --probe-set-debug), at `scale` times
// its values: the GI tiles hold bounced light, a fraction of the direct, and want lifting. A bad
// tile and a bad lookup are the same picture from outside; this is what separates them.
void lighting_atlas_debug_blit(const LightingAtlas* atlas, struct Engine* engine, int screen_w,
                               int screen_h, float scale);

#endif // _LIGHTING_ATLAS_H_
