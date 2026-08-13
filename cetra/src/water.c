#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include "water.h"

#include "engine.h"
#include "ext/log.h"
#include "ibl.h"
#include "profiler.h"
#include "program.h"
#include "scene.h"
#include "uniform.h"
#include "util.h"

/*
 * The three spectral bands, ported from the reference study.
 *
 * cutoff_low/high are the wavenumber window each band owns, in rad/m. The windows
 * OVERLAP slightly -- [0.30, 0.36] and [1.22, 1.42] -- so the handful of modes in
 * those strips are seeded in two cascades and their energy is counted twice. These
 * are the reference study's own numbers and the overlap is inherited from it, not
 * chosen; closing it means retuning a sea state against a look, which is a change
 * worth measuring rather than a constant worth editing in passing.
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
    uint32_t seed;
} WATER_CASCADE_CFG[WATER_CASCADE_COUNT] = {
    {240.0f, 0.024f, 0.36f, 0.45f, 0.22f, 0x51f15eu},
    {64.0f, 0.30f, 1.42f, 0.45f, 0.08f, 0x72a93bu},
    {12.0f, 1.22f, 24.0f, 0.82f, 0.0f, 0x19ce47u},
};

const float WATER_CASCADE_LENGTH[WATER_CASCADE_COUNT] = {240.0f, 64.0f, 12.0f};
// How hard each band's horizontal displacement pulls. The short band is damped:
// it exists to shade, and choppiness there sharpens nothing the mesh resolves.
const float WATER_CASCADE_CHOPPINESS[WATER_CASCADE_COUNT] = {1.18f, 1.05f, 0.40f};

// Sea state. One set of numbers for all three bands, so the bands are windows
// onto ONE spectrum rather than three independently authored looks.
#define WATER_SEA_DEPTH 54.0f
#define WATER_WIND_SPEED 11.5f
#define WATER_FETCH 120000.0f
#define WATER_WIND_ANGLE (-0.48f)
#define WATER_PEAK_ENHANCEMENT 3.3f
#define WATER_SWELL 0.38f

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

/*
 * Seed one cascade: the conjugate-symmetric initial spectrum, the per-mode wave
 * vector and dispersion, and (once) the Stockham twiddle table.
 *
 * initial holds h0(k) in .xy and conj(h0(-k)) in .zw, which is what lets the
 * evolution step produce a REAL surface from one complex multiply per mode
 * instead of enforcing symmetry afterwards.
 */
