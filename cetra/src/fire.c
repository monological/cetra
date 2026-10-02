#include "fire.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "light.h"
#include "spectrum.h"
#include "wind.h"
#include "ext/log.h"

void fire_params_defaults(FireParams* p) {
    memset(p, 0, sizeof(*p));
    p->ambient = 293.0f;
    // Wood's volatiles light at about 550-650 K.
    p->ignition = 600.0f;
    p->burn_rate = 6.0f;
    // Enough that fuel burning undiluted would reach about 1700 K, a wood flame's adiabatic
    // temperature; mixing with the air round it keeps the soot nearer 1100-1400 K.
    p->heat = 1400.0f;
    p->soot_yield = 1.5f;
    p->buoyancy = 1.0f;
    p->soot_weight = 0.0f;
    p->cooling = 2500.0f;
    p->vorticity = 1.5f;
    p->soot_burnout = 4.0f;
    p->soot_burnout_at = 1100.0f;
    p->smoke_fade = 0.4f;
    p->wind_response = 1.0f;
    // 6 pi E(m) / lambda at 550 nm with E(m) = 0.26, per part per million by volume: soot's
    // Rayleigh absorption, about 9 per metre for every ppm.
    p->soot_absorption = 8.9f;
    // Fresh soot absorbs nearly everything it intercepts.
    p->smoke_albedo = 0.25f;
    p->blue_core = 20.0f;
    p->brightness = 1.0f;
    p->shimmer = 0.0f;
    // A candle's luminous zone: soot near 1600 K, at a few ppm.
    p->flame_temperature = 1600.0f;
    p->flame_soot = 6.0f;
    p->flicker = 0.3f;
}

FireSystem* create_fire_system(void) {
    FireSystem* fs = calloc(1, sizeof(FireSystem));
    if (!fs) {
        log_error("Failed to allocate FireSystem");
        return NULL;
    }
    fs->sim_hz = 60.0f;
    fs->max_steps = 4;
    fs->jacobi_iterations = 24;
    fs->maccormack = true;
    fs->warmup = 2.0f;
    fs->debug_field = -1;
    return fs;
}

void free_fire_system(FireSystem* fs) {
    free(fs);
}

Fire* fire_system_add(FireSystem* fs, FireKind kind, const char* name) {
    if (!fs)
        return NULL;
    if (fs->count >= FIRE_MAX) {
        log_warn("Fire: %s refused, a scene holds at most %d fires", name ? name : "(unnamed)",
                 FIRE_MAX);
        return NULL;
    }
    Fire* fire = &fs->fires[fs->count++];
    memset(fire, 0, sizeof(*fire));
    snprintf(fire->name, sizeof(fire->name), "%s", name ? name : "fire");
    fire->kind = kind;
    fire->enabled = true;
    fire->floor = true;
    fire_params_defaults(&fire->params);
    if (kind == FIRE_FLAME) {
        glm_vec3_copy((vec3){0.012f, 0.035f, 0.012f}, fire->size);
    } else {
        glm_vec3_copy((vec3){0.8f, 1.2f, 0.8f}, fire->size);
        fire->cell = 0.025f;
    }
    glm_vec3_copy((vec3){1.0f, 1.0f, 1.0f}, fire->color);
    return fire;
}

Fire* fire_system_find(FireSystem* fs, const char* name) {
    if (!fs || !name)
        return NULL;
    for (int i = 0; i < fs->count; i++)
        if (strcmp(fs->fires[i].name, name) == 0)
            return &fs->fires[i];
    return NULL;
}

bool fire_system_active(const FireSystem* fs) {
    if (!fs)
        return false;
    for (int i = 0; i < fs->count; i++)
        if (fs->fires[i].enabled)
            return true;
    return false;
}

static int _cells(float extent, float cell, int max) {
    const int n = (int)ceilf(extent / cell - 1e-4f);
    return n < 2 ? 2 : (n > max ? max : n);
}

// A cell size of zero or less is the default's, rather than a division by zero.
static float _cell(const Fire* fire) {
    return fire->cell > 0.0f ? fire->cell : 0.025f;
}

void fire_grid_cells(const Fire* fire, int cells[3]) {
    const float cell = _cell(fire);
    cells[0] = _cells(fire->size[0], cell, FIRE_GRID_MAX_X);
    cells[1] = _cells(fire->size[1], cell, FIRE_GRID_MAX_Y);
    cells[2] = _cells(fire->size[2], cell, FIRE_GRID_MAX_Z);
}

