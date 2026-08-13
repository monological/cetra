#ifndef _WATER_H_
#define _WATER_H_

#include <GL/glew.h>
#include <stdbool.h>
#include <cglm/cglm.h>

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

// Cells per side of one patch.
#define WATER_GRID_RES 128

/*
 * Camera-snapped clipmap rings.
 *
 * A uniform grid puts its samples where the WORLD is; this puts them where the
 * PIXELS are. Level 0 is a full grid at `extent / 2^(levels-1)`, and each further
 * level is a ring at twice the half-extent with its inner quarter removed -- which
 * is exactly the level inside it, so the hole matches by construction.
 *
 * THE SNAP IS THE WHOLE TRICK. Every level snaps its origin to the COARSEST level's
 * cell, not its own. Level L's cell divides that exactly (2^(levels-1-L)) and every
 * half-extent is an integer multiple of it, so all ring boundaries land on one shared
 * grid and the rings tile with no gap, no overlap, and therefore no coplanar depth
 * tie. Snapping each level to its own cell -- the obvious thing -- is what puts a
 * mismatch at every boundary.
 */
#define WATER_RING_LEVELS 5

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

// Resolution of the baked bed heightfield. It only has to resolve the SHOALING
// ramp -- how fast the water shallows -- not the terrain's own detail, which the
// depth buffer already carries per fragment.
#define WATER_BED_RES 256

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

typedef struct Water {
    bool enabled;

    float level;  // still-water plane, world Y
    float extent; // half-size of the drawn surface, world units

    // Optical properties of the body, authored rather than derived from a
    // transparency slider. absorption is extinction per world unit per channel,
    // so a bigger number is a shorter sight line; scatter is the colour the
    // absorbed energy comes back as. Water absorbs red first, which is why the
    // default is ordered the way it is.
    vec3 absorption;
    vec3 scatter;

    float roughness; // interface roughness; picks the environment lobe's mip
    float ior;       // 1.333 for water -> F0 0.020

    // Wave train. amplitude/wavelength describe the LONGEST octave; the rest are
    // derived from it inside ocean.glsl. steepness is 0..1 rather than a Gerstner Q
    // -- 1 is the steepest crest whose horizontal map is still injective -- and is
    // clamped to that range on upload.
    vec2 wind_dir;
    float amplitude;
    float wavelength;
    float steepness;
    float spread; // per-octave direction fan, radians

    WaterHeightFn height_at; // optional bed provider; see WaterHeightFn
    void* height_ctx;

    WaterWaveModel wave_model;
    // Caustics on refracted geometry. Inert on the Gerstner path, whose steepness
    // is clamped so its mapping cannot compress and therefore cannot focus.
    bool caustics;

    // Lazily built GPU state, on the postfx ensure_* pattern. `failed` latches
    // so a missing program costs one log line rather than one per frame forever.
    GLuint grid_vao;
    GLuint grid_vbo;
    GLuint grid_ebo;
    int grid_index_count; // centre patch: the whole grid
    int ring_index_count; // one ring: the grid minus its inner quarter
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
} Water;

// Marks the baked bed stale; the next draw re-bakes it from height_at.
void water_invalidate_bed(Water* water);

/*
 * Flatten the body (or its absence) into postfx's per-frame block, so the froxel
 * volume can carry water as a second medium below the surface.
 *
 * Needs the camera to decide which side the eye is on, which is why it takes the
 * engine rather than just the water.
 */
void water_publish_to_postfx(const Water* water, struct Engine* engine);

// World-space tiling period of each cascade, in metres. Public because the
// surface shader needs the same numbers to build its sample UVs, and a second
// copy of them would be a silent mismatch rather than an error.
extern const float WATER_CASCADE_LENGTH[WATER_CASCADE_COUNT];
extern const float WATER_CASCADE_CHOPPINESS[WATER_CASCADE_COUNT];

Water* create_water(void);
void free_water(Water* water);

// Should the surface draw this frame.
bool water_active(const Water* water);

/*
 * Draw the surface into the currently bound scene FBO.
 *
 * Preconditions the caller owns: the refraction resolve has run this frame (the
 * surface samples it for transmission) and this is not a cube capture -- the
 * depth resolve blits at the main render size and re-binds the scene
 * framebuffer, which would redirect the rest of a capture.
 *
 * Restores the framebuffer binding, viewport, blend, depth test, cull face and the
 * draw-buffer list. Leaves its own program and texture bindings current.
 */
void water_render(Water* water, struct Scene* scene, struct Engine* engine, const mat4 view,
                  const mat4 draw_projection);

#endif // _WATER_H_
