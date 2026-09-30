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
 * takes water_frag from 16/16 to 11/16 and leaves units 2-6 and 9-10 free.
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
 * NOT SHADOW_MAP_TEXTURE_UNIT, which is 10. When this was chosen (11.42) unit 10 carried
 * cascadePrev1 as a sampler2D, and a program declaring BOTH a sampler2D and a sampler2DArray
 * against one image unit is an INVALID_OPERATION at draw -- so the array needed a unit whose
 * 2D_ARRAY binding point this program leaves alone. 11.45 folded the previous cascades into
 * one array on unit 1, so that specific collision is gone; the unit stays because moving it
 * buys nothing and 11 is already asserted against. 11's other tenant is the IBL irradiance
 * CUBE, a third binding point, and water declares no cube there -- its own prefiltered cube
 * is on 12. Same aliasing the depth and bed units above already rest on.
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
// The refracted-grid caustics (spec 13.2), one of the units the cascade consolidation freed. An
// ARRAY since 13.3, one layer per level; 3 is TEXUNIT_CLEARCOAT_NORMAL, a 2D binding point, and
// water declares nothing else there.
#define WATER_CAUSTIC_UNIT 3
_Static_assert(WATER_CAUSTIC_UNIT != WATER_CASCADE_UNIT && WATER_CAUSTIC_UNIT != WATER_PREV_UNIT &&
                   WATER_CAUSTIC_UNIT != WATER_DEPTH_UNIT && WATER_CAUSTIC_UNIT != WATER_BED_UNIT &&
                   WATER_CAUSTIC_UNIT != WATER_FOAM_PATTERN_UNIT &&
                   WATER_CAUSTIC_UNIT != WATER_SHADOW_UNIT &&
                   WATER_CAUSTIC_UNIT != WATER_FOAM_UNIT &&
                   WATER_CAUSTIC_UNIT != SKY_CLOUD_SHADOW_UNIT,
               "the caustic unit collides with one water already binds");
// The ripple band's transformed fields (spec 13.3), a 2D_ARRAY of its own because an array has
// one size for every layer and this band's is not the cascades'. 4 is TEXUNIT_HEIGHT, a 2D
// binding point; this is the ARRAY one, and water declares nothing else on the unit.
#define WATER_RIPPLE_UNIT 4
_Static_assert(WATER_RIPPLE_UNIT != WATER_CASCADE_UNIT && WATER_RIPPLE_UNIT != WATER_PREV_UNIT &&
                   WATER_RIPPLE_UNIT != WATER_DEPTH_UNIT && WATER_RIPPLE_UNIT != WATER_BED_UNIT &&
                   WATER_RIPPLE_UNIT != WATER_FOAM_PATTERN_UNIT &&
                   WATER_RIPPLE_UNIT != WATER_SHADOW_UNIT && WATER_RIPPLE_UNIT != WATER_FOAM_UNIT &&
                   WATER_RIPPLE_UNIT != WATER_CAUSTIC_UNIT &&
                   WATER_RIPPLE_UNIT != SKY_CLOUD_SHADOW_UNIT,
               "the ripple unit collides with one water already binds");
// Touch ripples' height and slopes (spec 13.4). 5 is TEXUNIT_EMISSIVE, a 2D binding point like
// this one, and water declares nothing else there.
#define WATER_TOUCH_UNIT 5
_Static_assert(WATER_TOUCH_UNIT != WATER_CASCADE_UNIT && WATER_TOUCH_UNIT != WATER_PREV_UNIT &&
                   WATER_TOUCH_UNIT != WATER_DEPTH_UNIT && WATER_TOUCH_UNIT != WATER_BED_UNIT &&
                   WATER_TOUCH_UNIT != WATER_FOAM_PATTERN_UNIT &&
                   WATER_TOUCH_UNIT != WATER_SHADOW_UNIT && WATER_TOUCH_UNIT != WATER_FOAM_UNIT &&
                   WATER_TOUCH_UNIT != WATER_CAUSTIC_UNIT &&
                   WATER_TOUCH_UNIT != WATER_RIPPLE_UNIT &&
                   WATER_TOUCH_UNIT != TEXUNIT_SCENE_COLOR &&
                   WATER_TOUCH_UNIT != SKY_CLOUD_SHADOW_UNIT,
               "the touch unit collides with one water already binds");
