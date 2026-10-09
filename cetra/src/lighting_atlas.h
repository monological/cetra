#ifndef _LIGHTING_ATLAS_H_
#define _LIGHTING_ATLAS_H_

#include <GL/glew.h>
#include <stdbool.h>
#include <stdint.h>

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

// The atlas's sampler unit in pbr_frag. Deliberately the same number as
// IBL_SKYBOX_TEXTURE_UNIT: sampler units are per PROGRAM, and pbr_frag has never
// sampled the skybox cube, so 14 is the only slot free to it. Reserved for this
// by the roadmap's global texture-unit ledger before either feature was built.
#define LIGHTING_ATLAS_TEXTURE_UNIT 14

struct Engine;

// A rectangle of the atlas in texels, from its lower-left corner.
typedef struct AtlasRect {
    int x, y, w, h;
} AtlasRect;

// What the atlas must hold. Each client states its own half -- gi_world_atlas_needs,
// probe_set_atlas_needs -- since only it knows how many of its items can be resident at once and
// how large each is.
typedef struct LightingAtlasLayout {
    int gi_slots;    // GI slots
    int gi_w, gi_h;  // each as large as this, the largest volume's extent
    int probe_slots; // probe columns
    int probe_row0;  // the columns' row-0 tile size; 0 = the default
} LightingAtlasLayout;

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

    bool refused; // the last layout asked for was past the driver's texture limit, said once
} LightingAtlas;

// `*atlas` grown to hold `layout`, allocated on first need, every slot and column carried
// across a regrow. NULL while the layout needs nothing, or while it is past the driver's texture
// limit -- refused by name, once, and asked again each time it is needed, so a layout that
// shrinks back under the limit gets its atlas.
LightingAtlas* lighting_atlas_reserve(LightingAtlas** atlas, const LightingAtlasLayout* layout,
                                      struct Engine* engine);
void free_lighting_atlas(LightingAtlas* atlas);

// A GI slot's left edge, in texels: the slots stand side by side from x = 0, each from y = 0.
int lighting_atlas_gi_x(const LightingAtlas* atlas, int slot);

// Resample a captured probe's prefiltered cube into its column, one row per
// roughness level, gutters included.
bool lighting_atlas_project_probe(LightingAtlas* atlas, const ReflectionProbe* probe, int index);

// Point a program's atlas sampler at its unit, and bind the atlas there when there is one. The
// sampler is pointed even with no atlas, the way the IBL samplers are: one left on its default
// unit 0 shares a slot with the material textures, which is only ever safe by accident.
void lighting_atlas_bind(const LightingAtlas* atlas, ShaderProgram* program);

// The GL name, for the one consumer that binds it outside a draw (SSR's
// per-frame publish). The only thing about the atlas postfx needs.
GLuint lighting_atlas_texture(const LightingAtlas* atlas);

// A probe column, gutters included: its corner is the only per-probe fact about the layout --
// everything else about a column is shared, which is what lighting_atlas_fill_column publishes.
// Columns stack where the GI slots are tall enough to hold more than one. Empty with no columns
// or past the last.
AtlasRect lighting_atlas_probe_rect(const LightingAtlas* atlas, int slot);

// A rectangle to the CPU, in memory the caller frees, and back. RGBA half floats both ways, so
// what comes back is bit for bit what went: how a streamed item that leaves residency returns
// without being captured again. NULL / false on failure.
uint16_t* lighting_atlas_keep(const LightingAtlas* atlas, AtlasRect rect);
bool lighting_atlas_restore(const LightingAtlas* atlas, AtlasRect rect, const uint16_t* texels);

// The same rectangle from the cook and into it (spec 13.42): a capture of a scene captured before
// is texels from disk where kept ones come from memory. The fetch is false on a miss, an invalid
// key or a stored rectangle of another size, and writes nothing then.
struct CookKey;
bool lighting_atlas_cook_fetch(const LightingAtlas* atlas, AtlasRect rect,
                               const struct CookKey* key);
void lighting_atlas_cook_store(const LightingAtlas* atlas, AtlasRect rect,
                               const struct CookKey* key);

// A rectangle back to what a new atlas holds, as though nothing had been written there. Leaves
// framebuffer 0 bound.
void lighting_atlas_clear(const LightingAtlas* atlas, AtlasRect rect);

// FNV-1a over a rectangle's texels: `kept` when given, else read from the atlas. Two captures of
// one place agree on it exactly, which is what says whether something was photographed.
uint32_t lighting_atlas_digest(const LightingAtlas* atlas, AtlasRect rect, const uint16_t* kept);

// The atlas-wide half of the probe layout: the gutter and last row index, plus
// each row's y origin and interior edge. Published to the GPU so the shader
// reads a table instead of re-deriving the halving rule -- which is what lets
// the row size be any value rather than a power of two.
void lighting_atlas_fill_column(const LightingAtlas* atlas, float out_column[4],
                                float out_rows[][4]);

void lighting_atlas_size(const LightingAtlas* atlas, int* out_w, int* out_h);

// Draw the atlas over the composited frame (--gi-debug, --probe-set-debug), at `scale` times
// its values: the GI tiles hold bounced light, a fraction of the direct, and want lifting. A bad
// tile and a bad lookup are the same picture from outside; this is what separates them.
void lighting_atlas_debug_blit(const LightingAtlas* atlas, struct Engine* engine, int screen_w,
                               int screen_h, float scale);

#endif // _LIGHTING_ATLAS_H_
