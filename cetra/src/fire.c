#include "fire.h"
#include "fire_internal.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json_util.h"
#include "light.h"
#include "material.h"
#include "scene.h"
#include "spectrum.h"
#include "texture.h"
#include "util.h"
#include "wind.h"
#include "ext/cJSON.h"
#include "ext/log.h"

const char* const FIRE_FIELD_NAMES[FIRE_FIELD_COUNT] = {"temperature", "soot", "reaction", "speed",
                                                        "core"};

// Seconds the running mean a fire's vigour is taken against settles over: long against a
// flicker, short against a fire dying down.
#define FIRE_VIGOUR_SECONDS 4.0f

static void _params_defaults(FireParams* p, FireKind kind) {
    memset(p, 0, sizeof(*p));
    p->ambient = 293.0f;
    // The soot in a wood fire's flames measures about 1100-1500 K; a candle's luminous zone runs
    // hotter, near 1700-1800 K.
    p->temperature = kind == FIRE_FLAME ? 1750.0f : 1400.0f;
    p->reaction_rate = 1.0f;
    // TASTE. Visible emission falls about twelvefold for every tenth below the peak, so the
    // flame is the gas within a tenth or so of it: at 600 K/s that lasts a few tenths of a
    // second, which with the lift is a hand's breadth to a forearm of flame.
    p->cooling = 600.0f;
    // TASTE: a third of the hot gas replaced by room air every tenth of a second or so.
    p->entrainment = 3.0f;
    // A frame or two of gas: thin, as the core is (sec. 3.1). TASTE.
    p->core = 0.04f;
    p->expansion = 4.0f;
    // TASTE, against a measurement: soot in wood flames runs to about 1 ppm by volume, and the
    // reacting gas carries this for under a second.
    p->soot_yield = 4.0f;
    // Soot oxidises above about 1000-1300 K wherever there is oxygen; a fire burning clean in a
    // drawing chimney leaves little smoke to see. TASTE in the rates.
    p->soot_burnout = 4.0f;
    p->soot_burnout_at = 1000.0f;
    p->smoke_fade = 1.5f;
    p->buoyancy = 1.0f;
    p->vorticity = 2.0f;
    p->wind_response = 1.0f;
    // 6 pi E(m) / lambda at 550 nm with E(m) = 0.26, per part per million by volume: soot's
    // Rayleigh absorption, about 9 per metre for every ppm.
    p->soot_absorption = 8.9f;
    // Fresh soot absorbs nearly everything it intercepts.
    p->smoke_albedo = 0.25f;
    // TASTE: faint against the soot, as a wood fire's blue is, and the adaptation lifts blue a
    // long way against a white this red; a candle's base shows more.
    p->blue_core = kind == FIRE_FLAME ? 150.0f : 6.0f;
    p->adaptation = 0.85f;
    p->brightness = 1.0f;
    p->flame_soot = 6.0f;
    p->flicker = 0.3f;
}

// XYZ to Hunt-Pointer-Estevez cone responses, normalised to D65 (Fairchild 1998). cglm's matrices
// are column-major: each inner triple is a COLUMN.
static const mat3 XYZ_TO_LMS = {
    {0.4002f, -0.2263f, 0.0f}, {0.7076f, 1.1653f, 0.0f}, {-0.0808f, 0.0457f, 0.9182f}};
static const vec3 D65_XYZ = {0.95047f, 1.0f, 1.08883f};

/*
 * Nguyen's von Kries transform (sec. 5, eq. 23) as a matrix on linear Rec.709: in
 * Hunt-Pointer-Estevez cone space, from the white of a blackbody at the fire's peak temperature
 * to D65, mixed with the identity by how far the eye has adapted, and scaled so that white
 * keeps its luminance -- adaptation changes what colour the fire reads as, not how bright it
 * is. Through the same Rec.709 matrices the blackbody table is built with.
 */
