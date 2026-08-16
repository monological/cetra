#include <math.h>
#include <stdlib.h>

#include <cglm/cglm.h>

#include "sand.h"

// The 2D noise, and its seeding hazard with it -- see vegetation_tex.h.
#include "vegetation_tex.h"

/*
 * Sand relief, as three things at three scales that a real beach also has three of:
 *
 *   RIPPLES, a directional train. The one feature that says "sand" rather than "noise":
 *   wind-blown ripples are regular, roughly parallel, and asymmetric. Made by warping a
 *   sine along the ripple axis with low-frequency noise, so the train wanders the way a
 *   real one does instead of reading as corrugated iron.
 *
 *   DRIFT, broad fBm. What keeps a large tiled area from looking flat: gentle mounding
 *   an order of magnitude wider than the ripples.
 *
 *   GRAIN, Worley at high frequency. Individual grains are far below a texel at any sane
 *   tiling, so this is not literal -- it is the sparkle that survives minification, and
 *   it is deliberately weak because sand that glitters per-texel reads as gravel.
 */
/*
 * The ripple frequency is a VIEWING decision, not a physical one, and the two disagree.
 * Real wind ripples sit 5 to 15 cm apart, which at this app's tiling is a cycle every few
 * texels and well under a pixel at any distance a beach is seen from -- so a physically
 * spaced train does not render as ripples, it renders as moiré beating against the mip
 * chain, and at grazing incidence isotropic mips make it worse. Coarser and much weaker is
 * what actually reads as raked sand.
 */
#define SAND_RIPPLE_CYCLES 7.0f
#define SAND_RIPPLE_WARP 1.8f
#define SAND_RIPPLE_WEIGHT 0.16f
/*
 * The drift is the tile's LOW frequency, and low frequency is what makes a repeat legible.
 *
 * It is here to stop a large tiled area reading flat, and at a weight where it dominated the
 * field it did the opposite: a beach is tens of tiles across, and a soft light-and-dark mottle
 * is precisely the feature the eye recognises when it comes round again. Fine detail repeating
 * is invisible; a blob repeating is wallpaper. Held low enough to break flatness without
 * carrying a signature -- the island's own dome supplies the large-scale shape here, so this
 * does not have to.
 */
#define SAND_DRIFT_WEIGHT 0.11f
#define SAND_GRAIN_WEIGHT 0.16f
// How much of the ripple survives where the drift says the sand is smooth. Ripples come in
// patches on a real beach; a train at constant amplitude over the whole surface is the
// single thing that most makes procedural sand look procedural.
#define SAND_RIPPLE_PATCHY 0.75f

/*
 * EVERY TERM HERE IS PERIODIC OVER THE TILE, and that is a hard requirement rather than a
 * refinement: this texture is repeated tens of times across a beach, so a field that does not
 * close at u = 1 puts a discontinuity on every tile boundary and the ground prints a hard
 * grid. That is not a filtering artefact and no amount of mip or anisotropy touches it.
 *
 * Which costs the ripple its free choice of angle. The train's phase is a whole number of
 * cycles in each of u and v, so the direction is quantised to the ratio of two integers --
 * the nearest such direction to the one asked for, which at the 45 degrees this is used at is
 * exact anyway.
 */
void sand_height_field(float* out, int width, int height, float ripple_angle) {
    if (!out || width <= 0 || height <= 0)
        return;

    // Whole cycles per tile along each axis. Rounded from the requested direction, and at
    // least one, or a near-axis angle would round to zero and kill the train on that axis.
    int nu = (int)lroundf(SAND_RIPPLE_CYCLES * cosf(ripple_angle));
    int nv = (int)lroundf(SAND_RIPPLE_CYCLES * sinf(ripple_angle));
    if (nu == 0 && nv == 0)
        nu = 1;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const float u = (float)x / (float)width;
            const float v = (float)y / (float)height;

            // The across-ripple coordinate, in CYCLES, so the sine below takes it directly.
            const float across = (float)nu * u + (float)nv * v;

            // The warp bends the crests rather than translating them. Its own frequency is a
            // whole number of cells so it wraps with everything else.
            const float warp = veg_fbm2_tiled(u * 3.0f, v * 3.0f, 3, 0.5f, 3, 3);
            float ripple = sinf((across + warp * SAND_RIPPLE_WARP) * 6.28318530718f);
            /*
             * Asymmetric on purpose: a wind ripple has a long shallow windward face and a
             * short steep lee one, and a bare sine has neither. Skewing toward the crest
             * is most of what separates this from a corrugation.
             */
            ripple = ripple >= 0.0f ? powf(ripple, 0.72f) : -powf(-ripple, 1.6f);

            // The offsets only shift the lattice; a whole number of cells keeps the wrap.
            const float drift = veg_fbm2_tiled(u * 4.0f + 31.0f, v * 4.0f + 17.0f, 4, 0.55f, 4, 4);
            const float grain = 1.0f - veg_worley2_tiled(u * 96.0f, v * 96.0f, 5150u, 96, 96);

            // Ripples in patches, driven by the same drift that mounds the surface: the
            // train is strongest where the sand has piled and fades where it has not.
            const float patch = 1.0f - SAND_RIPPLE_PATCHY * (1.0f - drift);

            float h = 0.5f + ripple * 0.5f * SAND_RIPPLE_WEIGHT * patch +
                      (drift - 0.5f) * SAND_DRIFT_WEIGHT + (grain - 0.5f) * SAND_GRAIN_WEIGHT;
            out[y * width + x] = fminf(fmaxf(h, 0.0f), 1.0f);
        }
    }
}