// The rain's cover (spec 13.9): the punctual shadow ARRAY, bound here for the one layer the
// occlusion map is, so rain does not ring water under a roof. 9 is TEXUNIT_SHEEN's Charlie
// CUBE in pbr, a different binding point, and water declares nothing else there.
#define WATER_RAIN_COVER_UNIT 9
_Static_assert(
    WATER_RAIN_COVER_UNIT != WATER_CASCADE_UNIT && WATER_RAIN_COVER_UNIT != WATER_PREV_UNIT &&
        WATER_RAIN_COVER_UNIT != WATER_DEPTH_UNIT && WATER_RAIN_COVER_UNIT != WATER_BED_UNIT &&
        WATER_RAIN_COVER_UNIT != WATER_FOAM_PATTERN_UNIT &&
        WATER_RAIN_COVER_UNIT != WATER_SHADOW_UNIT && WATER_RAIN_COVER_UNIT != WATER_FOAM_UNIT &&
        WATER_RAIN_COVER_UNIT != WATER_CAUSTIC_UNIT && WATER_RAIN_COVER_UNIT != WATER_RIPPLE_UNIT &&
        WATER_RAIN_COVER_UNIT != WATER_TOUCH_UNIT && WATER_RAIN_COVER_UNIT != TEXUNIT_SCENE_COLOR &&
        WATER_RAIN_COVER_UNIT != SKY_CLOUD_SHADOW_UNIT,
    "the rain cover unit collides with one water already binds");
// The touch simulation's square: texels a side, metres a side, steps a second, and how many
// drops one step presses in (shared with the step shader). 7 m at 256 is 2.7 cm a texel,
// Clearwater's.
#include "../shaders/include/water_touch_constants.glsl"
#define WATER_TOUCH_RES     256
#define WATER_TOUCH_SIZE_M  7.0f
#define WATER_TOUCH_STEP_HZ 60.0f
// Steps with nothing new pressed in before the simulation stops running; about fifteen seconds,
// by which Clearwater's damping has left nothing a pixel could show.
#define WATER_TOUCH_CALM_STEPS 900

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

// The ripple band's transform size, shared with ocean.glsl (spec 13.3).
#include "../shaders/include/water_ripple_constants.glsl"

// WATER_PROBE_MAX and the inversion's step cap and tolerance, shared with the query shader.
#include "../shaders/include/water_probe_constants.glsl"
// Passes between a query's render and its read. The pack-buffer ring holds this many.
#define WATER_PROBE_LATENCY 2

// The caustics window, lattice and ceiling (spec 13.2), shared with the shaders that draw and
// read them.
#include "../shaders/include/water_caustic_constants.glsl"

typedef enum WaterWaveModel {
    WATER_WAVES_GERSTNER = 0, // closed-form octaves; lake scale, no GPU state
    WATER_WAVES_FFT,          // spectral cascades; ocean scale
} WaterWaveModel;

// Draw the crest band as a binary mask instead of shading it (spec 11.47). See the field's
// own comment on Water.foam_debug for why a debug MODE rather than a render mode, and
// water_frag.glsl's override block for why the write is binary.
typedef enum WaterFoamDebug {
    WATER_FOAM_DEBUG_OFF = 0,
    WATER_FOAM_DEBUG_ERODED,   // the band as the frame draws it, after erosion
    WATER_FOAM_DEBUG_SELECTED, // the band before erosion, so the pass rate is measurable
    // Depth-limited breaking alone. The two above became the UNION of whitecaps and breaking
    // when 11.48 gave a breaker the crest band's ceiling, and separating them again is what
    // an elimination needs -- ruling crest foam out as the cause of a surf-zone artifact is
    // exactly what 11.48's own trace used mode 1 for.
    WATER_FOAM_DEBUG_BREAKING,
} WaterFoamDebug;