static void _adaptation(const FireParams* p, mat3 out) {
    vec3 white = {0.0f, 0.0f, 0.0f};
    spectrum_blackbody_xyz(p->temperature, white);
    if (!(white[1] > 0.0f)) {
        glm_mat3_identity(out);
        return;
    }
    glm_vec3_scale(white, 1.0f / white[1], white);
    vec3 lms_white = {0.0f, 0.0f, 0.0f}, lms_d65 = {0.0f, 0.0f, 0.0f};
    glm_mat3_mulv((vec3*)XYZ_TO_LMS, white, lms_white);
    glm_mat3_mulv((vec3*)XYZ_TO_LMS, (float*)D65_XYZ, lms_d65);
    mat3 scale = GLM_MAT3_ZERO_INIT;
    for (int c = 0; c < 3; c++)
        scale[c][c] = lms_d65[c] / lms_white[c];
    mat3 lms_to_xyz, a;
    mat3 xyz_to_rgb = GLM_MAT3_ZERO_INIT, rgb_to_xyz = GLM_MAT3_ZERO_INIT;
    glm_mat3_inv((vec3*)XYZ_TO_LMS, lms_to_xyz);
    spectrum_xyz_to_rec709_matrix(xyz_to_rgb);
    spectrum_rec709_to_xyz_matrix(rgb_to_xyz);
    glm_mat3_mul(scale, (vec3*)XYZ_TO_LMS, a);
    glm_mat3_mul(lms_to_xyz, a, a);
    glm_mat3_mul(xyz_to_rgb, a, a);
    glm_mat3_mul(a, rgb_to_xyz, a);
    const float k = glm_clamp(p->adaptation, 0.0f, 1.0f);
    for (int c = 0; c < 3; c++)
        for (int r = 0; r < 3; r++)
            a[c][r] = (r == c ? 1.0f - k : 0.0f) + k * a[c][r];
    vec3 white_rgb = {0.0f, 0.0f, 0.0f}, adapted = {0.0f, 0.0f, 0.0f};
    glm_mat3_mulv(xyz_to_rgb, white, white_rgb);
    glm_mat3_mulv(a, white_rgb, adapted);
    const float after = spectrum_luminance(adapted);
    glm_mat3_scale(a, after > 0.0f ? spectrum_luminance(white_rgb) / after : 1.0f);
    glm_mat3_copy(a, out);
}

void fire_system_init(FireSystem* fs) {
    memset(fs, 0, sizeof(*fs));
    fs->sim_hz = 60.0f;
    // A slow frame skips the time it lost rather than spending the next frame catching up,
    // which would make it slower still.
    fs->max_steps = 2;
    fs->jacobi_iterations = 24;
    fs->maccormack = true;
    fs->warmup = 2.0f;
    fs->debug_field = -1;
}

FireSystem* create_fire_system(void) {
    FireSystem* fs = malloc(sizeof(FireSystem));
    if (!fs) {
        log_error("Failed to allocate FireSystem");
        return NULL;
    }
    fire_system_init(fs);
    return fs;
}

// What a flipbook read, released, and its hold on the sheet with it.
static void _flipbook_release(FireFlipbook* b) {
    texture_release(b->sheet);
    free(b->intensity);
    free(b->centroid_y);
    memset(b, 0, sizeof(*b));
}

void free_fire_system(FireSystem* fs) {
    if (!fs)
        return;
    for (int i = 0; i < fs->count; i++)
        _flipbook_release(&fs->fires[i].flipbook);
    free(fs);
}

FireSource fire_source_default(void) {
    return (FireSource){.shape = FIRE_SHAPE_BOX,
                        .b = {0.05f, 0.05f, 0.05f},
                        .radius = 0.05f,
                        .coverage = 0.5f,
                        .lift = 0.5f};
}

