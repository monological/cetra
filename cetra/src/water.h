#ifndef _WATER_H_
#define _WATER_H_

#include <GL/glew.h>
#include <stdbool.h>
#include <cglm/cglm.h>

#include "common.h" // RenderMode, for water_will_draw
#include "sky.h"    // SKY_CLOUD_SHADOW_UNIT, asserted against this file's own ledger below

/*
 * Water surface (spec 11.32, roadmap D3).
 *
 * A single-layer water surface: one interface that reflects and transmits, drawn
 * with depth writes on rather than as translucent geometry, so it takes no part in
 * OIT and everything that must sort against it can test the depth it wrote.
 *
 * WHY ITS OWN PROGRAM. pbr_frag declares all sixteen fragment samplers the driver
 * allows and the driver counts declarations, not distinct units (see engine.h on
 * the moment atlas). Water needs the resolved scene depth, which pbr_frag has no
 * slot for. A dedicated program starts from sixteen again.
 */

/*
 * Lattice cells per side of the PROJECTED GRID (spec 11.35).
 *
 * The mesh is a fixed lattice in screen space, projected onto the water plane per vertex,
 * so this is a pixels-per-cell quality knob and not a world size: cell density is uniform
 * in pixels at any camera and any resolution, and how far the surface reaches is whatever
 * the frustum sees. Those two were one number under the clipmap this replaced -- rings tile
 * only because every level snaps to the coarsest cell, so reach and near-field detail were
 * welded together and the surface stopped 5 degrees short of the horizon.
 *
 * 256 is chosen to spend the clipmap's own triangle budget: 5 levels of a 128 grid came to
 * 131,072 triangles, and so does this, from 4/5 as many vertex invocations.
 */
#define WATER_GRID_RES 256

// Resolved single-sample scene depth, for the water column and the shoreline test.
//
// Sampler UNIFORMS are per program, but the bindings are global, so sharing a
// number with a material slot is only safe for a reason. 7 is TEXUNIT_LTC, whose
// tenant is a 2D_ARRAY -- a different binding point on the same unit. 8 below is
// TEXUNIT_SHEEN and IS the same target, safe because pbr rebinds it from the same
// pointer that gates its use.
#define WATER_DEPTH_UNIT 7
// Six transformed cascade fields (3 bands x 2 targets) start here. Units 0-5,
// which this program leaves free: its other tenants are 6, 7, 8 and the
// engine-bound IBL block at 9-15. Everything has to fit under 16 --
// GL_MAX_TEXTURE_IMAGE_UNITS is exactly that on this hardware, and spec 4.10
// records a slot at 16 surviving only on the driver's tolerance.
#define WATER_CASCADE_UNIT0 0
// The baked bed heightfield, sampled in the VERTEX stage for shoaling.
#define WATER_BED_UNIT 8
/*
 * The cascade shadow array, for the sun glitter (spec 11.42).
 *
 * NOT SHADOW_MAP_TEXTURE_UNIT, which is 10 and already carries cascadePrev1. Those are a
 * sampler2DArray and a sampler2D, and a program that declares BOTH types against one image
 * unit is an INVALID_OPERATION at draw -- so the array needs a unit whose 2D_ARRAY binding
 * point this program leaves alone. 11 is the IBL irradiance CUBE, a third binding point on
 * a unit water never samples as either, which is the same aliasing the depth and bed units
 * above already rest on.
 */
#define WATER_SHADOW_UNIT 11
// Last frame's transformed field for the two cascades that displace the mesh, for the
// spectral path's motion vectors. Units 9 and 10 hold the Charlie sheen cubemap and the
// shadow map array, both of which are OTHER binding points on those units -- the same
// reasoning WATER_DEPTH_UNIT above rests on.
//
// This program DOES sample the cascade array since spec 11.42, and takes it on
// WATER_SHADOW_UNIT rather than here: two sampler types against one image unit is an
// error, where two types on one unit across DIFFERENT programs is only aliasing.
#define WATER_PREV_UNIT0 9
// Only the long and medium bands reach the mesh; the short one shades the interface
// and never displaces, so it has no previous position to remember.
#define WATER_PREV_CASCADES 2
// The cloud deck's sun transmittance is SKY_CLOUD_SHADOW_UNIT (sky.h), shared with the
// catcher rather than allocated here: it is the sky's resource and neither consumer has a
// reason to disagree about where it lands. Asserted against this file's own range because
// WATER_PREV_CASCADES is derived and a third displacing band would walk into it.
_Static_assert(SKY_CLOUD_SHADOW_UNIT >= WATER_PREV_UNIT0 + WATER_PREV_CASCADES,
               "the cloud shadow unit collides with water's previous-cascade range");
