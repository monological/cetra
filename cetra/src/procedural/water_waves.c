#include <math.h>
#include <stddef.h>
#include <string.h>

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
 * injective, and water_effective_steepness is exactly the bound that keeps it injective.
 *
 * The iteration count is NOT fixed, and the reason is worth stating: differentiating the
 * map shows the contraction modulus is bounded by the steepness itself (the per-octave
 * normalisation makes q*a*k exactly steepness/N), so at the authored maximum of 1 there
 * is no contraction at all and a fixed step count would silently return a point on the
 * surface that is not the one over the query. So it runs until the step it just took is
 * small against the wave amplitude, with a cap. The last step size IS the residual, so
 * the loop knows exactly how well it converged.
 */
#define WAVES_INVERSE_MAX_STEPS 8
// As a fraction of the longest octave's amplitude: below this the parameter has stopped
// moving by anything the surface can express.
#define WAVES_INVERSE_EPS_FRAC 0.002f

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
    if (out_grad) {
        out_grad[0] = 0.0f;
        out_grad[1] = 0.0f;
    }
    if (!water->height_at)
        return 1.0f;

    const float bed = water->height_at(water->height_ctx, x, z);
    const float span = WAVES_SHOAL_FULL - WAVES_SHOAL_MIN;
    float u = (water->level - bed - WAVES_SHOAL_MIN) / span;
    u = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);

    // NULL out_grad is the common case and skips four of the five callback samples: the
    // inversion below needs only the factor, and on a real bed provider this callback is
    // multi-octave noise.
    if (out_grad) {
        const float cell = water->extent * 2.0f / (float)WATER_BED_RES;
        const float dbdx = (water->height_at(water->height_ctx, x + cell, z) -
                            water->height_at(water->height_ctx, x - cell, z)) /
                           (2.0f * cell);
        const float dbdz = (water->height_at(water->height_ctx, x, z + cell) -
                            water->height_at(water->height_ctx, x, z - cell)) /
                           (2.0f * cell);
        const float dfactor = 6.0f * u * (1.0f - u) / span;
        out_grad[0] = -dfactor * dbdx;
        out_grad[1] = -dfactor * dbdz;
    }
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
        // Through the accessor, not off the field: the field is public and unclamped, and
        // reading it raw gave the CPU a steeper train than the GPU was uploaded.
        const float q =
            water_effective_steepness(water) / fmaxf(k * amplitude * (float)WAVES_OCTAVES, 1e-4f);
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

bool water_waves_available(const Water* water) {
    return water && water->enabled && !water->failed && water->wave_model == WATER_WAVES_GERSTNER;
}

// Everything one query needs, from one solve. Split out so the height, the normal and the
// residual cannot come from separate solves -- which would let them describe different
// instants of the same sea, the failure water_waves.h warns about.
typedef struct WavesSolution {
    float px, pz;    // the recovered wave parameter
    float shoal;     // and the shoal factor there
    float grad[2];   // its gradient; zero unless asked for
    float disp[3];   // unshoaled displacement at the parameter
    float ddx[3];    // and its two derivative rows; zero unless asked for
    float ddz[3];
    float residual;  // how far the parameter lands from the query, world units
} WavesSolution;

/*
 * Invert the horizontal map: find the parameter whose displaced position is (x, z).
 *
 * The parameter p produces the world point p + disp(p) * shoal(p), so the iteration is
 * p <- target - disp(p) * shoal(p). The shoal factor stays INSIDE it: solving the
 * unshoaled map and shrinking the answer afterwards inverts a different map than the one
 * the raster used.
 *
 * The residual is exact rather than assumed, and it costs nothing. At iterate n the loop
 * holds u_n = disp(p_n) * shoal(p_n) and sets p_{n+1} = target - u_n, so the true
 * forward-map error at the final iterate is |u_K - u_{K-1}| -- the size of the last step.
 * That is what the loop tests to decide it is done, so convergence is measured in every
 * configuration instead of argued for one.
 */
static void _waves_solve(const Water* water, float x, float z, float t, bool want_derivatives,
                         WavesSolution* out) {
    memset(out, 0, sizeof(*out));
    float px = x;
    float pz = z;
    float prev_ux = 0.0f, prev_uz = 0.0f;
    const float eps = fmaxf(water->amplitude, 1e-4f) * WAVES_INVERSE_EPS_FRAC;

    for (int step = 0; step < WAVES_INVERSE_MAX_STEPS; step++) {
        const float shoal = _waves_shoal(water, px, pz, NULL);
        float disp[3];
        _waves_eval(water, px, pz, t, disp, NULL, NULL);
        const float ux = disp[0] * shoal;
        const float uz = disp[2] * shoal;
        px = x - ux;
        pz = z - uz;
        if (step > 0) {
            const float dx = ux - prev_ux;
            const float dz = uz - prev_uz;
            out->residual = sqrtf(dx * dx + dz * dz);
            if (out->residual <= eps)
                break;
        }
        prev_ux = ux;
        prev_uz = uz;
    }

    out->px = px;
    out->pz = pz;
    out->shoal = _waves_shoal(water, px, pz, want_derivatives ? out->grad : NULL);
    _waves_eval(water, px, pz, t, out->disp, want_derivatives ? out->ddx : NULL,
                want_derivatives ? out->ddz : NULL);
}

float water_waves_inverse_residual(const Water* water, float x, float z, float t) {
    if (!water || !water_waves_available(water))
        return 0.0f;
    WavesSolution s;
    _waves_solve(water, x, z, t, false, &s);
    return s.residual;
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

    WavesSolution s;
    _waves_solve(water, x, z, t, out_normal != NULL, &s);

    if (out_normal) {
        // Same rows and the same cross order as ocean.glsl, including the shoal
        // gradient's product-rule term. cross(dPdz, dPdx) and not the reverse: on a flat
        // surface that is +Y.
        vec3 dpdx = {1.0f + s.ddx[0] * s.shoal + s.disp[0] * s.grad[0],
                     s.ddx[1] * s.shoal + s.disp[1] * s.grad[0],
                     s.ddx[2] * s.shoal + s.disp[2] * s.grad[0]};
        vec3 dpdz = {s.ddz[0] * s.shoal + s.disp[0] * s.grad[1],
                     s.ddz[1] * s.shoal + s.disp[1] * s.grad[1],
                     1.0f + s.ddz[2] * s.shoal + s.disp[2] * s.grad[1]};
        vec3 n;
        glm_vec3_cross(dpdz, dpdx, n);
        glm_vec3_normalize(n);
        glm_vec3_copy(n, out_normal);
    }
    return water->level + s.disp[1] * s.shoal;
}
