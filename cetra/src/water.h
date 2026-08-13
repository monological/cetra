#ifndef _WATER_H_
#define _WATER_H_

#include <GL/glew.h>
#include <stdbool.h>
#include <cglm/cglm.h>

/*
 * Water surface (spec 11.32, roadmap D3).
 *
 * A single-layer water surface: it draws in the OPAQUE-like slot after the
 * refraction resolve, writes depth and the G-buffer, and does its own
 * transmission. It is not translucent geometry and takes no part in OIT --
 * sorting a water plane against itself is not a question anyone asked, and the
 * things that must sort against water (particles, translucents) run after it
 * against the depth it wrote.
 *
 * Writing the aux attachment is what makes fog and aerial perspective land at
 * the WATER's depth rather than at the depth of the bed behind it: the
 * atmosphere composite is driven by aux linear-Z, and the late pass writes no
 * aux, which is why a translucent surface is fogged wrong today and water is
 * not.
 *
 * WHY ITS OWN PROGRAM. pbr_frag declares all sixteen fragment samplers the
 * driver allows and the driver counts declarations, not distinct units (see
 * engine.h on the moment atlas). Water needs the resolved scene depth, which
 * pbr_frag has no slot for. A dedicated program starts from sixteen again.
 */

// Cells per side of the surface grid. The mesh is a plain indexed grid in the XZ
// plane; how it is placed and displaced is the vertex shader's business.
#define WATER_GRID_RES 128

// Resolved single-sample scene depth, for the water column and the shoreline
// fade. Sampler units are per program, so this collides with nothing -- it is
// the same argument particle_frag makes for its own depth tap on unit 7.
#define WATER_DEPTH_UNIT 7
// Six transformed cascade fields (3 bands x 2 targets) start here. Units 0-1 are
// albedo/normal by house convention and 6-7 are taken above, so this sits clear of
// both and of the engine-bound IBL block at 9-15.
#define WATER_CASCADE_UNIT0 16

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
    // derived from it inside ocean.glsl. steepness is a bounded 0..1 knob rather
    // than a Gerstner Q, so no value of it can fold the surface over itself.
    vec2 wind_dir;
    float amplitude;
    float wavelength;
    float steepness;
    float spread; // per-octave direction fan, radians

    WaterHeightFn height_at; // optional bed provider; see WaterHeightFn
    void* height_ctx;

    WaterWaveModel wave_model;

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
} Water;

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
 * Leaves the GL state it touched as it found it, including the draw-buffer list.
 */
void water_render(Water* water, struct Scene* scene, struct Engine* engine, const mat4 view,
                  const mat4 draw_projection);

#endif // _WATER_H_
