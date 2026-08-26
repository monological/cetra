#include "moon_surface.h"

#include "moon_map.h"
#include "thread.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * CRATER POPULATION. Three size tiers, stamped oldest-first, and the counts are
 * chosen against the bake's own resolution rather than by eye: at 2048 texels
 * around the equator a texel is 0.0031 rad, so a crater below ~0.005 rad is
 * under two texels and contributes nothing but noise -- that is the floor of
 * TIER3, and the grain field covers everything finer.
 *
 * The counts are what it takes to SATURATE. A crater of angular radius r covers
 * pi*r^2 steradians of a 4pi sphere, so covering the sphere once at TIER3's mean
 * radius takes tens of thousands. Anything less reads as scattered pockmarks on
 * a smooth ball, which is the look of every procedural moon that evaluates its
 * craters per pixel -- not a tuning failure, a population one.
 */
#define MS_TIER1_COUNT 110
#define MS_TIER1_MIN 0.040f
#define MS_TIER1_MAX 0.150f
#define MS_TIER2_COUNT 1100
#define MS_TIER2_MIN 0.014f
#define MS_TIER2_MAX 0.045f
#define MS_TIER3_COUNT 42000
#define MS_TIER3_MIN 0.0052f
#define MS_TIER3_MAX 0.016f

// Ray systems belong to the few youngest large craters and to nothing else, so
// they are a separate pass over a handful rather than a property every crater
// carries.
#define MS_RAY_CRATERS 7
#define MS_RAY_REACH 9.0f

/*
 * Relief, in units of the moon's own radius, so a slope is dh over d(arc) with
 * both in the same unit and the normal falls out with no scale factor.
 *
 * DEPTH is a FRACTION OF THE CRATER'S OWN RADIUS, which is the one number here
 * that is really measured: simple lunar craters run about 1:5 depth to diameter,
 * so 0.2 of the radius, and the parabolic bowl below puts the wall slope at
 * 2*0.2 = 0.4, about 22 degrees. That matches the real thing and needs no
 * exaggeration -- unlike the SILHOUETTE, which is why this bakes a normal and
 * displaces nothing (real relief is ~0.3% of a lunar radius and would not move
 * the limb by a pixel).
 *
 * Large basins are relatively shallower than small craters, which is what
 * MS_DEPTH_LARGE captures -- gravity relaxes them.
 */
#define MS_DEPTH_SMALL 0.20f
#define MS_DEPTH_LARGE 0.055f
#define MS_RIM_RATIO 0.075f

// Albedo levels, in the shader's own normalisation where the highlands sit near
// 1. Real lunar highland albedo is ~0.13 and mare ~0.07, and it is that RATIO
// that is preserved here, not the absolute value.
#define MS_ALBEDO_HIGHLAND 0.82f
#define MS_ALBEDO_MARE 0.44f
#define MS_ALBEDO_FLOOR 0.30f
#define MS_ALBEDO_CEIL 1.00f

// How much a mare floods the relief under it. Not 1: the flood is thin over the
// bigger structures and their ghosts still show through, which is a real and
// very recognisable feature of Imbrium and Serenitatis.
#define MS_MARIA_FLOOD 0.82f
#define MS_MARIA_DEPTH 0.004f

// ---------------------------------------------------------------------------
// Noise. Self-contained on an integer hash rather than reaching for noise.h's
// permutation table or vegetation_tex.h's rand() seeder -- both carry global
// state, and this bake is threaded.
// ---------------------------------------------------------------------------