struct Engine;
struct Light;
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
 * One wave train's spectrum (spec 11.48).
 *
 * A wind sea and a swell are the same kind of thing, so this is one type used twice rather
 * than one type and a hardcoded imitation of it alongside. A train's SIZE comes from its
 * physics -- a calmer train is a lower wind speed or a shorter fetch -- and `scale` only
 * re-weights one train against the other.
 *
 * `direction` is measured from `Water.wind_dir` rather than from an absolute bearing, so
 * turning the wind turns both trains and preserves the ANGLE BETWEEN them -- which is what
 * makes a second train read as an older swell instead of as more wind sea.
 */
typedef struct WaterWaveTrain {
    float wind_speed; // m/s, at the standard 10 m reference height
    float fetch;      // metres of open water the wind has blown across
    float direction;  // radians, relative to Water.wind_dir
    // Spectral density weight; 0 removes the train. NOT an absolute: the second train is
    // additionally weighted per cascade (WATER_CASCADE_CFG.secondary_scale, 0.22/0.08/0),
    // so an authored 1 is the fetch law's spectrum for the wind sea and a fraction of it for
    // the swell -- and no value at all reaches the short band, which weights it zero.
    float scale;
    float peak_enhancement; // JONSWAP gamma: how sharply the spectrum peaks
    // The two spread knobs both narrow the lobe, and they narrow DIFFERENT BANDS -- which is
    // the whole reason there are two. `spread_gain` multiplies the entire spread power, so
    // it narrows at every frequency including the peak, where it dominates. `focus` scales
    // only the saturating tanh term, which is negligible at the peak and dominant in both
    // tails, so it narrows the off-peak energy and leaves the peak's own width alone.
    float focus;        // 0..1, how narrowly the OFF-PEAK energy sits about the heading
    float spread_gain;  // outer factor on the whole directional spread power
    float spread_blend; // 0 = a broad cos^2 lobe, 1 = the focused cos^2s one
} WaterWaveTrain;

/*
 * The sea state the spectral cascades are seeded from (spec 11.42).
 *
 * One set of numbers for all three bands, so the bands stay windows onto ONE spectrum
 * rather than three independently authored looks.
 *
 * Gerstner reads none of this -- it has no sea state to ask, which is what `amplitude`
 * and `wavelength` are for. The wind DIRECTION is deliberately not here: both models
 * travel downwind and `Water.wind_dir` is the one place it is said.
 */