_Static_assert(SKY_CLOUD_SHADOW_UNIT < 16,
               "the cloud shadow unit exceeds GL_MAX_TEXTURE_IMAGE_UNITS");

// Resolution of the baked bed heightfield. It only has to resolve the SHOALING
// ramp -- how fast the water shallows -- not the terrain's own detail, which the
// depth buffer already carries per fragment.
#define WATER_BED_RES 256

/*
 * Clear seawater's extinction, per METRE, red first. Named here rather than left inside
 * create_water so a world at another scale can divide it without having to run
 * create_water to find out what it was dividing.
 */
#define WATER_CLEAR_ABSORPTION_PER_M ((vec3){0.45f, 0.09f, 0.06f})

/*
 * Spectral cascades (--water-waves fft).
 *
 * A Tessendorf ocean: the surface is described statistically in frequency space,
 * each Fourier mode is advanced by the gravity-wave dispersion relation, and an
 * inverse FFT recovers displacement. Three bands rather than one, because a
 * single periodic field asked to carry both swell and capillary detail either
 * repeats visibly or resolves neither.
 *
 * WHY THIS RUNS AT ALL ON GL 4.1. Every stage is a pure gather: the Stockham
 * butterfly reads exactly two texels plus a twiddle row and writes one texel to
 * two targets, and the spectrum evolution is per-texel. Neither needs shared
 * memory, atomics or scatter, so both are fragment passes with MRT and the
 * missing compute stage costs ping-pong draws rather than a redesign.
 *
 * 7 stages per axis, two axes, three cascades plus one evolve each = 45 draws at
 * 128 squared per frame. Small in pixels; the cost is per-pass overhead.
 */
#define WATER_SPECTRUM_RES  128
#define WATER_SPECTRUM_LOG  7 // log2(WATER_SPECTRUM_RES)
#define WATER_CASCADE_COUNT 3

typedef enum WaterWaveModel {
    WATER_WAVES_GERSTNER = 0, // closed-form octaves; lake scale, no GPU state
    WATER_WAVES_FFT,          // spectral cascades; ocean scale
} WaterWaveModel;

struct Engine;
struct Scene;

/*
 * Bed height under (x, z), in world units. Water needs a depth in the VERTEX
 * stage to shoal waves, and the screen-space depth buffer cannot answer there:
 * sampling it needs the displaced position that the depth is supposed to
 * displace. So a caller that has an analytic bed supplies it here.
 *
 * NULL is the normal case and is not a degraded one -- the surface then takes
 * its water column from the resolved scene depth per fragment, which works
 * against ARBITRARY geometry rather than only against a heightfield. What it
 * cannot do is shoal, because that is the vertex-stage question above.
 */
typedef float (*WaterHeightFn)(void* ctx, float x, float z);

/*
 * The sea state the spectral cascades are seeded from (spec 11.42).
 *
 * One set of numbers for all three bands, so the bands stay windows onto ONE spectrum
 * rather than three independently authored looks. Every field is a physical quantity in
 * METRES and m/s, not a look control: a calmer spectral ocean is a lower wind speed or a
 * shorter fetch, which is why the spectral path takes no amplitude knob at all.
 *
 * Gerstner reads none of this -- it has no sea state to ask, which is what `amplitude`
 * and `wavelength` are for. The wind DIRECTION is deliberately not here: both models
 * travel downwind and `Water.wind_dir` is the one place it is said.
 */
typedef struct WaterSeaState {
    float wind_speed;       // m/s, at the standard 10 m reference height
    float fetch;            // metres of open water the wind has blown across
    float sea_depth;        // metres; drives the TMA shallow-water correction
    float peak_enhancement; // JONSWAP gamma: how sharply the spectrum peaks
    float swell;            // 0..1, how much older cross-swell rides the wind sea
} WaterSeaState;

