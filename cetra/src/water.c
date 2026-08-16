#include <math.h>
#include <stdint.h>
#include <stdio.h> // water_fft_probe prints to stdout, like the CPU wave query
#include <stdlib.h>

#include "water.h"

#include "engine.h"
#include "ext/log.h"
#include "ibl.h"
#include "profiler.h"
#include "program.h"
#include "scene.h"
#include "shadow.h" // the cascades, for the glitter's shadow
#include "sky.h"    // sky_bind_cloud_shadow (the deck dims the caustics)
#include "texture.h"
#include "uniform.h"
#include "util.h"

/*
 * The three spectral bands, ported from the reference study.
 *
 * cutoff_low/high are the wavenumber window each band owns, in rad/m, and they ABUT:
 * each band's high bound is the next band's low bound, so every mode is seeded in
 * exactly one cascade. The reference study's own numbers overlapped -- [0.30, 0.36] and
 * [1.22, 1.42] -- and the modes in those two strips had their energy counted twice,
 * which is a brighter sea in two narrow bands that nobody chose. A mode landing exactly
 * on a boundary is still seeded twice; k here is a 2D float norm, so that is a
 * measure-zero case rather than a bound worth an extra comparison.
 *
 * The short band reaches 24 rad/m -- decimetre waves -- and is deliberately not
 * carried into the mesh, where it would alias into a ridged texture; it shades the
 * interface only.
 */
static const struct WaterCascadeConfig {
    float length_scale;
    float cutoff_low;
    float cutoff_high;
    float amplitude_scale;
    float secondary_scale; // weight of a second, cross-travelling swell
    // How hard this band's horizontal displacement pulls. The short band is damped: it
    // exists to shade, and choppiness there sharpens nothing the mesh resolves.
    float choppiness;
    uint32_t seed;
} WATER_CASCADE_CFG[WATER_CASCADE_COUNT] = {
    {240.0f, 0.024f, 0.30f, 0.45f, 0.22f, 1.18f, 0x51f15eu},
    {64.0f, 0.30f, 1.22f, 0.45f, 0.08f, 1.05f, 0x72a93bu},
    {12.0f, 1.22f, 24.0f, 0.82f, 0.0f, 0.40f, 0x19ce47u},
};

// The default sea state, in the units WaterSeaState documents. A moderate wind sea:
// force 6 over a long fetch, deep enough that the TMA correction barely bites.
//
// These were six file-scope #defines until spec 11.42. They are defaults now rather than
// constants, so a scene can author a calmer or a rougher sea -- and the wind DIRECTION
// left with them, to Water.wind_dir, which both wave models already shared.
#define WATER_DEFAULT_SEA_DEPTH 54.0f
#define WATER_DEFAULT_WIND_SPEED 11.5f
#define WATER_DEFAULT_FETCH 120000.0f
#define WATER_DEFAULT_PEAK_ENHANCEMENT 3.3f
#define WATER_DEFAULT_SWELL 0.38f

// The reference's deterministic PRNG, ported exactly rather than swapped for
// rand(): the spectrum it seeds IS the ocean's identity, so a different sequence
// is a different sea, and a headless render has to reproduce this one.
static float _water_rand(uint32_t* state) {
    *state += 0x6d2b79f5u;
    uint32_t v = *state;
    v = (v ^ (v >> 15)) * (v | 1u);
    v ^= v + (v ^ (v >> 7)) * (v | 61u);
    return (float)((v ^ (v >> 14))) / 4294967296.0f;
}

/*
 * The RNG state for one mode, from its grid index alone.
 *
 * This is what makes a cutoff a cutoff. Advancing one sequence across the grid and
 * drawing only for modes inside the window ties the RNG's POSITION to how many modes
 * passed -- so moving a cutoff by 0.06 rad/m re-randomises every phase after it, and
 * the edit lands as a different ocean rather than as the same one minus a strip.
 * Measured while closing the overlap above: 78% of the frame moved, where the energy
 * actually removed accounts for 28%.
 *
 * Seeding per mode also makes the sea independent of the traversal order and of the
 * grid resolution's relationship to it, which a sequence is not.
 */