typedef struct WaterSeaState {
    // Metres; drives the TMA shallow-water correction. A property of the water rather than
    // of either train, which is why it sits here and not in WaterWaveTrain.
    float sea_depth;
    WaterWaveTrain wind_sea;
    WaterWaveTrain swell; // the older cross-swell; `scale` 0 removes the train entirely
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
    // SETTINGS throughout, in feature order, except:
    //
    // ENGINE-OWNED, read only: every GLuint and ShaderProgram*, the grid and
    // the spectral state, the *_baked / *_ready / *_failed / *_frames latches,
    // the seeded_* and bed_* records of the inputs the last bake used, the
    // measured cascade variances, the foam accumulator, the swash film's chain
    // and UBO, and the traced shoreline.
    //
    // No field here needs a function: the bakes compare their inputs against
    // what they last used and re-run when one moved, so a setting written
    // directly takes effect at the next frame.
    bool enabled;

    float level; // still-water plane, world Y
    // Half-size of the shoaling bed's domain, world units. NOT a bound on the drawn
    // surface, which the projected grid takes from the frustum -- outside this the bed
    // field reads its nearest edge texel, which is open water.
    float extent;

    // Optical properties of the body, authored rather than derived from a
    // transparency slider. absorption is extinction per world unit per channel,
    // so a bigger number is a shorter sight line. Water absorbs red first, which
    // is why the default is ordered the way it is.
    //
    // PER WORLD UNIT, where WATER_CLEAR_ABSORPTION_PER_M is per METRE: a world whose
    // unit is not a metre divides it by its own units-per-metre, or its sea is that
    // factor too absorbing. The two in-scatter fields carry no length and never convert.
    vec3 absorption;
    // What the body sends back, split into a response and a floor (spec 11.84).
    //
    // scatter_albedo is DIMENSIONLESS -- the fraction of the light falling on the
    // water that comes back out of it -- so the sea tracks its own illumination and
    // goes dark when nothing is lighting it. scatter_glow is absolute radiance added
    // regardless, which is how a scene asks for a sea that glows at night: a look,
    // not a measurement, and zero by default.
    //
    // The two exist separately because one value cannot be both. An authored constant
    // reproduces a stylised night and CANNOT produce a dark realistic one; a pure
    // albedo does the reverse. Which is why the key they replace is refused by name
    // rather than reinterpreted -- the old numbers still parse and would silently mean
    // something a few times too large.
    vec3 scatter_albedo;
    vec3 scatter_glow;
    // Henyey-Greenstein asymmetry of the sun's in-scatter, -1..1 (spec 13.4): 0 sends it
    // every way alike, toward 1 on along the beam, so water seen toward the sun glows.
    float scatter_g;

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
    // Refracted-grid caustics on what the surface refracts, on either wave model (spec 13.2).
    bool caustics;
    // How many bands of the spectrum the caustics are traced in, each at its own index (spec
    // 13.4), up to WATER_CAUSTIC_REFERENCE_SAMPLES; 0 = one trace, spread across the spectrum by
    // the surface's lookup. Each band costs a whole trace.
    int caustic_bands;
    // false = the caustics ignore the bed's relief. On, a point's brightness against its
    // neighbourhood stands in for its height (spec 13.4): proud points shift the pattern and gaps
    // take less of the key.
    bool caustic_relief;
    // false = clear water carries nothing in it. On, sunlit motes at three depths along the
    // sight line give the column a volume (spec 13.4).
    bool specks;
    // false = no analytic sun lobe, which is every frame before spec 11.42. Live on both
    // wave models: its width comes from the slope the surface stopped resolving, and the
    // Gerstner path reports that from its dropped octaves.
    bool glitter;
    // false = a surface under the sea is lit as though it stood in air, which is every frame
    // before spec 13.4. On, the key and the sky both weaken with the depth of water above.
    bool downwell;
    // false = whitewater is selected from THIS frame's fold and forgotten, which is the
    // pre-11.42 foam exactly. Spectral only: the accumulator is a cascade texture, and the
    // Gerstner path reports no compression to accumulate.
    bool foam_history;
    // How fast foam gives up and returns to open water, per second. Lower lingers longer.
    float foam_decay;
    // How fast foam streams downwind, in METRES per second (spec 11.44). The accumulator
    // backtraces its history along this, so whitewater is carried by the sea rather than
    // decaying where it was born -- the orbital half of that motion is already free, since
    // the surface reads foam at the wave parcel's own label.
    float foam_drift;
    // WATER_FOAM_DEBUG_OFF unless a measurement instrument set otherwise -- see the enum.
    // Its own mode rather than a render mode: what it measures is a fraction of SEA AREA,
    // and a coverage read off the shaded frame would move with the foam's own opacity,
    // colour, the sun and the tonemap, all of which it exists to calibrate. The shore band
    // is deliberately not shown: it is calibrated against a swash, not a whitecap relation.
    WaterFoamDebug foam_debug;
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
    GLuint quad_vao, quad_vbo; // fullscreen quad for every offscreen water pass; made on first use
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
     * height_var had ONE reader, water_fft_probe, until spec 11.47 gave it a second: the
     * crest-height gate publishes its square root per band so foam can ask "how tall is this
     * point relative to what this band of the sea normally does" without a third quantity.
     * The projector-slab consumer it was originally accumulated for was measured harmful and
     * reverted (spec 11.42 phase 4); this is a different use of the same number, not that one
     * coming back.
     *
     * height_var is in METRES SQUARED, not world units squared as this comment used to say.
     * The seeding is entirely SI -- g = 9.81, wind in m/s, fetch in m, and length_scale in
     * WATER_CASCADE_CFG is documented in metres -- and nothing in _water_build_spectrum
     * converts it. The conversion happens once, at the very end of oceanSpectralDisplacement
     * in ocean.glsl, which is the only place the field becomes a world-space quantity. That
     * stale claim is exactly the class of bug spec 11.44 existed to close, and it survived in
     * a field nothing but a probe read. slope_var is dimensionless -- a mean square slope,
     * which is what Cox-Munk's tables are also in.
     *
     * PREDICTED, not measured: the inverse transform is unnormalised and the seeding
     * draws h0 as (ga + i*gb)*A rather than the textbook (1/sqrt2)(xi_r + i*xi_i)*sqrt(S),
     * so the constant relating these to the field the shader samples is exactly the thing
     * --water-fft-probe exists to check rather than assert.
     */
    float cascade_height_var[WATER_CASCADE_COUNT];
    float cascade_slope_var[WATER_CASCADE_COUNT];

    /*
     * The ripple band (spec 13.3): the band past the short one, at its own 512 over 6 m. Its own
     * seed pair, ping-pong pair and twiddle table, since every one of those is sized by the
     * transform and this transform is not the cascades'. It shades and never displaces, like the
     * short band, so it has no previous copy and no foam.
     *
     * `ripple_slope_var_lod` is the band's mean square slope as each MIP LEVEL of it carries it,
     * [0] being the whole band. The band is sampled at the level the footprint asks for, so what
     * a footprint removes is [0] minus the level it read -- a property of the spectrum, computed
     * once at seeding rather than guessed per pixel.
     */
    GLuint ripple_initial;
    GLuint ripple_wave;
    GLuint ripple_array[2]; // [buffer], two layers each: the transform's two targets
    GLuint ripple_fbo[2];
    GLuint ripple_twiddle_tex;
    float ripple_height_var;
    float ripple_slope_var_lod[WATER_RIPPLE_LODS];

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

    /*
     * The surface query's readback (spec 13.1): one RGBA32F texel per query slot, read into a
     * ring of pack buffers and consumed at FIXED latency -- always the slot issued
     * WATER_PROBE_LATENCY passes ago, never whichever fence signalled, so what a caller reads
     * is a pure function of frame history and a headless run stays bit-equal to itself.
     *
     * Each ring slot keeps the points and the clock it was rendered with, because the answer
     * arrives passes after the question: a slot registered or moved in between must be refused
     * rather than handed the answer to a point it no longer names.
     *
     * Engine-owned; read through water_probe_result.
     */
    GLuint probe_tex;
    GLuint probe_fbo;
    GLuint probe_pbo[WATER_PROBE_LATENCY];
    float probe_issued_t[WATER_PROBE_LATENCY];
    float probe_issued_points[WATER_PROBE_LATENCY][WATER_PROBE_MAX][2];
    int probe_issued_count[WATER_PROBE_LATENCY];
    float probe_points[WATER_PROBE_MAX][2];        // world (x, z) per slot, as registered
    int probe_count;                               // slots [0, probe_count) are live; 0 = no pass
    bool probe_failed;                             // no program or target; never retried
    long probe_passes;                             // readbacks issued
    float probe_result[WATER_PROBE_MAX][4];        // (height, normal.x, normal.z, residual)
    float probe_result_points[WATER_PROBE_MAX][2]; // the points that answer is about
    int probe_answered;                            // slots in that answer; 0 = none yet
    float probe_result_t;                          // the clock it was rendered at

    /*
     * The caustics targets (specs 13.2 and 13.3): how much the key light is concentrated on the
     * floor, 1 where the water is flat, over windows that move with the camera -- one layer per
     * LEVEL, each a quarter of the one before. `caustic_origin` is where THIS frame's targets lie,
     * which the surface needs to look them up; their sizes and the depth they were traced to are
     * fixed (water_caustic_constants.glsl). Engine-owned.
     */
    GLuint caustic_tex; // 2D array, one layer per level, mipped; R16F, RGBA16F when banded
    GLuint caustic_fbo[WATER_CAUSTIC_LEVELS];
    // (G+1)^2 RGBA32F: each lattice corner's landed and source point. One for every level, which
    // trace and draw in turn.
    GLuint caustic_land_tex;
    GLuint caustic_land_fbo;
    GLuint caustic_vao;
    GLuint caustic_ebo;
    float caustic_origin[WATER_CAUSTIC_LEVELS][2]; // world xz of each target's corner
    bool caustic_drawn[WATER_CAUSTIC_LEVELS]; // false = skipped this frame, its window too deep
    bool caustic_ready;  // rendered this frame; false = the surface reads no caustics
    bool caustic_failed; // no program or target; never retried
    // The spectrum the lookup spreads the one trace across (spec 13.4): each band's index against
    // 550 nm's and its share of R, G and B. Constant, set at creation.
    float caustic_spectrum_dn[WATER_CAUSTIC_SPECTRAL_TAPS];
    vec3 caustic_spectrum_w[WATER_CAUSTIC_SPECTRAL_TAPS];
    bool caustic_target_colour; // the target was made RGBA16F for traced bands, else R16F

    /*
     * Touch ripples (spec 13.4): a height field over WATER_TOUCH_SIZE_M round where the camera
     * looks, stepped at WATER_TOUCH_STEP_HZ from the drops water_ripple_drop queues. Engine-owned.
     */
    GLuint touch_tex[2]; // RGBA16F (height, velocity), ping-pong
    GLuint touch_fbo[2];
    GLuint touch_slope_tex; // RGBA16F (height, dh/dx, dh/dz)
    GLuint touch_slope_fbo;
    int touch_current;     // which of touch_tex holds the latest step
    float touch_origin[2]; // world xz of the square's corner
    bool touch_placed;     // the square has been put somewhere yet
    double touch_clock;    // the clock the last step was taken at
    int touch_calm_steps;  // steps since the last drop; stops at WATER_TOUCH_CALM_STEPS
    float touch_drops[WATER_TOUCH_MAX_DROPS][4]; // world x, z, radius, depth, until stepped
    int touch_drop_count;
    bool touch_ready;  // there is a field to read; false = the surface reads flat water
    bool touch_failed; // no program or target; never retried

    // Settings. 0 off; 1 draws what the caustics multiplied the bed by, 2 the raw target on the
    // surface, 3 the target through the spectrum and the key's disc at the reference plane's
    // depth -- all half grey where nothing is concentrated.
    int caustic_debug;
} Water;

