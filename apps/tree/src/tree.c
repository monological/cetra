#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <cglm/cglm.h>

#include "cetra/common.h"
#include "cetra/mesh.h"
#include "cetra/shader.h"
#include "cetra/program.h"
#include "cetra/scene.h"
#include "cetra/util.h"
#include "cetra/engine.h"
#include "cetra/render.h"
#include "cetra/geometry.h"
#include "cetra/transform.h"
#include "cetra/light.h"
#include "cetra/texture.h"
#include "cetra/app.h"
#include "cetra/sky.h"
#include "cetra/ibl.h"
#include "cetra/shadow.h"
#include "cetra/wind.h"
#include "cetra/postfx.h"
#include "cetra/particle_system.h"
#include "cetra/particle_emitter.h"
#include "cetra/particle_module.h"
#include "cetra/particle_renderer.h"
#include "cetra/particle_sim.h"

#include "tree_gen.h"
#include "ground.h"
#include "grass.h"

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include "cimgui.h"

#define TEXTURE_SIZE      512
#define BARK_TEXTURE_SIZE 1024

/*
 * Perlin Noise Implementation
 */
static int perm[512];
static int perm_initialized = 0;

static void init_perlin(unsigned int seed) {
    srand(seed);
    int p[256];
    for (int i = 0; i < 256; i++)
        p[i] = i;
    for (int i = 255; i > 0; i--) {
        int j = rand() % (i + 1);
        int tmp = p[i];
        p[i] = p[j];
        p[j] = tmp;
    }
    for (int i = 0; i < 512; i++)
        perm[i] = p[i & 255];
    perm_initialized = 1;
}

static float fade(float t) {
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

static float lerp_f(float a, float b, float t) {
    return a + t * (b - a);
}

static float grad(int hash, float x, float y) {
    int h = hash & 7;
    float u = h < 4 ? x : y;
    float v = h < 4 ? y : x;
    return ((h & 1) ? -u : u) + ((h & 2) ? -2.0f * v : 2.0f * v);
}

static float perlin_noise_2d(float x, float y) {
    if (!perm_initialized)
        init_perlin(12345);

    int xi = (int)floorf(x) & 255;
    int yi = (int)floorf(y) & 255;
    float xf = x - floorf(x);
    float yf = y - floorf(y);

    float u = fade(xf);
    float v = fade(yf);

    int aa = perm[perm[xi] + yi];
    int ab = perm[perm[xi] + yi + 1];
    int ba = perm[perm[xi + 1] + yi];
    int bb = perm[perm[xi + 1] + yi + 1];

    float x1 = lerp_f(grad(aa, xf, yf), grad(ba, xf - 1, yf), u);
    float x2 = lerp_f(grad(ab, xf, yf - 1), grad(bb, xf - 1, yf - 1), u);

    return (lerp_f(x1, x2, v) + 1.0f) * 0.5f;
}

static float fbm_noise(float x, float y, int octaves, float persistence) {
    float total = 0.0f;
    float amplitude = 1.0f;
    float frequency = 1.0f;
    float max_value = 0.0f;

    for (int i = 0; i < octaves; i++) {
        total += perlin_noise_2d(x * frequency, y * frequency) * amplitude;
        max_value += amplitude;
        amplitude *= persistence;
        frequency *= 2.0f;
    }

    return total / max_value;
}

/*
 * Worley (Cellular) Noise for bark cracks
 */
static float worley_noise_2d(float x, float y, unsigned int seed) {
    int xi = (int)floorf(x);
    int yi = (int)floorf(y);

    float min_dist = 999.0f;

    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            int cx = xi + dx;
            int cy = yi + dy;

            // Hash cell to get feature point
            unsigned int h = (unsigned int)(cx * 374761393 + cy * 668265263 + seed);
            h = (h ^ (h >> 13)) * 1274126177;

            float fx = (float)cx + (float)(h & 0xFFFF) / 65536.0f;
            float fy = (float)cy + (float)((h >> 16) & 0xFFFF) / 65536.0f;

            float dist = (x - fx) * (x - fx) + (y - fy) * (y - fy);
            if (dist < min_dist)
                min_dist = dist;
        }
    }

    return sqrtf(min_dist);
}

/*
 * Generate bark albedo texture
 */
static float smoothstep(float edge0, float edge1, float x) {
    x = fmaxf(0.0f, fminf(1.0f, (x - edge0) / (edge1 - edge0)));
    return x * x * (3.0f - 2.0f * x);
}

// The bark relief, in [0,1]: 0 deep in a fissure, 1 on a plate face. Every bark
// map derives from this one field, so the albedo's dark cracks, the normal
// map's ridges and the POM displacement all describe the same surface instead
// of three independently-noisy ones that happen to sit on the same texel.
//
// The structure that matters is LARGE: fissures running the length of the trunk
// with plates between them. Fine mottling alone -- what this used to be -- has
// no feature big enough to survive minification, so the trunk averages out to
// flat colour at any real viewing distance.
static void bark_height_field(float* out, int width, int height) {
    init_perlin(42);

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            float u = (float)x / width;
            float v = (float)y / height;

            // Warp the fissure coordinate so the cracks wander up the trunk
            // rather than running as straight parallel stripes.
            float warp = (fbm_noise(u * 2.5f, v * 0.9f, 4, 0.5f) - 0.5f) * 0.9f;

            // Vertical fissures. The ridged |sin| gives sharp valleys between
            // broad plates; the power sharpens the valley floor.
            // Narrow V-grooves, not broad swells. The normal map is a finite
            // difference between adjacent texels, so a fissure a hundred texels
            // wide has a per-texel slope near zero and lights up flat no matter
            // how strong the map is. Compressing the transition into a fraction
            // of the period is what actually produces a normal to catch light.
            float fissure = fabsf(sinf((u * 5.0f + warp) * (float)M_PI));
            fissure = smoothstep(0.0f, 0.22f, fissure);

            // Vary the plates along their length, gently -- break them up hard
            // and the fissures stop reading as continuous grooves and turn into
            // a scatter of dashes.
            float along = fbm_noise(u * 3.0f, v * 1.6f, 4, 0.55f);
            fissure *= 0.74f + 0.26f * along;

            // Cellular plate boundaries, stretched vertically like real bark.
            // Tight edges for the same reason as the fissures above.
            float cell = worley_noise_2d(u * 5.0f, v * 2.0f, 91);
            float plate = smoothstep(0.02f, 0.14f, cell);

            float h = fissure * 0.62f + plate * 0.38f;

            // Sparse cross-breaks. Bark does split across the grain, but never
            // at a regular spacing, so these are the CONTOUR of a noise field
            // rather than a wave: the level set of a field that varies quickly
            // up the trunk and slowly around it runs horizontally, wanders, and
            // reappears at irregular heights with no period to detect. (A wave
            // cannot do this -- warping shifts each crack but leaves the average
            // spacing intact, which reads as brickwork.)
            float across_field = fbm_noise(u * 2.2f, v * 6.0f, 4, 0.5f);
            float contour = fabsf(across_field - 0.5f);
            float split = smoothstep(0.0f, 0.060f, contour);

            // A second, slower field decides where breaks are allowed at all,
            // so most of the trunk carries none and a few stretches carry one.
            float gate = smoothstep(0.56f, 0.78f,
                                    fbm_noise(u * 1.3f + 31.0f, v * 1.1f + 17.0f, 3, 0.5f));
            split = 1.0f - (1.0f - split) * gate;

            // Shallower than the vertical fissures: a cross-break interrupts a
            // plate, it does not cut the trunk open.
            h *= 0.44f + 0.56f * split;

            // Fine grain on top, small enough to only matter up close.
            h += (fbm_noise(u * 22.0f, v * 7.0f, 3, 0.5f) - 0.5f) * 0.14f;

            out[y * width + x] = fmaxf(0.0f, fminf(1.0f, h));
        }
    }
}

