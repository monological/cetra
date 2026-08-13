#include <math.h>
#include <stddef.h>

#include "water_waves.h"

// The octave set, and it MUST match ocean.glsl's. A CPU answer that used a different
// falloff would agree with the shader at the origin and drift everywhere else, which is
// the worst failure shape available: plausible, and wrong by an amount that grows with
// distance from wherever it was checked.
#define WAVES_OCTAVES 4
#define WAVES_LENGTH_FALLOFF 0.42f
#define WAVES_AMPLITUDE_FALLOFF 0.44f
#define WAVES_GRAVITY 9.81f
#define WAVES_SHOAL_MIN 0.14f
#define WAVES_SHOAL_FULL 2.7f

/*
 * Fixed-point iterations that invert the horizontal map.
 *
 * A Gerstner wave moves its surface points sideways, so the point whose WORLD xz is the
 * one being asked about did not start there -- the parameter that produces it is offset
 * by the displacement itself. Callers ask about a world position (a hull, a float, a
 * splash), so the parameter has to be recovered rather than assumed, or every answer is
 * out by the horizontal displacement: 0.14 m on the longest octave at the default
 * steepness, which is more than twice the default amplitude.
 *
 * Fixed-point rather than Newton because the map is a contraction wherever it is
 * injective, and water.h's steepness clamp is exactly the condition that keeps it
 * injective. Three passes take the residual well below the amplitude; a fourth is
 * measurable only in the last bits.
 */
#define WAVES_INVERSE_STEPS 3

// One octave's direction, fanned off the wind and alternating sides so the set stays
// centred on it -- ocean.glsl's own construction.
static void _waves_dir(const float base[2], float spread, int i, float out[2]) {
    const float fan = spread * (float)i * ((i % 2 == 0) ? 1.0f : -1.0f);
    const float c = cosf(fan);
    const float s = sinf(fan);
    out[0] = base[0] * c - base[1] * s;
    out[1] = base[0] * s + base[1] * c;
}

/*
 * The shoal factor and its world-space gradient at a point.
 *
 * Taken from height_at directly rather than from the baked texture the vertex stage
 * samples. The two are the same function; the shader sees a WATER_BED_RES resample of
 * it, so the answers agree to the bake's resolution and not to the last bit. Central
 * differences at the bake's own cell size, so the gradient matches what the bake stored
 * rather than being a sharper truth the surface never used.
 */
static float _waves_shoal(const Water* water, float x, float z, float out_grad[2]) {
    out_grad[0] = 0.0f;
    out_grad[1] = 0.0f;
    if (!water->height_at)
        return 1.0f;

    const float cell = water->extent * 2.0f / (float)WATER_BED_RES;
    const float bed = water->height_at(water->height_ctx, x, z);
    const float span = WAVES_SHOAL_FULL - WAVES_SHOAL_MIN;
    float u = (water->level - bed - WAVES_SHOAL_MIN) / span;
    u = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);

    const float dbdx = (water->height_at(water->height_ctx, x + cell, z) -
                        water->height_at(water->height_ctx, x - cell, z)) /
                       (2.0f * cell);
    const float dbdz = (water->height_at(water->height_ctx, x, z + cell) -
                        water->height_at(water->height_ctx, x, z - cell)) /
                       (2.0f * cell);
    const float dfactor = 6.0f * u * (1.0f - u) / span;
    out_grad[0] = -dfactor * dbdx;
    out_grad[1] = -dfactor * dbdz;
    return u * u * (3.0f - 2.0f * u);
}

/*
 * Evaluate the train at parameter (px, pz): the displacement and, optionally, its two
 * partial derivatives. Unshoaled, because the shoal factor scales the whole thing and
 * its gradient is a product-rule term the caller applies once.
 */
static void _waves_eval(const Water* water, float px, float pz, float t, float out_disp[3],
                        float out_ddx[3], float out_ddz[3]) {
    for (int c = 0; c < 3; c++) {
        out_disp[c] = 0.0f;
        if (out_ddx)
            out_ddx[c] = 0.0f;
        if (out_ddz)
            out_ddz[c] = 0.0f;
    }

    float base[2] = {water->wind_dir[0] + 1e-6f, water->wind_dir[1]};
    const float len = sqrtf(base[0] * base[0] + base[1] * base[1]);
    base[0] /= len;
    base[1] /= len;

    float wavelength = water->wavelength;
    float amplitude = water->amplitude;

    for (int i = 0; i < WAVES_OCTAVES; i++) {
        float dir[2];
        _waves_dir(base, water->spread, i, dir);

        const float k = 6.28318530718f / fmaxf(wavelength, 0.01f);
        const float omega = sqrtf(WAVES_GRAVITY * k);
        const float phase = k * (dir[0] * px + dir[1] * pz) - omega * t;
        const float sinp = sinf(phase);
        const float cosp = cosf(phase);

        // Q normalised by this octave's own A*k and by the octave count, so the
        // authored steepness sums to itself across the set. See water.h.
        const float q = water->steepness / fmaxf(k * amplitude * (float)WAVES_OCTAVES, 1e-4f);
        const float qa = q * amplitude;

        out_disp[0] += qa * dir[0] * cosp;
        out_disp[1] += amplitude * sinp;
        out_disp[2] += qa * dir[1] * cosp;

        if (out_ddx && out_ddz) {
            const float dqa = qa * k * sinp;
            const float dah = amplitude * k * cosp;
            out_ddx[0] -= dqa * dir[0] * dir[0];
            out_ddx[1] += dah * dir[0];
            out_ddx[2] -= dqa * dir[1] * dir[0];
            out_ddz[0] -= dqa * dir[0] * dir[1];
            out_ddz[1] += dah * dir[1];
            out_ddz[2] -= dqa * dir[1] * dir[1];
        }

        wavelength *= WAVES_LENGTH_FALLOFF;
        amplitude *= WAVES_AMPLITUDE_FALLOFF;
    }
}