/*
 * Flatten the body (or its absence) into postfx's per-frame block, so the froxel
 * volume can carry water as a second medium below the surface.
 *
 * Takes the engine for the camera, which decides which side of the surface the eye is
 * on, and the scene for the light falling on the water -- the medium's in-scatter is
 * DERIVED from it (spec 11.84) rather than copied off the Water.
 */
void water_publish_to_postfx(const Water* water, const struct Scene* scene, struct Engine* engine);

/*
 * Publish the sea to a program that is not the water: where the swash has been (shore.glsl)
 * and what the water does to light reaching a surface under it (water_light.glsl). Scalars
 * only, no sampler unit. A NULL water publishes the off state.
 */
void water_bind_sea(const Water* water, const struct Scene* scene, ShaderProgram* program);

/*
 * The sea state the run-up runs at, for a caller that wants to evaluate the CPU twin.
 *
 * Returns false where there is no surf to describe, in which case `out` is untouched. Exists
 * so --shore-probe reads the SAME numbers the film is driven by rather than reconstructing
 * them, which would make the probe agree with itself instead of with the engine.
 */
bool water_shore_runup_params(const Water* water, const struct Scene* scene, ShoreRunupParams* out);

/*
 * The directional the surface takes its caustics, sun lobe and foam lighting from: the
 * one DELIVERING most to a horizontal surface, which is light_effective_intensity times
 * the cosine against up. NULL where none of them delivers anything -- no radiance, or
 * pointing up. A real state and not an error: an overcast night has no key, and caustics
 * that keep focusing one are focusing nothing.
 *
 * Published so a diagnostic can name the choice. The pick is not observable in a frame:
 * a sea lit by the wrong directional still looks like a sea. Both fixtures have always
 * authored a key beside the sky's sun -- what the moon changed is that one of the others
 * can now win.
 */
