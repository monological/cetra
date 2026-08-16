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
#define SAND_DRIFT_WEIGHT 0.34f
#define SAND_GRAIN_WEIGHT 0.16f
// How much of the ripple survives where the drift says the sand is smooth. Ripples come in
// patches on a real beach; a train at constant amplitude over the whole surface is the
// single thing that most makes procedural sand look procedural.
#define SAND_RIPPLE_PATCHY 0.75f

void sand_height_field(float* out, int width, int height, float ripple_angle) {
    if (!out || width <= 0 || height <= 0)
        return;

    const float ca = cosf(ripple_angle);
    const float sa = sinf(ripple_angle);

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const float u = (float)x / (float)width;
            const float v = (float)y / (float)height;

            // The ripple axis, rotated. Only the across-ripple coordinate drives the
            // train; the along-ripple one drives the warp, which is what bends the
            // crests rather than translating them.
            const float across = u * ca + v * sa;
            const float along = -u * sa + v * ca;

            const float warp = veg_fbm2(along * 3.0f, across * 1.5f, 3, 0.5f);
            float ripple = sinf((across * SAND_RIPPLE_CYCLES + warp * SAND_RIPPLE_WARP) *
                                6.28318530718f);
            /*
             * Asymmetric on purpose: a wind ripple has a long shallow windward face and a
             * short steep lee one, and a bare sine has neither. Skewing toward the crest
             * is most of what separates this from a corrugation.
             */
            ripple = ripple >= 0.0f ? powf(ripple, 0.72f) : -powf(-ripple, 1.6f);

            const float drift = veg_fbm2(u * 4.0f + 31.0f, v * 4.0f + 17.0f, 4, 0.55f);
            const float grain = 1.0f - veg_worley2(u * 96.0f, v * 96.0f, 5150u);

            // Ripples in patches, driven by the same drift that mounds the surface: the
            // train is strongest where the sand has piled and fades where it has not.
            const float patch = 1.0f - SAND_RIPPLE_PATCHY * (1.0f - drift);

            float h = 0.5f + ripple * 0.5f * SAND_RIPPLE_WEIGHT * patch +
                      (drift - 0.5f) * SAND_DRIFT_WEIGHT + (grain - 0.5f) * SAND_GRAIN_WEIGHT;
            out[y * width + x] = fminf(fmaxf(h, 0.0f), 1.0f);
        }
    }
}

unsigned char* sand_albedo(int width, int height, const float* field) {
    if (!field)
        return NULL;
    unsigned char* data = malloc((size_t)width * height * 3);
    if (!data)
        return NULL;

    /*
     * Near-neutral and narrow-range, because vertex colour multiplies this and carries the
     * hue -- dry sand, wet sand, and the upland it grades into are all the same texture.
     * A map with its own strong colour would fight every one of them.
     *
     * Centred slightly above 0.5 so the product lands on the vertex colour rather than
     * under it: a texture that averages dark makes every authored colour read muddy.
     */
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const float h = field[y * width + x];
            const float shade = 0.72f + (h - 0.5f) * 0.34f;
            // A whisper of warmth in the troughs, where damper, finer material collects.
            const float warm = (1.0f - h) * 0.05f;
            const int idx = (y * width + x) * 3;
            data[idx + 0] = (unsigned char)(fminf(shade + warm, 1.0f) * 255.0f);
            data[idx + 1] = (unsigned char)(fminf(shade + warm * 0.6f, 1.0f) * 255.0f);
            data[idx + 2] = (unsigned char)(fminf(shade, 1.0f) * 255.0f);
        }
    }
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