void fire_grid_bounds(const Fire* fire, vec3 min, vec3 max) {
    const float cell = _cell(fire);
    int n[3];
    fire_grid_cells(fire, n);
    for (int a = 0; a < 3; a++) {
        min[a] = fire->center[a] - 0.5f * (float)n[a] * cell;
        max[a] = fire->center[a] + 0.5f * (float)n[a] * cell;
    }
}

/*
 * The blackbody table, shared by both halves: the GPU uploads exactly these floats and reads
 * them back by texelFetch with the same interpolation, so the light the CPU computes from a
 * flame's profile and the emission the shader draws agree to float precision. Built once, on
 * the main thread, from spectrum.h's exact integral.
 */
static float g_blackbody[FIRE_BB_LUT_SIZE * 4];
static bool g_blackbody_built;

const float* fire_blackbody_table(void) {
    if (!g_blackbody_built) {
        for (int i = 0; i < FIRE_BB_LUT_SIZE; i++) {
            const float t = (float)i / (float)(FIRE_BB_LUT_SIZE - 1);
            const float kelvin = FIRE_BB_T_MIN + t * (FIRE_BB_T_MAX - FIRE_BB_T_MIN);
            vec3 xyz = {0.0f, 0.0f, 0.0f}, rgb = {0.0f, 0.0f, 0.0f};
            spectrum_blackbody_xyz(kelvin, xyz);
            spectrum_xyz_to_rec709(xyz, rgb);
            const float y = fmaxf(xyz[1], 1e-30f);
            for (int c = 0; c < 3; c++)
                g_blackbody[4 * i + c] = fmaxf(rgb[c], 0.0f) / y;
            g_blackbody[4 * i + 3] = log10f(y);
        }
        g_blackbody_built = true;
    }
    return g_blackbody;
}

// The shader's blackbodyNits, in C: nits per channel at `kelvin`, and the luminance.
float fire_blackbody(float kelvin, vec3 rgb) {
    if (!(kelvin >= FIRE_BB_T_MIN)) {
        glm_vec3_zero(rgb);
        return 0.0f;
    }
    const float* lut = fire_blackbody_table();
    const float x = fminf((kelvin - FIRE_BB_T_MIN) / (FIRE_BB_T_MAX - FIRE_BB_T_MIN), 1.0f) *
                    (float)(FIRE_BB_LUT_SIZE - 1);
    const int i0 = (int)floorf(x);
    const int i1 = i0 + 1 < FIRE_BB_LUT_SIZE ? i0 + 1 : i0;
    const float f = x - (float)i0;
    float texel[4];
    for (int c = 0; c < 4; c++)
        texel[c] = lut[4 * i0 + c] + (lut[4 * i1 + c] - lut[4 * i0 + c]) * f;
    const float lum = powf(10.0f, texel[3]);
    for (int c = 0; c < 3; c++)
        rgb[c] = texel[c] * lum;
    return lum;
}

// The reaction zone's own light, CH* and C2* chemiluminescence: what makes a flame's base
// blue. Its colour, luminance 1; the shader's FIRE_BLUE.
static const vec3 FIRE_BLUE = {0.31f, 0.68f, 6.2f};

/*
 * A FLAME's profile at height `u` (0 the wick, 1 the tip) and normalised distance `q` from its
 * axis (1 the edge): temperature, soot and the blue core's strength. The base burns blue
 * with no soot -- fuel and air meet there before any soot has formed -- the soot glows in the
 * middle, and it burns out toward the tip. The shader's flameProfile is this, line for line.
 */
static void _flame_profile(const FireParams* p, float u, float q, float* kelvin, float* soot,
                           float* blue) {
    const float inside = q < 1.0f ? 1.0f - q * q : 0.0f;
    const float lit = glm_smoothstep(0.08f, 0.32f, u) * (1.0f - glm_smoothstep(0.7f, 1.0f, u));
    *soot = p->flame_soot * inside * lit;
    *kelvin = p->ambient + (p->flame_temperature - p->ambient) * (1.0f - 0.35f * q * q) *
                               (1.0f - 0.4f * glm_smoothstep(0.6f, 1.0f, u));
    *blue = inside * (1.0f - glm_smoothstep(0.05f, 0.3f, u));
}

// The flame's radius at `u`, as a fraction of its half-width: a teardrop, widest a third up.
static float _flame_radius(float u) {
    return powf(fmaxf(sinf(GLM_PIf * powf(glm_clamp(u, 0.0f, 1.0f), 0.7f)), 0.0f), 0.8f);
}