static uint32_t _water_mode_seed(uint32_t seed, int x, int y) {
    // Weyl-style mixing of the two indices, then an integer avalanche so neighbouring
    // modes are uncorrelated -- adjacent seeds differing in low bits would otherwise
    // hand adjacent wavenumbers near-identical phases and print as a directional
    // pattern in the surface.
    uint32_t h = seed + (uint32_t)x * 0x9e3779b9u + (uint32_t)y * 0x85ebca6bu;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

static void _water_gaussian(uint32_t* state, float* out_a, float* out_b) {
    float u = fmaxf(_water_rand(state), 1e-7f);
    float v = _water_rand(state);
    float radius = sqrtf(-2.0f * logf(u));
    float angle = 6.28318530718f * v;
    *out_a = radius * cosf(angle);
    *out_b = radius * sinf(angle);
}

static float _water_wrap_angle(float a) {
    while (a > 3.14159265359f)
        a -= 6.28318530718f;
    while (a < -3.14159265359f)
        a += 6.28318530718f;
    return a;
}

// Normalisation for the cos^(2s)(theta/2) directional spread, via the gamma
// function's Stirling series. Without it a narrower spread would also be a
// brighter one, and "directional focus" would double as an amplitude knob.
static float _water_spread_norm(float spread) {
    // ln(Gamma(s + 1)) - ln(Gamma(s + 0.5)) evaluated by Stirling; the ratio is
    // what the normalisation needs and it stays well-conditioned where the
    // gammas themselves overflow.
    float s = spread;
    float a = s + 1.0f;
    float b = s + 0.5f;
    float lga = (a - 0.5f) * logf(a) - a + 0.9189385332f + 1.0f / (12.0f * a);
    float lgb = (b - 0.5f) * logf(b) - b + 0.9189385332f + 1.0f / (12.0f * b);
    return expf(lga - lgb) / (2.0f * sqrtf(3.14159265359f));
}

// JONSWAP with the TMA shallow-water correction, at one wavenumber.
static float _water_jonswap(float omega, float peak_omega, float alpha, float tma,
                            float enhancement) {
    const float g = 9.81f;
    float sigma = omega <= peak_omega ? 0.07f : 0.09f;
    float peak_distance = (omega - peak_omega) / fmaxf(sigma * peak_omega, 1e-5f);
    float peak_shape = expf(-0.5f * peak_distance * peak_distance);
    float peak_ratio = peak_omega / omega;
    return tma * alpha * g * g / powf(omega, 5.0f) *
           expf(-1.25f * powf(peak_ratio, 4.0f)) * powf(enhancement, peak_shape);
}

// The JONSWAP peak angular frequency for a sea state, rad/s: the fetch-limited relation, in
// one place because the seeding shapes the spectrum around it and the surf runs at it.
static float _water_peak_omega(const WaterSeaState* sea) {
    const float g = 9.81f;
    const float wind_speed = fmaxf(sea->wind_speed, 0.1f);
    const float fetch = fmaxf(sea->fetch, 1.0f);
    return 22.0f * powf(wind_speed * fetch / (g * g), -0.33f);
}

// Significant wave height of the seeded sea, metres: 4 sigma, with the variance summed over
// the cascades since they own disjoint wavenumber windows. Meaningful once seeded.
static float _water_significant_height(const Water* water) {
    float var = 0.0f;
    for (int c = 0; c < WATER_CASCADE_COUNT; c++)
        var += water->cascade_height_var[c];
    return 4.0f * sqrtf(fmaxf(var, 0.0f));
}

/*
 * Seed one cascade: the conjugate-symmetric initial spectrum, and the per-mode wave
 * vector and dispersion.
 *
 * initial holds h0(k) in .xy and conj(h0(-k)) in .zw, which is what lets the
 * evolution step produce a REAL surface from one complex multiply per mode
 * instead of enforcing symmetry afterwards.
 */
static bool _water_build_spectrum(int size, const struct WaterCascadeConfig* cfg,
                                  const WaterSeaState* sea, float wind_angle, float* initial,
                                  float* wave_data, float* out_height_var, float* out_slope_var) {
    const float g = 9.81f;
    const float delta_k = 6.28318530718f / cfg->length_scale;
    // Guarded rather than trusted: these are authored now, and a zero wind speed or fetch
    // divides here rather than at some later texel. The floors are far below any sea a
    // scene would ask for, so they can only bind on a value that was never a sea state.
    const float wind_speed = fmaxf(sea->wind_speed, 0.1f);
    const float fetch = fmaxf(sea->fetch, 1.0f);
    const float sea_depth = fmaxf(sea->sea_depth, 0.1f);
    const float alpha = 0.076f * powf(g * fetch / (wind_speed * wind_speed), -0.22f);
    const float peak_omega = _water_peak_omega(sea);

    // Reported rather than shrugged off: returning quietly would leave wave_data
    // unwritten and the twiddle table all zeros, so every butterfly would read
    // texel 0 and the transform would collapse into a flat ocean with no error --
    // and for a later cascade the buffers still hold the PREVIOUS one's spectrum,
    // which is worse than flat because it looks plausible.
    float* h0 = calloc((size_t)size * size * 2, sizeof(float));
    if (!h0)
        return false;

    // Accumulated in double: 16k modes whose amplitudes span decades, and the small ones
    // are the short waves that carry most of the SLOPE. Summing those into a float total
    // dominated by the swell loses exactly the half the roughness handover reads.
    double sum_a2 = 0.0;
    double sum_k2_a2 = 0.0;

    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            const int pixel = y * size + x;
            const float kx = (float)(x - size / 2) * delta_k;
            const float kz = (float)(y - size / 2) * delta_k;
            const float k_len = sqrtf(kx * kx + kz * kz);
            float* wd = &wave_data[pixel * 4];

            if (k_len < cfg->cutoff_low || k_len > cfg->cutoff_high) {
                // Outside this band's window. 1/k is stored as 1 rather than 0
                // so the evolution's multiply stays finite on a dead mode.
                wd[0] = 0.0f;
                wd[1] = 1.0f;
                wd[2] = 0.0f;
                wd[3] = 0.0f;
                continue;
            }

            const float kh = fminf(k_len * sea_depth, 20.0f);
            const float tanh_kh = tanhf(kh);
            const float omega = sqrtf(g * k_len * tanh_kh);
            const float sech2 = 1.0f - tanh_kh * tanh_kh;
            // d(omega)/dk, which converts a spectral density in frequency to one
            // in wavenumber. Getting this wrong scales the whole sea state.
            const float domega =
                g * (sea_depth * k_len * sech2 + tanh_kh) / fmaxf(omega * 2.0f, 1e-5f);
            const float omega_h = omega * sqrtf(sea_depth / g);
            const float tma = omega_h <= 1.0f ? 0.5f * omega_h * omega_h
                              : omega_h < 2.0f ? 1.0f - 0.5f * (2.0f - omega_h) * (2.0f - omega_h)
                                               : 1.0f;

            const float jonswap =
                _water_jonswap(omega, peak_omega, alpha, tma, sea->peak_enhancement);
            const float theta = _water_wrap_angle(atan2f(kz, kx) - wind_angle);
            const float omega_ratio = omega / peak_omega;
            const float spread_power =
                ((omega > peak_omega ? 9.77f * powf(omega_ratio, -2.5f)
                                     : 6.97f * powf(omega_ratio, 5.0f)) +
                 16.0f * tanhf(fminf(omega_ratio, 20.0f)) * sea->swell * sea->swell) *
                0.58f;
            const float focused = _water_spread_norm(spread_power) *
                                  powf(fabsf(cosf(theta * 0.5f)), 2.0f * spread_power);
            const float broad = 2.0f / 3.14159265359f * powf(fmaxf(cosf(theta), 0.0f), 2.0f);
            const float direction = focused * 0.68f + broad * 0.32f;
            // Rolls the very short modes off before the band edge, so the window
            // does not end in a hard spectral cliff that rings after transform.
            const float short_fade = expf(-0.00016f * k_len * k_len);
            float density = jonswap * direction * short_fade;

            if (cfg->secondary_scale > 0.0f) {
                // A second swell train, older and crossing the wind. One
                // direction of travel, however well spread, reads as corduroy.
                const float sw_wind = 8.4f;
                const float sw_fetch = 310000.0f;
                const float sw_peak = 22.0f * powf(sw_wind * sw_fetch / (g * g), -0.33f);
                const float sw_alpha = 0.076f * powf(g * sw_fetch / (sw_wind * sw_wind), -0.22f);
                const float sw_spectrum = _water_jonswap(omega, sw_peak, sw_alpha, tma, 2.6f);
                // Crossing the wind by a fixed 0.82 rad. Not authorable with the rest:
                // the angle BETWEEN the two trains is what makes this read as an older
                // swell rather than as more wind sea, so it belongs to the model, and it
                // follows the authored wind rather than sitting at an absolute bearing.
                const float sw_theta = _water_wrap_angle(atan2f(kz, kx) - (wind_angle + 0.82f));
                const float sw_ratio = omega / sw_peak;
                const float sw_spread =
                    ((omega > sw_peak ? 9.77f * powf(sw_ratio, -2.5f)
                                      : 6.97f * powf(sw_ratio, 5.0f)) +
                     9.0f) *
                    0.72f;
                const float sw_dir = _water_spread_norm(sw_spread) *
                                     powf(fabsf(cosf(sw_theta * 0.5f)), 2.0f * sw_spread);
                density += sw_spectrum * sw_dir * short_fade * cfg->secondary_scale;
            }

            const float amplitude =
                sqrtf(fmaxf(0.0f, 2.0f * density * fabsf(domega) / k_len * delta_k * delta_k)) *
                cfg->amplitude_scale;
            float ga, gb;
            uint32_t rng = _water_mode_seed(cfg->seed, x, y);
            _water_gaussian(&rng, &ga, &gb);
            h0[pixel * 2 + 0] = ga * amplitude;
            h0[pixel * 2 + 1] = gb * amplitude;
            // The mode's expected power, before the RNG: E[|h0|^2] is 2A^2 because ga and
            // gb are independent standard normals. Accumulated from A rather than from the
            // drawn h0 so the totals describe the SPECTRUM rather than this one seed's
            // realisation -- a different seed is the same sea, and a bound taken from one
            // realisation would move with it.
            sum_a2 += (double)amplitude * (double)amplitude;
            sum_k2_a2 += (double)k_len * (double)k_len * (double)amplitude * (double)amplitude;

            wd[0] = kx;
            wd[1] = 1.0f / k_len;
            wd[2] = kz;
            wd[3] = omega;
        }
    }

    // Pack h0(k) beside conj(h0(-k)). The mirror index wraps at 0 because -0 is
    // 0 in a periodic grid, which is why the modulo is here and not a negation.
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            const int pixel = y * size + x;
            const int mirror = ((size - y) % size) * size + ((size - x) % size);
            initial[pixel * 4 + 0] = h0[pixel * 2 + 0];
            initial[pixel * 4 + 1] = h0[pixel * 2 + 1];
            initial[pixel * 4 + 2] = h0[mirror * 2 + 0];
            initial[pixel * 4 + 3] = -h0[mirror * 2 + 1];
        }
    }
    free(h0);

    /*
     * The variance the transformed field is expected to carry.
     *
     * The evolve pairs each mode with its conjugate at -k, so the transformed coefficient
     * has E[|H(k)|^2] = 2A(k)^2 + 2A(-k)^2, and the unnormalised inverse sum makes the
     * spatial variance the sum of those over the grid -- which is 4 * sum(A^2), since the
     * -k term walks the same set. The slope field is i*k*H, so its variance carries the
     * extra k^2 and is already the sum of BOTH components.
     *
     * Stated as arithmetic rather than trusted: --water-fft-probe measures the field and
     * prints the ratio, which is the only way the normalisation gets checked at all.
     */
    if (out_height_var)
        *out_height_var = (float)(4.0 * sum_a2);
    if (out_slope_var)
        *out_slope_var = (float)(4.0 * sum_k2_a2);

    return true;
}

/*
 * Stockham twiddles: per stage and per output index, the rotation and the two input
 * indices the butterfly reads. Precomputing the INDICES is what keeps the shader a
 * pure gather with no bit-reversal pass of its own.
 *
 * A function of the transform SIZE alone -- no cascade, no sea state -- which is why
 * one table serves every cascade and why a re-seed leaves it standing.
 */
static void _water_build_twiddle(int size, float* twiddle) {
    for (int stage = 0; stage < WATER_SPECTRUM_LOG; stage++) {
        const int block = size >> (stage + 1);
        for (int out = 0; out < size / 2; out++) {
            const int first = (2 * block * (out / block) + out % block) % size;
            const float angle = -6.28318530718f / (float)size * (float)(out / block) * (float)block;
            const float c = cosf(angle);
            const float s = sinf(angle);
            float* lo = &twiddle[(stage * size + out) * 4];
            float* hi = &twiddle[(stage * size + out + size / 2) * 4];
            lo[0] = c;
            lo[1] = s;
            lo[2] = (float)first;
            lo[3] = (float)(first + block);
            hi[0] = -c;
            hi[1] = -s;
            hi[2] = (float)first;
            hi[3] = (float)(first + block);
        }
    }
}

Water* create_water(void) {
    Water* water = calloc(1, sizeof(Water));
    if (!water) {
        log_error("Failed to allocate memory for water");
        return NULL;
    }

    water->enabled = true;
    water->level = 0.0f;
    water->extent = 60.0f;
    // Not a look control at the scale of a pond -- over the couple of metres a lake
    // fixture spans these barely bite -- but it is the physical ordering, and the
    // depth-graded colour that reads as water comes from it rather than from a tint.
    // Stored per world unit, so this default is the physical figure for a world where
    // one unit is one metre; see the field's own contract.
    glm_vec3_copy(WATER_CLEAR_ABSORPTION_PER_M, water->absorption);
    glm_vec3_copy((vec3){0.02f, 0.10f, 0.12f}, water->scatter);
    water->roughness = 0.04f;
    water->ior = 1.333f;
    // Lake-scale defaults: a 6 m longest wave at 6 cm, which is a light breeze
    // rather than a sea state. An ocean wants both numbers an order up.
    glm_vec2_copy((vec2){0.86f, 0.51f}, water->wind_dir);
    water->amplitude = 0.06f;
    water->wavelength = 6.0f;
    water->steepness = 0.6f;
    water->spread = 0.42f;
    // The spectral sea state, inert until --water-waves fft. Unlike the train above it is
    // physical rather than lake-scaled: the spectrum decides its own height from the wind
    // and the fetch, which is why there is no amplitude here to scale it with.
    water->sea.wind_speed = WATER_DEFAULT_WIND_SPEED;
    water->sea.fetch = WATER_DEFAULT_FETCH;
    water->sea.sea_depth = WATER_DEFAULT_SEA_DEPTH;
    water->sea.peak_enhancement = WATER_DEFAULT_PEAK_ENHANCEMENT;
    water->sea.swell = WATER_DEFAULT_SWELL;
    // Gerstner by default: it allocates no GPU state, costs no passes, and is the
    // right model at the scale most scenes put water at. The spectral path is an
    // ocean, and asks for 45 passes and 24 textures to say so.
    water->wave_model = WATER_WAVES_GERSTNER;
    water->caustics = true;
    water->glitter = true;
    water->foam_history = true;
    // Slow enough that a crest leaves a visible trail behind it and fast enough that open
    // water is not permanently white. The reference this is ported from calls the same
    // number 0.4 and means the same thing by it.
    water->foam_decay = 0.4f;
    // Surface drift, m/s. A few per cent of a moderate wind, which is the order the
    // literature gives for what a wind sea carries its surface at.
    water->foam_drift = 0.35f;
    water->shore_coverage = true;
    water->surf = true;
    water->far_lod = true;
    return water;
}