const struct Light* water_key_light(const struct Scene* scene);

/*
 * How much light falls on a horizontal water surface, in absolute scene radiance: the
 * sky's ambient plus the key directional's own irradiance. `scatter_albedo` is a FRACTION
 * of this, so it is the quantity that makes the sea know what time it is.
 *
 * The CPU twin of what water_frag builds from the prefiltered environment -- the froxel
 * medium is driven from this one, the surface from its own. Published because it is the
 * term 11.84 added and no frame reports it: a sea lit by the wrong amount still looks
 * like a sea, which is how the constant it replaced survived since 11.32.
 */
void water_incident_light(const struct Scene* scene, vec3 out);

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

/*
 * Read the caustics targets back and print each level's statistics over the inner 80% of its
 * window (spec 13.2) -- mean, min and max, one line per level. The mean is the energy check: a
 * target conserves light, so it sits at 1 for any sea. Stalls the pipeline once, so a diagnostic
 * rather than something the render loop may call.
 */
void water_caustic_probe(const Water* water);

/*
 * Press a ripple into the water at world (x, z) (spec 13.4): a dimple `radius_m` across and
 * `depth_m` deep, both in metres, pressed in at the next step and carried outward as rings.
 * Queued, up to WATER_TOUCH_MAX_DROPS a step; more in one step are dropped. Anywhere outside
 * the square round the view is dropped too, since nothing there is simulated.
 */