static uint32_t ms_hash(uint32_t h) {
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

static uint32_t ms_hash3(int x, int y, int z) {
    return ms_hash((uint32_t)x * 0x8da6b343u + (uint32_t)y * 0xd8163841u +
                   (uint32_t)z * 0xcb1ab31fu);
}

static float ms_unit(uint32_t h) { return (float)(h >> 8) * (1.0f / 16777216.0f); }

static float ms_rand(uint32_t* state) {
    *state = *state * 1664525u + 1013904223u;
    return ms_unit(ms_hash(*state));
}

// Trilinear value noise in [0,1].
static float ms_value3(float x, float y, float z) {
    const float fx = floorf(x), fy = floorf(y), fz = floorf(z);
    const int ix = (int)fx, iy = (int)fy, iz = (int)fz;
    float tx = x - fx, ty = y - fy, tz = z - fz;
    tx = tx * tx * (3.0f - 2.0f * tx);
    ty = ty * ty * (3.0f - 2.0f * ty);
    tz = tz * tz * (3.0f - 2.0f * tz);
    float c[8];
    for (int k = 0; k < 2; k++)
        for (int j = 0; j < 2; j++)
            for (int i = 0; i < 2; i++)
                c[k * 4 + j * 2 + i] = ms_unit(ms_hash3(ix + i, iy + j, iz + k));
    const float x00 = c[0] + (c[1] - c[0]) * tx, x10 = c[2] + (c[3] - c[2]) * tx;
    const float x01 = c[4] + (c[5] - c[4]) * tx, x11 = c[6] + (c[7] - c[6]) * tx;
    const float y0 = x00 + (x10 - x00) * ty, y1 = x01 + (x11 - x01) * ty;
    return y0 + (y1 - y0) * tz;
}

static float ms_fbm3(float x, float y, float z, int octaves) {
    float sum = 0.0f, amp = 0.5f, norm = 0.0f;
    for (int i = 0; i < octaves; i++) {
        sum += amp * ms_value3(x, y, z);
        norm += amp;
        amp *= 0.5f;
        x *= 2.03f;
        y *= 2.03f;
        z *= 2.03f;
    }
    return sum / norm;
}

// Ridged: the fold at the peak is what makes a crest rather than a swell.
static float ms_ridge3(float x, float y, float z) {
    const float n = ms_value3(x, y, z);
    const float r = 1.0f - fabsf(2.0f * n - 1.0f);
    return r * r;
}

static float ms_smoothstep(float a, float b, float x) {
    float t = (x - a) / (b - a);
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}

static float ms_clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// ---------------------------------------------------------------------------
// The crater list
// ---------------------------------------------------------------------------

typedef struct {
    float cx, cy, cz;  // unit centre in the body frame
    float radius;      // angular radius, radians
    float depth;       // bowl depth, radius units
    float rim;         // rim crest above the plain, radius units
    float fresh;       // 0 ancient and invisible in albedo, 1 bright ejecta
    float lat;         // centre latitude, radians -- the row-range test
} MoonCrater;

/*
 * Radii are drawn as u^3 across the tier, which is a stand-in for the real
 * size-frequency distribution (N larger than D goes roughly as D^-2): heavily
 * weighted to the small end with a thin tail of big ones. A uniform draw gives
 * every crater about the same size and the field reads as a golf ball.
 */
static void ms_fill_tier(MoonCrater* out, int count, float rmin, float rmax, uint32_t seed) {
    uint32_t s = seed;
    for (int i = 0; i < count; i++) {
        // Uniform on the sphere: z uniform, longitude uniform. Sampling
        // latitude uniformly instead crowds both poles, which on a body whose
        // poles sit at the top and bottom of the drawn disc is visible.
        const float u = ms_rand(&s), v = ms_rand(&s), w = ms_rand(&s);
        const float sy = 1.0f - 2.0f * u;
        const float rad = sqrtf(fmaxf(0.0f, 1.0f - sy * sy));
        const float lon = v * 6.28318530718f;
        const float t = w * w * w;
        const float radius = rmin + (rmax - rmin) * t;
        // The big end of a tier relaxes toward the shallow basin ratio.
        const float relax = ms_smoothstep(MS_TIER2_MAX, MS_TIER1_MAX, radius);
        const float dratio = MS_DEPTH_SMALL + (MS_DEPTH_LARGE - MS_DEPTH_SMALL) * relax;
        // Freshness is rare and heavily skewed: nearly every crater on the Moon
        // is old and has no albedo signature left, which is exactly why the few
        // that do -- Tycho, Copernicus -- dominate the full-moon face.
        const float f = ms_rand(&s);
        out[i].cx = rad * sinf(lon);
        out[i].cy = sy;
        out[i].cz = rad * cosf(lon);
        out[i].radius = radius;
        out[i].depth = radius * dratio;
        out[i].rim = radius * MS_RIM_RATIO;
        out[i].fresh = f * f * f * f;
        out[i].lat = asinf(ms_clampf(sy, -1.0f, 1.0f));
    }
}

// ---------------------------------------------------------------------------
// The bake
// ---------------------------------------------------------------------------

typedef struct {
    int w, h;
    float* height;
    float* albedo;
    float* cover;
    unsigned char* out;
    const MoonCrater* craters;
    int crater_count;
    const MoonCrater* rays;
    int ray_count;
} MoonBake;

// Bilinear sample of the compiled-in maria coverage, wrapping in longitude and
// clamping in latitude -- the same pair the GL texture uses, for the same
// reason: wrapping latitude mirrors the south pole onto the north.
static float ms_cover_at(float u, float v) {
    float x = u * (float)MOON_MAP_W - 0.5f;
    float y = v * (float)MOON_MAP_H - 0.5f;
    const float fx = floorf(x), fy = floorf(y);
    const float tx = x - fx, ty = y - fy;
    int x0 = (int)fx, y0 = (int)fy;
    int x1 = x0 + 1, y1 = y0 + 1;
    x0 = ((x0 % MOON_MAP_W) + MOON_MAP_W) % MOON_MAP_W;
    x1 = ((x1 % MOON_MAP_W) + MOON_MAP_W) % MOON_MAP_W;
    y0 = y0 < 0 ? 0 : (y0 > MOON_MAP_H - 1 ? MOON_MAP_H - 1 : y0);
    y1 = y1 < 0 ? 0 : (y1 > MOON_MAP_H - 1 ? MOON_MAP_H - 1 : y1);
    const float c00 = MOON_MAP[y0 * MOON_MAP_W + x0] * (1.0f / 255.0f);
    const float c10 = MOON_MAP[y0 * MOON_MAP_W + x1] * (1.0f / 255.0f);
    const float c01 = MOON_MAP[y1 * MOON_MAP_W + x0] * (1.0f / 255.0f);
    const float c11 = MOON_MAP[y1 * MOON_MAP_W + x1] * (1.0f / 255.0f);
    const float a = c00 + (c10 - c00) * tx;
    const float b = c01 + (c11 - c01) * tx;
    return a + (b - a) * ty;
}

static void ms_texel_dir(const MoonBake* bk, int i, int j, float* p) {
    // The frame moon.glsl reads back: u = atan2(x, z)/tau + 0.5, so longitude
    // runs east from -pi at column 0; v = 0.5 - asin(y)/pi, so row 0 is +90.
    const float lon = (((float)i + 0.5f) / (float)bk->w - 0.5f) * 6.28318530718f;
    const float lat = (0.5f - ((float)j + 0.5f) / (float)bk->h) * 3.14159265359f;
    const float cl = cosf(lat);
    p[0] = cl * sinf(lon);
    p[1] = sinf(lat);
    p[2] = cl * cosf(lon);
}

/*
 * PASS 1: the ground the craters land on -- maria coverage, the base albedo,
 * and the large-scale relief.
 */
static void ms_pass_base(void* ctx, int begin, int end) {
    MoonBake* bk = (MoonBake*)ctx;
    for (int j = begin; j < end; j++) {
        const float v = ((float)j + 0.5f) / (float)bk->h;
        for (int i = 0; i < bk->w; i++) {
            const float u = ((float)i + 0.5f) / (float)bk->w;
            const int idx = j * bk->w + i;
            float p[3];
            ms_texel_dir(bk, i, j, p);

            const float cover = ms_cover_at(u, v);
            bk->cover[idx] = cover;

            // Highland relief: broken, ridged terrain between the craters. The
            // maria drown it, which is what makes their shores read as
            // coastlines rather than as a change of paint.
            const float land = 1.0f - cover;
            float hgt = land * 0.0055f * (ms_fbm3(p[0] * 7.0f, p[1] * 7.0f, p[2] * 7.0f, 4) - 0.45f);
            hgt += land * 0.0030f * ms_ridge3(p[0] * 17.0f, p[1] * 17.0f, p[2] * 17.0f);
            // Wrinkle ridges: low sinuous welts on the mare floors, and one of
            // the few things visible INSIDE a sea at all.
            hgt += cover * 0.0016f * ms_ridge3(p[0] * 11.0f, p[1] * 11.0f, p[2] * 11.0f);
            hgt -= cover * MS_MARIA_DEPTH;
            bk->height[idx] = hgt;

            float alb = MS_ALBEDO_HIGHLAND + (MS_ALBEDO_MARE - MS_ALBEDO_HIGHLAND) * cover;
            // Mottle, so neither the highlands nor a sea is a flat wash. Real
            // mare basalts differ in titanium content patch to patch and the
            // difference is visible.
            alb *= 1.0f + 0.10f * (ms_fbm3(p[0] * 23.0f, p[1] * 23.0f, p[2] * 23.0f, 3) - 0.5f);
            bk->albedo[idx] = alb;
        }
    }
}

/*
 * PASS 2: the craters.
 *
 * Each band walks the WHOLE crater list in index order and stamps only the rows
 * it owns, which is what makes the result independent of the worker count: a
 * texel's history is the same fixed sequence however the rows are split. The
 * per-band cost of walking craters it does not touch is one latitude compare
 * each, against a stamp cost that is the sum of the footprints -- so the split
 * is free and the ordering is exact.
 *
 * An impact EXCAVATES. Inside the rim it erases what was there and drops the
 * floor; at the rim it piles the material back up. That order-dependent erase is
 * what makes overprinting read -- a young crater cutting a clean notch through
 * an old rim -- and it is the one thing a summed field of bumps cannot do,
 * because summing averages the overlap into mush.
 */
static void ms_pass_craters(void* ctx, int begin, int end) {
    MoonBake* bk = (MoonBake*)ctx;
    // Rows [begin, end) cover this latitude span; row j is centred at
    // lat = (0.5 - (j+0.5)/h) * pi, which DECREASES with j.
    const float band_hi = (0.5f - ((float)begin + 0.5f) / (float)bk->h) * 3.14159265359f;
    const float band_lo = (0.5f - ((float)end - 0.5f) / (float)bk->h) * 3.14159265359f;

    for (int c = 0; c < bk->crater_count; c++) {
        const MoonCrater* cr = &bk->craters[c];
        const float reach = cr->radius * 1.7f;
        if (cr->lat + reach < band_lo || cr->lat - reach > band_hi)
            continue;

        // Row range, clipped to the band. Row index rises as latitude falls.
        int j0 = (int)floorf((0.5f - (cr->lat + reach) / 3.14159265359f) * (float)bk->h);
        int j1 = (int)ceilf((0.5f - (cr->lat - reach) / 3.14159265359f) * (float)bk->h);
        if (j0 < begin) j0 = begin;
        if (j1 > end) j1 = end;

        const float inv_r = 1.0f / cr->radius;
        for (int j = j0; j < j1; j++) {
            const float lat = (0.5f - ((float)j + 0.5f) / (float)bk->h) * 3.14159265359f;
            // Longitude half-width of the footprint at this latitude. A cap of
            // fixed angular radius spans more longitude the further from the
            // equator it sits, without limit at the pole -- so past the point
            // where it wraps, take the whole row.
            const float cl = cosf(lat);
            int i0, i1;
            if (cl < 1e-3f || reach / cl >= 3.14159265359f) {
                i0 = 0;
                i1 = bk->w;
            } else {
                const float half = reach / cl;
                const float clon = atan2f(cr->cx, cr->cz);
                const float cu = clon / 6.28318530718f + 0.5f;
                i0 = (int)floorf((cu - half / 6.28318530718f) * (float)bk->w) - 1;
                i1 = (int)ceilf((cu + half / 6.28318530718f) * (float)bk->w) + 1;
            }
            const float sl = sinf(lat);
            for (int ii = i0; ii < i1; ii++) {
                const int i = ((ii % bk->w) + bk->w) % bk->w;
                const float lon = (((float)i + 0.5f) / (float)bk->w - 0.5f) * 6.28318530718f;
                const float px = cl * sinf(lon), py = sl, pz = cl * cosf(lon);
                // Chord for the angular distance: the two agree to a part in a
                // thousand at these radii, and acos in the inner loop of a
                // forty-thousand-crater bake is not worth that.
                const float ex = px - cr->cx, ey = py - cr->cy, ez = pz - cr->cz;
                const float d = sqrtf(ex * ex + ey * ey + ez * ez) * inv_r;
                if (d >= 1.7f)
                    continue;

                const int idx = j * bk->w + i;
                // A mare floods the crater that was there before it.
                const float damp = 1.0f - MS_MARIA_FLOOD * bk->cover[idx];
                if (damp <= 0.02f)
                    continue;

                float hgt = bk->height[idx];
                if (d < 1.0f) {
                    // Erase, then excavate. The erase is partial at the rim and
                    // total on the floor, so the two blend across the wall
                    // instead of stepping.
                    const float t = ms_smoothstep(1.0f, 0.70f, d);
                    hgt *= 1.0f - 0.90f * t * damp;
                    hgt -= cr->depth * (1.0f - d * d) * damp;
                }
                // Rim crest, peaking at the radius, plus the ejecta apron
                // falling away outside it.
                const float rt = 1.0f - fminf(1.0f, fabsf(d - 1.0f) / 0.30f);
                hgt += cr->rim * rt * rt * (3.0f - 2.0f * rt) * damp;
                if (d > 1.0f) {
                    const float at = 1.0f - (d - 1.0f) / 0.7f;
                    hgt += cr->rim * 0.30f * at * at * damp;
                }
                bk->height[idx] = hgt;

                // ALBEDO. Fresh craters expose bright unweathered material --
                // this is what is actually visible at full phase, where the
                // relief above contributes nothing at all because the light is
                // head-on. An old crater (fresh ~ 0) changes only its floor.
                float alb = bk->albedo[idx];
                if (d < 0.85f)
                    alb *= 1.0f - 0.10f * damp;
                if (cr->fresh > 0.02f) {
                    const float halo = ms_smoothstep(1.6f, 0.55f, d);
                    alb += cr->fresh * 0.55f * halo * (0.5f + 0.5f * damp);
                }
                bk->albedo[idx] = alb;
            }
        }
    }
}

/*
 * PASS 3: ray systems.
 *
 * Separate from the crater stamp and applied with a max, because rays lie ON TOP
 * of everything -- they are a thin veneer of ejecta thrown clear, so a later
 * small crater does not erase them and the order must not matter. They also
 * reach nine radii out, far past anything the stamp loop is shaped for.
 *
 * Frankly a painting rather than a simulation: streaks in bearing about the
 * centre, thresholded out of angular noise. Only a handful of craters have them
 * and it is the single most recognisable thing on a full moon after the seas.
 */
static void ms_pass_rays(void* ctx, int begin, int end) {
    MoonBake* bk = (MoonBake*)ctx;
    for (int r = 0; r < bk->ray_count; r++) {
        const MoonCrater* cr = &bk->rays[r];
        const float reach = cr->radius * MS_RAY_REACH;
        const float cos_reach = cosf(fminf(reach, 3.0f));
        // A tangent pair at the centre, for the bearing.
        float ax[3] = {0.0f, 1.0f, 0.0f};
        if (fabsf(cr->cy) > 0.9f) {
            ax[0] = 1.0f;
            ax[1] = 0.0f;
        }
        float t1[3] = {ax[1] * cr->cz - ax[2] * cr->cy, ax[2] * cr->cx - ax[0] * cr->cz,
                       ax[0] * cr->cy - ax[1] * cr->cx};
        const float t1n = sqrtf(t1[0] * t1[0] + t1[1] * t1[1] + t1[2] * t1[2]);
        t1[0] /= t1n;
        t1[1] /= t1n;
        t1[2] /= t1n;
        const float t2[3] = {cr->cy * t1[2] - cr->cz * t1[1], cr->cz * t1[0] - cr->cx * t1[2],
                             cr->cx * t1[1] - cr->cy * t1[0]};

        for (int j = begin; j < end; j++) {
            for (int i = 0; i < bk->w; i++) {
                float p[3];
                ms_texel_dir(bk, i, j, p);
                const float dp = p[0] * cr->cx + p[1] * cr->cy + p[2] * cr->cz;
                if (dp <= cos_reach)
                    continue;
                const float ang = acosf(ms_clampf(dp, -1.0f, 1.0f));
                // Bearing about the centre, from the tangential component.
                const float bx = p[0] - dp * cr->cx, by = p[1] - dp * cr->cy,
                            bz = p[2] - dp * cr->cz;
                const float bn = sqrtf(bx * bx + by * by + bz * bz);
                if (bn < 1e-6f)
                    continue;
                const float e1 = (bx * t1[0] + by * t1[1] + bz * t1[2]) / bn;
                const float e2 = (bx * t2[0] + by * t2[1] + bz * t2[2]) / bn;
                // Noise sampled on a ring of the bearing direction, so the
                // streaks stay radial instead of breaking into blobs. Two
                // frequencies: a few broad rays with fine ones between them.
                float streak = ms_value3(e1 * 6.0f + cr->cx * 31.0f, e2 * 6.0f + cr->cy * 31.0f,
                                         cr->cz * 31.0f);
                streak = 0.65f * streak +
                         0.35f * ms_value3(e1 * 19.0f, e2 * 19.0f, cr->cz * 53.0f + 7.0f);
                streak = ms_smoothstep(0.54f, 0.78f, streak);
                // Fade with distance, and hold off inside the ejecta blanket
                // the crater stamp already drew.
                const float fade = (1.0f - ang / reach) * ms_smoothstep(1.1f, 2.4f, ang / cr->radius);
                const int idx = j * bk->w + i;
                const float gain = cr->fresh * 0.42f * streak * fade * fade *
                                   (1.0f - 0.55f * bk->cover[idx]);
                // Additive, and so independent of the order the ray craters are
                // visited in -- two systems really do overlap near Tycho.
                bk->albedo[idx] += gain;
            }
        }
    }
}

/*
 * PASS 4: differentiate the height into a normal, and pack.
 *
 * Central differences in ARC LENGTH, not in texel index -- a step in longitude
 * is cos(lat) times shorter on the ground than a step in latitude, so
 * differencing in index space tilts every normal toward the poles. The cosine is
 * floored because it reaches zero there and the gradient would run away on a row
 * that is really one point.
 *
 * The normal is stored in the LOCAL TANGENT FRAME (east, north, up), which makes
 * it exactly normalize(-ge, -gn, 1) and costs no trigonometry at all. It is also
 * what makes the MIPS correct: a tangent-space normal averages toward (0,0,1),
 * which is flat, so a moon drawn a few pixels across reads as a smooth sphere.
 * Stored in the body frame instead, the average over a coarse mip tends toward
 * the average of the sphere's own normals -- which is zero, and normalising zero
 * is how a distant moon turns to noise.
 */
static void ms_pass_normals(void* ctx, int begin, int end) {
    MoonBake* bk = (MoonBake*)ctx;
    const float dlat_arc = 3.14159265359f / (float)bk->h;
    for (int j = begin; j < end; j++) {
        const float lat = (0.5f - ((float)j + 0.5f) / (float)bk->h) * 3.14159265359f;
        const float cl = fmaxf(cosf(lat), 0.02f);
        const float dlon_arc = 6.28318530718f / (float)bk->w * cl;
        const int jn = j > 0 ? j - 1 : 0;
        const int js = j < bk->h - 1 ? j + 1 : bk->h - 1;
        const float inv_lat = 1.0f / ((float)(js - jn) * dlat_arc);
        for (int i = 0; i < bk->w; i++) {
            const int ie = (i + 1) % bk->w;
            const int iw = (i + bk->w - 1) % bk->w;
            const float ge =
                (bk->height[j * bk->w + ie] - bk->height[j * bk->w + iw]) / (2.0f * dlon_arc);
            const float gn = (bk->height[jn * bk->w + i] - bk->height[js * bk->w + i]) * inv_lat;

            const float nl = sqrtf(ge * ge + gn * gn + 1.0f);
            const float nx = -ge / nl, ny = -gn / nl, nz = 1.0f / nl;

            const float a = ms_clampf(bk->albedo[j * bk->w + i], MS_ALBEDO_FLOOR, MS_ALBEDO_CEIL);
            unsigned char* o = &bk->out[(j * bk->w + i) * 4];
            o[0] = (unsigned char)ms_clampf((nx * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f, 255.0f);
            o[1] = (unsigned char)ms_clampf((ny * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f, 255.0f);
            o[2] = (unsigned char)ms_clampf((nz * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f, 255.0f);
            o[3] = (unsigned char)ms_clampf(a * 255.0f + 0.5f, 0.0f, 255.0f);
        }
    }
}

unsigned char* moon_surface_bake(int w, int h, int workers) {
    if (w <= 0 || h <= 0)
        return NULL;

    const int total = MS_TIER1_COUNT + MS_TIER2_COUNT + MS_TIER3_COUNT;
    MoonCrater* craters = (MoonCrater*)malloc(sizeof(MoonCrater) * (size_t)total);
    float* height = (float*)malloc(sizeof(float) * (size_t)w * (size_t)h);
    float* albedo = (float*)malloc(sizeof(float) * (size_t)w * (size_t)h);
    float* cover = (float*)malloc(sizeof(float) * (size_t)w * (size_t)h);
    unsigned char* out = (unsigned char*)malloc((size_t)w * (size_t)h * 4u);
    if (!craters || !height || !albedo || !cover || !out) {
        free(craters);
        free(height);
        free(albedo);
        free(cover);
        free(out);
        return NULL;
    }

    // Oldest first: the tiers ARE the stratigraphy, so a small crater cuts a
    // clean notch through a basin rim and never the reverse.
    ms_fill_tier(craters, MS_TIER1_COUNT, MS_TIER1_MIN, MS_TIER1_MAX, 0x9e3779b9u);
    ms_fill_tier(craters + MS_TIER1_COUNT, MS_TIER2_COUNT, MS_TIER2_MIN, MS_TIER2_MAX,
                 0x85ebca6bu);
    ms_fill_tier(craters + MS_TIER1_COUNT + MS_TIER2_COUNT, MS_TIER3_COUNT, MS_TIER3_MIN,
                 MS_TIER3_MAX, 0xc2b2ae35u);

    // The ray casters: the largest of the fresh ones, which is the correlation
    // the real thing has -- a big young crater keeps its rays for a billion
    // years, a small one loses them.
    MoonCrater rays[MS_RAY_CRATERS];
    int ray_count = 0;
    for (int pass = 0; pass < 2 && ray_count < MS_RAY_CRATERS; pass++) {
        const float bar = pass == 0 ? 0.35f : 0.12f;
        for (int i = 0; i < MS_TIER1_COUNT + MS_TIER2_COUNT && ray_count < MS_RAY_CRATERS; i++) {
            if (craters[i].fresh > bar && craters[i].radius > MS_TIER2_MIN * 1.5f) {
                bool seen = false;
                for (int k = 0; k < ray_count; k++)
                    seen = seen || (rays[k].cx == craters[i].cx && rays[k].cy == craters[i].cy);
                if (!seen) {
                    rays[ray_count] = craters[i];
                    // Rays are the crater's signature whether or not the draw
                    // made it especially fresh; the selection above is what
                    // decides, not the magnitude.
                    rays[ray_count].fresh = fmaxf(craters[i].fresh, 0.55f);
                    ray_count++;
                }
            }
        }
    }

    MoonBake bk = {.w = w,
                   .h = h,
                   .height = height,
                   .albedo = albedo,
                   .cover = cover,
                   .out = out,
                   .craters = craters,
                   .crater_count = total,
                   .rays = rays,
                   .ray_count = ray_count};

    const int nw = cetra_bake_workers(workers, h);
    cetra_bake_bands(h, nw, ms_pass_base, &bk);
    cetra_bake_bands(h, nw, ms_pass_craters, &bk);
    cetra_bake_bands(h, nw, ms_pass_rays, &bk);
    cetra_bake_bands(h, nw, ms_pass_normals, &bk);

    free(craters);
    free(height);
    free(albedo);
    free(cover);
    return out;
}