void free_water(Water* water) {
    if (!water)
        return;
    // No `if (handle)` guards: glDelete* on 0 is a no-op (util.h says so), and the
    // cascade arrays are contiguous GLuint, so each family is one call rather than a
    // nest of loops over single deletes.
    glDeleteVertexArrays(1, &water->grid_vao);
    glDeleteBuffers(1, &water->grid_vbo);
    glDeleteBuffers(1, &water->grid_ebo);
    glDeleteVertexArrays(1, &water->fft_vao);
    glDeleteBuffers(1, &water->fft_vbo);
    glDeleteTextures(1, &water->twiddle_tex);
    glDeleteTextures(1, &water->bed_tex);
    glDeleteTextures(WATER_CASCADE_COUNT, water->cascade_initial);
    glDeleteTextures(WATER_CASCADE_COUNT, water->cascade_wave);
    glDeleteTextures(WATER_CASCADE_COUNT * 4, &water->cascade_field[0][0][0]);
    glDeleteFramebuffers(WATER_CASCADE_COUNT * 2, &water->cascade_fbo[0][0]);
    glDeleteTextures(WATER_PREV_CASCADES, water->cascade_prev);
    glDeleteTextures(2, water->foam_tex);
    glDeleteFramebuffers(2, water->foam_fbo);
    free(water);
}

void water_publish_to_postfx(const Water* water, struct Engine* engine) {
    if (!engine || !engine->postfx)
        return;
    PostFX* fx = engine->postfx;

    if (!water_active(water)) {
        fx->water_medium = 0;
        fx->water_suppress_aerial = 0;
        return;
    }

    /*
     * Only while the eye is submerged. Above the surface the body is behind the
     * water's own absorption, which the surface shader already integrates, so a volume
     * there would buy nothing -- and water_medium is what ARMS the froxel pass, so
     * publishing it unconditionally would bill every water scene for a volume.
     *
     * Aerial perspective is suppressed for the same frame: it holds the sky-view
     * integral, which is air the sight line never crosses down here, so leaving it on
     * hazes the seabed with atmosphere.
     *
     * Both are published as REQUESTS rather than written into the flags that answer
     * them. fog_enabled belongs to the app and the GUI checkbox and nothing resets it
     * per frame, so setting it here would turn volumetric fog on permanently after one
     * submerged frame; aerial_volume belongs to the sky, which republishes every frame,
     * so clearing it here would make publish call order load-bearing.
     */
    const bool submerged = engine->camera && engine->camera->position[1] < water->level;
    fx->water_medium = submerged ? 1 : 0;
    fx->water_suppress_aerial = submerged ? 1 : 0;
    fx->water_level_y = water->level;
    memcpy(fx->water_extinction, water->absorption, sizeof(vec3));
    memcpy(fx->water_inscatter, water->scatter, sizeof(vec3));
}

/*
 * Bake height_at over the drawn extent: the height in R, and its world-space
 * gradient in G,B.
 *
 * The gradient is here rather than taken with fwidth in the shader because a VERTEX
 * stage has no derivatives, and it is the vertex stage that needs it -- the shoal
 * factor scales the displacement, so the factor's own slope is a product-rule term in
 * the surface derivatives. Central differences over the baked grid, which is the
 * gradient of the field the shader actually samples rather than of the callback
 * behind it; a finer difference than the texels would describe a bed the surface
 * never sees.
 *
 * CLAMP so a vertex just outside the baked square reads the nearest shore rather than
 * wrapping to the far side of the scene, and RGBA32F because a 3-channel float texture
 * is not reliably filterable everywhere this runs.
 */
static void _water_bake_bed(Water* water, float units_per_metre) {
    if (!water->height_at)
        return;
    // Self-checking, on the engine's own pattern for a lazily sized target
    // (_ensure_scene_depth_target): store what was baked and rebuild when the request
    // disagrees. Manual invalidation had the dependency backwards -- the GUI re-armed it
    // from the level when the bake did not read it, and nothing re-armed it when the
    // EXTENT moved, which it did. The level is an input now, for the foreshore slope.
    if (water->bed_baked && water->bed_extent == water->extent &&
        water->bed_level == water->level && water->bed_units_per_metre == units_per_metre)
        return;

    const int res = WATER_BED_RES;
    float* heights = malloc((size_t)res * res * 4 * sizeof(float));
    if (!heights) {
        log_error("Water bed bake allocation failed; shoaling disabled");
        water->bed_baked = true; // latched: retrying every frame would not help
        return;
    }

    const float span = water->extent * 2.0f;
    for (int z = 0; z < res; z++) {
        for (int x = 0; x < res; x++) {
            // Texel CENTRES, so the sampled field lines up with the bilinear tap
            // the vertex shader makes rather than sitting half a texel off it.
            const float wx = ((float)x + 0.5f) / (float)res * span - water->extent;
            const float wz = ((float)z + 0.5f) / (float)res * span - water->extent;
            heights[(z * res + x) * 4] = water->height_at(water->height_ctx, wx, wz);
        }
    }

    // Second pass, so every central difference reads baked neighbours rather than
    // re-entering the callback. One-sided at the border, where the far neighbour does
    // not exist -- CLAMP means the field is flat past the edge anyway.
    //
    // The foreshore slope is accumulated in the same pass: the mean gradient magnitude
    // over the texels within a metre of the still line, above or below it. See the field.
    const float cell = span / (float)res;
    const float foreshore = 1.0f * units_per_metre;
    double slope_sum = 0.0;
    int slope_count = 0;
    for (int z = 0; z < res; z++) {
        for (int x = 0; x < res; x++) {
            const int x0 = x > 0 ? x - 1 : x;
            const int x1 = x < res - 1 ? x + 1 : x;
            const int z0 = z > 0 ? z - 1 : z;
            const int z1 = z < res - 1 ? z + 1 : z;
            float* t = &heights[(z * res + x) * 4];
            t[1] = (heights[(z * res + x1) * 4] - heights[(z * res + x0) * 4]) /
                   ((float)(x1 - x0) * cell);
            t[2] = (heights[(z1 * res + x) * 4] - heights[(z0 * res + x) * 4]) /
                   ((float)(z1 - z0) * cell);
            t[3] = 0.0f;
            if (fabsf(t[0] - water->level) <= foreshore) {
                slope_sum += sqrt((double)t[1] * t[1] + (double)t[2] * t[2]);
                slope_count++;
            }
        }
    }
    water->bed_foreshore_slope = slope_count > 0 ? (float)(slope_sum / slope_count) : 0.0f;

    // The engine's own float-LUT upload: LINEAR / CLAMP_TO_EDGE / no mips is exactly
    // this texture's policy, so there is nothing here to hand-roll. Deleted and recreated
    // rather than re-uploaded because that helper returns a fresh name; the extent change
    // that brought us here is rare enough that the churn does not matter.
    gl_delete_texture(&water->bed_tex);
    water->bed_tex = create_texture_2d_float(res, res, GL_RGBA32F, GL_RGBA, heights);

    free(heights);
    water->bed_baked = true;
    water->bed_extent = water->extent;
    water->bed_level = water->level;
    water->bed_units_per_metre = units_per_metre;
    log_info("Water: bed baked at %d^2 over %.0f units, foreshore slope %.3f", res,
             (double)span, (double)water->bed_foreshore_slope);
}

bool water_active(const Water* water) {
    return water && water->enabled && !water->failed;
}

float water_effective_steepness(const Water* water) {
    // Clamped rather than trusted, and clamped HERE rather than at the uniform upload,
    // because the CPU wave query reads the same number. Applied only on the way out, it
    // gave the GPU 1.0 and the CPU whatever a .cscn authored, and the two surfaces then
    // disagreed -- which is the one failure the CPU query exists to avoid.
    //
    // Above 1 the summed Gerstner steepness makes the horizontal map non-injective and
    // the surface folds through itself. The shader's per-octave normalisation guarantees
    // that bound only for an in-range value.
    return water ? glm_clamp(water->steepness, 0.0f, 1.0f) : 0.0f;
}

bool water_will_draw(const Water* water, const struct Engine* engine, RenderMode render_mode) {
    // Debug modes take a passthrough blit and skip the whole surface; a cube capture
    // must not run the depth resolve, which blits at the main render size and re-binds
    // the scene framebuffer. Both were previously terms at the DRAW site only, so the
    // catcher and the fog medium disagreed with the draw about whether water existed.
    return water_active(water) && engine && render_mode == RENDER_MODE_PBR &&
           !engine->capturing;
}