// A step's deterministic noise in -1..1, from integers alone.
static float _hash_signed(uint32_t a, uint32_t b) {
    uint32_t h = a * 747796405u + b * 2891336453u + 0x9e3779b9u;
    h = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u;
    h = (h >> 22u) ^ h;
    return (float)h / 2147483647.5f - 1.0f;
}

static void _flame_rest(Fire* fire) {
    const float h = fire->size[1];
    for (int i = 0; i < FIRE_SPINE_POINTS; i++) {
        const float u = (float)i / (float)(FIRE_SPINE_POINTS - 1);
        fire->spine[i][0] = fire->center[0];
        fire->spine[i][1] = fire->center[1] + u * h;
        fire->spine[i][2] = fire->center[2];
        fire->spine[i][3] = 0.5f * fire->size[0] * _flame_radius(u);
        glm_vec3_zero(fire->spine_velocity[i]);
    }
}

/*
 * One fixed step of a FLAME's spine. Each point is held a rest length above the one below
 * it by a stiff spring, so the flame keeps its height, and sideways by a soft one, so it
 * sways; the wind leans it, and seeded impulses -- a draught, the wick's own puffing -- set it
 * flickering. The base stays on the wick.
 */
static void _flame_step(Fire* fire, int index, const vec3 wind, float dt) {
    const FireParams* p = &fire->params;
    const float h = fire->size[1];
    const float seg = h / (float)(FIRE_SPINE_POINTS - 1);
    // Height breathes with the flicker: a flame that pulls itself up and drops back.
    const float stretch = 1.0f + 0.15f * p->flicker * _hash_signed((uint32_t)fire->steps, 977u);
    glm_vec3_copy(fire->center, fire->spine[0]);
    for (int i = 1; i < FIRE_SPINE_POINTS; i++) {
        const float u = (float)i / (float)(FIRE_SPINE_POINTS - 1);
        vec3 target = {fire->spine[i - 1][0], fire->spine[i - 1][1] + seg * stretch,
                       fire->spine[i - 1][2]};
        // The wind leans the upper flame further than the base.
        target[0] += wind[0] * p->wind_response * 0.004f * u;
        target[2] += wind[2] * p->wind_response * 0.004f * u;
        vec3 accel = {0.0f, 0.0f, 0.0f};
        for (int a = 0; a < 3; a++) {
            const float k = a == 1 ? 900.0f : 220.0f;
            accel[a] = k * (target[a] - fire->spine[i][a]) - 9.0f * fire->spine_velocity[i][a];
        }
        // The flicker's impulses grow up the flame, which is where a candle visibly moves.
        const uint32_t seed = (uint32_t)(index * 131 + i);
        accel[0] += p->flicker * 6.0f * u * _hash_signed((uint32_t)fire->steps, seed);
        accel[2] += p->flicker * 6.0f * u * _hash_signed((uint32_t)fire->steps, seed + 7919u);
        for (int a = 0; a < 3; a++) {
            fire->spine_velocity[i][a] += accel[a] * dt;
            fire->spine[i][a] += fire->spine_velocity[i][a] * dt;
        }
        fire->spine[i][3] = 0.5f * fire->size[0] * _flame_radius(u);
    }
    fire->spine[0][3] = 0.0f;
}

void fire_flame_field(const Fire* fire, const vec3 p, float* kelvin, float* soot) {
    // The nearest point on the spine, and how far along it that is.
    float best = 1e30f, best_u = 0.0f, best_r = 0.0f;
    for (int i = 0; i + 1 < FIRE_SPINE_POINTS; i++) {
        vec3 a, b, ab, ap;
        glm_vec3_copy((float*)fire->spine[i], a);
        glm_vec3_copy((float*)fire->spine[i + 1], b);
        glm_vec3_sub(b, a, ab);
        glm_vec3_sub((float*)p, a, ap);
        const float len2 = fmaxf(glm_vec3_dot(ab, ab), 1e-12f);
        const float s = glm_clamp(glm_vec3_dot(ap, ab) / len2, 0.0f, 1.0f);
        vec3 c = {0.0f, 0.0f, 0.0f};
        glm_vec3_copy(a, c);
        glm_vec3_muladds(ab, s, c);
        const float d2 = glm_vec3_distance2((float*)p, c);
        if (d2 < best) {
            best = d2;
            best_u = ((float)i + s) / (float)(FIRE_SPINE_POINTS - 1);
            best_r = fire->spine[i][3] + (fire->spine[i + 1][3] - fire->spine[i][3]) * s;
        }
    }
    const float q = best_r > 1e-6f ? sqrtf(best) / best_r : 2.0f;
    float blue;
    _flame_profile(&fire->params, best_u, q, kelvin, soot, &blue);
}