/*
 * Separable box blur that WRAPS, run twice to approximate a Gaussian.
 *
 * Wrapping because the result is subtracted from a field that tiles: a blur that clamped
 * at the edges would leave a rim the subtraction turns into a seam every tile, which is
 * the artefact the subtraction exists to remove.
 */
static void _blur_wrap(const float* src, float* dst, float* tmp, int width, int height,
                       int radius) {
    const float inv = 1.0f / (float)(radius * 2 + 1);
    const float* in = src;
    for (int pass = 0; pass < 2; pass++) {
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                float acc = 0.0f;
                for (int i = -radius; i <= radius; i++)
                    acc += in[y * width + ((x + i) % width + width) % width];
                tmp[y * width + x] = acc * inv;
            }
        }
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                float acc = 0.0f;
                for (int i = -radius; i <= radius; i++)
                    acc += tmp[(((y + i) % height + height) % height) * width + x];
                dst[y * width + x] = acc * inv;
            }
        }
        in = dst;
    }
}

unsigned char* sand_albedo(int width, int height, const float* field) {
    if (!field)
        return NULL;
    unsigned char* data = malloc((size_t)width * height * 3);
    if (!data)
        return NULL;

    /*
     * HIGH-PASSED, and that is what stops a tiled beach reading as tiles.
     *
     * The height field's largest term is the drift -- broad fBm, an order of magnitude wider
     * than the ripples -- and it is there to keep the RELIEF from looking flat. Printed into
     * colour it does the opposite: a beach is tens of tiles across, and a soft light-and-dark
     * mottle is exactly the kind of feature the eye recognises when it comes round again. Fine
     * detail repeating is invisible; a blob repeating is wallpaper.
     *
     * So the colour keeps only what survives subtracting a wide blur, and the drift stays in
     * the height, where it still mounds the surface and where a normal map's much gentler
     * shading does not carry a recognisable signature.
     *
     * Near-neutral and narrow-range, because vertex colour multiplies this and carries the hue
     * -- dry sand, wet sand, and the upland it grades into are all the same texture, and a map
     * with its own strong colour would fight every one of them.
     *
     * The base is where dry white sand actually sits. sRGB 0.88 is about 0.75 linear, and
     * against the 0.93 vertex colour that lands near the 0.6-0.7 albedo measured for dry
     * carbonate sand. The 0.72 it started at is 0.47 linear, which is wet-sand dark and made
     * every authored colour above it read muddy.
     */
    const float base = 0.88f;
    const float detail_gain = 1.30f;
    float* low = malloc((size_t)width * height * sizeof(float));
    float* tmp = malloc((size_t)width * height * sizeof(float));
    if (low && tmp) {
        // An eighth of the tile: wider than the ripple train, narrower than the drift, so
        // the subtraction takes the mottle and leaves the ripples.
        _blur_wrap(field, low, tmp, width, height, width / 8 > 1 ? width / 8 : 1);
    }
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const float h = field[y * width + x];
            const float detail = (low && tmp) ? h - low[y * width + x] : h - 0.5f;
            const float shade = base + detail * detail_gain;
            // A whisper of warmth in the troughs, where damper, finer material collects.
            const float warm = (0.5f - detail) * 0.05f;
            const int idx = (y * width + x) * 3;
            data[idx + 0] = (unsigned char)(fminf(fmaxf(shade + warm, 0.0f), 1.0f) * 255.0f);
            data[idx + 1] =
                (unsigned char)(fminf(fmaxf(shade + warm * 0.6f, 0.0f), 1.0f) * 255.0f);
            data[idx + 2] = (unsigned char)(fminf(fmaxf(shade, 0.0f), 1.0f) * 255.0f);
        }
    }
    free(low);
    free(tmp);
    return data;
}

unsigned char* sand_normal(int width, int height, const float* field) {
    if (!field)
        return NULL;
    unsigned char* data = malloc((size_t)width * height * 3);
    if (!data)
        return NULL;

    // Gentler than bark's 14: sand relief is millimetres over a tile that covers metres,
    // and a strong normal here reads as rock rather than as a raked surface. Gentler again
    // than the 5.5 this started at, which lit the ripple train hard enough that its own
    // aliasing became the dominant feature of a beach seen at any distance.
    const float strength = 2.2f;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const int x0 = (x - 1 + width) % width;
            const int x1 = (x + 1) % width;
            const int y0 = (y - 1 + height) % height;
            const int y1 = (y + 1) % height;

            const float dX = field[y * width + x1] - field[y * width + x0];
            const float dY = field[y1 * width + x] - field[y0 * width + x];

            vec3 normal = {-dX * strength, -dY * strength, 1.0f};
            glm_vec3_normalize(normal);

            const int idx = (y * width + x) * 3;
            data[idx + 0] = (unsigned char)((normal[0] * 0.5f + 0.5f) * 255.0f);
            data[idx + 1] = (unsigned char)((normal[1] * 0.5f + 0.5f) * 255.0f);
            data[idx + 2] = (unsigned char)((normal[2] * 0.5f + 0.5f) * 255.0f);
        }
    }
    return data;
}

unsigned char* sand_roughness(int width, int height, const float* field) {
    if (!field)
        return NULL;
    unsigned char* data = malloc((size_t)width * height * 3);
    if (!data)
        return NULL;

    // Troughs are packed and a little smoother; crests are loose grain and fully rough.
    // A narrow range, since none of this is wet -- the wet band is the vertex colour's job.
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const float h = field[y * width + x];
            const float rough = 0.82f + h * 0.14f;
            const unsigned char v = (unsigned char)(fminf(rough, 1.0f) * 255.0f);
            const int idx = (y * width + x) * 3;
            data[idx + 0] = v;
            data[idx + 1] = v;
            data[idx + 2] = v;
        }
    }
    return data;
}