// One indexed lattice over [-0.5, 0.5]^2, positions only. The vertex stage reads it as
// SCREEN space and casts a ray through each point onto the water plane, so this buffer
// depends on neither the resolution nor the extent and is built once. No normals: a
// displaced surface's normal is the derivative of the displacement, and a stored one would
// just be overwritten.
static bool water_ensure_grid(Water* water) {
    if (water->grid_vao)
        return true;
    if (water->failed)
        return false;

    const int res = WATER_GRID_RES;
    const int verts_per_side = res + 1;
    const int vert_count = verts_per_side * verts_per_side;
    const int index_count = res * res * 6;

    float* verts = malloc((size_t)vert_count * 2 * sizeof(float));
    unsigned* indices = malloc((size_t)index_count * sizeof(unsigned));
    if (!verts || !indices) {
        log_error("Water grid allocation failed; disabling water");
        free(verts);
        free(indices);
        water->failed = true;
        return false;
    }

    for (int z = 0; z < verts_per_side; z++) {
        for (int x = 0; x < verts_per_side; x++) {
            const int i = (z * verts_per_side + x) * 2;
            verts[i + 0] = (float)x / (float)res - 0.5f;
            verts[i + 1] = (float)z / (float)res - 0.5f;
        }
    }
    /*
     * WINDING, and it is load-bearing rather than cosmetic.
     *
     * Culling is off, but water_frag flips its normal on !gl_FrontFacing -- so a
     * consistently BACK-facing surface is shaded upside down everywhere, which reads as a
     * plausible-looking sea whose reflection comes out of the environment's lower
     * hemisphere. The clipmap this replaced had the opposite winding for the same index
     * order, because a lattice in NDC maps STRAIGHT to window coordinates (x right, y up)
     * where a world-XZ grid maps through the view: increasing Z runs toward a camera
     * looking down -Z, so its screen orientation is mirrored. Same indices, opposite
     * face.
     */
    int w = 0;
    for (int z = 0; z < res; z++) {
        for (int x = 0; x < res; x++) {
            const unsigned a = (unsigned)(z * verts_per_side + x);
            const unsigned b = a + 1;
            const unsigned c = a + (unsigned)verts_per_side;
            const unsigned d = c + 1;
            const unsigned quad[6] = {a, b, c, b, d, c};
            for (int k = 0; k < 6; k++)
                indices[w++] = quad[k];
        }
    }

    glGenVertexArrays(1, &water->grid_vao);
    glBindVertexArray(water->grid_vao);
    glGenBuffers(1, &water->grid_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, water->grid_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)vert_count * 2 * sizeof(float), verts,
                 GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glGenBuffers(1, &water->grid_ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, water->grid_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)index_count * sizeof(unsigned),
                 indices, GL_STATIC_DRAW);
    glBindVertexArray(0);

    free(verts);
    free(indices);
    water->grid_index_count = index_count;
    log_info("Water: projected grid %dx%d, %d triangles, level %.2f, bed extent %.1f", res,
             res, index_count / 3, (double)water->level, (double)water->extent);
    return true;
}

/*
 * One transformed cascade field.
 *
 * `mipped` is for the bands that DISPLACE the mesh, and it is what makes the far field
 * possible (spec 11.35): a projected grid's distant cells cover more than a wave period, so
 * level 0 there is one arbitrary phase per cell rather than detail. The mip chain is
 * generated after each frame's transform, and the vertex stage selects a level from the
 * cell's world footprint. Costs no sampler unit -- these are textures water already binds.
 *
 * The short band is NOT mipped: it never reaches the mesh, and its own energy already
 * leaves through the distance fade in water_frag.
 */
static GLuint _water_make_field(int size, bool mipped) {
    GLuint tex = 0;
    // Unit 0 explicitly: these run lazily on whatever unit the previous pass left
    // active, and the trailing unbind would otherwise clear that unit's 2D slot.
    glActiveTexture(GL_TEXTURE0);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    // RGBA16F is enough for a transformed field and halves the bandwidth of 45
    // passes. The SOURCE data stays fp32 (below): a spectrum spans decades, and
    // quantising it before the transform quantises the sea state itself.
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, size, size, 0, GL_RGBA, GL_FLOAT, NULL);
    // LINEAR + REPEAT: the surface samples these as tiling world-space fields.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    mipped ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    // Allocate the chain now rather than on the first generate, so the texture is complete
    // from the moment it is bound -- an incomplete mipmapped texture samples as black, and
    // it would do so on whichever frame happened to read it first.
    if (mipped)
        glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

// Which cascades displace the mesh, and therefore need a mip chain. The same count the
// retained previous fields use: only a band that reaches the mesh has a position to filter
// or to have moved.
static bool _water_cascade_mipped(int cascade) {
    return cascade < WATER_PREV_CASCADES;
}

static GLuint _water_make_data_tex(int w, int h, const float* data) {
    GLuint tex = 0;
    // Unit 0 explicitly: these run lazily on whatever unit the previous pass left
    // active, and the trailing unbind would otherwise clear that unit's 2D slot.
    glActiveTexture(GL_TEXTURE0);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, w, h, 0, GL_RGBA, GL_FLOAT, data);
    // NEAREST and CLAMP throughout: every read of these is an exact texelFetch by
    // integer index, and a filtered twiddle index would be a different butterfly.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

/*
 * The direction the waves travel, as the angle the spectral seeding wants.
 *
 * Derived from wind_dir rather than authored beside it, so the two models cannot describe
 * seas running different ways -- which is exactly what they did until spec 11.42, when
 * this was a private constant and a scene's windDirection reached the Gerstner train only.
 */
static float _water_wind_angle(const Water* water) {
    return atan2f(water->wind_dir[1], water->wind_dir[0]);
}

// Everything the seeding reads, and nothing else. Compared by value rather than by a
// dirty flag: a flag has to be set by every writer, and the writers are a scene file, the
// CLI and the GUI.
static bool _water_seed_is_current(const Water* water) {
    const WaterSeaState* a = &water->sea;
    const WaterSeaState* b = &water->seeded_sea;
    return a->wind_speed == b->wind_speed && a->fetch == b->fetch &&
           a->sea_depth == b->sea_depth && a->peak_enhancement == b->peak_enhancement &&
           a->swell == b->swell && water->wind_dir[0] == water->seeded_wind_dir[0] &&
           water->wind_dir[1] == water->seeded_wind_dir[1];
}

static void _water_record_seed(Water* water) {
    water->seeded_sea = water->sea;
    glm_vec2_copy(water->wind_dir, water->seeded_wind_dir);
}

/*
 * Build the two seed textures, and the cascade variances with them, from the current
 * sea state. The first build and every later re-seed run this same body.
 *
 * Only those two textures: the transformed fields, their framebuffers and the twiddle
 * table are functions of the RESOLUTION alone, so a re-seed touches none of them and the
 * ping-pong keeps running through the change. Deleting and recreating rather than
 * sub-uploading, because that reuses the one texture-creation policy this file already
 * states -- and glDeleteTextures on a zero handle is defined as a no-op, which is what
 * lets the first build take the same path.
 *
 * A failure leaves the previous spectrum in place. That is the conservative direction:
 * the alternative is a half-written seed, which transforms into a plausible-looking sea
 * that is not the one asked for. The caller decides what a failure MEANS -- fatal on the
 * first build, survivable on a re-seed -- so this one only reports which cascade.
 */
static bool _water_seed_cascades(Water* water) {
    const int size = WATER_SPECTRUM_RES;
    float* initial = calloc((size_t)size * size * 4, sizeof(float));
    float* wave = calloc((size_t)size * size * 4, sizeof(float));
    if (!initial || !wave) {
        log_error("Water spectrum allocation failed");
        free(initial);
        free(wave);
        return false;
    }
    const float wind_angle = _water_wind_angle(water);
    for (int c = 0; c < WATER_CASCADE_COUNT; c++) {
        if (!_water_build_spectrum(size, &WATER_CASCADE_CFG[c], &water->sea, wind_angle, initial,
                                   wave, &water->cascade_height_var[c],
                                   &water->cascade_slope_var[c])) {
            log_error("Water cascade %d seeding failed", c);
            free(initial);
            free(wave);
            return false;
        }
        glDeleteTextures(1, &water->cascade_initial[c]);
        glDeleteTextures(1, &water->cascade_wave[c]);
        water->cascade_initial[c] = _water_make_data_tex(size, size, initial);
        water->cascade_wave[c] = _water_make_data_tex(size, size, wave);
    }
    free(initial);
    free(wave);
    _water_record_seed(water);
    float carried = 0.0f;
    for (int c = 0; c < WATER_CASCADE_COUNT; c++)
        carried += water->cascade_slope_var[c];
    log_info("Water: seeded sea Hs %.2f m, Tp %.1f s, slope var %.4f of Cox-Munk %.4f",
             (double)_water_significant_height(water),
             (double)(6.28318530718f / _water_peak_omega(&water->sea)), (double)carried,
             (double)(0.003f + 0.00512f * water->sea.wind_speed));
    return true;
}

// A re-seed of a running sea: the seed textures change under an unchanged ping-pong, so
// the two histories that describe the OLD sea have to go with it.
static bool _water_reseed(Water* water) {
    if (!_water_seed_cascades(water)) {
        log_error("Water re-seed failed; keeping the previous sea state");
        return false;
    }
    /*
     * Both histories describe the OLD sea and have to go with it.
     *
     * cascade_prev holds last frame's displacement, so a frame that kept it would report a
     * velocity between two unrelated oceans -- a full-surface smear through TAA and motion
     * blur. Zeroing the counter reuses the startup path exactly: two frames of no reported
     * wave motion, which is already the known-good state on frame one.
     *
     * The foam accumulator is the same argument: it is a running minimum of a Jacobian the
     * new spectrum never folded.
     */
    water->spectral_frames = 0;
    water->foam_frames = 0;
    log_info("Water: re-seeded at wind %.1f m/s, fetch %.0f m, depth %.0f m",
             (double)water->sea.wind_speed, (double)water->sea.fetch,
             (double)water->sea.sea_depth);
    return true;
}