typedef struct Water {
    bool enabled;

    float level; // still-water plane, world Y
    // Half-size of the shoaling bed's domain, world units. NOT a bound on the drawn
    // surface, which the projected grid takes from the frustum -- outside this the bed
    // field reads its nearest edge texel, which is open water.
    float extent;

    // Optical properties of the body, authored rather than derived from a
    // transparency slider. absorption is extinction per world unit per channel,
    // so a bigger number is a shorter sight line; scatter is the colour the
    // absorbed energy comes back as. Water absorbs red first, which is why the
    // default is ordered the way it is.
    //
    // PER WORLD UNIT, and the physical figures below are per METRE: a world whose
    // unit is not a metre divides them by its own units-per-metre, or its sea is
    // that factor too absorbing. scatter carries no length and does not convert.
    vec3 absorption;
    vec3 scatter;

    float roughness; // interface roughness; picks the environment lobe's mip
    float ior;       // 1.333 for water -> F0 0.020

    // Wave train. amplitude/wavelength describe the LONGEST octave; the rest are
    // derived from it inside ocean.glsl. steepness is 0..1 rather than a Gerstner Q
    // -- 1 is the steepest crest whose horizontal map is still injective -- and is
    // clamped to that range on upload.
    // Travel direction of the waves, XZ. Read by BOTH models -- the Gerstner octaves fan
    // off it, and the spectral seeding centres its directional spread on it -- so the two
    // cannot describe seas running different ways. It was private to Gerstner until spec
    // 11.42, which is why a scene authoring it got a spectral sea travelling somewhere
    // else entirely.
    vec2 wind_dir;
    float amplitude;
    float wavelength;
    float steepness;
    float spread; // per-octave direction fan, radians

    // Spectral sea state; see WaterSeaState. Inert on the Gerstner path.
    WaterSeaState sea;

    WaterHeightFn height_at; // optional bed provider; see WaterHeightFn
    void* height_ctx;

    WaterWaveModel wave_model;
    // Caustics on refracted geometry. Inert on the Gerstner path, whose steepness
    // is clamped so its mapping cannot compress and therefore cannot focus.
    bool caustics;
    // false = no analytic sun lobe, which is every frame before spec 11.42. Live on both
    // wave models: its width comes from the slope the surface stopped resolving, and the
    // Gerstner path reports that from its dropped octaves.
    bool glitter;
    // false = the shoreline is a hard cutoff at the pixel the water column closes.
    // Inert wherever the target has no samples to spend coverage on.
    bool shore_coverage;
    // false = every vertex evaluates the wave field at full detail regardless of how much
    // world its cell covers, and no slope energy is handed to roughness. Bisect lever, and
    // the only way back to the aliased far field a projected grid has without it.
    bool far_lod;

    // Lazily built GPU state, on the postfx ensure_* pattern. `failed` latches
    // so a missing program costs one log line rather than one per frame forever.
    GLuint grid_vao;
    GLuint grid_vbo;
    GLuint grid_ebo;
    int grid_index_count;
    bool failed;

    // Spectral state, allocated only under WATER_WAVES_FFT. The initial spectrum
    // and wave data are seeded once on the CPU and never change; the field pair
    // ping-pongs through the FFT stages every frame.
    //
    // Two RGBA16F targets per buffer because the transform carries FOUR complex
    // fields at once -- displacement xz, height with a cross derivative, the two
    // slopes, and the two horizontal derivatives. Packing them into one transform
    // is what makes the derivatives free rather than three more FFTs.
    GLuint cascade_initial[WATER_CASCADE_COUNT];
    GLuint cascade_wave[WATER_CASCADE_COUNT];
    GLuint cascade_field[WATER_CASCADE_COUNT][2][2]; // [cascade][buffer][target]
    GLuint cascade_fbo[WATER_CASCADE_COUNT][2];      // one per buffer, both targets attached
    GLuint twiddle_tex;
    GLuint fft_vao, fft_vbo; // fullscreen quad for the spectral passes
    bool spectra_ready;

    /*
     * The sea state the seed textures currently hold, compared each frame the way
     * bed_extent is (spec 11.42). Editing wind or fetch re-seeds; editing anything the
     * seeding does not read -- the level, the optics, the Gerstner train -- does not,
     * rather than every caller having to know which is which.
     *
     * Only the two SEED textures are rewritten on a re-seed. The transformed fields, the
     * framebuffers and the twiddle table are all functions of the resolution alone, so
     * nothing is torn down and the ping-pong keeps running through the change.
     */
    WaterSeaState seeded_sea;
    vec2 seeded_wind_dir;

    /*
     * What the seeded spectrum's variance actually is, per cascade (spec 11.42).
     *
     * Accumulated over the modes as they are drawn, which is the only place the
     * per-mode amplitude exists. Three consumers, and they are why this is stored rather
     * than re-derived: the displaceable slab's half-thickness (the projector has to know
     * how far a crest can reach above the still plane), the far field's roughness (the
     * slope energy filtering removed has to arrive somewhere, and it needs the total to
     * be a fraction OF), and the glitter lobe's facet distribution.
     *
     * height_var is in world units squared, slope_var is dimensionless -- a mean square
     * slope, which is what Cox-Munk's tables are also in.
     *
     * PREDICTED, not measured: the inverse transform is unnormalised and the seeding
     * draws h0 as (ga + i*gb)*A rather than the textbook (1/sqrt2)(xi_r + i*xi_i)*sqrt(S),
     * so the constant relating these to the field the shader samples is exactly the thing
     * --water-fft-probe exists to check rather than assert.
     */
    float cascade_height_var[WATER_CASCADE_COUNT];
    float cascade_slope_var[WATER_CASCADE_COUNT];

    /*
     * Last frame's target 0 for the two cascades that displace the mesh, copied out
     * before this frame's transform overwrites it.
     *
     * A motion vector needs the position this vertex HELD, and the cascades only ever
     * hold one instant -- so without this the spectral path could only report camera
     * motion, and TAA reprojected travelling waves as though they were static. Target
     * 0 alone is enough: it carries the horizontal displacement and the height, which
     * is the whole position. The slopes in target 1 belong to the normal, and a
     * previous normal is not a thing anything reads.
     *
     * Counted rather than flagged, because the first frame has no previous: at 0 there
     * is nothing to copy, at 1 the copy is this frame's own work, and only from 2 does
     * the pair hold a frame the surface actually drew.
     */
    GLuint cascade_prev[WATER_PREV_CASCADES];
    int spectral_frames;

    /*
     * The bed, baked from height_at once.
     *
     * A CPU callback cannot be called from a vertex shader, so Tier 3 arrives as a
     * texture rather than as the function itself. Baked rather than streamed
     * because the drawn surface is a fixed extent about the origin: the same
     * region every frame, so the same samples every frame.
     *
     * Resolution is chosen for the shoaling RAMP, not for the terrain. Per-fragment
     * water depth still comes from the resolved scene depth, which is exact and
     * works against geometry no heightfield describes; this exists only to answer
     * the one question screen depth cannot, in the stage it has to be answered in.
     */
    GLuint bed_tex;
    bool bed_baked;
    // The extent the bake was taken at. Compared against `extent` each frame, so moving
    // the extent re-bakes and moving anything the bake does NOT read (the level) does
    // not -- rather than every caller having to know which is which.
    float bed_extent;
} Water;