bool fire_set_flipbook(Fire* fire, TexturePool* pool, const char* path) {
    if (!fire)
        return false;
    FireFlipbook* b = &fire->flipbook;
    _flipbook_release(b);
    if (!path || !path[0])
        return false;
    snprintf(b->path, sizeof(b->path), "%s", path);
    char* text = read_entire_file(path, NULL);
    cJSON* root = text ? cJSON_Parse(text) : NULL;
    free(text);
    if (!root) {
        log_error("Fire: '%s' has no flipbook sidecar at %s", fire->name, path);
        return false;
    }
    b->frames = json_int_or(root, "frames", 0);
    b->cols = json_int_or(root, "cols", 0);
    b->rows = json_int_or(root, "rows", 0);
    b->width = json_int_or(root, "width", 0);
    b->height = json_int_or(root, "height", 0);
    b->fps = json_float_or(root, "fps", 30.0f);
    b->peak_nits = json_float_or(root, "peak_nits", 1.0f);
    float box[2] = {1.0f, 1.0f};
    json_floats(root, "box", box, 2);
    glm_vec2_copy(box, b->box);
    glm_vec3_one(b->color);
    json_floats(root, "color", b->color, 3);
    // The sheet sits beside its sidecar.
    const char* sheet = json_string_or(root, "sheet");
    char sheet_path[sizeof(b->path) + 64] = "";
    bool ok = sheet && b->frames > 0 && b->cols > 0 && b->rows > 0;
    if (ok) {
        const char* slash = path_last_sep(path);
        if (slash)
            snprintf(sheet_path, sizeof(sheet_path), "%.*s/%s", (int)(slash - path), path, sheet);
        else
            snprintf(sheet_path, sizeof(sheet_path), "%s", sheet);
        b->intensity = calloc((size_t)b->frames, sizeof(float));
        b->centroid_y = calloc((size_t)b->frames, sizeof(float));
        ok = b->intensity && b->centroid_y &&
             json_floats(root, "intensity", b->intensity, b->frames) &&
             json_floats(root, "centroid_y", b->centroid_y, b->frames);
    }
    cJSON_Delete(root);
    if (!ok) {
        log_error("Fire: '%s''s flipbook sidecar %s is incomplete", fire->name, path);
        _flipbook_release(b);
        return false;
    }
    // Premultiplied, so its alpha is coverage and not a cutout's to dilate; clamped, so the
    // sheet's outer frames do not filter in the far edge's.
    Texture* tex = texture_load_path(
        pool, sheet_path,
        (TextureDesc){.is_srgb = true, .alpha = TEXTURE_ALPHA_DATA, .use = TEXTURE_USE_COLOUR});
    if (!tex) {
        log_error("Fire: '%s' has no flipbook sheet at %s", fire->name, sheet_path);
        _flipbook_release(b);
        return false;
    }
    texture_apply_wrap(tex, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE);
    b->sheet = texture_retain(tex);
    for (int f = 0; f < b->frames; f++)
        b->mean_intensity += b->intensity[f] / (float)b->frames;
    return true;
}