static bool _water_build_spectrum(int size, const struct WaterCascadeConfig* cfg, float* initial,
                                  float* wave_data, float* twiddle) {
    const float g = 9.81f;
    const float delta_k = 6.28318530718f / cfg->length_scale;
    const float alpha =
        0.076f * powf(g * WATER_FETCH / (WATER_WIND_SPEED * WATER_WIND_SPEED), -0.22f);
    const float peak_omega =
        22.0f * powf(WATER_WIND_SPEED * WATER_FETCH / (g * g), -0.33f);

    // Reported rather than shrugged off: returning quietly would leave wave_data
    // unwritten and the twiddle table all zeros, so every butterfly would read
    // texel 0 and the transform would collapse into a flat ocean with no error --
    // and for a later cascade the buffers still hold the PREVIOUS one's spectrum,
    // which is worse than flat because it looks plausible.
    float* h0 = calloc((size_t)size * size * 2, sizeof(float));
    if (!h0)
        return false;
    uint32_t rng = cfg->seed;

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

            const float kh = fminf(k_len * WATER_SEA_DEPTH, 20.0f);
            const float tanh_kh = tanhf(kh);
            const float omega = sqrtf(g * k_len * tanh_kh);
            const float sech2 = 1.0f - tanh_kh * tanh_kh;
            // d(omega)/dk, which converts a spectral density in frequency to one
            // in wavenumber. Getting this wrong scales the whole sea state.
            const float domega =
                g * (WATER_SEA_DEPTH * k_len * sech2 + tanh_kh) / fmaxf(omega * 2.0f, 1e-5f);
            const float omega_h = omega * sqrtf(WATER_SEA_DEPTH / g);
            const float tma = omega_h <= 1.0f ? 0.5f * omega_h * omega_h
                              : omega_h < 2.0f ? 1.0f - 0.5f * (2.0f - omega_h) * (2.0f - omega_h)
                                               : 1.0f;

            const float jonswap =
                _water_jonswap(omega, peak_omega, alpha, tma, WATER_PEAK_ENHANCEMENT);
            const float theta = _water_wrap_angle(atan2f(kz, kx) - WATER_WIND_ANGLE);
            const float omega_ratio = omega / peak_omega;
            const float spread_power =
                ((omega > peak_omega ? 9.77f * powf(omega_ratio, -2.5f)
                                     : 6.97f * powf(omega_ratio, 5.0f)) +
                 16.0f * tanhf(fminf(omega_ratio, 20.0f)) * WATER_SWELL * WATER_SWELL) *
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
                const float sw_theta =
                    _water_wrap_angle(atan2f(kz, kx) - (WATER_WIND_ANGLE + 0.82f));
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
            _water_gaussian(&rng, &ga, &gb);
            h0[pixel * 2 + 0] = ga * amplitude;
            h0[pixel * 2 + 1] = gb * amplitude;

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

    if (!twiddle)
        return true;
    // Stockham twiddles: per stage and per output index, the rotation and the two
    // input indices the butterfly reads. Precomputing the INDICES is what keeps
    // the shader a pure gather with no bit-reversal pass of its own.
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
    return true;
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
    // Clear-water extinction per metre, red first. Not a look control at the
    // scale of a pond -- over the couple of metres a lake fixture spans these
    // barely bite -- but it is the physical ordering, and the depth-graded
    // colour that reads as water comes from it rather than from a tint.
    glm_vec3_copy((vec3){0.45f, 0.09f, 0.06f}, water->absorption);
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
    // Gerstner by default: it allocates no GPU state, costs no passes, and is the
    // right model at the scale most scenes put water at. The spectral path is an
    // ocean, and asks for 45 passes and 24 textures to say so.
    water->wave_model = WATER_WAVES_GERSTNER;
    water->caustics = true;
    return water;
}

void free_water(Water* water) {
    if (!water)
        return;
    if (water->grid_vao)
        glDeleteVertexArrays(1, &water->grid_vao);
    if (water->grid_vbo)
        glDeleteBuffers(1, &water->grid_vbo);
    if (water->grid_ebo)
        glDeleteBuffers(1, &water->grid_ebo);
    if (water->fft_vao)
        glDeleteVertexArrays(1, &water->fft_vao);
    if (water->fft_vbo)
        glDeleteBuffers(1, &water->fft_vbo);
    if (water->twiddle_tex)
        glDeleteTextures(1, &water->twiddle_tex);
    if (water->bed_tex)
        glDeleteTextures(1, &water->bed_tex);
    for (int c = 0; c < WATER_CASCADE_COUNT; c++) {
        if (water->cascade_initial[c])
            glDeleteTextures(1, &water->cascade_initial[c]);
        if (water->cascade_wave[c])
            glDeleteTextures(1, &water->cascade_wave[c]);
        for (int b = 0; b < 2; b++) {
            for (int t = 0; t < 2; t++)
                if (water->cascade_field[c][b][t])
                    glDeleteTextures(1, &water->cascade_field[c][b][t]);
            if (water->cascade_fbo[c][b])
                glDeleteFramebuffers(1, &water->cascade_fbo[c][b]);
        }
    }
    free(water);
}

void water_invalidate_bed(Water* water) {
    if (water)
        water->bed_baked = false;
}

// Bake height_at over the drawn extent. One R32F texel per sample, CLAMP so a
// vertex just outside the baked square reads the nearest shore rather than
// wrapping to the far side of the scene.
static void _water_bake_bed(Water* water) {
    if (water->bed_baked || !water->height_at)
        return;

    const int res = WATER_BED_RES;
    float* heights = malloc((size_t)res * res * sizeof(float));
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
            heights[z * res + x] = water->height_at(water->height_ctx, wx, wz);
        }
    }

    glActiveTexture(GL_TEXTURE0); // see _water_make_field
    if (!water->bed_tex)
        glGenTextures(1, &water->bed_tex);
    glBindTexture(GL_TEXTURE_2D, water->bed_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, res, res, 0, GL_RED, GL_FLOAT, heights);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    free(heights);
    water->bed_baked = true;
    log_info("Water: bed baked at %d^2 over %.0f units", res, (double)span);
}