/*
 * Recover the parameter whose displaced position is (x, z).
 *
 * The parameter p produces the world point p + disp(p) * shoal(p), so the iteration is
 * p <- target - disp(p) * shoal(p). The shoal factor stays INSIDE it: solving the
 * unshoaled map and shrinking the answer afterwards inverts a different map than the one
 * the raster used.
 */
static void _waves_invert(const Water* water, float x, float z, float t, float* out_px,
                          float* out_pz) {
    float px = x;
    float pz = z;
    for (int step = 0; step < WAVES_INVERSE_STEPS; step++) {
        float grad[2];
        const float shoal = _waves_shoal(water, px, pz, grad);
        float disp[3];
        _waves_eval(water, px, pz, t, disp, NULL, NULL);
        px = x - disp[0] * shoal;
        pz = z - disp[2] * shoal;
    }
    *out_px = px;
    *out_pz = pz;
}

float water_waves_inverse_residual(const Water* water, float x, float z, float t) {
    if (!water || !water_waves_available(water))
        return 0.0f;
    float px, pz;
    _waves_invert(water, x, z, t, &px, &pz);
    float grad[2];
    const float shoal = _waves_shoal(water, px, pz, grad);
    float disp[3];
    _waves_eval(water, px, pz, t, disp, NULL, NULL);
    // Push the recovered parameter forward and measure how far it lands from the query.
    const float dx = px + disp[0] * shoal - x;
    const float dz = pz + disp[2] * shoal - z;
    return sqrtf(dx * dx + dz * dz);
}

bool water_waves_available(const Water* water) {
    return water && water->enabled && !water->failed && water->wave_model == WATER_WAVES_GERSTNER;
}

float water_surface_at(const Water* water, float x, float z, float t, vec3 out_normal) {
    if (out_normal) {
        out_normal[0] = 0.0f;
        out_normal[1] = 1.0f;
        out_normal[2] = 0.0f;
    }
    if (!water)
        return 0.0f;
    if (!water_waves_available(water))
        return water->level;

    // Recover the parameter that lands on (x, z). The shoal factor scales the
    // displacement, so it belongs inside the iteration rather than applied after it --
    // solving the unshoaled map and then shrinking the answer inverts a different map
    // than the one the raster used.
    float px, pz;
    _waves_invert(water, x, z, t, &px, &pz);

    float disp[3];
    float grad[2];
    const float shoal = _waves_shoal(water, px, pz, grad);
    float ddx[3], ddz[3];
    _waves_eval(water, px, pz, t, disp, ddx, ddz);

    if (out_normal) {
        // Same rows and the same cross order as ocean.glsl, including the shoal
        // gradient's product-rule term. cross(dPdz, dPdx) and not the reverse: on a flat
        // surface that is +Y.
        const float dpdx[3] = {1.0f + ddx[0] * shoal + disp[0] * grad[0],
                               ddx[1] * shoal + disp[1] * grad[0],
                               ddx[2] * shoal + disp[2] * grad[0]};
        const float dpdz[3] = {ddz[0] * shoal + disp[0] * grad[1],
                               ddz[1] * shoal + disp[1] * grad[1],
                               1.0f + ddz[2] * shoal + disp[2] * grad[1]};
        vec3 nx = {dpdz[1] * dpdx[2] - dpdz[2] * dpdx[1], dpdz[2] * dpdx[0] - dpdz[0] * dpdx[2],
                   dpdz[0] * dpdx[1] - dpdz[1] * dpdx[0]};
        glm_vec3_normalize(nx);
        glm_vec3_copy(nx, out_normal);
    }
    return water->level + disp[1] * shoal;
}

float water_height_at(const Water* water, float x, float z, float t) {
    return water_surface_at(water, x, z, t, NULL);
}