/*
 * Flatten the body (or its absence) into postfx's per-frame block, so the froxel
 * volume can carry water as a second medium below the surface.
 *
 * Needs the camera to decide which side the eye is on, which is why it takes the
 * engine rather than just the water.
 */
void water_publish_to_postfx(const Water* water, struct Engine* engine);

// The per-cascade tiling periods and choppiness are columns of water.c's own cascade
// table and are uploaded from there. They were briefly published here as two extra
// arrays, on the reasoning that a second copy "would be a silent mismatch rather than an
// error" -- which is precisely what publishing them created, five lines from the table.

Water* create_water(void);
void free_water(Water* water);

// Is there a usable surface at all: authored on, and no latched failure.
bool water_active(const Water* water);

// The steepness the surface is actually built from, clamped to the range where the
// Gerstner horizontal map stays injective. Every consumer must read it through here --
// the GPU and the CPU query both evaluate the same train, so a value clamped on only one
// of the two paths is two different surfaces.
float water_effective_steepness(const Water* water);

/*
 * Will the surface actually rasterize this frame.
 *
 * The whole predicate, in one place, because more than the draw depends on it -- the
 * shadow catcher steps aside for water and the froxel volume takes it as a second
 * medium, and both were reading `water_active` alone. A debug render mode or a cube
 * capture then suppressed the ground plane on behalf of a surface that never drew,
 * leaving the frame with no floor.
 */
bool water_will_draw(const Water* water, const struct Engine* engine, RenderMode render_mode);

/*
 * Draw the surface into the currently bound scene FBO. Callers gate on
 * water_will_draw; this repeats the check rather than trusting it.
 *
 * The one precondition still on the caller: the refraction resolve has run this frame,
 * since the surface samples it for transmission.
 *
 * Restores the framebuffer binding, viewport, blend, depth test, cull face and the
 * draw-buffer list. Leaves its own program and texture bindings current.
 */
void water_render(Water* water, struct Scene* scene, struct Engine* engine, const mat4 view,
                  const mat4 draw_projection);

/*
 * Measure the transformed cascades and print them beside what the seeding predicted
 * (spec 11.42). Stalls the pipeline once per cascade, so this is a diagnostic and not
 * something the render loop may call.
 *
 * Requires a spectral surface that has run at least one frame; anything else prints
 * `available=0` rather than a number, since a caller reading silence as agreement is the
 * failure this exists to prevent.
 */
void water_fft_probe(const Water* water, struct Engine* engine);

#endif // _WATER_H_