static unsigned char* generate_bark_albedo(int width, int height, const float* field) {
    unsigned char* data = malloc((size_t)width * height * 3);
    if (!data)
        return NULL;

    init_perlin(7);

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            float u = (float)x / width;
            float v = (float)y / height;

            // Widen the grooves for colour purposes by taking the darkest
            // sample across a short horizontal span. The relief needs sharp
            // edges to produce a normal at all, but a one-texel-wide dark line
            // vanishes the moment the texture is minified -- so the shading
            // keeps the sharp field while the colour gets bands broad enough to
            // still read from across the field.
            float h = field[y * width + x];
            for (int d = -7; d <= 7; d += 2) {
                int xs = ((x + d) % width + width) % width;
                float s = field[y * width + xs];
                if (s < h)
                    h = s;
            }

            // Weathered grey-brown on the exposed plates, dark damp wood down
            // in the fissures. The spread between the two is what actually
            // reads as bark from a distance.
            const float fissure_rgb[3] = {0.040f, 0.030f, 0.024f};
            const float plate_rgb[3] = {0.28f, 0.215f, 0.150f};

            float t = smoothstep(0.20f, 0.86f, h);
            float r = fissure_rgb[0] + (plate_rgb[0] - fissure_rgb[0]) * t;
            float g = fissure_rgb[1] + (plate_rgb[1] - fissure_rgb[1]) * t;
            float b = fissure_rgb[2] + (plate_rgb[2] - fissure_rgb[2]) * t;

            // Patchy lichen-ish greying on the plate faces only.
            float lichen = fbm_noise(u * 6.0f, v * 3.0f, 4, 0.5f);
            float grey = smoothstep(0.58f, 0.88f, lichen) * t * 0.40f;
            r += (0.26f - r) * grey;
            g += (0.28f - g) * grey;
            b += (0.24f - b) * grey;

            // Fine tonal break-up so the plates are not flat swatches.
            float grain = (fbm_noise(u * 30.0f, v * 10.0f, 3, 0.5f) - 0.5f) * 0.09f;
            r += grain;
            g += grain * 0.85f;
            b += grain * 0.7f;

            int idx = (y * width + x) * 3;
            data[idx + 0] = (unsigned char)(fmaxf(0.0f, fminf(1.0f, r)) * 255);
            data[idx + 1] = (unsigned char)(fmaxf(0.0f, fminf(1.0f, g)) * 255);
            data[idx + 2] = (unsigned char)(fmaxf(0.0f, fminf(1.0f, b)) * 255);
        }
    }

    return data;
}

// Differentiate the shared relief. Strong, because the fissures are the whole
// point: a timid normal map on a cylinder is indistinguishable from no normal
// map at all.
static unsigned char* generate_bark_normal(int width, int height, const float* field) {
    unsigned char* data = malloc((size_t)width * height * 3);
    if (!data)
        return NULL;

    // Tuned against the tile size: one tile spans TG_BARK_TILE world units
    // across 1024 texels, so this is roughly what a couple of centimetres of
    // bark relief works out to as a per-texel slope.
    const float strength = 14.0f;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int x0 = (x - 1 + width) % width;
            int x1 = (x + 1) % width;
            int y0 = (y - 1 + height) % height;
            int y1 = (y + 1) % height;

            float dX = field[y * width + x1] - field[y * width + x0];
            float dY = field[y1 * width + x] - field[y0 * width + x];

            vec3 normal = {-dX * strength, -dY * strength, 1.0f};
            glm_vec3_normalize(normal);

            int idx = (y * width + x) * 3;
            data[idx + 0] = (unsigned char)((normal[0] * 0.5f + 0.5f) * 255);
            data[idx + 1] = (unsigned char)((normal[1] * 0.5f + 0.5f) * 255);
            data[idx + 2] = (unsigned char)((normal[2] * 0.5f + 0.5f) * 255);
        }
    }

    return data;
}

// The same relief as a scalar, so POM can march it and the plates occlude each
// other at grazing angles instead of being a flat surface wearing a picture.
static unsigned char* generate_bark_height(int width, int height, const float* field) {
    unsigned char* data = malloc((size_t)width * height);
    if (!data)
        return NULL;
    for (int i = 0; i < width * height; i++)
        data[i] = (unsigned char)(field[i] * 255);
    return data;
}

/*
 * Generate bark roughness map
 */
// NB three channels, with the value replicated. The mask array is sampled with
// glTF ORM semantics -- roughness comes from GREEN (pbr_frag.glsl), metallic
// from blue, occlusion from red -- and a single-channel source samples as
// (r, 0, 0, 1), i.e. roughness 0, a mirror. Replicating keeps the map correct
// whichever slot it is bound to.
static unsigned char* generate_bark_roughness(int width, int height, const float* field) {
    unsigned char* data = malloc((size_t)width * height * 3);
    if (!data)
        return NULL;

    for (int i = 0; i < width * height; i++) {
        // Weathered plate faces are the roughest; the sheltered fissures keep a
        // little more sheen. All of it is rough -- wood is never glossy.
        float roughness = 0.72f + 0.26f * field[i];
        unsigned char q = (unsigned char)(fminf(1.0f, roughness) * 255);
        data[i * 3 + 0] = q;
        data[i * 3 + 1] = q;
        data[i * 3 + 2] = q;
    }

    return data;
}

/*
 * Leaf cluster atlas
 *
 * A production foliage card carries a whole sprig, not one leaf: several
 * overlapping leaves baked into one alpha texture, so a single quad buys an
 * irregular silhouette and the canopy densifies without more geometry. One leaf
 * per quad is what made the old canopy read as a pile of ovals.
 *
 * The atlas tiles along U only. The wind shader uses UV0.y as the flutter
 * weight so a card pivots about its stem at v = 0; splitting V across rows
 * would move that pivot and scale flutter differently per row.
 *
 * Albedo, normal, and roughness are rasterized in one pass so all three agree
 * per texel -- each leaf writes its own vein ridge and its own cuticle
 * roughness at the same time it writes its color.
 */

// Leaves per cluster and clusters per atlas.
#define LEAVES_PER_CLUSTER 6
#define LEAF_VARIANTS      TG_LEAF_VARIANTS

// Half-width of a leaf blade at `t` along its length (0 = stem, 1 = tip), in
// units of the blade's half-length. Widest just under halfway, tapering to a
// point, with a toothed edge and a thin petiole at the base.
static float leaf_half_width(float t, float skew) {
    if (t < 0.10f)
        return 0.035f; // petiole: a stalk, so the blade attaches to something
    float body = sinf((float)M_PI * powf(t, 0.55f));
    float serration = 1.0f + 0.035f * sinf(t * 14.0f * (float)M_PI);
    return 0.72f * body * serration * (1.0f + skew);
}

// Texture-space randomness only. The tree generator keeps its own stream
// (tree_gen.c) so shape never depends on how much texture work ran first.
static float rand_range(float min_val, float max_val) {
    return min_val + (float)rand() / (float)RAND_MAX * (max_val - min_val);
}

// Secondary veins, as a 0..1 mask. Each one leaves the midrib at its own point
// along the blade and runs diagonally out toward the tip, so they nest inside
// one another. (Expressing the vein position as a repeating ramp instead draws
// a ladder of parallel bars straight across the leaf -- a comb, not a leaf.)
// `across` is -1..1 edge to edge, `t` is 0..1 stem to tip.
#define LEAF_SECONDARY_VEINS 6

static float secondary_veins(float across, float t) {
    float a = fabsf(across);
    float sum = 0.0f;
    for (int k = 0; k < LEAF_SECONDARY_VEINS; k++) {
        float base = 0.14f + (float)k * 0.13f; // where it leaves the midrib
        float reach = (t - base) / 0.5f;       // how far out it has travelled
        if (reach <= 0.02f || reach >= 1.0f)
            continue;
        float d = a - reach;
        // Fade each vein out as it nears the blade edge.
        sum += expf(-d * d * 320.0f) * (1.0f - reach * 0.35f);
    }
    return sum > 1.0f ? 1.0f : sum;
}

