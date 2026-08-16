#ifndef _WATER_H_
#define _WATER_H_

#include <GL/glew.h>
#include <stdbool.h>
#include <cglm/cglm.h>

#include "common.h"      // RenderMode, for water_will_draw
#include "shore_runup.h" // ShoreRunupParams, for the CPU twin the film is driven by
#include "sky.h"         // SKY_CLOUD_SHADOW_UNIT, asserted against this file's own ledger below

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
/*
 * The transformed cascades: ONE 2D_ARRAY of six layers (3 bands x 2 targets), spec 11.45.
 *
 * These were six separate sampler2D on units 0-5, and the driver counts DECLARATIONS rather
 * than units -- so six of water_frag's sixteen went on the cascades alone and the program sat
 * exactly at its ceiling. An array is one declaration holding the same six images, which
 * takes water_frag from 16/16 to 10/16 and leaves units 2-6 and 9-10 free.
 *
 * Not a workaround for the cap: a cap counted in declarations is a cap on how many DISTINCT
 * shapes of data a program reads, and six identical fields were only ever one shape.
 */
#define WATER_CASCADE_UNIT 0
// The baked bed heightfield, sampled in the VERTEX stage for shoaling.
#define WATER_BED_UNIT 8
// The foam pattern (spec 11.45), on one of the units the cascade consolidation freed. Mipped,
// which is the whole reason it is a texture rather than the ALU version it replaced: whitewater
// is centimetre detail seen from a metre to the horizon, and procedural noise has no chain.
#define WATER_FOAM_PATTERN_UNIT 2
/*
 * The cascade shadow array, for the sun glitter (spec 11.42).
 *
 * NOT SHADOW_MAP_TEXTURE_UNIT, which is 10 and already carries cascadePrev1. Those are a
 * sampler2DArray and a sampler2D, and a program that declares BOTH types against one image
 * unit is an INVALID_OPERATION at draw -- so the array needs a unit whose 2D_ARRAY binding
 * point this program leaves alone. 11's other tenant is the IBL irradiance CUBE, a third
 * binding point, and water declares no cube there -- its own prefiltered cube is on 12.
 * Same aliasing the depth and bed units above already rest on.
 */
#define WATER_SHADOW_UNIT 11
// The accumulated foam, three bands in three channels (spec 11.42). 15 is the punctual
// shadow ARRAY, a different binding point from this sampler2D, and water samples no
// punctual shadows -- the last free unit under the ledger, which this spends.
#define WATER_FOAM_UNIT 15
// Last frame's transformed field for the cascades that displace the mesh, for the spectral
// path's motion vectors. A second 2D_ARRAY, one layer per cascade -- and a second unit rather
// than more layers of the one above, because these two arrays have different LIFETIMES: the
// fields are this frame's render targets and these are a copy taken before they are
// overwritten.
#define WATER_PREV_UNIT 1
// Only the long and medium bands reach the mesh; the short one shades the interface
// and never displaces, so it has no previous position to remember.
#define WATER_PREV_CASCADES 2
// The cloud deck's sun transmittance is SKY_CLOUD_SHADOW_UNIT (sky.h), shared with the
// catcher rather than allocated here: it is the sky's resource and neither consumer has a
// reason to disagree about where it lands.
_Static_assert(SKY_CLOUD_SHADOW_UNIT != WATER_CASCADE_UNIT &&
                   SKY_CLOUD_SHADOW_UNIT != WATER_PREV_UNIT &&
                   SKY_CLOUD_SHADOW_UNIT != WATER_DEPTH_UNIT &&
                   SKY_CLOUD_SHADOW_UNIT != WATER_BED_UNIT,
               "the cloud shadow unit collides with one water already binds");
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

/*
 * One point on the traced waterline (spec 11.45), in order along the shore.
 *
 * `s` is the ALONGSHORE ARC LENGTH, which is the coordinate a height field cannot supply and
 * everything non-local at a shore needs: refraction's phase runs along it, foam that rides the
 * swash is indexed by it, and a per-column swash film is a column per step of it.
 *
 * The normal points LANDWARD, taken from the bed's own gradient rather than from the polyline's
 * winding -- uphill is inland whichever way the contour happens to be traced.
 */