void fire_set_embers(Fire* fire, Material* embers) {
    if (!fire)
        return;
    if (fire->embers && fire->embers != embers)
        fire->embers->emissive_drive = 1.0f;
    fire->embers = embers;
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
    _params_defaults(&fire->params, kind);
    glm_vec3_one(fire->color);
    glm_mat3_identity(fire->adaptation);
    fire->vigour = 1.0f;
    switch (kind) {
        case FIRE_GRID:
            glm_vec3_copy((vec3){0.8f, 1.2f, 0.8f}, fire->grid.size);
            fire->grid.cell = 0.025f;
            fire->grid.floor = true;
            break;
        case FIRE_FLAME:
            fire->flame.width = 0.012f;
            fire->flame.height = 0.035f;
            break;
        case FIRE_FLIPBOOK:
            break;
    }
    return fire;
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
float fire_grid_cell(const Fire* fire) {
    return fire->grid.cell > 0.0f ? fire->grid.cell : 0.025f;
}

void fire_grid_cells(const Fire* fire, int cells[3]) {
    const float cell = fire_grid_cell(fire);
    cells[0] = _cells(fire->grid.size[0], cell, FIRE_GRID_MAX_X);
    cells[1] = _cells(fire->grid.size[1], cell, FIRE_GRID_MAX_Y);
    cells[2] = _cells(fire->grid.size[2], cell, FIRE_GRID_MAX_Z);
}

void fire_grid_bounds(const Fire* fire, vec3 min, vec3 max) {
    const float cell = fire_grid_cell(fire);
    int n[3];
    fire_grid_cells(fire, n);
    for (int a = 0; a < 3; a++) {
        min[a] = fire->grid.center[a] - 0.5f * (float)n[a] * cell;
        max[a] = fire->grid.center[a] + 0.5f * (float)n[a] * cell;
    }
}

double fire_card_frame(const FireFlipbook* b, const FireCard* card, double t) {
    double pos = fmod(t * (double)b->fps + (double)card->phase * (double)b->frames, b->frames);
    return pos < 0.0 ? pos + (double)b->frames : pos;
}

void fire_card_size(const Fire* fire, const FireCard* card, vec2 out) {
    if (card->size[0] > 0.0f && card->size[1] > 0.0f)
        glm_vec2_copy((float*)card->size, out);
    else
        glm_vec2_copy((float*)fire->flipbook.box, out);
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
            // Unclamped: below about 1900 K a blackbody lies outside Rec.709 on the red side and
            // its blue is negative, and the adaptation has to see that negative to bring the
            // fire's white back to white. The clamp comes after it.
            const float y = fmaxf(xyz[1], 1e-30f);
            for (int c = 0; c < 3; c++)
                g_blackbody[4 * i + c] = rgb[c] / y;
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

const float* fire_blue_color(void) {
    // The reaction zone's own light is band emission from intermediate radicals (Nguyen et al.
    // sec. 3): CH* at 431 nm and C2*'s Swan band at 516 nm, taken here as equal parts of
    // radiance, through the observer -- unclamped, like the blackbody, and luminance 1.
    static vec3 blue;
    static bool built;
    if (!built) {
        vec3 ch = {0.0f, 0.0f, 0.0f}, c2 = {0.0f, 0.0f, 0.0f}, xyz = {0.0f, 0.0f, 0.0f};
        spectrum_cie_xyz(431.0f, ch);
        spectrum_cie_xyz(516.0f, c2);
        glm_vec3_add(ch, c2, xyz);
        glm_vec3_scale(xyz, 1.0f / xyz[1], xyz);
        spectrum_xyz_to_rec709(xyz, blue);
        built = true;
    }
    return blue;
}

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
    *kelvin = p->ambient + (p->temperature - p->ambient) * (1.0f - 0.35f * q * q) *
                               (1.0f - 0.4f * glm_smoothstep(0.6f, 1.0f, u));
    // The blue is the reaction zone, a thin shell round the base: strongest at the flame's
    // edge, where fuel meets air, and gone by the time soot has formed.
    *blue = inside * q * q * (1.0f - glm_smoothstep(0.05f, 0.3f, u));
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

// A FLAME's wick, in the world.
static void _flame_wick(const Fire* fire, vec3 out) {
    glm_vec3_add((float*)fire->origin, (float*)fire->flame.wick, out);
}

static void _flame_rest(Fire* fire) {
    vec3 wick = {0.0f, 0.0f, 0.0f};
    _flame_wick(fire, wick);
    for (int i = 0; i < FIRE_SPINE_POINTS; i++) {
        const float u = (float)i / (float)(FIRE_SPINE_POINTS - 1);
        fire->spine[i][0] = wick[0];
        fire->spine[i][1] = wick[1] + u * fire->flame.height;
        fire->spine[i][2] = wick[2];
        fire->spine[i][3] = 0.5f * fire->flame.width * _flame_radius(u);
        glm_vec3_zero(fire->spine_velocity[i]);
    }
}

/*
 * One fixed step of a FLAME's spine. Each point is held a rest length above the one below
 * it by a stiff spring, so the flame keeps its height, and sideways by a soft one, so it
 * sways; the wind leans it, and seeded impulses -- a draught, the wick's own puffing -- set it
 * flickering. The base stays on the wick, so a flame carried along trails behind it.
 */
static void _flame_step(Fire* fire, int index, const vec3 wind, float dt) {
    const FireParams* p = &fire->params;
    const float seg = fire->flame.height / (float)(FIRE_SPINE_POINTS - 1);
    // Height breathes with the flicker: a flame that pulls itself up and drops back.
    const float stretch = 1.0f + 0.15f * p->flicker * _hash_signed((uint32_t)fire->steps, 977u);
    _flame_wick(fire, fire->spine[0]);
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
        fire->spine[i][3] = 0.5f * fire->flame.width * _flame_radius(u);
    }
    fire->spine[0][3] = 0.0f;
}

// Radial and lengthwise samples of the profile the light integrates.
#define FLAME_QUAD_U 24
#define FLAME_QUAD_Q 12

// What a FLAME casts, from its spine: intensity, centroid and colour. Exact against what is
// drawn, since the same profile is integrated.
static void _flame_light(Fire* fire) {
    const FireParams* p = &fire->params;
    const float* blue_rgb = fire_blue_color();
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
            const float glow = p->blue_core * blue;
            const float area = 2.0f * GLM_PIf * radius * radius * q / (float)FLAME_QUAD_Q;
            ring_lum += (sigma * lum + glow) * area;
            glm_vec3_muladds(rgb, sigma * area, ring_rgb);
            glm_vec3_muladds((float*)blue_rgb, glow * area, ring_rgb);
        }
        const float dl = du_len / (float)FLAME_QUAD_U;
        total += ring_lum * dl;
        glm_vec3_muladds(ring_rgb, dl, rgb_sum);
        glm_vec3_muladds(at, ring_lum * dl, weighted);
    }
    fire->intensity = total * p->brightness;
    if (total > 0.0f) {
        glm_vec3_scale(weighted, 1.0f / total, fire->centroid);
        // The light takes the colour the flame is drawn in, adapted as it is, then clamped into
        // the gamut a light can carry.
        glm_mat3_mulv(fire->adaptation, rgb_sum, rgb_sum);
        glm_vec3_maxv(rgb_sum, GLM_VEC3_ZERO, rgb_sum);
        const float lum = spectrum_luminance(rgb_sum);
        if (lum > 0.0f)
            glm_vec3_scale(rgb_sum, 1.0f / lum, fire->color);
    } else {
        _flame_wick(fire, fire->centroid);
    }
    fire->heat_release = 0.0f;
    fire->answered = true;
}