// Rasterize one leaf into the RGBA/normal/roughness buffers. The leaf is placed
// at (cx, cy) in pixels, rotated by `rot`, with half-length `len` in pixels.
static void draw_leaf(unsigned char* albedo, unsigned char* normal, unsigned char* rough, int w,
                      int h, float cx, float cy, float rot, float len, float hue, float bright,
                      float roughness, float skew) {
    float ca = cosf(rot), sa = sinf(rot);
    int pad = (int)(len * 1.2f) + 2;
    int x0 = (int)cx - pad, x1 = (int)cx + pad;
    int y0 = (int)cy - pad, y1 = (int)cy + pad;
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > w - 1)
        x1 = w - 1;
    if (y1 > h - 1)
        y1 = h - 1;

    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            // Into leaf-local space: `lv` runs stem(0) to tip(1), `lu` across.
            float dx = ((float)x + 0.5f - cx) / len;
            float dy = ((float)y + 0.5f - cy) / len;
            float lu = dx * ca + dy * sa;
            float lv = (-dx * sa + dy * ca) * 0.5f + 0.5f;
            if (lv < 0.0f || lv > 1.0f)
                continue;

            float hw = leaf_half_width(lv, skew);
            float edge = fabsf(lu) - hw;
            // Soft edge about a pixel wide, so mips have something to filter.
            float alpha = 1.0f - smoothstep(-1.5f / len, 0.5f / len, edge);
            if (alpha <= 0.003f)
                continue;

            float across = hw > 1e-4f ? lu / hw : 0.0f;

            // Veins: a midrib plus angled secondaries, slightly lighter and
            // yellower than the blade.
            float midrib = expf(-across * across * 90.0f);
            float vein = fminf(1.0f, midrib + secondary_veins(across, lv) * 0.5f);

            float r = (0.17f + 0.10f * hue) * bright;
            float g = (0.36f + 0.10f * (1.0f - hue)) * bright;
            float b = (0.12f + 0.05f * hue) * bright;
            // Blade darkens toward the edge, lightens along the veins.
            float shade = 0.82f + 0.18f * (1.0f - fabsf(across));
            r = r * shade + vein * 0.07f;
            g = g * shade + vein * 0.06f;
            b = b * shade + vein * 0.02f;

            int ia = (y * w + x) * 4;
            float dst_a = albedo[ia + 3] / 255.0f;
            // Painted in order, later leaves over earlier ones.
            float out_a = alpha + dst_a * (1.0f - alpha);
            float wsrc = out_a > 1e-4f ? alpha / out_a : 0.0f;
            albedo[ia + 0] = (unsigned char)(fminf(1.0f, r) * 255 * wsrc + albedo[ia + 0] * (1.0f - wsrc));
            albedo[ia + 1] = (unsigned char)(fminf(1.0f, g) * 255 * wsrc + albedo[ia + 1] * (1.0f - wsrc));
            albedo[ia + 2] = (unsigned char)(fminf(1.0f, b) * 255 * wsrc + albedo[ia + 2] * (1.0f - wsrc));
            albedo[ia + 3] = (unsigned char)(out_a * 255);

            if (wsrc > 0.5f) {
                // Vein ridge: the blade folds up along the midrib and tips down
                // toward each edge, rotated back into atlas space.
                float slope_u = -across * 0.55f - midrib * across * 1.6f;
                vec3 n = {slope_u * ca, slope_u * sa, 1.0f};
                glm_vec3_normalize(n);
                int in3 = (y * w + x) * 3;
                normal[in3 + 0] = (unsigned char)((n[0] * 0.5f + 0.5f) * 255);
                normal[in3 + 1] = (unsigned char)((n[1] * 0.5f + 0.5f) * 255);
                normal[in3 + 2] = (unsigned char)((n[2] * 0.5f + 0.5f) * 255);
                // Cuticle: tighter on the blade, duller along the veins.
                // Replicated across RGB: the mask array reads roughness from
                // green (glTF ORM), and a 1-channel source samples green as 0.
                unsigned char q = (unsigned char)(fminf(1.0f, roughness + vein * 0.12f) * 255);
                rough[(y * w + x) * 3 + 0] = q;
                rough[(y * w + x) * 3 + 1] = q;
                rough[(y * w + x) * 3 + 2] = q;
            }
        }
    }
}

// A single leaf on its own transparent tile, for the falling-leaf particles.
// They cannot use the cluster atlas: a billboard samples UV 0..1 across its
// quad, which would squash all LEAF_VARIANTS cells into one sprite. A drifting
// leaf should be one leaf anyway, not a whole sprig.
static unsigned char* generate_leaf_sprite(int size) {
    unsigned char* albedo = calloc((size_t)size * size * 4, 1);
    unsigned char* normal = malloc((size_t)size * size * 3);
    unsigned char* rough = malloc((size_t)size * size * 3);
    if (!albedo || !normal || !rough) {
        free(albedo);
        free(normal);
        free(rough);
        return NULL;
    }

    srand(77);
    draw_leaf(albedo, normal, rough, size, size, size * 0.5f, size * 0.5f, 0.0f, size * 0.46f,
              0.45f, 1.0f, 0.6f, 0.0f);

    free(normal);
    free(rough);
    return albedo;
}

// Rasterize the whole atlas: LEAF_VARIANTS clusters side by side.
static void generate_leaf_cluster_maps(int width, int height, unsigned char** out_albedo,
                                       unsigned char** out_normal, unsigned char** out_rough) {
    unsigned char* albedo = calloc((size_t)width * height * 4, 1);
    unsigned char* normal = malloc((size_t)width * height * 3);
    unsigned char* rough = malloc((size_t)width * height * 3);
    if (!albedo || !normal || !rough) {
        free(albedo);
        free(normal);
        free(rough);
        *out_albedo = *out_normal = *out_rough = NULL;
        return;
    }

    // Flat normal and mid roughness everywhere the leaves do not cover.
    for (int i = 0; i < width * height; i++) {
        normal[i * 3 + 0] = 128;
        normal[i * 3 + 1] = 128;
        normal[i * 3 + 2] = 255;
        rough[i * 3 + 0] = 160;
        rough[i * 3 + 1] = 160;
        rough[i * 3 + 2] = 160;
    }

    srand(4242);
    int cell_w = width / LEAF_VARIANTS;

    for (int v = 0; v < LEAF_VARIANTS; v++) {
        float ox = (float)(v * cell_w);
        // Vary the sprig's own build, not just where its leaves land, so the
        // variants differ in structure and the repeat is harder to spot.
        int count = LEAVES_PER_CLUSTER + (int)rand_range(-2.0f, 2.99f);
        float fan = rand_range(0.35f, 0.75f);
        float reach = rand_range(0.52f, 0.72f);
        for (int i = 0; i < count; i++) {
            // Fan the sprig out from a stem near the bottom of the cell so the
            // cluster has a direction rather than being a random scatter.
            float along = (float)i / (float)(count - 1);
            float spread = rand_range(-fan, fan);
            float cx = ox + cell_w * (0.5f + spread * (0.15f + along * 0.30f));
            float cy = (float)height * (0.88f - along * reach + rand_range(-0.05f, 0.05f));
            float rot = spread * 1.5f + rand_range(-0.35f, 0.35f);
            float len = (float)height * (0.30f - along * 0.09f) * rand_range(0.8f, 1.15f);
            draw_leaf(albedo, normal, rough, width, height, cx, cy, rot, len,
                      rand_range(0.0f, 1.0f), rand_range(0.82f, 1.18f), rand_range(0.55f, 0.78f),
                      rand_range(-0.12f, 0.12f));
        }
    }

    *out_albedo = albedo;
    *out_normal = normal;
    *out_rough = rough;
}

/*
 * Generate all procedural textures
 */
static Texture* bark_albedo_tex = NULL;
static Texture* bark_normal_tex = NULL;
static Texture* bark_roughness_tex = NULL;
static Texture* bark_height_tex = NULL;
static Texture* leaf_albedo_tex = NULL;
static Texture* leaf_normal_tex = NULL;
static Texture* leaf_roughness_tex = NULL;
static Texture* leaf_sprite_tex = NULL;
static Texture* island_albedo_tex = NULL;
static Texture* island_normal_tex = NULL;

/*
 * Generate island/ground normal texture (mostly flat with some variation)
 */
static unsigned char* generate_island_normal(int width, int height) {
    unsigned char* data = malloc(width * height * 3);
    if (!data)
        return NULL;

    init_perlin(1000);

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = (y * width + x) * 3;

            float nx = (float)x / width * 16.0f;
            float ny = (float)y / height * 16.0f;

            // Subtle height variation for normal calculation
            float h = fbm_noise(nx, ny, 3, 0.5f) * 0.1f;
            float hx = fbm_noise(nx + 0.1f, ny, 3, 0.5f) * 0.1f;
            float hy = fbm_noise(nx, ny + 0.1f, 3, 0.5f) * 0.1f;

            // Derive normal from height differences. Tangent space puts the
            // surface normal on +Z, as the bark map does -- writing it on +Y
            // (the world-space convention) aims every ground texel sideways
            // along its bitangent, and the ground then never faces the sun.
            float dx = (hx - h) * 2.0f;
            float dy = (hy - h) * 2.0f;

            vec3 normal = {-dx, -dy, 1.0f};
            glm_vec3_normalize(normal);

            // Convert to 0-255 range
            data[idx] = (unsigned char)((normal[0] * 0.5f + 0.5f) * 255);
            data[idx + 1] = (unsigned char)((normal[1] * 0.5f + 0.5f) * 255);
            data[idx + 2] = (unsigned char)((normal[2] * 0.5f + 0.5f) * 255);
        }
    }

    return data;
}

/*
 * Generate island/ground albedo texture
 */