typedef struct WaterShorePoint {
    float x, z;   // world position on the waterline
    float nx, nz; // unit landward normal
    float s;      // cumulative arc length from the chain's start
} WaterShorePoint;

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
    // false = whitewater is selected from THIS frame's fold and forgotten, which is the
    // pre-11.42 foam exactly. Spectral only: the accumulator runs over the cascades, and
    // the Gerstner path's steepness is clamped so its map cannot fold at all.
    bool foam_history;
    // How fast foam gives up and returns to open water, per second. Lower lingers longer.
    float foam_decay;
    // How fast foam streams downwind, in METRES per second (spec 11.44). The accumulator
    // backtraces its history along this, so whitewater is carried by the sea rather than
    // decaying where it was born -- the orbital half of that motion is already free, since
    // the surface reads foam at the wave parcel's own label.
    float foam_drift;
    // false = the shoreline is a hard cutoff at the pixel the water column closes.
    // Inert wherever the target has no samples to spend coverage on.
    bool shore_coverage;
    // false = no incident wave at the shore: the wave field shoals to nothing and the sea
    // meets the sand as a still line, which is every frame before spec 11.44. Inert with no
    // bed, since there is no shore for anything to come in to.
    bool surf;
    // false = every vertex evaluates the wave field at full detail regardless of how much
    // world its cell covers, and no slope energy is handed to roughness. Bisect lever, and
    // the only way back to the aliased far field a projected grid has without it.
    bool far_lod;
    // false = the swash wets nothing: sand a wave just ran over shades exactly as dry sand
    // does. Reaches the frame before spec 11.45 exactly, and only materials that set
    // shore_wetness can see it either way.
    bool wetness;

    // false = no swash film: the water's edge is the closed-form run-up alone, which is every
    // frame before spec 11.45. The film needs a traced shoreline, so it is inert without one.
    bool film;
    // The film itself and the buffer its tips reach the shaders through. Both lazily made,
    // both NULL where no shoreline was traced.
    struct ShoreChain* chain;
    struct Ubo* film_ubo;
    bool film_logged; // the one-shot report of what the solver settled to

    // The waterline, traced out of the baked bed (spec 11.45). NULL where the bed has no
    // shore in it at all -- open water, or a level below everything. Rebuilt with the bed.
    WaterShorePoint* shore_pts;
    int shore_count;
    float shore_length; // total arc length, world units
    // true = the chain closes on itself (an island, a lake), so the alongshore coordinate
    // wraps; false = it runs off the bed's edge and clamps at both ends.
    bool shore_closed;

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
    // The transformed fields, one ARRAY per ping-pong buffer, six layers each packed as
    // cascade * 2 + target. Six separate textures were six sampler declarations in every
    // program that read them, which is what held water_frag at its ceiling; see ocean.glsl.
    GLuint cascade_array[2];                    // [buffer]
    GLuint cascade_fbo[WATER_CASCADE_COUNT][2]; // one per buffer, both targets attached
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
     * Accumulated over the modes as they are drawn, which is the only place the per-mode
     * amplitude exists.
     *
     * slope_var has two consumers and is why this is stored rather than re-derived: the
     * far field's roughness, and the glitter lobe's facet distribution. Both need what the
     * filtering removed to be a fraction OF something.
     *
     * height_var has ONE reader, and it is water_fft_probe. It exists to check the
     * transform's normalisation, not to feed shading. The projector-slab consumer it was
     * accumulated for was measured harmful and reverted (spec 11.42 phase 4), so anything
     * reading it as a live displacement bound is reading a number nothing acts on.
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
     * Accumulated foam, ping-ponged (spec 11.42).
     *
     * One RGB target, each channel a band's whitewater in that band's own tiling space.
     * `foam_index` is which of the pair holds the CURRENT frame, so the surface samples
     * that one and next frame's pass reads it as history -- a parity rather than a copy,
     * because unlike cascade_prev nothing else needs the previous value and there is no
     * mip chain to keep in step.
     *
     * Counted like spectral_frames: at 0 there is no history and the pass seeds un-foamed,
     * because seeding from a cleared texture would put the whole sea under whitewater for
     * as long as the recovery takes.
     */
    GLuint foam_tex[2];
    GLuint foam_fbo[2];
    int foam_index;
    int foam_frames;

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
    // Last frame's target 0 for the bands that displace, one layer per cascade.
    GLuint cascade_prev_array;
    // The tiling foam web, generated once on first use. Independent of wave model and sea
    // state, so nothing invalidates it.
    GLuint foam_pattern_tex;
    // The bake was attempted and failed. A fixed-size allocation that failed once will fail
    // again, so this stops the attempt repeating every frame; the shader is told separately
    // and skips the erosion rather than thresholding against an unbound sampler.
    bool foam_pattern_failed;
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
    // What the bake was taken at, compared against the live values each frame so a change
    // in any input re-bakes and a change in anything else does not -- rather than every
    // caller having to know which is which. The level and the metre are inputs since spec
    // 11.44, because the foreshore slope below is measured about the waterline.
    float bed_extent;
    float bed_level;
    float bed_units_per_metre;
    /*
     * The beach face's mean slope, rise per run, measured from the baked bed over the strip
     * within a metre of the still line (spec 11.44). Zero with no bed or no shore.
     *
     * The surf's timing and its run-up are properties of the BEACH, not of the bump under
     * one vertex: the incident wave's travel time to shore goes as one over the slope, so
     * reading the slope locally re-timed the wave by the shore's own wobble and printed the
     * crest as a comb of humps at the wobble's period. One number for the whole shore.
     */
    float bed_foreshore_slope;
} Water;

/*
 * Flatten the body (or its absence) into postfx's per-frame block, so the froxel
 * volume can carry water as a second medium below the surface.
 *
 * Needs the camera to decide which side the eye is on, which is why it takes the
 * engine rather than just the water.
 */
void water_publish_to_postfx(const Water* water, struct Engine* engine);

/*
 * Publish the scalars shore.glsl stands on to a program that is not the water, so a lit
 * surface can ask where the swash has been. Costs no sampler unit: the run-up is a closed
 * form over these seven floats and nothing else.
 */
void water_bind_shore(const Water* water, const struct Scene* scene, ShaderProgram* program);

/*
 * The sea state the run-up runs at, for a caller that wants to evaluate the CPU twin.
 *
 * Returns false where there is no surf to describe, in which case `out` is untouched. Exists
 * so --shore-probe reads the SAME numbers the film is driven by rather than reconstructing
 * them, which would make the probe agree with itself instead of with the engine.
 */
bool water_shore_runup_params(const Water* water, const struct Scene* scene, ShoreRunupParams* out);

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
 * Advance the water's simulation for the frame: the bed's bake and the swash film's step.
 *
 * Called at the FRAME TOP, before any pass draws, and not from water_render -- the film's tips
 * are read by the opaque pass and by the late pass, so a step between them leaves those two
 * readers a frame apart. It must also run on frames where the surface does not draw, or the
 * lit surfaces read a film that stopped advancing.
 *
 * Idempotent per frame and safe with no water; both halves self-guard.
 */
void water_update(Water* water, const struct Scene* scene, float t, float dt);

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