bool water_active(const Water* water) {
    return water && water->enabled && !water->failed;
}

// One indexed grid in the XZ plane over [-0.5, 0.5]^2, positions only. Where it
// lands and how it is displaced is the vertex shader's business, so the mesh
// carries no normals: a displaced surface's normal is the derivative of the
// displacement, and a stored one would just be overwritten.
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
    // Two index sets over the same vertices. The centre patch is the whole grid;
    // a ring is the whole grid MINUS its inner quarter, which is exactly the
    // extent of the level inside it -- so the hole matches by construction rather
    // than by tuning, and the ring set is written second into the same buffer.
    unsigned* ring_indices = malloc((size_t)index_count * sizeof(unsigned));
    if (!ring_indices) {
        log_error("Water ring allocation failed; disabling water");
        free(verts);
        free(indices);
        water->failed = true;
        return false;
    }

    int w = 0;
    int rw = 0;
    for (int z = 0; z < res; z++) {
        for (int x = 0; x < res; x++) {
            const unsigned a = (unsigned)(z * verts_per_side + x);
            const unsigned b = a + 1;
            const unsigned c = a + (unsigned)verts_per_side;
            const unsigned d = c + 1;
            const unsigned quad[6] = {a, c, b, b, c, d};
            for (int k = 0; k < 6; k++)
                indices[w++] = quad[k];

            // Cell CENTRE against the inner quarter, so a cell is either wholly in
            // the hole or wholly in the ring. Testing a corner instead would leave
            // the boundary row half-classified and tear the seam the levels meet on.
            const float cx = ((float)x + 0.5f) / (float)res - 0.5f;
            const float cz = ((float)z + 0.5f) / (float)res - 0.5f;
            if (fabsf(cx) < 0.25f && fabsf(cz) < 0.25f)
                continue;
            for (int k = 0; k < 6; k++)
                ring_indices[rw++] = quad[k];
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
    // Both sets in ONE element buffer, centre first: the ring draw is the same VAO
    // at an index offset, so switching between them costs no rebind.
    glGenBuffers(1, &water->grid_ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, water->grid_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 (GLsizeiptr)(index_count + rw) * sizeof(unsigned), NULL, GL_STATIC_DRAW);
    glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, (GLsizeiptr)index_count * sizeof(unsigned),
                    indices);
    glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, (GLintptr)index_count * sizeof(unsigned),
                    (GLsizeiptr)rw * sizeof(unsigned), ring_indices);
    glBindVertexArray(0);

    free(verts);
    free(indices);
    free(ring_indices);
    water->grid_index_count = index_count;
    water->ring_index_count = rw;
    log_info("Water: %d levels, %dx%d grid, %d centre + %d ring triangles, level %.2f, "
             "extent %.1f",
             WATER_RING_LEVELS, res, res, index_count / 3, rw / 3, (double)water->level,
             (double)water->extent);
    return true;
}