// Radial and lengthwise samples of the profile the light integrates.
#define FLAME_QUAD_U 24
#define FLAME_QUAD_Q 12

void fire_flame_light(Fire* fire) {
    const FireParams* p = &fire->params;
    float total = 0.0f;
    vec3 rgb_sum = GLM_VEC3_ZERO_INIT;
    vec3 weighted = GLM_VEC3_ZERO_INIT;
    for (int iu = 0; iu < FLAME_QUAD_U; iu++) {
        const float u = ((float)iu + 0.5f) / (float)FLAME_QUAD_U;
        const float x = u * (float)(FIRE_SPINE_POINTS - 1);
        const int i0 = (int)floorf(x);
        const int i1 = i0 + 1 < FIRE_SPINE_POINTS ? i0 + 1 : i0;
        const float f = x - (float)i0;
        vec3 at = {0.0f, 0.0f, 0.0f}, seg = {0.0f, 0.0f, 0.0f};
        glm_vec3_lerp((float*)fire->spine[i0], (float*)fire->spine[i1], f, at);
        glm_vec3_sub((float*)fire->spine[i1], (float*)fire->spine[i0], seg);
        // The tube's length along this stretch of spine, per unit of u.
        const float du_len = glm_vec3_norm(seg) * (float)(FIRE_SPINE_POINTS - 1);
        const float radius = fire->spine[i0][3] + (fire->spine[i1][3] - fire->spine[i0][3]) * f;
        if (radius <= 0.0f || du_len <= 0.0f)
            continue;
        // The disc at this height: rings of area 2 pi r^2 q dq.
        float ring_lum = 0.0f;
        vec3 ring_rgb = GLM_VEC3_ZERO_INIT;
        for (int iq = 0; iq < FLAME_QUAD_Q; iq++) {
            const float q = ((float)iq + 0.5f) / (float)FLAME_QUAD_Q;
            float kelvin, soot, blue;
            _flame_profile(p, u, q, &kelvin, &soot, &blue);
            vec3 rgb = {0.0f, 0.0f, 0.0f};
            const float lum = fire_blackbody(kelvin, rgb);
            const float sigma = soot * p->soot_absorption;
            const float glow = p->blue_core * FIRE_FLAME_REACTION * blue;
            const float area = 2.0f * GLM_PIf * radius * radius * q / (float)FLAME_QUAD_Q;
            ring_lum += (sigma * lum + glow) * area;
            glm_vec3_muladds(rgb, sigma * area, ring_rgb);
            glm_vec3_muladds((float*)FIRE_BLUE, glow * area, ring_rgb);
        }
        const float dl = du_len / (float)FLAME_QUAD_U;
        total += ring_lum * dl;
        glm_vec3_muladds(ring_rgb, dl, rgb_sum);
        glm_vec3_muladds(at, ring_lum * dl, weighted);
    }
    fire->intensity = total * p->brightness;
    if (total > 0.0f) {
        glm_vec3_scale(weighted, 1.0f / total, fire->centroid);
        const float lum = 0.2126f * rgb_sum[0] + 0.7152f * rgb_sum[1] + 0.0722f * rgb_sum[2];
        if (lum > 0.0f)
            glm_vec3_scale(rgb_sum, 1.0f / lum, fire->color);
    } else {
        glm_vec3_copy(fire->center, fire->centroid);
    }
    fire->heat_release = 0.0f;
    fire->answered = true;
}

void fire_wind_air(const Wind* wind, double t, vec3 out) {
    glm_vec3_zero(out);
    if (!wind || !(wind->air_speed > 0.0f))
        return;
    vec3 across = {wind->direction[0], 0.0f, wind->direction[2]};
    const float len = glm_vec3_norm(across);
    if (len > 1e-6f)
        glm_vec3_scale(across, wind->air_speed * wind_gust(wind, (float)t) / len, out);
}