static bool _water_ensure_spectra(Water* water) {
    if (water->spectra_ready)
        return _water_seed_is_current(water) ? true : _water_reseed(water);
    if (water->failed)
        return false;

    const int size = WATER_SPECTRUM_RES;
    if (!_water_seed_cascades(water)) {
        log_error("Water spectrum seeding failed; disabling water");
        water->failed = true;
        return false;
    }

    float* twiddle = calloc((size_t)size * WATER_SPECTRUM_LOG * 4, sizeof(float));
    if (!twiddle) {
        log_error("Water twiddle allocation failed; disabling water");
        water->failed = true;
        return false;
    }
    _water_build_twiddle(size, twiddle);
    water->twiddle_tex = _water_make_data_tex(size, WATER_SPECTRUM_LOG, twiddle);
    free(twiddle);

    // Restored on every exit past this point, including the failure ones. Binding 0 and
    // returning would leave the WINDOW framebuffer current, and every pass after water --
    // the transparent lane, the OIT accumulate, the particle depth resolve --
    // would draw into it instead of the scene target.
    GLint saved_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &saved_fbo);

    for (int c = 0; c < WATER_CASCADE_COUNT; c++) {
        for (int b = 0; b < 2; b++) {
            for (int t = 0; t < 2; t++)
                water->cascade_field[c][b][t] = _water_make_field(size, _water_cascade_mipped(c));

            glGenFramebuffers(1, &water->cascade_fbo[c][b]);
            glBindFramebuffer(GL_FRAMEBUFFER, water->cascade_fbo[c][b]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                   water->cascade_field[c][b][0], 0);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D,
                                   water->cascade_field[c][b][1], 0);
            const GLenum targets[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
            glDrawBuffers(2, targets);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                log_error("Water spectral framebuffer incomplete; disabling water");
                glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)saved_fbo);
                water->failed = true;
                return false;
            }
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)saved_fbo);

    // Same format and filtering as the fields they hold a copy of, because that is
    // exactly what they are -- mip chain included, since the vertex stage has to read a
    // previous position at the SAME level as the current one or the velocity describes a
    // surface that was never drawn. No framebuffer: they are only ever written by a copy
    // out of one of the buffers above.
    for (int c = 0; c < WATER_PREV_CASCADES; c++)
        water->cascade_prev[c] = _water_make_field(size, true);

    // The foam pair. No mip chain: it is sampled at LOD 0 wherever the surface reads it,
    // and a mip of a running minimum is not the running minimum of a mip.
    for (int b = 0; b < 2; b++) {
        water->foam_tex[b] = _water_make_field(size, false);
        glGenFramebuffers(1, &water->foam_fbo[b]);
        glBindFramebuffer(GL_FRAMEBUFFER, water->foam_fbo[b]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               water->foam_tex[b], 0);
        const GLenum foam_target = GL_COLOR_ATTACHMENT0;
        glDrawBuffers(1, &foam_target);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            log_error("Water foam framebuffer incomplete; disabling water");
            glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)saved_fbo);
            water->failed = true;
            return false;
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)saved_fbo);

    create_fullscreen_quad_vao(&water->fft_vao, &water->fft_vbo);
    water->spectra_ready = true;
    log_info("Water: %d spectral cascades at %d^2, %d passes/frame", WATER_CASCADE_COUNT, size,
             WATER_CASCADE_COUNT * (1 + WATER_SPECTRUM_LOG * 2));
    return true;
}

/*
 * Advance every cascade one frame: evolve the spectrum, then transform it.
 *
 * No barriers and no read-write hazard anywhere: the evolve writes buffer 0 while
 * reading only the immutable seed textures, and each FFT stage reads one buffer
 * and writes the other. GL 4.1 has no texture barrier, and this needs none --
 * which is the same structure postfx.c's froxel inject/integrate pair relies on.
 *
 * 14 stages, so the final result lands back in buffer 0 (stage 13 reads 1, writes
 * 0). The surface shader therefore always samples buffer 0 and needs no parity.
 */
/*
 * One complete inverse transform over a ping-pong pair, in place.
 *
 * 2 * WATER_SPECTRUM_LOG stages, so the result lands back in buffer 0 and no caller needs a
 * parity. The caller supplies the pair and has already bound the fullscreen quad.
 *
 * SHARED with the impulse probe, and that is the whole point rather than tidiness. The
 * probe's claim is that it exercises the transform the sea runs; a second copy of this loop
 * would make that claim true of the shader and false of the SCHEDULE, so reordering the
 * stages here would leave the probe testing the old order and still reporting near-zero
 * error -- the same shape of green-over-wrong the probe exists to catch.
 */
static void _water_fft_transform(ShaderProgram* fft, GLuint twiddle, const GLuint fbo[2],
                                 const GLuint field[2][2]) {
    glUseProgram(fft->id);
    uniform_set_int(fft->uniforms, "twiddleTex", 0);
    uniform_set_int(fft->uniforms, "in0", 1);
    uniform_set_int(fft->uniforms, "in1", 2);
    uniform_set_int(fft->uniforms, "size", WATER_SPECTRUM_RES);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, twiddle);

    for (int pass = 0; pass < WATER_SPECTRUM_LOG * 2; pass++) {
        const int src = pass % 2;
        const int dst = 1 - src;
        glBindFramebuffer(GL_FRAMEBUFFER, fbo[dst]);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, field[src][0]);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, field[src][1]);
        uniform_set_int(fft->uniforms, "axis", pass < WATER_SPECTRUM_LOG ? 0 : 1);
        uniform_set_int(fft->uniforms, "stage", pass % WATER_SPECTRUM_LOG);
        // The fftshift folds into the LAST stage as a checkerboard sign, which
        // is why it is a uniform rather than a separate pass.
        uniform_set_int(fft->uniforms, "finalize", pass == WATER_SPECTRUM_LOG * 2 - 1 ? 1 : 0);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
}

/*
 * The six transformed fields on WATER_CASCADE_UNIT0.., plus the per-band choppiness.
 *
 * Everything a consumer of ocean.glsl's cascade group needs, and nothing that belongs to
 * only one of them: `cascadeLength` and `cascadeSlopeVar` are the surface's alone, since the
 * foam pass works in texel space and selects folds rather than shading them.
 *
 * `fft` false points the units at texture 0 rather than skipping them. A sampler left at its
 * default is a type mismatch against whatever 2D texture happens to occupy that unit, not an
 * unused binding.
 */
/*
 * How many world units make a metre, for every constant in the ocean that is a physical
 * length (spec 11.44).
 *
 * Taken from the SKY, which already owns this fact and already spends it on the atmosphere.
 * A second copy on Water would be a second answer to one question about the world, and an
 * ocean disagreeing with its own sky about the size of a metre is the defect this fixes
 * rather than a shape it should take.
 *
 * Falls back to the glTF convention when there is no sky, on the same reasoning sky.c's own
 * default rests on: 1 unit is a metre until something says otherwise. Never zero -- the
 * shoal window divides by this.
 */
static float _water_units_per_metre(const struct Scene* scene) {
    const float per_km =
        (scene && scene->sky && scene->sky->world_units_per_km > 0.0f)
            ? scene->sky->world_units_per_km
            : 1000.0f;
    return per_km / 1000.0f;
}

static void _water_set_units_per_metre(UniformManager* u, const struct Scene* scene) {
    uniform_set_float(u, "waterUnitsPerMetre", _water_units_per_metre(scene));
}

static void _water_bind_cascades(const Water* water, UniformManager* u, bool fft) {
    for (int c = 0; c < WATER_CASCADE_COUNT; c++) {
        for (int t = 0; t < 2; t++) {
            const int unit = WATER_CASCADE_UNIT0 + c * 2 + t;
            char name[32];
            snprintf(name, sizeof(name), "cascade%d_%d", c, t);
            glActiveTexture(GL_TEXTURE0 + unit);
            glBindTexture(GL_TEXTURE_2D, fft ? water->cascade_field[c][0][t] : 0);
            uniform_set_int(u, name, unit);
        }
        char chop[32];
        snprintf(chop, sizeof(chop), "cascadeChoppiness[%d]", c);
        uniform_set_float(u, chop, WATER_CASCADE_CFG[c].choppiness);
    }
}