/*
 * What a FLIPBOOK fire casts: each card's baked intensity at its own frame, scaled by its area
 * against the area the frame was made at, summed -- a card twice as wide and tall is four times
 * the light -- at the cards' intensity-weighted centroid, in the sheet's colour. Its loop's mean
 * is known, so that is what its vigour is taken against.
 */
static void _flipbook_light(Fire* fire, double t) {
    const FireFlipbook* b = &fire->flipbook;
    float total = 0.0f, mean = 0.0f;
    vec3 weighted = GLM_VEC3_ZERO_INIT;
    for (int c = 0; c < fire->cards.count; c++) {
        const FireCard* card = &fire->cards.list[c];
        const double pos = fire_card_frame(b, card, t);
        const int f0 = (int)pos % b->frames;
        const int f1 = (f0 + 1) % b->frames;
        const float blend = (float)(pos - floor(pos));
        vec2 size = {0.0f, 0.0f};
        fire_card_size(fire, card, size);
        const float scale = size[0] * size[1] / fmaxf(b->box[0] * b->box[1], 1e-6f);
        const float i = (b->intensity[f0] + (b->intensity[f1] - b->intensity[f0]) * blend) * scale;
        const float y = b->centroid_y[f0] + (b->centroid_y[f1] - b->centroid_y[f0]) * blend;
        total += i;
        mean += b->mean_intensity * scale;
        vec3 at = {0.0f, 0.0f, 0.0f};
        glm_vec3_add((float*)fire->origin, (float*)card->base, at);
        at[1] += y * size[1];
        glm_vec3_muladds(at, i, weighted);
    }
    fire->intensity = total * fire->params.brightness;
    fire->mean_intensity = mean * fire->params.brightness;
    if (total > 0.0f)
        glm_vec3_scale(weighted, 1.0f / total, fire->centroid);
    else
        glm_vec3_copy(fire->origin, fire->centroid);
    glm_vec3_copy((float*)b->color, fire->color);
    fire->heat_release = 0.0f;
    fire->answered = true;
}

// The air a scene's wind moves at time `t`, m/s, horizontal: what blows through a fire. Zero
// for no wind, or one that states no air speed.
static void _wind_air(const Wind* wind, double t, vec3 out) {
    glm_vec3_zero(out);
    if (!wind || !(wind->air_speed > 0.0f))
        return;
    vec3 across = {wind->direction[0], 0.0f, wind->direction[2]};
    const float len = glm_vec3_norm(across);
    if (len > 1e-6f)
        glm_vec3_scale(across, wind->air_speed * wind_gust(wind, (float)t) / len, out);
}

