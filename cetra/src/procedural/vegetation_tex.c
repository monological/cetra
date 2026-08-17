#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <cglm/cglm.h>

#include "vegetation_tex.h"

// For TG_LEAF_VARIANTS. The atlas cell count is the GEOMETRY's contract -- the
// leaf mesh remaps UV0.u into one of those cells -- so the generator that builds
// the atlas follows the header that defines it rather than restating the number.
#include "tree_gen.h"


/*
 * Perlin Noise Implementation
 */
static int perm[512];
static int perm_initialized = 0;

void veg_noise_seed(unsigned int seed) {
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

float veg_perlin2(float x, float y) {
    if (!perm_initialized)
        veg_noise_seed(12345);

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

float veg_fbm2(float x, float y, int octaves, float persistence) {
    float total = 0.0f;
    float amplitude = 1.0f;
    float frequency = 1.0f;
    float max_value = 0.0f;

    for (int i = 0; i < octaves; i++) {
        total += veg_perlin2(x * frequency, y * frequency) * amplitude;
        max_value += amplitude;
        amplitude *= persistence;
        frequency *= 2.0f;
    }

    return total / max_value;
}

/*
 * Worley (Cellular) Noise for bark cracks
 */
float veg_worley2(float x, float y, unsigned int seed) {
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

// Lattice index wrapped into [0, p). Negative inputs are real: a caller may offset its
// coordinates, and C's % keeps the sign.
static int wrap_cell(int v, int p) {
    if (p <= 0)
        return v;
    const int m = v % p;
    return m < 0 ? m + p : m;
}

float veg_perlin2_tiled(float x, float y, int px, int py) {
    if (!perm_initialized)
        veg_noise_seed(12345);

    // The permutation table indexes as perm[perm[a] + b], so both a and b must stay under
    // 256 for the second lookup to stay inside 512.
    px = px > 0 && px < 256 ? px : 256;
    py = py > 0 && py < 256 ? py : 256;

    const int xi = (int)floorf(x);
    const int yi = (int)floorf(y);
    const float xf = x - floorf(x);
    const float yf = y - floorf(y);

    const float u = fade(xf);
    const float v = fade(yf);

    // The wrap is the whole difference from veg_perlin2: the cell AFTER the last one is the
    // first one, so the gradients at u = 1 are the gradients at u = 0 and the field closes.
    const int x0 = wrap_cell(xi, px);
    const int x1 = wrap_cell(xi + 1, px);
    const int y0 = wrap_cell(yi, py);
    const int y1 = wrap_cell(yi + 1, py);

    const int aa = perm[perm[x0] + y0];
    const int ab = perm[perm[x0] + y1];
    const int ba = perm[perm[x1] + y0];
    const int bb = perm[perm[x1] + y1];

    const float r1 = lerp_f(grad(aa, xf, yf), grad(ba, xf - 1.0f, yf), u);
    const float r2 = lerp_f(grad(ab, xf, yf - 1.0f), grad(bb, xf - 1.0f, yf - 1.0f), u);

    return (lerp_f(r1, r2, v) + 1.0f) * 0.5f;
}

float veg_fbm2_tiled(float x, float y, int octaves, float persistence, int px, int py) {
    float total = 0.0f;
    float amplitude = 1.0f;
    float frequency = 1.0f;
    float max_value = 0.0f;
    int cx = px;
    int cy = py;

    for (int i = 0; i < octaves; i++) {
        total += veg_perlin2_tiled(x * frequency, y * frequency, cx, cy) * amplitude;
        max_value += amplitude;
        amplitude *= persistence;
        frequency *= 2.0f;
        // The octave samples twice as fine, so its period is twice as many cells. Past the
        // table's 256 the wrap stops being exact and the octave contributes an unwrapped
        // field -- which is why the doc caps the useful octave count rather than the period.
        cx = cx * 2 < 256 ? cx * 2 : 256;
        cy = cy * 2 < 256 ? cy * 2 : 256;
    }

    return total / max_value;
}

float veg_worley2_tiled(float x, float y, unsigned int seed, int px, int py) {
    const int xi = (int)floorf(x);
    const int yi = (int)floorf(y);

    float min_dist = 999.0f;

    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            const int cx = xi + dx;
            const int cy = yi + dy;

            // The HASH takes the wrapped cell so the feature points repeat, but the DISTANCE
            // is measured to the unwrapped one -- the point has to sit in the neighbour's
            // place, not be teleported back inside the tile, or every edge cell measures to
            // the wrong side of the texture.
            const int hx = wrap_cell(cx, px);
            const int hy = wrap_cell(cy, py);
            unsigned int h = (unsigned int)(hx * 374761393 + hy * 668265263 + (int)seed);
            h = (h ^ (h >> 13)) * 1274126177;

            const float fx = (float)cx + (float)(h & 0xFFFF) / 65536.0f;
            const float fy = (float)cy + (float)((h >> 16) & 0xFFFF) / 65536.0f;

            const float dist = (x - fx) * (x - fx) + (y - fy) * (y - fy);
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
void veg_bark_height_field(float* out, int width, int height) {
    veg_noise_seed(42);

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            float u = (float)x / width;
            float v = (float)y / height;

            // Warp the fissure coordinate so the cracks wander up the trunk
            // rather than running as straight parallel stripes.
            float warp = (veg_fbm2(u * 2.5f, v * 0.9f, 4, 0.5f) - 0.5f) * 0.9f;

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
            float along = veg_fbm2(u * 3.0f, v * 1.6f, 4, 0.55f);
            fissure *= 0.74f + 0.26f * along;

            // Cellular plate boundaries, stretched vertically like real bark.
            // Tight edges for the same reason as the fissures above.
            float cell = veg_worley2(u * 5.0f, v * 2.0f, 91);
            float plate = smoothstep(0.02f, 0.14f, cell);

            float h = fissure * 0.62f + plate * 0.38f;

            // Sparse cross-breaks. Bark does split across the grain, but never
            // at a regular spacing, so these are the CONTOUR of a noise field
            // rather than a wave: the level set of a field that varies quickly
            // up the trunk and slowly around it runs horizontally, wanders, and
            // reappears at irregular heights with no period to detect. (A wave
            // cannot do this -- warping shifts each crack but leaves the average
            // spacing intact, which reads as brickwork.)
            float across_field = veg_fbm2(u * 2.2f, v * 6.0f, 4, 0.5f);
            float contour = fabsf(across_field - 0.5f);
            float split = smoothstep(0.0f, 0.060f, contour);

            // A second, slower field decides where breaks are allowed at all,
            // so most of the trunk carries none and a few stretches carry one.
            float gate = smoothstep(0.56f, 0.78f,
                                    veg_fbm2(u * 1.3f + 31.0f, v * 1.1f + 17.0f, 3, 0.5f));
            split = 1.0f - (1.0f - split) * gate;

            // Shallower than the vertical fissures: a cross-break interrupts a
            // plate, it does not cut the trunk open.
            h *= 0.44f + 0.56f * split;

            // Fine grain on top, small enough to only matter up close.
            h += (veg_fbm2(u * 22.0f, v * 7.0f, 3, 0.5f) - 0.5f) * 0.14f;

            out[y * width + x] = fmaxf(0.0f, fminf(1.0f, h));
        }
    }
}

unsigned char* veg_bark_albedo(int width, int height, const float* field) {
    unsigned char* data = malloc((size_t)width * height * 3);
    if (!data)
        return NULL;

    veg_noise_seed(7);

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
            float lichen = veg_fbm2(u * 6.0f, v * 3.0f, 4, 0.5f);
            float grey = smoothstep(0.58f, 0.88f, lichen) * t * 0.40f;
            r += (0.26f - r) * grey;
            g += (0.28f - g) * grey;
            b += (0.24f - b) * grey;

            // Fine tonal break-up so the plates are not flat swatches.
            float grain = (veg_fbm2(u * 30.0f, v * 10.0f, 3, 0.5f) - 0.5f) * 0.09f;
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
unsigned char* veg_bark_normal(int width, int height, const float* field) {
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
unsigned char* veg_bark_height(int width, int height, const float* field) {
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
unsigned char* veg_bark_roughness(int width, int height, const float* field) {
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
//
// The division is in DOUBLE. RAND_MAX is 0x7fffffff, which float cannot represent, so
// (float)RAND_MAX rounds the divisor UP to 2^31 and the quotient never quite reaches 1
// -- the explicit cast that used to be here silenced the compiler's warning about
// exactly that rather than answering it. double holds RAND_MAX exactly, so the only
// rounding left is the single one on the way out.
float veg_rand_range(float min_val, float max_val) {
    return min_val + (float)(rand() / (double)RAND_MAX) * (max_val - min_val);
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
unsigned char* veg_leaf_sprite(int size) {
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
void veg_leaf_cluster_maps(int width, int height, unsigned char** out_albedo,
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
        int count = LEAVES_PER_CLUSTER + (int)veg_rand_range(-2.0f, 2.99f);
        float fan = veg_rand_range(0.35f, 0.75f);
        float reach = veg_rand_range(0.52f, 0.72f);
        for (int i = 0; i < count; i++) {
            // Fan the sprig out from a stem near the bottom of the cell so the
            // cluster has a direction rather than being a random scatter.
            float along = (float)i / (float)(count - 1);
            float spread = veg_rand_range(-fan, fan);
            float cx = ox + cell_w * (0.5f + spread * (0.15f + along * 0.30f));
            float cy = (float)height * (0.88f - along * reach + veg_rand_range(-0.05f, 0.05f));
            float rot = spread * 1.5f + veg_rand_range(-0.35f, 0.35f);
            float len = (float)height * (0.30f - along * 0.09f) * veg_rand_range(0.8f, 1.15f);
            draw_leaf(albedo, normal, rough, width, height, cx, cy, rot, len,
                      veg_rand_range(0.0f, 1.0f), veg_rand_range(0.82f, 1.18f), veg_rand_range(0.55f, 0.78f),
                      veg_rand_range(-0.12f, 0.12f));
        }
    }

    *out_albedo = albedo;
    *out_normal = normal;
    *out_rough = rough;
}