static void _water_run_spectral(Water* water, const struct Scene* scene, struct Engine* engine,
                                float time) {
    ShaderProgram* evolve = get_engine_shader_program_by_name(engine, "water_spectrum");
    ShaderProgram* fft = get_engine_shader_program_by_name(engine, "water_fft");
    if (!evolve || !fft) {
        log_error("Water spectral programs missing; disabling water");
        water->failed = true;
        return;
    }

    GLint saved_viewport[4];
    GLint saved_fbo = 0;
    glGetIntegerv(GL_VIEWPORT, saved_viewport);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &saved_fbo);
    const GLboolean depth_was_enabled = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean blend_was_enabled = glIsEnabled(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glViewport(0, 0, WATER_SPECTRUM_RES, WATER_SPECTRUM_RES);

    // Keep the frame that is about to be overwritten. Buffer 0 holds the completed
    // transform of the last frame -- 14 stages land back where they started -- and the
    // evolve below is the first thing that writes over it, so this is the only moment
    // the previous surface still exists.
    //
    // A copy rather than a third ping-pong buffer: the alternative is a parity the
    // surface shader would have to know about, which is state in the wrong place for
    // one texture fetch's worth of saving.
    if (water->spectral_frames > 0) {
        glActiveTexture(GL_TEXTURE0);
        for (int c = 0; c < WATER_PREV_CASCADES; c++) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, water->cascade_fbo[c][0]);
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            glBindTexture(GL_TEXTURE_2D, water->cascade_prev[c]);
            glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, WATER_SPECTRUM_RES,
                                WATER_SPECTRUM_RES);
            // The copy writes level 0 only, so the chain under it is last frame's -- which
            // is a level mismatch against level 0 rather than a stale frame, and shows up
            // as far-field velocity that disagrees with the near field.
            glGenerateMipmap(GL_TEXTURE_2D);
        }
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    glBindVertexArray(water->fft_vao);

    for (int c = 0; c < WATER_CASCADE_COUNT; c++) {
        glUseProgram(evolve->id);
        glBindFramebuffer(GL_FRAMEBUFFER, water->cascade_fbo[c][0]);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, water->cascade_initial[c]);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, water->cascade_wave[c]);
        uniform_set_int(evolve->uniforms, "initialSpectrum", 0);
        uniform_set_int(evolve->uniforms, "waveData", 1);
        uniform_set_float(evolve->uniforms, "time", time);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        _water_fft_transform(fft, water->twiddle_tex, water->cascade_fbo[c],
                             water->cascade_field[c]);
    }

    glBindVertexArray(0);

    // Mip chains for the bands that displace, AFTER the ping-pong: these textures are the
    // render targets it writes, so generating earlier would filter a half-transformed field.
    // 14 stages land back in buffer 0, which is why buffer 0 is the one that gets a chain --
    // the same reason the previous-frame copy above reads it.
    glActiveTexture(GL_TEXTURE0);
    for (int c = 0; c < WATER_CASCADE_COUNT; c++) {
        if (!_water_cascade_mipped(c))
            continue;
        for (int t = 0; t < 2; t++) {
            glBindTexture(GL_TEXTURE_2D, water->cascade_field[c][0][t]);
            glGenerateMipmap(GL_TEXTURE_2D);
        }
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    /*
     * Foam accumulation, one pass, after the transform it reads and after the mips.
     *
     * Reads the pair's current texel and writes the other, so the read/write separation the
     * FFT ping-pong relies on holds here too -- GL 4.1 has no texture barrier and this
     * needs none.
     *
     * Costs one draw on top of 45. The submission budget is the real price of this whole
     * chain (spec 11.32 measured +0.24 ms GPU against +1.19 ms CPU for the 45), which is
     * why the three bands share one target instead of taking one pass each.
     */
    ShaderProgram* foam =
        water->foam_history ? get_engine_shader_program_by_name(engine, "water_foam") : NULL;
    // A missing program is reported and the feature switched off, not shrugged through a
    // truthiness test every frame: this file's policy is stated at the evolve/fft lookups
    // above, and the failure it guards against is the same one -- a sea that renders
    // plausibly while silently missing what was asked for, with a green gate over it.
    // Clearing the flag is what makes it one log line rather than one per frame.
    if (water->foam_history && !foam) {
        log_error("Water: foam program missing; foam history disabled");
        water->foam_history = false;
    }
    if (foam) {
        const int src = water->foam_index;
        const int dst = 1 - src;
        glUseProgram(foam->id);
        glBindFramebuffer(GL_FRAMEBUFFER, water->foam_fbo[dst]);
        glBindVertexArray(water->fft_vao);
        _water_bind_cascades(water, foam->uniforms, true);
        glActiveTexture(GL_TEXTURE0 + WATER_FOAM_UNIT);
        glBindTexture(GL_TEXTURE_2D, water->foam_tex[src]);
        uniform_set_int(foam->uniforms, "prevFoam", WATER_FOAM_UNIT);
        uniform_set_int(foam->uniforms, "foamHistoryAvailable", water->foam_frames > 0 ? 1 : 0);
        _water_set_units_per_metre(foam->uniforms, scene);
        /*
         * The drift backtrace needs the wind and each band's tiling period, which this pass
         * did not read until the foam started being advected (spec 11.44) -- it worked in
         * texel space and needed neither.
         *
         * In world units, like the surface's own copy: the backtrace divides a world-space
         * travel by them to get a UV step.
         */
        uniform_set_vec2(foam->uniforms, "waterWindDir", (const float*)&water->wind_dir);
        const float foam_units_per_metre = _water_units_per_metre(scene);
        for (int c = 0; c < WATER_CASCADE_COUNT; c++) {
            char len[32];
            snprintf(len, sizeof(len), "cascadeLength[%d]", c);
            uniform_set_float(foam->uniforms, len,
                              WATER_CASCADE_CFG[c].length_scale * foam_units_per_metre);
        }
        uniform_set_float(foam->uniforms, "foamDriftSpeed", water->foam_drift);
        uniform_set_float(foam->uniforms, "foamDt", (float)engine->render_delta);
        uniform_set_float(foam->uniforms, "foamDecay", water->foam_decay);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray(0);
        water->foam_index = dst;
        water->foam_frames++;
    }

    water->spectral_frames++;
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)saved_fbo);
    glViewport(saved_viewport[0], saved_viewport[1], saved_viewport[2], saved_viewport[3]);
    if (depth_was_enabled)
        glEnable(GL_DEPTH_TEST);
    if (blend_was_enabled)
        glEnable(GL_BLEND);
    glActiveTexture(GL_TEXTURE0);
    check_gl_error("water spectral");
}