void fire_update(FireSystem* fs, const Wind* wind, double t) {
    if (!fs || !(fs->sim_hz > 0.0f))
        return;
    const double hz = (double)fs->sim_hz;
    const float dt = 1.0f / fs->sim_hz;
    // The step the clock is on, from the absolute time rather than an accumulator, so a
    // headless run takes exactly the same steps however its frames are paced.
    const int target = (int)floor(t * hz + 1e-6);
    fs->clock_step = target;
    vec3 air = {0.0f, 0.0f, 0.0f};
    fire_wind_air(wind, t, air);
    for (int i = 0; i < fs->count; i++) {
        Fire* fire = &fs->fires[i];
        if (fire->kind == FIRE_GRID)
            fire_grid_cells(fire, fire->grid);
        if (!fire->enabled) {
            fire->pending = 0;
            continue;
        }
        int due;
        if (!fire->started) {
            // A fire that starts has already burnt `warmup` seconds, taken in one go and
            // uncapped, so the scene opens on it burning.
            const int warm = (int)lroundf(fmaxf(fs->warmup, 0.0f) * fs->sim_hz);
            fire->started = true;
            fire->start_step = target - warm;
            fire->steps = 0;
            if (fire->kind == FIRE_FLAME)
                _flame_rest(fire);
            due = warm;
        } else {
            due = target - fire->start_step - fire->steps;
            if (due > fs->max_steps) {
                // Behind by more than a frame may catch up: skip the rest. A fire that
                // stalled for a second resumes rather than racing through it.
                fire->steps += due - fs->max_steps;
                due = fs->max_steps;
            }
        }
        fire->pending = due > 0 ? due : 0;
        if (fire->kind == FIRE_FLAME) {
            for (int s = 0; s < fire->pending; s++) {
                _flame_step(fire, i, air, dt);
                fire->steps++;
            }
            fire->pending = 0;
            fire_flame_light(fire);
        }
    }
}

void fire_drive_light(const Fire* fire, Light* light) {
    if (!fire || !light || !fire->answered)
        return;
    const float intensity = fire->enabled ? fmaxf(fire->intensity, 0.0f) : 0.0f;
    glm_vec3_copy((float*)fire->color, light->color);
    switch (light->type) {
        case LIGHT_POINT:
        case LIGHT_SPOT: {
            // Stored in the type's canonical unit, candela, whatever the light was authored in.
            light->intensity = intensity;
            vec3 at = {0.0f, 0.0f, 0.0f};
            glm_vec3_add((float*)fire->centroid, (float*)fire->light_offset, at);
            light_set_position(light, at);
            break;
        }
        case LIGHT_AREA: {
            // A Lambertian panel's intensity along its normal is its luminance times its area.
            const float area = light->size[0] * light->size[1];
            light->intensity = area > 0.0f ? intensity / area : 0.0f;
            break;
        }
        default:
            break;
    }
}

// The ladder the blackbody rows are printed at: the visible threshold, a wood flame, the
// old candela's platinum point, Illuminant A, the table's top, and a daylight white.
static const float FIRE_PROBE_KELVIN[] = {800.0f, 1500.0f, 2041.4f, 2856.0f, 4000.0f, 6500.0f};

void fire_probe_print(const FireSystem* fs) {
    for (size_t i = 0; i < sizeof(FIRE_PROBE_KELVIN) / sizeof(FIRE_PROBE_KELVIN[0]); i++) {
        const float k = FIRE_PROBE_KELVIN[i];
        vec3 xyz = {0.0f, 0.0f, 0.0f}, table = {0.0f, 0.0f, 0.0f};
        spectrum_blackbody_xyz(k, xyz);
        const float sum = xyz[0] + xyz[1] + xyz[2];
        const float table_lum = fire_blackbody(k, table);
        printf("fire-probe blackbody kelvin=%.9g x=%.9g y=%.9g lum=%.9g table_lum=%.9g\n",
               (double)k, (double)(xyz[0] / sum), (double)(xyz[1] / sum), (double)xyz[1],
               (double)table_lum);
    }
    if (!fs)
        return;
    printf("fire-probe system count=%d sim_hz=%.9g jacobi=%d maccormack=%d warmup=%.9g\n",
           fs->count, (double)fs->sim_hz, fs->jacobi_iterations, fs->maccormack ? 1 : 0,
           (double)fs->warmup);
    for (int i = 0; i < fs->count; i++) {
        const Fire* f = &fs->fires[i];
        printf("fire-probe fire index=%d kind=%d enabled=%d steps=%d start=%d answered=%d "
               "intensity=%.9g cx=%.9g cy=%.9g cz=%.9g r=%.9g g=%.9g b=%.9g heat=%.9g\n",
               i, (int)f->kind, f->enabled ? 1 : 0, f->steps, f->start_step, f->answered ? 1 : 0,
               (double)f->intensity, (double)f->centroid[0], (double)f->centroid[1],
               (double)f->centroid[2], (double)f->color[0], (double)f->color[1],
               (double)f->color[2], (double)f->heat_release);
    }
}