// The steps a GRID or FLAME fire owes at the clock's step `target`, and the fire started.
static int _steps_due(Fire* fire, const FireSystem* fs, int target) {
    if (!fire->started) {
        // A fire that starts has already burnt `warmup` seconds, taken in one go and uncapped,
        // so the scene opens on it burning.
        const int warm = (int)lroundf(fmaxf(fs->warmup, 0.0f) * fs->sim_hz);
        fire->started = true;
        fire->start_step = target - warm;
        fire->steps = 0;
        if (fire->kind == FIRE_FLAME)
            _flame_rest(fire);
        return warm;
    }
    int due = target - fire->start_step - fire->steps;
    if (due > fs->max_steps) {
        // Behind by more than a frame may catch up: skip the rest. A fire that stalled for a
        // second resumes rather than racing through it.
        fire->steps += due - fs->max_steps;
        due = fs->max_steps;
    }
    return due > 0 ? due : 0;
}

void fire_update(FireSystem* fs, const Wind* wind, double t) {
    if (!fs || !(fs->sim_hz > 0.0f))
        return;
    const float dt = 1.0f / fs->sim_hz;
    // The step the clock is on, from the absolute time rather than an accumulator, so a
    // headless run takes exactly the same steps however its frames are paced.
    const int target = (int)floor(t * (double)fs->sim_hz + 1e-6);
    _wind_air(wind, t, fs->air);
    for (int i = 0; i < fs->count; i++) {
        Fire* fire = &fs->fires[i];
        if (fire->node)
            glm_vec3_copy(fire->node->global_transform[3], fire->origin);
        else
            glm_vec3_zero(fire->origin);
        fire->pending = 0;
        if (!fire->enabled)
            continue;
        if (fire->kind == FIRE_FLIPBOOK) {
            // Nothing to step: the frame is a function of the clock.
            fire->started = true;
            if (fire->flipbook.sheet)
                _flipbook_light(fire, t);
            continue;
        }
        const FireParams* p = &fire->params;
        if (fire->adapted_for[0] != p->temperature || fire->adapted_for[1] != p->adaptation) {
            _adaptation(p, fire->adaptation);
            fire->adapted_for[0] = p->temperature;
            fire->adapted_for[1] = p->adaptation;
        }
        fire->pending = _steps_due(fire, fs, target);
        if (fire->kind == FIRE_FLAME) {
            const bool stepped = fire->pending > 0;
            for (int s = 0; s < fire->pending; s++) {
                _flame_step(fire, i, fs->air, dt);
                fire->steps++;
            }
            fire->pending = 0;
            if (stepped || !fire->answered)
                _flame_light(fire);
        }
    }
}