void water_render(Water* water, struct Scene* scene, struct Engine* engine, const mat4 view,
                  const mat4 draw_projection) {
    if (!water_active(water) || !scene || !engine)
        return;
    if (!water_ensure_grid(water))
        return;

    _water_bake_bed(water, _water_units_per_metre(scene));

    // The spectral bands, before anything that samples them. No profiler scope of
    // its own: it would have to nest inside the caller's, and the simulation is
    // part of what water costs anyway.
    const bool fft = water->wave_model == WATER_WAVES_FFT;
    if (fft) {
        if (!_water_ensure_spectra(water))
            return;
        _water_run_spectral(water, scene, engine, (float)engine->render_time);
    }

    ShaderProgram* program = get_engine_shader_program_by_name(engine, "water");
    if (!program) {
        log_error("Water program missing; disabling water");
        water->failed = true;
        return;
    }

    // The surface's own transmission source. Both resolves re-bind the scene
    // framebuffer on the way out, so this is safe here and the pass below draws
    // into the scene target as usual.
    //
    // Held LOCAL, never published to engine->scene_color_this_frame. That flag is
    // the one place engine->refraction_enabled is enforced, and the transmissive
    // lane reads it downstream -- setting it here would refract glass that
    // --no-refraction had switched off, silently.
    bool scene_color = engine->scene_color_this_frame;
    if (!scene_color)
        scene_color = engine_resolve_opaque_color(engine);
    const GLuint scene_depth = engine_resolve_scene_depth(engine);

    glUseProgram(program->id);
    UniformManager* u = program->uniforms;

    uniform_set_mat4(u, "view", (const float*)view);
    uniform_set_mat4(u, "projection", (const float*)draw_projection);
    // Motion vectors come from the UN-JITTERED pair while the raster above uses
    // the jittered projection: the jitter is a sub-pixel sampling offset, and
    // letting it into the velocity would report it as scene motion.
    uniform_set_mat4(u, "uCurrViewProjNoJitter", (const float*)engine->view_proj);
    uniform_set_mat4(u, "uPrevViewProj", (const float*)engine->prev_view_proj);

    uniform_set_float(u, "waterLevel", water->level);
    uniform_set_float(u, "waterExtent", water->extent);
    _water_set_units_per_metre(u, scene);
    // The projector's origin. Every lattice vertex is a ray from here through its own
    // screen position, so this is the surface's whole placement input.
    uniform_set_vec3(u, "waterCamPos", (const float*)&engine->camera->position);
    // The lattice's own resolution, which is how the vertex stage sizes a cell, and the
    // cascades', which is how a cell size becomes a mip level.
    uniform_set_int(u, "waterGridRes", WATER_GRID_RES);
    uniform_set_float(u, "cascadeRes", (float)WATER_SPECTRUM_RES);
    uniform_set_int(u, "waterFarLod", water->far_lod ? 1 : 0);
    uniform_set_float(u, "waterRoughness", water->roughness);
    uniform_set_float(u, "waterIor", water->ior);
    uniform_set_vec3(u, "waterAbsorption", (const float*)&water->absorption);
    uniform_set_vec3(u, "waterScatter", (const float*)&water->scatter);
    uniform_set_vec2(u, "waterWindDir", (const float*)&water->wind_dir);
    uniform_set_float(u, "waterAmplitude", water->amplitude);
    uniform_set_float(u, "waterWavelength", water->wavelength);
    uniform_set_float(u, "waterSteepness", water_effective_steepness(water));
    uniform_set_float(u, "waterSpread", water->spread);
    // The animation clock, not the wall clock: frame N must be phase N or a
    // headless run stops being comparable to itself.
    uniform_set_float(u, "time", (float)engine->render_time);
    // The advance of the SAME clock `time` came from, so the previous-frame
    // surface is one step back rather than one wall-clock tick back.
    uniform_set_float(u, "uDeltaTime", (float)engine->render_delta);

    int rw, rh;
    engine_render_size(engine, &rw, &rh);
    uniform_set_vec2(u, "screenSize", (vec2){(float)rw, (float)rh});

    // The sun, for caustics: light focusing is a property of the path from the SUN
    // through the surface, so it needs the direction light arrives from rather than
    // anything about the view.
    //
    // The SKY's sun first, and that is not a preference (spec 11.41). Scanning for the
    // first directional finds whichever light the scene file happened to list first,
    // which in assets/water_fixture.cscn is a non-shadowing key -- so the caustics were
    // focused from one light while the deck above occluded a different one. Falls back
    // to the scan for a scene with no sky.
    vec3 sun_dir = {0.0f, 1.0f, 0.0f};
    const Light* sun = (scene->sky && scene->sky->sun_light) ? scene->sky->sun_light : NULL;
    for (size_t i = 0; !sun && i < scene->light_count; i++) {
        if (scene->lights[i] && scene->lights[i]->type == LIGHT_DIRECTIONAL)
            sun = scene->lights[i];
    }
    // Scene radiance, un-pre-exposed, exactly as the clustered UBO packs it for every
    // other surface -- and the sky has already folded atmospheric transmittance into the
    // colour (sky_apply_sun_to_light), so this one vector is the sun's magnitude AND its
    // reddening near the horizon. The shader multiplies by preExposure; see view.glsl.
    vec3 sun_radiance = {0.0f, 0.0f, 0.0f};
    // Which cascade the deck occludes, for the glitter's shadow. -1 covers a sun that
    // casts nothing, which is also the state cloud_shadow.glsl reads as "no deck".
    if (sun) {
        // Lights store the direction they SHINE; the shader wants the direction
        // toward the source.
        glm_vec3_negate_to((float*)sun->direction, sun_dir);
        glm_vec3_normalize(sun_dir);
        glm_vec3_scale((float*)sun->color, sun->intensity, sun_radiance);
    }
    uniform_set_vec3(u, "sunDir", (const float*)&sun_dir);
    uniform_set_int(u, "sunAvailable", sun ? 1 : 0);
    uniform_set_vec3(u, "sunRadiance", (const float*)&sun_radiance);

    /*
     * The cascades, for the glitter's shadow. csm.glsl's CSM_OUTERMOST_PCF path reads only
     * the widest cascade, which is scene-fitted and camera-independent -- the right lookup
     * for a surface that runs to the horizon, and the same one the catcher and the particle
     * motes take.
     *
     * The slot is published FROM the binder's own answer rather than from the light beside
     * it. The two were separate expressions and could disagree: the shader gates only on
     * the slot, so switching shadows off mid-session -- which leaves the sun holding a
     * stale shadow_map_index, since only the depth pass clears it and that pass is itself
     * gated on `enabled` -- took the lookup against an unbound array and a never-uploaded
     * cascadeCount, read full occlusion, and the glitter vanished from the whole sea.
     */
    const bool shadows = bind_outermost_cascades_to_program(scene->shadow_system, program,
                                                            WATER_SHADOW_UNIT) &&
                         sun && sun->cast_shadows && sun->shadow_map_index >= 0;
    uniform_set_int(u, "sunShadowSlot", shadows ? sun->shadow_map_index : -1);
    uniform_set_int(u, "causticsEnabled", water->caustics ? 1 : 0);
    uniform_set_int(u, "glitterEnabled", water->glitter ? 1 : 0);
    // The deck dims the caustics it focuses (spec 11.41) and the sun lobe it lights
    // (spec 11.42). Not the reflection, which is an environment lookup already carrying it.
    sky_bind_cloud_shadow(scene->sky, program, SKY_CLOUD_SHADOW_UNIT);

    // Which side of the surface the eye is on. Compared against the still level
    // rather than the displaced surface: a camera within a wave height of the
    // waterline would otherwise flip models several times a second as crests pass
    // it, and every temporal history in the frame would reset each time.
    vec3 cam_world;
    glm_vec3_copy(engine->camera->position, cam_world);
    uniform_set_int(u, "cameraSubmerged", cam_world[1] < water->level ? 1 : 0);

    // The shoreline's coverage needs samples to be dithered into, and the same
    // two conditions the opaque lane's masked materials read decide whether
    // there are any: the sample count the APP asked for -- the right question
    // even now that a 1-sample request is honoured (spec 11.34), since this asks
    // which AA mode was chosen -- and whether a capture is bound. Capture
    // targets are always single-sample, so reading the scene target's count
    // alone would spend coverage against no coverage hardware and write the
    // shoreline sliver at full strength into the capture.
    const bool a2c = water->shore_coverage && engine->msaa_samples > 1 && !engine->capturing;
    uniform_set_int(u, "alphaToCoverage", a2c ? 1 : 0);

    glActiveTexture(GL_TEXTURE0 + TEXUNIT_SCENE_COLOR);
    glBindTexture(GL_TEXTURE_2D, engine->opaque_color_texture);
    uniform_set_int(u, "sceneColorTex", TEXUNIT_SCENE_COLOR);
    uniform_set_int(u, "sceneColorAvailable", scene_color ? 1 : 0);

    glActiveTexture(GL_TEXTURE0 + WATER_DEPTH_UNIT);
    glBindTexture(GL_TEXTURE_2D, scene_depth);
    uniform_set_int(u, "sceneDepthTex", WATER_DEPTH_UNIT);
    uniform_set_int(u, "sceneDepthAvailable", scene_depth ? 1 : 0);

    // The baked bed, for vertex-stage shoaling. Absent is the normal case and not
    // a degraded one: the per-fragment water column still comes from the depth
    // resolve above, which is exact and works against geometry a heightfield
    // cannot describe. What the bed buys is the one thing screen depth cannot
    // answer in the vertex stage -- how much to shorten a wave that is running out
    // of water underneath it.
    glActiveTexture(GL_TEXTURE0 + WATER_BED_UNIT);
    glBindTexture(GL_TEXTURE_2D, water->bed_tex);
    uniform_set_int(u, "bedTex", WATER_BED_UNIT);
    uniform_set_int(u, "bedAvailable", water->bed_tex ? 1 : 0);

    // The split-sum BRDF table is engine-owned and bound for every scene,
    // environment or not, so the Fresnel lobe's lookup is always valid.
    glActiveTexture(GL_TEXTURE0 + IBL_BRDF_LUT_TEXTURE_UNIT);
    glBindTexture(GL_TEXTURE_2D, engine->brdf_lut);
    uniform_set_int(u, "brdfLUT", IBL_BRDF_LUT_TEXTURE_UNIT);

    // The environment reflection: the same split-sum lookup every other material
    // makes, and it follows the sun for free because the procedural sky bakes
    // into this cubemap.
    if (scene->ibl && scene->ibl->precomputed) {
        bind_ibl_textures(scene->ibl, program);
    } else {
        // The samplerCube still has to be POINTED at its unit even with nothing
        // to sample. Left at the default 0, where a 2D texture lives, it is a
        // sampler type mismatch -- undefined for the whole program, not just for
        // the branch that reads it.
        uniform_set_int(u, "prefilteredMap", IBL_PREFILTER_TEXTURE_UNIT);
        uniform_set_int(u, "iblEnabled", 0);
        uniform_set_float(u, "iblIntensity", 1.0f);
        uniform_set_float(u, "maxReflectionLOD", 0.0f);
    }

    // The transformed cascades. Bound whichever model is running, and the units
    // are pointed at unconditionally for the same reason the samplerCube above
    // is: a sampler left at the default 0 is a type mismatch against whatever 2D
    // texture happens to live there.
    uniform_set_int(u, "waveModel", fft ? 1 : 0);
    _water_bind_cascades(water, u, fft);
    // The two the foam pass does not read: it works in cascade texel space, so it needs no
    // tiling period, and it selects folds rather than shading them, so it needs no variance.
    const float units_per_metre = _water_units_per_metre(scene);
    for (int c = 0; c < WATER_CASCADE_COUNT; c++) {
        char len[32], svar[32];
        snprintf(len, sizeof(len), "cascadeLength[%d]", c);
        snprintf(svar, sizeof(svar), "cascadeSlopeVar[%d]", c);
        // In WORLD UNITS, where the table holds metres (spec 11.44). The tile a band repeats
        // over is a physical length -- 240 m of swell -- and uploading it raw made the sea
        // repeat every 240 UNITS, which in a world at 22 units to the metre is a swell
        // pattern restarting every eleven metres. The seeding stays in metres, where its
        // gravity and its wind speed already are.
        uniform_set_float(u, len, WATER_CASCADE_CFG[c].length_scale * units_per_metre);
        // Zero on the Gerstner path, whose octaves report the slope they dropped directly
        // -- it has no seeded spectrum to have measured, and a stale variance from a
        // previous spectral scene would widen its lobe for waves it never carried.
        uniform_set_float(u, svar, fft ? water->cascade_slope_var[c] : 0.0f);
    }

    /*
     * The sea state the surf runs at: significant height in metres and peak frequency.
     *
     * Spectral: from the seeded spectrum -- Hs is 4 sigma, and the variance is the sum over
     * the cascades since they own disjoint wavenumber windows. Gerstner: from the authored
     * train, whose longest octave has crest-to-trough 2A and deep-water frequency sqrt(gk).
     * Its amplitude and wavelength are WORLD units, so both convert; the Gerstner path's own
     * dispersion runs g against world-unit k and is not corrected here, since that is the
     * train's look and this is the surf's.
     */
    if (water->surf) {
        float hs, omega;
        if (fft) {
            hs = _water_significant_height(water);
            omega = _water_peak_omega(&water->sea);
        } else {
            hs = 2.0f * water->amplitude / units_per_metre;
            const float k = 6.28318530718f / fmaxf(water->wavelength / units_per_metre, 0.01f);
            omega = sqrtf(9.81f * k);
        }
        uniform_set_float(u, "waterSurfHeight", hs);
        uniform_set_float(u, "waterSurfOmega", omega);
    } else {
        uniform_set_float(u, "waterSurfHeight", 0.0f);
        uniform_set_float(u, "waterSurfOmega", 1.0f);
    }
    uniform_set_float(u, "waterBeachSlope", water->bed_foreshore_slope);

    // Last frame's displacement, for the spectral path's motion vectors. Only usable
    // from the third frame on: the first has nothing to copy from and the second holds
    // a copy of the first, so the count has to reach 2 before this is a real previous
    // surface rather than the current one under a different name.
    const bool prev_ready = fft && water->spectral_frames >= 2;
    for (int c = 0; c < WATER_PREV_CASCADES; c++) {
        const int unit = WATER_PREV_UNIT0 + c;
        char name[32];
        snprintf(name, sizeof(name), "cascadePrev%d", c);
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, prev_ready ? water->cascade_prev[c] : 0);
        uniform_set_int(u, name, unit);
    }
    uniform_set_int(u, "prevAvailable", prev_ready ? 1 : 0);

    // The accumulated foam. Live only once the pass has actually run a frame: before that
    // the pair holds whatever the allocation left, and reading it as whitewater would put
    // the sea under foam on exactly the frames a capture is most likely to take.
    const bool foam_ready = fft && water->foam_history && water->foam_frames > 0;
    glActiveTexture(GL_TEXTURE0 + WATER_FOAM_UNIT);
    glBindTexture(GL_TEXTURE_2D, foam_ready ? water->foam_tex[water->foam_index] : 0);
    uniform_set_int(u, "foamTex", WATER_FOAM_UNIT);
    uniform_set_int(u, "foamAvailable", foam_ready ? 1 : 0);

    glActiveTexture(GL_TEXTURE0);

    // Depth writes ON, unlike the skybox and the late pass. The particle depth
    // resolve and everything that sorts against the surface read this.
    const GLboolean cull_was_enabled = glIsEnabled(GL_CULL_FACE);
    const GLboolean blend_was_enabled = glIsEnabled(GL_BLEND);
    glDisable(GL_CULL_FACE);
    // Nothing here is translucent: the shoreline's fractional alpha is spent as
    // sample coverage, never as a blend, and the G-buffer list about to be bound
    // carries indexed blend disables that a blanket glEnable(GL_BLEND) would wipe.
    glDisable(GL_BLEND);
    if (a2c)
        glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    engine_set_scene_draw_buffers(engine, true);

    glBindVertexArray(water->grid_vao);
    glDrawElements(GL_TRIANGLES, water->grid_index_count, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);

    // Submission counters, so water has a SUBMISSION row and a gate can assert integers on
    // the lattice. meshes_seen must equal instances + culled or the profiler's own sum
    // check fires.
    SubmitStats* submit = profiler_submit(engine->profiler);
    if (submit) {
        submit->draws += 1;
        submit->instances += 1;
        submit->meshes_seen += 1;
        submit->triangles += (size_t)water->grid_index_count / 3;
    }

    engine_set_scene_draw_buffers(engine, false);
    if (a2c)
        glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    if (blend_was_enabled)
        glEnable(GL_BLEND);
    if (cull_was_enabled)
        glEnable(GL_CULL_FACE);

    check_gl_error("water surface");
}