static unsigned char* generate_island_albedo(int width, int height) {
    unsigned char* data = malloc(width * height * 3);
    if (!data)
        return NULL;

    init_perlin(999);

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = (y * width + x) * 3;

            // Base noise for variation
            float nx = (float)x / width * 8.0f;
            float ny = (float)y / height * 8.0f;

            float noise = fbm_noise(nx, ny, 4, 0.5f);
            float detail = fbm_noise(nx * 4.0f, ny * 4.0f, 2, 0.5f) * 0.3f;

            float combined = noise + detail;

            // Earth/dirt brown base
            float r = 0.35f + combined * 0.15f;
            float g = 0.25f + combined * 0.12f;
            float b = 0.15f + combined * 0.08f;

            // Add some green patches (grass)
            float grass = fbm_noise(nx * 2.0f + 100.0f, ny * 2.0f, 3, 0.6f);
            if (grass > 0.3f) {
                float grass_blend = (grass - 0.3f) * 1.5f;
                grass_blend = fminf(grass_blend, 0.6f);
                r = r * (1.0f - grass_blend) + 0.2f * grass_blend;
                g = g * (1.0f - grass_blend) + 0.4f * grass_blend;
                b = b * (1.0f - grass_blend) + 0.15f * grass_blend;
            }

            data[idx] = (unsigned char)(fminf(fmaxf(r, 0.0f), 1.0f) * 255);
            data[idx + 1] = (unsigned char)(fminf(fmaxf(g, 0.0f), 1.0f) * 255);
            data[idx + 2] = (unsigned char)(fminf(fmaxf(b, 0.0f), 1.0f) * 255);
        }
    }

    return data;
}

// Bake one CPU buffer into a pooled texture and release the buffer. Going
// through the pool (rather than a hand-rolled glTexImage2D) is what gets the
// albedo maps decoded as sRGB and, for the leaf cutout, gets the transparent
// texels' RGB dilated so mipping doesn't fringe the leaf edges with black.
static Texture* bake_texture(Scene* scene, unsigned char* data, int width, int height,
                             int channels, bool is_srgb, const char* key) {
    if (!data)
        return NULL;
    Texture* tex =
        load_texture_from_memory(scene->tex_pool, key, data, width, height, channels, is_srgb);
    free(data);
    return tex;
}