static GLuint _water_make_field(int size) {
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
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
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

static bool _water_ensure_spectra(Water* water) {
    if (water->spectra_ready)
        return true;
    if (water->failed)
        return false;

    // Restored on every exit, including the failure ones. Binding 0 and returning
    // would leave the WINDOW framebuffer current, and every pass after water --
    // the transparent lane, the OIT accumulate, the particle depth resolve --
    // would draw into it instead of the scene target.
    GLint saved_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &saved_fbo);

    const int size = WATER_SPECTRUM_RES;
    float* initial = calloc((size_t)size * size * 4, sizeof(float));
    float* wave = calloc((size_t)size * size * 4, sizeof(float));
    float* twiddle = calloc((size_t)size * WATER_SPECTRUM_LOG * 4, sizeof(float));
    if (!initial || !wave || !twiddle) {
        log_error("Water spectrum allocation failed; disabling water");
        free(initial);
        free(wave);
        free(twiddle);
        water->failed = true;
        return false;
    }

    for (int c = 0; c < WATER_CASCADE_COUNT; c++) {
        // The twiddle table depends only on the transform size, so it is built
        // with the first cascade and shared by all of them.
        if (!_water_build_spectrum(size, &WATER_CASCADE_CFG[c], initial, wave,
                                   c == 0 ? twiddle : NULL)) {
            log_error("Water cascade %d seeding failed; disabling water", c);
            glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)saved_fbo);
            free(initial);
            free(wave);
            free(twiddle);
            water->failed = true;
            return false;
        }
        water->cascade_initial[c] = _water_make_data_tex(size, size, initial);
        water->cascade_wave[c] = _water_make_data_tex(size, size, wave);
        if (c == 0)
            water->twiddle_tex = _water_make_data_tex(size, WATER_SPECTRUM_LOG, twiddle);

        for (int b = 0; b < 2; b++) {
            for (int t = 0; t < 2; t++)
                water->cascade_field[c][b][t] = _water_make_field(size);

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
                free(initial);
                free(wave);
                free(twiddle);
                water->failed = true;
                return false;
            }
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)saved_fbo);

    free(initial);
    free(wave);
    free(twiddle);
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
static void _water_run_spectral(Water* water, struct Engine* engine, float time) {
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

        glUseProgram(fft->id);
        uniform_set_int(fft->uniforms, "twiddleTex", 0);
        uniform_set_int(fft->uniforms, "in0", 1);
        uniform_set_int(fft->uniforms, "in1", 2);
        uniform_set_int(fft->uniforms, "size", WATER_SPECTRUM_RES);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, water->twiddle_tex);

        for (int pass = 0; pass < WATER_SPECTRUM_LOG * 2; pass++) {
            const int src = pass % 2;
            const int dst = 1 - src;
            glBindFramebuffer(GL_FRAMEBUFFER, water->cascade_fbo[c][dst]);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, water->cascade_field[c][src][0]);
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, water->cascade_field[c][src][1]);
            uniform_set_int(fft->uniforms, "axis", pass < WATER_SPECTRUM_LOG ? 0 : 1);
            uniform_set_int(fft->uniforms, "stage", pass % WATER_SPECTRUM_LOG);
            // The fftshift folds into the LAST stage as a checkerboard sign, which
            // is why it is a uniform rather than a separate pass.
            uniform_set_int(fft->uniforms, "finalize",
                            pass == WATER_SPECTRUM_LOG * 2 - 1 ? 1 : 0);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
    }

    glBindVertexArray(0);
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

    _water_bake_bed(water);

    // The spectral bands, before anything that samples them. No profiler scope of
    // its own: it would have to nest inside the caller's, and the simulation is
    // part of what water costs anyway.
    const bool fft = water->wave_model == WATER_WAVES_FFT;
    if (fft) {
        if (!_water_ensure_spectra(water))
            return;
        _water_run_spectral(water, engine, (float)engine->render_time);
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
    // The clipmap's placement inputs. base is the CENTRE patch's half-extent, so
    // the outermost ring reaches `extent`: base * 2^(levels-1) == extent.
    uniform_set_float(u, "waterRingBase",
                      water->extent / (float)(1 << (WATER_RING_LEVELS - 1)));
    uniform_set_int(u, "waterRingLevels", WATER_RING_LEVELS);
    uniform_set_int(u, "waterGridRes", WATER_GRID_RES);
    uniform_set_vec3(u, "waterCamPos", (const float*)&engine->camera->position);
    uniform_set_float(u, "waterRoughness", water->roughness);
    uniform_set_float(u, "waterIor", water->ior);
    uniform_set_vec3(u, "waterAbsorption", (const float*)&water->absorption);
    uniform_set_vec3(u, "waterScatter", (const float*)&water->scatter);
    uniform_set_vec2(u, "waterWindDir", (const float*)&water->wind_dir);
    uniform_set_float(u, "waterAmplitude", water->amplitude);
    uniform_set_float(u, "waterWavelength", water->wavelength);
    // Clamped here rather than trusted. Above 1 the summed Gerstner steepness makes
    // the horizontal map non-injective and the surface folds through itself; the
    // shader's normalisation guarantees the bound only for an in-range value.
    uniform_set_float(u, "waterSteepness", glm_clamp(water->steepness, 0.0f, 1.0f));
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
    // anything about the view. First directional light wins; a scene with two suns
    // has a bigger problem than its caustics.
    vec3 sun_dir = {0.0f, 1.0f, 0.0f};
    bool has_sun = false;
    for (size_t i = 0; i < scene->light_count && !has_sun; i++) {
        const Light* light = scene->lights[i];
        if (light && light->type == LIGHT_DIRECTIONAL) {
            // Lights store the direction they SHINE; the shader wants the direction
            // toward the source.
            glm_vec3_negate_to((float*)light->direction, sun_dir);
            glm_vec3_normalize(sun_dir);
            has_sun = true;
        }
    }
    uniform_set_vec3(u, "sunDir", (const float*)&sun_dir);
    uniform_set_int(u, "sunAvailable", has_sun ? 1 : 0);
    uniform_set_int(u, "causticsEnabled", water->caustics ? 1 : 0);

    // Which side of the surface the eye is on. Compared against the still level
    // rather than the displaced surface: a camera within a wave height of the
    // waterline would otherwise flip models several times a second as crests pass
    // it, and every temporal history in the frame would reset each time.
    vec3 cam_world;
    glm_vec3_copy(engine->camera->position, cam_world);
    uniform_set_int(u, "cameraSubmerged", cam_world[1] < water->level ? 1 : 0);

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
    for (int c = 0; c < WATER_CASCADE_COUNT; c++) {
        for (int t = 0; t < 2; t++) {
            const int unit = WATER_CASCADE_UNIT0 + c * 2 + t;
            char name[32];
            snprintf(name, sizeof(name), "cascade%d_%d", c, t);
            glActiveTexture(GL_TEXTURE0 + unit);
            glBindTexture(GL_TEXTURE_2D, fft ? water->cascade_field[c][0][t] : 0);
            uniform_set_int(u, name, unit);
        }
        char len[32], chop[32];
        snprintf(len, sizeof(len), "cascadeLength[%d]", c);
        snprintf(chop, sizeof(chop), "cascadeChoppiness[%d]", c);
        uniform_set_float(u, len, WATER_CASCADE_LENGTH[c]);
        uniform_set_float(u, chop, WATER_CASCADE_CHOPPINESS[c]);
    }

    glActiveTexture(GL_TEXTURE0);

    // Depth writes ON, unlike the skybox and the late pass. The particle depth
    // resolve and everything that sorts against the surface read this.
    const GLboolean cull_was_enabled = glIsEnabled(GL_CULL_FACE);
    const GLboolean blend_was_enabled = glIsEnabled(GL_BLEND);
    glDisable(GL_CULL_FACE);
    // Nothing here is translucent -- coverage is a discard, not an alpha -- and
    // the G-buffer list about to be bound carries indexed blend disables that a
    // blanket glEnable(GL_BLEND) would wipe.
    glDisable(GL_BLEND);
    engine_set_scene_draw_buffers(engine, true);

    glBindVertexArray(water->grid_vao);
    // Centre patch, then every ring in one instanced draw. gl_InstanceID + 1 is the
    // level, so the vertex stage needs no per-ring uniform and the ring count is a
    // draw parameter rather than a loop.
    uniform_set_int(u, "waterLevelBase", 0);
    glDrawElements(GL_TRIANGLES, water->grid_index_count, GL_UNSIGNED_INT, 0);
    if (WATER_RING_LEVELS > 1) {
        uniform_set_int(u, "waterLevelBase", 1);
        glDrawElementsInstanced(
            GL_TRIANGLES, water->ring_index_count, GL_UNSIGNED_INT,
            (const void*)((size_t)water->grid_index_count * sizeof(unsigned)),
            WATER_RING_LEVELS - 1);
    }
    glBindVertexArray(0);

    // Submission counters, so water has a SUBMISSION row and a gate can assert
    // integers on the ring structure. meshes_seen must equal instances + culled or
    // the profiler's own sum check fires.
    SubmitStats* submit = profiler_submit(engine->profiler);
    if (submit) {
        const size_t rings = WATER_RING_LEVELS > 1 ? (size_t)WATER_RING_LEVELS - 1 : 0;
        submit->draws += rings > 0 ? 2 : 1;
        submit->instances += 1 + rings;
        submit->meshes_seen += 1 + rings;
        submit->triangles += (size_t)water->grid_index_count / 3 +
                             rings * ((size_t)water->ring_index_count / 3);
    }

    engine_set_scene_draw_buffers(engine, false);
    if (blend_was_enabled)
        glEnable(GL_BLEND);
    if (cull_was_enabled)
        glEnable(GL_CULL_FACE);

    check_gl_error("water surface");
}