/*
 * Inverse-transform ONE Fourier mode and compare against its closed form.
 *
 * The isolation test the variance rows cannot be. A transform whose butterfly reads the
 * wrong partner, whose twiddle is conjugated the wrong way, or which drops the fftshift
 * still produces a field with the right variance -- so it still renders a plausible sea,
 * still reproduces across runs, and still differs from Gerstner. Every existing arm passes
 * on it. Against a single mode there is one right answer and it is a closed form.
 *
 * A centred impulse at (N/2 + fx, N/2 + fy) must come back as exp(i*2pi*(fx*x + fy*y)/N),
 * which is what the finalize stage's (-1)^(x+y) is FOR: the seeding centres k on the grid,
 * so the shift belongs in the transform rather than at every consumer.
 *
 * Run on fp32 scratch rather than the fp16 the cascades use. What is under test is the
 * indexing, the twiddles and the shift, none of which depend on the format, and fp16 would
 * put its own 1e-3 floor exactly where the tolerance sits.
 */
static bool _water_fft_impulse(const Water* water, struct Engine* engine, int fx, int fy,
                               double* out_max_err) {
    ShaderProgram* fft = get_engine_shader_program_by_name(engine, "water_fft");
    if (!fft || !water->twiddle_tex || !water->fft_vao)
        return false;

    const int size = WATER_SPECTRUM_RES;
    const size_t texels = (size_t)size * size;
    float* data = calloc(texels * 4, sizeof(float));
    if (!data)
        return false;
    // Real unit impulse in the first packed complex pair; the second stays zero and rides
    // along, which is also a check that the two halves do not leak into each other.
    data[(((size_t)(size / 2 + fy) * size) + (size_t)(size / 2 + fx)) * 4 + 0] = 1.0f;

    GLint saved_viewport[4];
    GLint saved_fbo = 0;
    glGetIntegerv(GL_VIEWPORT, saved_viewport);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &saved_fbo);

    GLuint tex[2][2];
    GLuint fbo[2];
    bool complete = true;
    for (int b = 0; b < 2; b++) {
        for (int t = 0; t < 2; t++)
            tex[b][t] = _water_make_data_tex(size, size, (b == 0 && t == 0) ? data : NULL);
        glGenFramebuffers(1, &fbo[b]);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo[b]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex[b][0], 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, tex[b][1], 0);
        const GLenum targets[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
        glDrawBuffers(2, targets);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            complete = false;
    }
    free(data);

    const GLboolean depth_was_enabled = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean blend_was_enabled = glIsEnabled(GL_BLEND);

    if (complete) {
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glViewport(0, 0, size, size);
        glBindVertexArray(water->fft_vao);
        // The sea's OWN transform, not a copy of it -- which is what makes this test's
        // result a statement about the shipping path rather than about a sibling that
        // happens to agree today.
        _water_fft_transform(fft, water->twiddle_tex, fbo, tex);
        glBindVertexArray(0);

        float* out = malloc(texels * 4 * sizeof(float));
        if (out) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, tex[0][0]);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, out);
            double worst = 0.0;
            for (int y = 0; y < size; y++) {
                for (int x = 0; x < size; x++) {
                    const double phase =
                        6.283185307179586 * ((double)fx * x + (double)fy * y) / (double)size;
                    const size_t o = ((size_t)y * size + (size_t)x) * 4;
                    const double dr = out[o + 0] - cos(phase);
                    const double di = out[o + 1] - sin(phase);
                    if (fabs(dr) > worst)
                        worst = fabs(dr);
                    if (fabs(di) > worst)
                        worst = fabs(di);
                }
            }
            *out_max_err = worst;
            free(out);
        } else {
            complete = false;
        }
    }

    glDeleteFramebuffers(2, fbo);
    glDeleteTextures(4, &tex[0][0]);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)saved_fbo);
    glViewport(saved_viewport[0], saved_viewport[1], saved_viewport[2], saved_viewport[3]);
    // Restored on every exit including the failure ones, matching _water_run_spectral. The
    // only caller today runs after the loop has stopped, so nothing downstream would notice
    // -- but a diagnostic that leaves the pipeline in a different state than it found it
    // cannot later be called from anywhere else, and nothing in the signature says so.
    if (depth_was_enabled)
        glEnable(GL_DEPTH_TEST);
    if (blend_was_enabled)
        glEnable(GL_BLEND);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    return complete;
}

/*
 * MEASURE the transformed field and print it beside what the seeding predicted.
 *
 * The only instrument on the transform itself. Everything else in this subsystem reads
 * pixels, and pixels cannot distinguish a correct ocean from a wrong-but-deterministic one
 * -- that is the shape of defect spec 11.39 shipped in the cloud shadow map with two green
 * arms over it, and the impulse pair below is what closes it.
 *
 * Reading back inside the library rather than from the app, unlike --water-probe: the
 * cascades are water's own GPU state, and an app reaching into them would be reaching past
 * the seam that owns them.
 *
 * Costs a full pipeline stall per cascade. That is fine here and would not be per frame,
 * which is why nothing calls this from the render loop.
 */
void water_fft_probe(const Water* water, struct Engine* engine) {
    /*
     * Declining is a result, not a failure: the Gerstner path has no spectrum to measure,
     * and saying so is what stops a caller reading silence as agreement.
     *
     * Which of the four it is, though, is the caller's business. "No spectral sea in this
     * build" and "the readback allocation failed" both used to arrive as a bare
     * available=0, so a reader saw an empty result and could only report the symptom.
     */
    const char* declined = NULL;
    if (!water || !engine)
        declined = "nowater";
    else if (water->wave_model != WATER_WAVES_FFT)
        declined = "gerstner";
    else if (!water->spectra_ready)
        declined = "unseeded";
    if (declined) {
        printf("water-fft-probe header available=0 reason=%s\n", declined);
        return;
    }

    const int size = WATER_SPECTRUM_RES;
    const size_t texels = (size_t)size * size;
    float* buf = malloc(texels * 4 * sizeof(float));
    if (!buf) {
        printf("water-fft-probe header available=0 reason=alloc\n");
        return;
    }

    // Every line leads with its kind. The reader used to tell them apart by whether the
    // first field parsed as a number, which makes any new header key a parse change.
    printf("water-fft-probe header available=1 cascades=%d res=%d\n", WATER_CASCADE_COUNT, size);
    glActiveTexture(GL_TEXTURE0);
    for (int c = 0; c < WATER_CASCADE_COUNT; c++) {
        // Target 0 channel b is the height, target 1 channels r,g are the two slopes --
        // the same packing ocean.glsl samples, so this measures what the surface reads
        // rather than an intermediate nothing consumes.
        glBindTexture(GL_TEXTURE_2D, water->cascade_field[c][0][0]);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, buf);
        double mean = 0.0, sq = 0.0, peak = 0.0;
        for (size_t i = 0; i < texels; i++) {
            const double h = buf[i * 4 + 2];
            mean += h;
            sq += h * h;
            if (fabs(h) > peak)
                peak = fabs(h);
        }
        mean /= (double)texels;
        const double height_var = sq / (double)texels - mean * mean;

        glBindTexture(GL_TEXTURE_2D, water->cascade_field[c][0][1]);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, buf);
        double slope_sq = 0.0;
        for (size_t i = 0; i < texels; i++) {
            const double sx = buf[i * 4 + 0];
            const double sz = buf[i * 4 + 1];
            slope_sq += sx * sx + sz * sz;
        }
        const double slope_var = slope_sq / (double)texels;

        const double hp = water->cascade_height_var[c];
        const double sp = water->cascade_slope_var[c];
        printf("water-fft-probe cascade index=%d height_pred=%.6f height_meas=%.6f "
               "height_ratio=%.4f peak=%.4f slope_pred=%.6f slope_meas=%.6f "
               "slope_ratio=%.4f\n",
               c, hp, height_var, height_var / fmax(hp, 1e-12), peak, sp, slope_var,
               slope_var / fmax(sp, 1e-12));
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    free(buf);

    // Two modes, because one is not enough. The centred impulse checks the shift and the
    // overall scale; the neighbouring one checks that a mode lands at the wavenumber it
    // was given, which is what an off-by-one butterfly partner breaks and what a constant
    // field cannot distinguish.
    double err_dc = 0.0;
    double err_one = 0.0;
    const bool ran = _water_fft_impulse(water, engine, 0, 0, &err_dc) &&
                     _water_fft_impulse(water, engine, 1, 0, &err_one);
    if (ran)
        printf("water-fft-probe impulse dc_err=%.8f mode_err=%.8f\n", err_dc, err_one);
    else
        printf("water-fft-probe impulse available=0\n");

    check_gl_error("water fft probe");
}