static void generate_procedural_textures(Scene* scene) {
    const int B = BARK_TEXTURE_SIZE;
    const int T = TEXTURE_SIZE;
    // The leaf atlas is one row of square cluster cells.
    const int LW = TEXTURE_SIZE * LEAF_VARIANTS;
    const int LH = TEXTURE_SIZE;

    printf("Generating procedural bark textures...\n");
    // One relief, four maps derived from it -- they describe the same surface,
    // and the field is only built once instead of per map.
    float* bark_field = malloc((size_t)B * B * sizeof(float));
    if (bark_field) {
        bark_height_field(bark_field, B, B);
        bark_albedo_tex =
            bake_texture(scene, generate_bark_albedo(B, B, bark_field), B, B, 3, true,
                         "proc_bark_albedo");
        bark_normal_tex =
            bake_texture(scene, generate_bark_normal(B, B, bark_field), B, B, 3, false,
                         "proc_bark_normal");
        bark_roughness_tex =
            bake_texture(scene, generate_bark_roughness(B, B, bark_field), B, B, 3, false,
                         "proc_bark_roughness");
        bark_height_tex =
            bake_texture(scene, generate_bark_height(B, B, bark_field), B, B, 1, false,
                         "proc_bark_height");
        free(bark_field);
    }

    printf("Generating procedural leaf cluster atlas...\n");
    unsigned char *leaf_a = NULL, *leaf_n = NULL, *leaf_r = NULL;
    generate_leaf_cluster_maps(LW, LH, &leaf_a, &leaf_n, &leaf_r);
    leaf_albedo_tex = bake_texture(scene, leaf_a, LW, LH, 4, true, "proc_leaf_albedo");
    leaf_normal_tex = bake_texture(scene, leaf_n, LW, LH, 3, false, "proc_leaf_normal");
    leaf_roughness_tex = bake_texture(scene, leaf_r, LW, LH, 3, false, "proc_leaf_roughness");
    leaf_sprite_tex =
        bake_texture(scene, generate_leaf_sprite(T), T, T, 4, true, "proc_leaf_sprite");

    printf("Generating procedural island textures...\n");
    island_albedo_tex =
        bake_texture(scene, generate_island_albedo(T, T), T, T, 3, true, "proc_island_albedo");
    island_normal_tex =
        bake_texture(scene, generate_island_normal(T, T), T, T, 3, false, "proc_island_normal");

    printf("Procedural textures generated.\n");

    // Clear any pending GL errors and reset state to avoid affecting subsequent operations
    while (glGetError() != GL_NO_ERROR) {
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
}

/*
 * Constants
 */
const unsigned int HEIGHT = 900;
const unsigned int WIDTH = 1400;

/*
 * Globals
 */
static TreeParams params;
static TreeParams prev_params;
static Material* bark_material = NULL;
static Material* leaf_material = NULL;
static SceneNode* tree_root = NULL;
static SceneNode* island_node = NULL;
static Material* island_material = NULL;

static GrassParams grass_params;
static GrassParams prev_grass_params;
static SceneNode* grass_node = NULL;
static Material* grass_material = NULL;

static SkyAtmosphere* sky = NULL;
static IBLResources* ibl = NULL;
static Light* sun_light = NULL;
static Wind* scene_wind = NULL;

// Falling leaves: the spawn module is held so the GUI can gate it without
// tearing down the emitter (existing leaves finish their fall).
static ParticleModule* leaf_spawn_module = NULL;
static bool falling_leaves_on = true;
static float leaf_spawn_rate = 2.5f;

// Season tint multiplies the leaf albedo texture: 0 = summer green, 1 = autumn.
static float season = 0.0f;
static float prev_season = -1.0f;

static float sun_elevation = 14.0f;
static float sun_azimuth = 235.0f;

/*
 * Mouse drag controller
 */
static MouseDragController* drag_controller = NULL;

/*
 * Generate island mesh - a domed disc
 */
static void generate_island_mesh(Mesh* mesh, float radius, float height, int rings, int segments,
                                 float uv_tiles) {
    // Create a domed disc with rings from center to edge
    int num_vertices = 1 + rings * segments; // center + rings
    int num_triangles = segments + (rings - 1) * segments * 2;

    mesh->vertex_count = num_vertices;
    mesh->vertices = malloc(num_vertices * 3 * sizeof(float));
    mesh->normals = malloc(num_vertices * 3 * sizeof(float));
    mesh->tex_coords = malloc(num_vertices * 2 * sizeof(float));
    mesh->tangents = malloc(num_vertices * 4 * sizeof(float)); // xyz + handedness
    mesh->index_count = num_triangles * 3;
    mesh->indices = malloc(mesh->index_count * sizeof(unsigned int));

    // Center vertex (top of dome)
    mesh->vertices[0] = 0.0f;
    mesh->vertices[1] = height;
    mesh->vertices[2] = 0.0f;
    mesh->normals[0] = 0.0f;
    mesh->normals[1] = 1.0f;
    mesh->normals[2] = 0.0f;
    mesh->tangents[0] = 1.0f;
    mesh->tangents[1] = 0.0f;
    mesh->tangents[2] = 0.0f;
    mesh->tangents[3] = 1.0f; // cross((0,1,0), (1,0,0)) = (0,0,1)
    mesh->tex_coords[0] = 0.5f * uv_tiles;
    mesh->tex_coords[1] = 0.5f * uv_tiles;

    // Generate ring vertices
    int vi = 1;
    for (int r = 1; r <= rings; r++) {
        float ring_radius = radius * (float)r / rings;
        float ring_height = height * (1.0f - ((float)r / rings) * ((float)r / rings));

        for (int s = 0; s < segments; s++) {
            float angle = 2.0f * (float)M_PI * s / segments;
            float x = ring_radius * cosf(angle);
            float z = ring_radius * sinf(angle);

            mesh->vertices[vi * 3] = x;
            mesh->vertices[vi * 3 + 1] = ring_height;
            mesh->vertices[vi * 3 + 2] = z;

            // True surface normal of the dome y = height * (1 - (d/radius)^2),
            // whose slope at distance d is 2*height*d/radius^2. The old normal
            // was the radial direction, which tilted the ground up to 60 degrees
            // off vertical -- it faced sideways and never caught the sun.
            float slope = 2.0f * height * ring_radius / (radius * radius);
            vec3 normal = {cosf(angle) * slope, 1.0f, sinf(angle) * slope};
            glm_vec3_normalize(normal);
            mesh->normals[vi * 3] = normal[0];
            mesh->normals[vi * 3 + 1] = normal[1];
            mesh->normals[vi * 3 + 2] = normal[2];

            // Tangent along the circle (perpendicular to radial). The shader
            // derives the bitangent as cross(N, T), which for this normal and
            // tangent works out to (cos, -slope, sin) -- the slope-tilted
            // vector this used to store explicitly -- so the handedness is +1.
            mesh->tangents[vi * 4] = -sinf(angle);
            mesh->tangents[vi * 4 + 1] = 0.0f;
            mesh->tangents[vi * 4 + 2] = cosf(angle);
            mesh->tangents[vi * 4 + 3] = 1.0f;

            // UV coordinates, tiled so a terrain-sized disc keeps texel detail
            mesh->tex_coords[vi * 2] = (0.5f + 0.5f * x / radius) * uv_tiles;
            mesh->tex_coords[vi * 2 + 1] = (0.5f + 0.5f * z / radius) * uv_tiles;

            vi++;
        }
    }

    // Generate indices
    int ii = 0;

    // Center fan (first ring). Wound counter-clockwise as seen from above, so
    // the lit side faces the sky -- the original order was reversed, which put
    // every ground triangle's front face underground where back-face culling
    // threw it away.
    for (int s = 0; s < segments; s++) {
        mesh->indices[ii++] = 0;
        mesh->indices[ii++] = 1 + (s + 1) % segments;
        mesh->indices[ii++] = 1 + s;
    }

    // Remaining rings
    for (int r = 1; r < rings; r++) {
        int ring_start = 1 + (r - 1) * segments;
        int next_ring_start = 1 + r * segments;

        for (int s = 0; s < segments; s++) {
            int curr = ring_start + s;
            int next = ring_start + (s + 1) % segments;
            int curr_outer = next_ring_start + s;
            int next_outer = next_ring_start + (s + 1) % segments;

            // Two triangles per quad
            mesh->indices[ii++] = curr;
            mesh->indices[ii++] = next_outer;
            mesh->indices[ii++] = curr_outer;

            mesh->indices[ii++] = curr;
            mesh->indices[ii++] = next;
            mesh->indices[ii++] = next_outer;
        }
    }

    mesh->draw_mode = MESH_TRIANGLES;
    // Required: the renderer frustum-culls on this. Left at the zero AABB
    // create_mesh starts with, the ground collapses to a point at the origin
    // and gets culled the moment that point leaves the view.
    calculate_aabb(mesh);
}

/*
 * Create the ground
 *
 * Wide enough to reach the horizon: at the old radius of 120 the disc read as
 * a saucer floating in the sky's dark virtual ground rather than as terrain.
 * The dome is nearly flat across the near field and falls away at the rim, and
 * it is dropped by its own height so its crown lands at y = 0, where the tree
 * roots start.
 */
// The one place the dome's shape is defined. generate_island_mesh builds its
// rings from the same expression, and grass roots itself with this, so the
// surface and the things standing on it cannot drift apart. Includes the node
// translation, so the value is a world height ready to use.
float ground_height_at(float x, float z) {
    float d = sqrtf(x * x + z * z);
    if (d >= GROUND_RADIUS)
        return -GROUND_HEIGHT;
    float t = d / GROUND_RADIUS;
    return GROUND_HEIGHT * (1.0f - t * t) - GROUND_HEIGHT;
}

static void create_island(SceneNode* parent) {
    island_node = create_node();
    set_node_name(island_node, "ground");

    Mesh* mesh = create_mesh();
    generate_island_mesh(mesh, GROUND_RADIUS, GROUND_HEIGHT, 24, 64, 40.0f);
    mesh->material = island_material;

    glm_mat4_identity(island_node->original_transform);
    glm_translate(island_node->original_transform, (vec3){0.0f, -GROUND_HEIGHT, 0.0f});

    add_mesh_to_node(island_node, mesh);
    add_child_node(parent, island_node);
    // Static for the program's lifetime, so it uploads once here rather than
    // riding along with every tree rebuild.
    upload_buffers_to_gpu_for_nodes(island_node);
}

/*
 * Regenerate tree
 *
 * All the bark lands in one mesh and all the leaves in another, so the whole
 * tree is two draw calls no matter how many branches the sliders ask for.
 *
 * The node itself outlives every rebuild and only its meshes are swapped. An
 * earlier version rebuilt by walking the root and freeing any child that was
 * not a light or the ground -- a blacklist, which silently freed the
 * falling-leaves node as soon as that was added, while the Scene still held
 * its particle system and dereferenced the dead node every tick.
 */
static void regenerate_tree(const TreeParams* p) {
    if (!tree_root)
        return;

    for (size_t i = 0; i < tree_root->mesh_count; i++)
        free_mesh(tree_root->meshes[i]);
    tree_root->mesh_count = 0;

    TreeSkeleton skel;
    memset(&skel, 0, sizeof(skel));
    tree_skeleton_build(&skel, p);

    Mesh* bark = create_mesh();
    if (tree_mesh_bark(&skel, p, bark)) {
        bark->material = bark_material;
        add_mesh_to_node(tree_root, bark);
    } else {
        free_mesh(bark);
    }

    Mesh* leaves = create_mesh();
    if (tree_mesh_leaves(&skel, p, leaves)) {
        leaves->material = leaf_material;
        add_mesh_to_node(tree_root, leaves);
    } else {
        free_mesh(leaves);
    }

    printf("Tree: %d branches, %zu bark verts, %zu leaf verts\n", skel.branch_count,
           tree_root->mesh_count > 0 ? tree_root->meshes[0]->vertex_count : (size_t)0,
           tree_root->mesh_count > 1 ? tree_root->meshes[1]->vertex_count : (size_t)0);

    tree_skeleton_free(&skel);

    // Only the tree's own meshes: the ground is static and uploaded once.
    upload_buffers_to_gpu_for_nodes(tree_root);
}

/*
 * Regenerate the grass field
 *
 * Same shape as the tree: the node outlives every rebuild and only its mesh is
 * swapped, so nothing else parented to the root is ever at risk.
 */
static void regenerate_grass(const GrassParams* p) {
    if (!grass_node)
        return;

    for (size_t i = 0; i < grass_node->mesh_count; i++)
        free_mesh(grass_node->meshes[i]);
    grass_node->mesh_count = 0;

    Mesh* grass = create_mesh();
    if (grass_build_mesh(p, grass)) {
        grass->material = grass_material;
        add_mesh_to_node(grass_node, grass);
        printf("Grass: %zu verts\n", grass->vertex_count);
    } else {
        free_mesh(grass);
    }

    upload_buffers_to_gpu_for_nodes(grass_node);
}

// Leaf color across the season slider. The albedo factor multiplies the leaf
// texture, so this rides on top of the procedural green rather than replacing
// it; the subsurface tint follows so backlit leaves warm up with the canopy.
static void apply_season(float t) {
    if (!leaf_material)
        return;
    vec3 summer = {1.0f, 1.0f, 1.0f};
    vec3 autumn = {1.35f, 0.62f, 0.18f};
    glm_vec3_lerp(summer, autumn, t, leaf_material->albedo);

    vec3 green_sss = {0.5f, 0.8f, 0.15f};
    vec3 amber_sss = {0.95f, 0.5f, 0.1f};
    glm_vec3_lerp(green_sss, amber_sss, t, leaf_material->subsurface_color);
}

/*
 * Render tree parameters GUI
 */
static void render_tree_gui(const Engine* engine, Scene* scene) {
    (void)scene;

    if (!engine || !engine->show_gui)
        return;

    igSetNextWindowPos((ImVec2){15, 15}, ImGuiCond_FirstUseEver, (ImVec2){0, 0});
    igSetNextWindowSize((ImVec2){300, 720}, ImGuiCond_FirstUseEver);
    if (igBegin("Tree", NULL, 0)) {
        igSeparatorText("Seed");
        igSliderInt("Seed", &params.seed, 0, 9999, "%d", 0);

        igSeparatorText("Structure");
        igSliderInt("Max Depth", &params.max_depth, 1, 6, "%d", 0);
        igSliderInt("Branches", &params.branches_per_node, 1, 5, "%d", 0);
        igSliderFloat("Laterals", &params.lateral_density, 0.0f, 3.0f, "%.2f", 0);

        igSeparatorText("Dimensions");
        igSliderFloat("Trunk Len", &params.trunk_length, 10.0f, 200.0f, "%.1f", 0);
        igSliderFloat("Trunk Rad", &params.trunk_radius, 1.0f, 30.0f, "%.1f", 0);
        igSliderFloat("Len Decay", &params.length_decay, 0.3f, 0.95f, "%.3f", 0);
        igSliderFloat("Taper", &params.taper, 0.45f, 0.85f, "%.3f", 0);
        igSliderFloat("Twig Scale", &params.twig_scale, 0.5f, 2.0f, "%.2f", 0);

        igSeparatorText("Angles");
        igSliderFloat("Angle", &params.branch_angle, 5.0f, 90.0f, "%.1f", 0);
        igSliderFloat("Variance", &params.angle_variance, 0.0f, 45.0f, "%.1f", 0);
        igSliderFloat("Twist", &params.twist, 0.0f, 180.0f, "%.1f", 0);

        igSeparatorText("Curvature");
        igSliderFloat("Droop", &params.droop, 0.0f, 1.0f, "%.2f", 0);
        igSliderFloat("Curve Noise", &params.curve_noise, 0.0f, 1.0f, "%.2f", 0);
        igSliderFloat("Phototropism", &params.phototropism, 0.0f, 1.0f, "%.2f", 0);

        igSeparatorText("Leaves");
        bool show_leaves = params.show_leaves != 0;
        if (igCheckbox("Show Leaves", &show_leaves))
            params.show_leaves = show_leaves;
        igSliderFloat("Leaf Size", &params.leaf_size, 1.0f, 30.0f, "%.1f", 0);
        igSliderFloat("Leaf Density", &params.leaf_density, 0.5f, 8.0f, "%.2f", 0);
        igSliderFloat("Season", &season, 0.0f, 1.0f, "%.2f", 0);

        igSeparatorText("Grass");
        igSliderFloat("Density", &grass_params.density, 0.0f, 12.0f, "%.2f", 0);
        igSliderFloat("Patchiness", &grass_params.patchiness, 0.0f, 1.0f, "%.2f", 0);
        igSliderFloat("Blade Height", &grass_params.height, 1.0f, 14.0f, "%.2f", 0);
        igSliderFloat("Bend", &grass_params.bend, 0.0f, 1.2f, "%.2f", 0);
        igSliderFloat("Flowers", &grass_params.flower_amount, 0.0f, 0.3f, "%.3f", 0);
        igSliderFloat("Seed Heads", &grass_params.seed_head_amount, 0.0f, 0.4f, "%.3f", 0);
        igSliderFloat("Field Radius", &grass_params.radius, 20.0f, 220.0f, "%.0f", 0);

        igSeparatorText("Wind");
        if (scene_wind) {
            igSliderFloat("Strength", &scene_wind->strength, 0.0f, 8.0f, "%.2f", 0);
            igSliderFloat("Speed", &scene_wind->speed, 0.0f, 4.0f, "%.2f", 0);
            igSliderFloat("Gust Freq", &scene_wind->gust_frequency, 0.0f, 2.0f, "%.2f", 0);
            igSliderFloat("Gust Amount", &scene_wind->gust_amount, 0.0f, 1.0f, "%.2f", 0);
            igSliderFloat("Turbulence", &scene_wind->turbulence, 0.0f, 1.0f, "%.2f", 0);
        }

        igSeparatorText("Falling Leaves");
        if (igCheckbox("Enabled", &falling_leaves_on) && leaf_spawn_module) {
            particle_module_spawn_rate_set(leaf_spawn_module,
                                           falling_leaves_on ? leaf_spawn_rate : 0.0f);
        }
        if (igSliderFloat("Rate", &leaf_spawn_rate, 0.0f, 15.0f, "%.1f", 0) && leaf_spawn_module &&
            falling_leaves_on) {
            particle_module_spawn_rate_set(leaf_spawn_module, leaf_spawn_rate);
        }

        igSeparatorText("Atmosphere");
        if (engine->postfx) {
            igCheckbox("Fog", &engine->postfx->fog_enabled);
            igSliderFloat("Fog Density", &engine->postfx->fog_density, 0.0f, 0.0015f, "%.5f", 0);
            igSliderFloat("Fog Height", &engine->postfx->fog_height_falloff, 5.0f, 200.0f, "%.0f",
                          0);
        }

        igSeparatorText("Sun");
        bool sun_moved = igSliderFloat("Elevation", &sun_elevation, -5.0f, 89.0f, "%.1f", 0);
        sun_moved |= igSliderFloat("Azimuth", &sun_azimuth, 0.0f, 360.0f, "%.1f", 0);
        if (sun_moved && sky) {
            sky->sun_elevation_deg = sun_elevation;
            sky->sun_azimuth_deg = sun_azimuth;
            sky_update_sun(sky, ibl, (Engine*)engine);
        }
    }
    igEnd();
}

/*
 * Callbacks
 */
void mouse_button_callback(Engine* engine, int button, int action, int mods) {
    if (drag_controller) {
        double x, y;
        glfwGetCursorPos(engine->window, &x, &y);
        mouse_drag_on_button(drag_controller, button, action, mods, x, y);
    }
}

void key_callback(Engine* engine, int key, int scancode, int action, int mods) {
    (void)scancode;

    // Camera movement
    if (drag_controller && camera_controller_on_key(drag_controller, key, action, mods)) {
        return;
    }

    if (action != GLFW_PRESS) {
        return;
    }

    switch (key) {
        case GLFW_KEY_ESCAPE:
            glfwSetWindowShouldClose(engine->window, GLFW_TRUE);
            break;
        case GLFW_KEY_G:
            set_engine_show_gui(engine, !engine->show_gui);
            break;
        case GLFW_KEY_X:
            set_engine_show_xyz(engine, !engine->show_xyz);
            break;
        case GLFW_KEY_T:
            set_engine_show_wireframe(engine, !engine->show_wireframe);
            break;
        default:
            break;
    }
}

void render_scene_callback(Engine* engine, Scene* scene) {
    if (!engine || !scene || !scene->root_node) {
        return;
    }

    // Render custom GUI first
    render_tree_gui(engine, scene);

    // Check for parameter changes
    if (memcmp(&params, &prev_params, sizeof(TreeParams)) != 0) {
        regenerate_tree(&params);
        memcpy(&prev_params, &params, sizeof(TreeParams));
    }

    if (season != prev_season) {
        apply_season(season);
        prev_season = season;
    }

    if (memcmp(&grass_params, &prev_grass_params, sizeof(GrassParams)) != 0) {
        regenerate_grass(&grass_params);
        memcpy(&prev_grass_params, &grass_params, sizeof(GrassParams));
    }

    // Update camera - only if not hovering over GUI. Deliberately the wall
    // clock, not the frame clock: drag damping is input response.
    if (drag_controller && app_can_process_3d_input(engine)) {
        mouse_drag_update(drag_controller, glfwGetTime());
    }

    // Apply transforms
    Transform t = {.position = {0, 0, 0}, .rotation = {0, 0, 0}, .scale = {1, 1, 1}};
    reset_and_apply_transform(&engine->model_matrix, &t);
    apply_transform_to_nodes(scene->root_node, engine->model_matrix);

    render_current_scene(engine);
}

/*
 * Command line
 */
typedef struct {
    int headless;
    int frames;
    int screenshot_every;
    const char* screenshot;
    int width, height;
    int no_shadows;
    int no_fog;
    int no_falling_leaves;
    int seed;
    float sun_elevation;
    float sun_azimuth;
} TreeArgs;

static void print_usage(const char* prog) {
    printf("Usage: %s [options]\n", prog);
    printf("  -x, --headless          Run with a hidden window (for capture / CI)\n");
    printf("  -f, --frames N          Exit after N frames\n");
    printf("  -S, --screenshot PATH   Write the final frame as a binary PPM\n");
    printf("      --screenshot-every N  Also write every Nth frame\n");
    printf("  -W, --width N           Window width (default %u)\n", WIDTH);
    printf("  -H, --height N          Window height (default %u)\n", HEIGHT);
    printf("      --seed N            Tree seed\n");
    printf("      --sun-elevation D   Sun elevation in degrees\n");
    printf("      --sun-azimuth D     Sun azimuth in degrees\n");
    printf("      --no-shadows        Disable the shadow pass\n");
    printf("      --no-fog            Disable the volumetric fog\n");
    printf("      --no-falling-leaves Disable the falling-leaf particles\n");
    printf("  -h, --help              This message\n");
}

static bool parse_args(int argc, char** argv, TreeArgs* a) {
    memset(a, 0, sizeof(*a));
    a->width = (int)WIDTH;
    a->height = (int)HEIGHT;
    a->seed = 42;
    a->sun_elevation = 14.0f;
    a->sun_azimuth = 235.0f;

    for (int i = 1; i < argc; i++) {
        const char* s = argv[i];
        bool has_next = (i + 1) < argc;

        if (!strcmp(s, "-x") || !strcmp(s, "--headless")) {
            a->headless = 1;
        } else if ((!strcmp(s, "-f") || !strcmp(s, "--frames")) && has_next) {
            a->frames = atoi(argv[++i]);
        } else if ((!strcmp(s, "-S") || !strcmp(s, "--screenshot")) && has_next) {
            a->screenshot = argv[++i];
        } else if (!strcmp(s, "--screenshot-every") && has_next) {
            a->screenshot_every = atoi(argv[++i]);
        } else if ((!strcmp(s, "-W") || !strcmp(s, "--width")) && has_next) {
            a->width = atoi(argv[++i]);
        } else if ((!strcmp(s, "-H") || !strcmp(s, "--height")) && has_next) {
            a->height = atoi(argv[++i]);
        } else if (!strcmp(s, "--seed") && has_next) {
            a->seed = atoi(argv[++i]);
        } else if (!strcmp(s, "--sun-elevation") && has_next) {
            a->sun_elevation = (float)atof(argv[++i]);
        } else if (!strcmp(s, "--sun-azimuth") && has_next) {
            a->sun_azimuth = (float)atof(argv[++i]);
        } else if (!strcmp(s, "--no-shadows")) {
            a->no_shadows = 1;
        } else if (!strcmp(s, "--no-fog")) {
            a->no_fog = 1;
        } else if (!strcmp(s, "--no-falling-leaves")) {
            a->no_falling_leaves = 1;
        } else if (!strcmp(s, "-h") || !strcmp(s, "--help")) {
            print_usage(argv[0]);
            return false;
        } else {
            fprintf(stderr, "Unknown or incomplete option: %s\n", s);
            print_usage(argv[0]);
            return false;
        }
    }
    return true;
}

/*
 * Falling leaves
 *
 * Sparse enough to read as individual leaves rather than weather. They spawn in
 * a box around the canopy, tumble on their own roll, drift downwind, and stop
 * on the island rather than sinking through it.
 */
static void create_falling_leaves(Engine* engine, Scene* scene, float canopy_radius,
                                  float canopy_top) {
    ShaderProgram* particle_prog = create_particle_program();
    if (!particle_prog)
        return;
    add_shader_program_to_engine(engine, particle_prog);

    ParticleSystem* sys = create_particle_system("falling_leaves");
    if (!sys)
        return;
    particle_system_set_backend(sys, create_cpu_particle_sim_backend());

    ParticleEmitter* em = create_particle_emitter("leaf", 256);
    if (!em)
        return;

    ParticleRenderer* pr = create_billboard_particle_renderer(particle_prog);
    // hdr_gain 1.0: these are lit surfaces, not glowing motes -- the mote
    // default of 6.0 would blow them into bloom.
    // The single-leaf sprite, not the cluster atlas: a billboard samples UV
    // 0..1 across its quad, so the atlas would arrive as all its cells crushed
    // into one particle.
    billboard_renderer_set_sprite(pr, leaf_sprite_tex, 1.0f);
    particle_emitter_set_renderer(em, pr);

    leaf_spawn_module = particle_module_spawn_rate(leaf_spawn_rate);
    particle_emitter_add_module(em, leaf_spawn_module);

    vec3 spawn_min = {-canopy_radius, canopy_top * 0.45f, -canopy_radius};
    vec3 spawn_max = {canopy_radius, canopy_top, canopy_radius};
    particle_emitter_add_module(em, particle_module_init_box_location(spawn_min, spawn_max));
    particle_emitter_add_module(em, particle_module_init_lifetime(14.0f, 22.0f));
    particle_emitter_add_module(em, particle_module_init_size(2.0f, 3.5f));
    particle_emitter_add_module(em,
                                particle_module_init_color((vec4){1.0f, 0.85f, 0.55f, 1.0f}, 0.1f));

    particle_emitter_add_module(em, particle_module_update_rotation(0.4f, 1.2f));
    particle_emitter_add_module(em, particle_module_update_curl_noise(0.02f, 5.0f, 0.1f));

    vec3 fall = {scene_wind ? scene_wind->direction[0] * 1.5f : 1.5f, -3.5f,
                 scene_wind ? scene_wind->direction[2] * 1.5f : 0.0f};
    particle_emitter_add_module(em, particle_module_update_drift(fall));
    particle_emitter_add_module(em, particle_module_update_integrate(0.96f));

    // The ground's crown sits at y = 0.
    vec3 ground_p = {0.0f, 0.0f, 0.0f};
    vec3 ground_n = {0.0f, 1.0f, 0.0f};
    particle_emitter_add_module(
        em, particle_module_collider_plane(ground_p, ground_n, 0.0f));

    particle_system_add_emitter(sys, em);
    add_particle_system_to_scene(scene, sys);

    SceneNode* node = create_node();
    set_node_name(node, "falling_leaves");
    set_node_particle_system(node, sys);
    add_child_node(scene->root_node, node);
}

/*
 * Main
 */
int main(int argc, char** argv) {
    TreeArgs args;
    if (!parse_args(argc, argv, &args))
        return 0;

    sun_elevation = args.sun_elevation;
    sun_azimuth = args.sun_azimuth;

    Engine* engine = create_engine("Procedural Tree", args.width, args.height);
    set_engine_headless(engine, args.headless != 0);
    set_engine_screenshot_path(engine, args.screenshot);
    set_engine_screenshot_every(engine, args.screenshot_every);
    set_engine_exit_after_frames(engine, args.frames);
    // TAAU: render the scene at 70% and reconstruct temporally. Set before
    // init_engine, because create_postfx sizes every target from it. Headless
    // drops back to full resolution unless --headless-jitter, since the resolve
    // reconstructs from the jitter and headless suppresses it.
    set_engine_render_scale(engine, 0.70f);

    if (init_engine(engine) != 0) {
        fprintf(stderr, "Failed to initialize engine\n");
        return -1;
    }

    set_engine_mouse_button_callback(engine, mouse_button_callback);
    set_engine_key_callback(engine, key_callback);

    ShaderProgram* pbr_program = get_engine_shader_program_by_name(engine, "pbr");
    if (!pbr_program) {
        fprintf(stderr, "Failed to get PBR shader\n");
        return -1;
    }
    ShaderProgram* xyz_program = get_engine_shader_program_by_name(engine, "xyz");

    // Camera: low and off-axis so the canopy tops the frame and the low sun
    // rakes its shadows toward the viewer.
    Camera* camera = create_camera();
    vec3 cam_pos = {140.0f, 95.0f, 600.0f};
    vec3 look_at = {0.0f, 145.0f, 0.0f};
    vec3 up = {0.0f, 1.0f, 0.0f};
    set_camera_position(camera, cam_pos);
    set_camera_look_at(camera, look_at);
    set_camera_up_vector(camera, up);
    set_camera_perspective(camera, 0.55f, 2.0f, 3000.0f);
    set_engine_camera(engine, camera);
    camera->distance = glm_vec3_distance(cam_pos, look_at);

    drag_controller = create_mouse_drag_controller(engine);

    Scene* scene = create_scene();
    SceneNode* root = create_node();
    set_node_name(root, "root");
    set_scene_root_node(scene, root);
    add_scene_to_engine(engine, scene);

    if (xyz_program) {
        set_scene_xyz_shader_program(scene, xyz_program);
    }

    // Textures go through the scene's pool, so the scene must exist first.
    generate_procedural_textures(scene);

    /*
     * Lighting: physically based sky, IBL baked from it, and one sun coupled
     * to the atmosphere. A tree is mostly ambient-lit -- without an environment
     * to sample, every leaf that faces away from the key light goes black.
     */
    sky = create_sky_atmosphere();
    ibl = create_ibl_resources();
    if (sky && ibl) {
        sky->sun_elevation_deg = sun_elevation;
        sky->sun_azimuth_deg = sun_azimuth;
        sky_update_sun_dir(sky);

        if (sky_bake_static_luts(sky, engine) == 0 && sky_bake(sky, ibl, engine) == 0) {
            scene->sky = sky;
            scene->ibl = ibl;
            scene->render_skybox = true;
            scene->skybox_brightness = 1.0f;
            scene->skybox_ground_projection = false;

            sun_light = create_light();
            set_light_name(sun_light, "sun");
            set_light_type(sun_light, LIGHT_DIRECTIONAL);
            set_light_cast_shadows(sun_light, true);
            // Emitter size drives the PCSS penumbra: contact shadows stay
            // crisp under the canopy and soften further from the caster.
            set_light_size(sun_light, 6.0f, 6.0f);
            sky->sun_light = sun_light;
            sky->sun_base_intensity = 10.0f;
            sky_apply_sun_to_light(sky);
            add_light_to_scene(scene, sun_light);

            SceneNode* sun_node = create_node();
            set_node_name(sun_node, "sun");
            set_node_light(sun_node, sun_light);
            add_child_node(root, sun_node);

            printf("Sky: sun at elevation %.1f azimuth %.1f\n", sky->sun_elevation_deg,
                   sky->sun_azimuth_deg);
        }
    }

    // Shadows, sized to the tree rather than the engine's 2000-unit default.
    ShadowSystem* ss = scene->shadow_system;
    if (ss) {
        ss->enabled = args.no_shadows == 0;
        ss->ortho_size = 300.0f;
        ss->near_plane = 0.1f;
        ss->far_plane = 1200.0f;
        ss->pcss_enabled = true;
        ss->pcss_softness = 1.5f;
        ss->cascade_count = SHADOW_CASCADES;
    }

    /*
     * Wind. The tree's materials opt in per-mode; the island stays rigid.
     */
    scene_wind = create_wind("breeze");
    glm_vec3_copy((vec3){1.0f, 0.0f, 0.35f}, scene_wind->direction);
    scene_wind->strength = 2.5f;
    scene_wind->speed = 1.2f;
    scene_wind->gust_frequency = 0.35f;
    scene_wind->gust_amount = 0.55f;
    scene_wind->turbulence = 0.5f;
    set_scene_wind(scene, scene_wind);

    /*
     * Materials
     *
     * None of these may take an AO texture: the PBR shader reads UV1 as the AO
     * map's UV, and UV1 on the tree meshes carries wind data.
     */
    bark_material = create_material();
    glm_vec3_one(bark_material->albedo);
    bark_material->roughness = 1.0f; // the map carries it (factor x map)
    bark_material->metallic = 0.0f;
    bark_material->ao = 1.0f;
    bark_material->wind_response = 1.0f;
    bark_material->wind_mode = 1; // vegetation branch
    bark_material->parallax_scale = 0.03f;
    set_material_shader_program(bark_material, pbr_program);
    set_material_albedo_tex(bark_material, bark_albedo_tex);
    set_material_normal_tex(bark_material, bark_normal_tex);
    set_material_roughness_tex(bark_material, bark_roughness_tex);
    set_material_height_tex(bark_material, bark_height_tex);

    leaf_material = create_material();
    glm_vec3_one(leaf_material->albedo);
    // 1.0 because the roughness map is authoritative: the shader multiplies
    // factor by map, so any factor below 1 darkens the whole range and pushes
    // the canopy glossy enough to mirror the sky.
    leaf_material->roughness = 1.0f;
    leaf_material->metallic = 0.0f;
    leaf_material->ao = 1.0f;
    // Alpha-masked cutout, drawn from both sides, and -- unlike hair cards --
    // allowed into the shadow map, which is what dapples the ground.
    leaf_material->alpha_mode = ALPHA_MASK;
    leaf_material->alphaCutoff = 0.4f;
    leaf_material->doubleSided = true;
    leaf_material->foliage_shadows = true;
    leaf_material->wind_response = 1.0f;
    leaf_material->wind_mode = 2; // vegetation leaf (adds flutter)
    // Thin leaves transmit light: without this the canopy reads as opaque
    // plastic whenever the sun is behind it.
    leaf_material->subsurface = 0.6f;
    set_material_shader_program(leaf_material, pbr_program);
    set_material_albedo_tex(leaf_material, leaf_albedo_tex);
    set_material_normal_tex(leaf_material, leaf_normal_tex);
    set_material_roughness_tex(leaf_material, leaf_roughness_tex);

    if (engine->postfx) {
        postfx_reset_sss_profiles(engine->postfx);
        leaf_material->subsurface_profile =
            postfx_add_sss_profile(engine->postfx, (vec3){0.45f, 0.75f, 0.2f}, 0.25f);
    }
    apply_season(season);
    prev_season = season;

    island_material = create_material();
    glm_vec3_one(island_material->albedo);
    island_material->roughness = 0.9f;
    island_material->metallic = 0.0f;
    island_material->ao = 1.0f;
    set_material_shader_program(island_material, pbr_program);
    set_material_albedo_tex(island_material, island_albedo_tex);
    set_material_normal_tex(island_material, island_normal_tex);

    // Grass. Opaque, so it casts and receives shadows with no special handling
    // -- the canopy dapple landing on it is the point of having it. Colour is
    // entirely per-vertex, so no textures and no AO map to collide with UV1.
    grass_material = create_material();
    glm_vec3_one(grass_material->albedo);
    grass_material->roughness = 0.78f;
    grass_material->metallic = 0.0f;
    grass_material->ao = 1.0f;
    grass_material->doubleSided = true;
    // Grass is far more mobile than wood.
    grass_material->wind_response = 1.7f;
    grass_material->wind_mode = 2; // vegetation leaf: sway plus tip flutter
    // Thin blades glow when the sun is behind them, like the leaves.
    grass_material->subsurface = 0.45f;
    glm_vec3_copy((vec3){0.45f, 0.70f, 0.18f}, grass_material->subsurface_color);
    // A real profile slot is not optional once subsurface is non-zero: the
    // shader tags the skin-diffuse buffer with profile + 1, so an unassigned
    // -1 writes the tag reserved for "not a subsurface surface". The blur then
    // skips those pixels while their diffuse is still sitting in the buffer,
    // and the unblurred energy composites back as blown-out speckle.
    if (engine->postfx)
        grass_material->subsurface_profile =
            postfx_add_sss_profile(engine->postfx, (vec3){0.40f, 0.70f, 0.16f}, 0.15f);
    set_material_shader_program(grass_material, pbr_program);

    create_island(root);

    /*
     * Post-processing: a film look rather than the engine defaults, on PBR
     * Neutral so foliage colour stays faithful rather than being pushed.
     */
    PostFX* fx = engine->postfx;
    if (fx) {
        fx->tonemap_mode = POSTFX_TONEMAP_NEUTRAL;
        postfx_apply_film_look(fx);
        fx->grain_strength = 0.015f;

        // TAAU is a temporal reconstruction, so the render scale above does
        // nothing without this: the seam only dispatches when the resolve runs,
        // and unset it would render at 70% and simply magnify.
        fx->taa_enabled = true;

        fx->fog_enabled = false;
        fx->fog_density = 0.0005f;
        fx->fog_height_falloff = 75.0f;
        fx->fog_floor_y = 0.0f;
        fx->fog_far = 800.0f;

        fx->dof_enabled = false;
        fx->dof_autofocus = true;
        fx->dof_focus_distance = 620.0f;
        fx->dof_focus_range = 320.0f;

        if (fx->ssao_radius < 1.6f)
            fx->ssao_radius = 1.6f;
        fx->contact_shadows_enabled = true;
        fx->ssr_enabled = true;
        fx->ssgi_enabled = true;
    }
    engine->oit_enabled = true;

    // Tree shape
    params.seed = args.seed;
    params.max_depth = 4;
    // A tall, upright habit: a long trunk, branches held closer to vertical,
    // and a stronger pull toward the light, which narrows the crown rather
    // than letting it spread into a ball.
    params.trunk_length = 125.0f;
    params.trunk_radius = 9.0f;
    params.branches_per_node = 3;
    params.length_decay = 0.70f;
    params.taper = 0.62f;
    params.branch_angle = 27.0f;
    params.angle_variance = 12.0f;
    params.twist = 137.5f;
    params.droop = 0.32f;
    params.curve_noise = 0.4f;
    params.phototropism = 0.45f;
    params.lateral_density = 1.0f;
    params.twig_scale = 1.0f;
    params.show_leaves = 1;
    // A card carries a whole sprig, so it is sized as one and spaced sparsely:
    // the canopy should show its branch structure through the foliage.
    params.leaf_size = 15.0f;
    params.leaf_density = 1.3f;

    // Grass field
    grass_params.seed = args.seed;
    grass_params.radius = 130.0f;
    grass_params.clear_radius = 11.0f;
    grass_params.density = 5.5f;
    grass_params.patchiness = 0.55f;
    grass_params.height = 5.5f;
    grass_params.blade_width = 0.55f;
    grass_params.bend = 0.42f;
    grass_params.flower_amount = 0.02f;
    grass_params.seed_head_amount = 0.09f;

    // The tree node is created once and outlives every rebuild; regeneration
    // only swaps its meshes, so nothing else parented to the root is at risk.
    tree_root = create_node();
    set_node_name(tree_root, "tree");
    add_child_node(root, tree_root);

    grass_node = create_node();
    set_node_name(grass_node, "grass");
    add_child_node(root, grass_node);
    regenerate_grass(&grass_params);
    memcpy(&prev_grass_params, &grass_params, sizeof(GrassParams));

    // Build once here so the canopy bounds are known before the leaf emitter
    // is sized; the render callback picks up any later slider change.
    regenerate_tree(&params);
    memcpy(&prev_params, &params, sizeof(TreeParams));

    if (!args.no_falling_leaves) {
        float canopy_top = 200.0f;
        float canopy_radius = 110.0f;
        if (tree_root && tree_root->mesh_count > 0) {
            canopy_top = tree_root->meshes[0]->aabb.max[1];
            float rx = fmaxf(fabsf(tree_root->meshes[0]->aabb.min[0]),
                             fabsf(tree_root->meshes[0]->aabb.max[0]));
            float rz = fmaxf(fabsf(tree_root->meshes[0]->aabb.min[2]),
                             fabsf(tree_root->meshes[0]->aabb.max[2]));
            canopy_radius = fmaxf(rx, rz);
        }
        create_falling_leaves(engine, scene, canopy_radius, canopy_top);
    }

    set_engine_show_gui(engine, !args.headless);
    set_engine_show_fps(engine, !args.headless);
    set_engine_show_wireframe(engine, false);
    set_engine_show_xyz(engine, false);

    engine_run(engine, NULL, render_scene_callback);

    printf("Cleaning up...\n");
    free_mouse_drag_controller(drag_controller);
    // The scene owns the wind, sky, and IBL; free_engine takes them with it.
    free_engine(engine);

    printf("Goodbye!\n");
    return 0;
}