void water_ripple_drop(Water* water, float x, float z, float radius_m, float depth_m);

/*
 * A body's wake (spec 13.4): what water_wake remembers between calls, one per body. Zero is a
 * wake that has laid nothing yet.
 */
typedef struct WaterWake {
    float last[2]; // world xz of the last ripple
    bool placed;   // a ripple has been laid since the body last left the water
} WaterWake;

/*
 * Lay a body's wake: a ripple at world (x, z) every `pace` world units it moves while
 * `in_water`, and the first as soon as it enters. By distance rather than by time, so a body
 * standing still leaves the water to settle. Out of the water, or with no sea, the wake resets.
 */
void water_wake(Water* water, WaterWake* wake, float x, float z, float pace, bool in_water);

/*
 * Read the touch ripples back and print their deepest point and how far it lies from `(x, z)`,
 * the world point they were dropped at -- a ring's radius. Stalls the pipeline once; a
 * diagnostic, not something the render loop may call.
 */
void water_touch_probe(const Water* water, const struct Scene* scene, float x, float z);

/*
 * The surface query (spec 13.1): where the water is over a world (x, z), on either wave model,
 * through one call so nothing downstream branches on which sea it was handed.
 *
 * Register a point with water_probe_set, then read it each frame with water_probe_result. Slots
 * are dense: setting slot n makes [0, n] live. Both models answer from the same GPU pass, so
 * both are WATER_PROBE_LATENCY passes old; a caller that wants the Gerstner train at exactly
 * now has water_surface_at, its closed form.
 */
bool water_probe_set(Water* water, int slot, float x, float z);

typedef struct WaterSample {
    float height; // world Y of the surface over the query
    vec3 normal;
    float residual; // how far the recovered parameter lands from the query, world units
    float t;        // the clock this answer describes
} WaterSample;

/*
 * The latest answer for `slot`. false, with `out` untouched, whenever there is no real answer
 * -- never a silent still level. water_probe_refusal names the reason.
 */
bool water_probe_result(const Water* water, int slot, WaterSample* out);

// Why water_probe_result would refuse `slot`, as the probe grammar's reason token -- nowater,
// unset, failed, unfilled, or stale for a slot registered or moved since its answer was
// rendered -- or NULL when it would answer.
const char* water_probe_refusal(const Water* water, int slot);

#endif // _WATER_H_