// The fire's light onto its light: a point or spot gets the intensity in candela at the
// centroid, placed in the frame of the node it hangs on; an area panel gets the luminance that
// gives the same intensity along its normal, and keeps its place.
static void _drive_light(Fire* fire, SceneNode* root) {
    Light* light = fire->light;
    if (!light || !fire->answered)
        return;
    if (fire->light_seen != light) {
        fire->light_node = node_find_light(root, light);
        fire->light_seen = light;
    }
    const float intensity = fire->enabled ? fmaxf(fire->intensity, 0.0f) : 0.0f;
    glm_vec3_copy(fire->color, light->color);
    switch (light->type) {
        case LIGHT_POINT:
        case LIGHT_SPOT: {
            // Stored in the type's canonical unit, candela, whatever the light was authored in.
            light->intensity = intensity;
            vec3 at = {0.0f, 0.0f, 0.0f}, local = {0.0f, 0.0f, 0.0f};
            glm_vec3_add(fire->centroid, fire->light_offset, at);
            glm_vec3_copy(at, local);
            if (fire->light_node) {
                mat4 inv;
                glm_mat4_inv(fire->light_node->global_transform, inv);
                glm_mat4_mulv3(inv, at, 1.0f, local);
            }
            // The authored copy in its node's frame, which the next walk carries back to `at`,
            // and this frame's world copy, since this frame's walk has already run.
            light_set_position(light, local);
            glm_vec3_copy(at, light->global_position);
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

void fire_system_drive(FireSystem* fs, SceneNode* root, float dt) {
    if (!fs)
        return;
    const float follow = 1.0f - expf(-fmaxf(dt, 0.0f) / FIRE_VIGOUR_SECONDS);
    for (int i = 0; i < fs->count; i++) {
        Fire* fire = &fs->fires[i];
        if (fire->answered) {
            // A flipbook's mean is its loop's; the others' is a running one.
            if (fire->kind != FIRE_FLIPBOOK) {
                if (fire->mean_intensity > 0.0f)
                    fire->mean_intensity += (fire->intensity - fire->mean_intensity) * follow;
                else
                    fire->mean_intensity = fire->intensity;
            }
            fire->vigour =
                fire->mean_intensity > 0.0f ? fire->intensity / fire->mean_intensity : 1.0f;
        }
        _drive_light(fire, root);
        if (fire->embers)
            fire->embers->emissive_drive = fire->enabled ? fire->vigour : 0.0f;
    }
}

FireAnswer fire_answer_decode(const float t[8], const vec3 fallback_centroid) {
    FireAnswer a = {.intensity = t[0], .color = {1.0f, 1.0f, 1.0f}, .heat_release = t[7]};
    if (t[0] > 0.0f)
        glm_vec3_scale((vec3){t[1], t[2], t[3]}, 1.0f / t[0], a.centroid);
    else
        glm_vec3_copy((float*)fallback_centroid, a.centroid);
    const vec3 rgb = {t[4], t[5], t[6]};
    const float lum = spectrum_luminance(rgb);
    if (lum > 0.0f)
        glm_vec3_scale((float*)rgb, 1.0f / lum, a.color);
    return a;
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
        // The adaptation as a matrix, and what it makes of the peak's blackbody (which must come
        // out white) and of one 20% cooler.
        const Fire* f = &fs->fires[i];
        vec3 peak = {0.0f, 0.0f, 0.0f}, cool = {0.0f, 0.0f, 0.0f};
        const float t_peak = f->params.temperature;
        fire_blackbody(t_peak, peak);
        fire_blackbody(0.8f * t_peak, cool);
        glm_mat3_mulv((vec3*)f->adaptation, peak, peak);
        glm_mat3_mulv((vec3*)f->adaptation, cool, cool);
        glm_vec3_scale(peak, 1.0f / fmaxf(glm_vec3_max(peak), 1e-30f), peak);
        glm_vec3_scale(cool, 1.0f / fmaxf(glm_vec3_max(cool), 1e-30f), cool);
        printf("fire-probe adaptation index=%d r0=%.4g,%.4g,%.4g r1=%.4g,%.4g,%.4g "
               "r2=%.4g,%.4g,%.4g peak=%.4g,%.4g,%.4g cooler=%.4g,%.4g,%.4g\n",
               i, (double)f->adaptation[0][0], (double)f->adaptation[1][0],
               (double)f->adaptation[2][0], (double)f->adaptation[0][1],
               (double)f->adaptation[1][1], (double)f->adaptation[2][1],
               (double)f->adaptation[0][2], (double)f->adaptation[1][2],
               (double)f->adaptation[2][2], (double)peak[0], (double)peak[1], (double)peak[2],
               (double)cool[0], (double)cool[1], (double)cool[2]);
    }
    for (int i = 0; i < fs->count; i++) {
        const Fire* f = &fs->fires[i];
        printf("fire-probe fire index=%d kind=%d enabled=%d steps=%d start=%d answered=%d "
               "intensity=%.9g cx=%.9g cy=%.9g cz=%.9g r=%.9g g=%.9g b=%.9g heat=%.9g "
               "vigour=%.9g\n",
               i, (int)f->kind, f->enabled ? 1 : 0, f->steps, f->start_step, f->answered ? 1 : 0,
               (double)f->intensity, (double)f->centroid[0], (double)f->centroid[1],
               (double)f->centroid[2], (double)f->color[0], (double)f->color[1],
               (double)f->color[2], (double)f->heat_release, (double)f->vigour);
    }
}
