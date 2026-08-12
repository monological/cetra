#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "grass.h"
#include "ground.h"
#include "cetra/mesh_builder.h"
#include "cetra/noise.h"

// Cap, for the same reason the tree has one: the sliders multiply out and a
// drag must not be able to stall the frame that moved it.
#define GRASS_MAX_TUFTS 12000

#define BLADE_ROWS 4 // 4 rows x 2 columns = 8 verts, 3 quads

// Local deterministic RNG. Not libc's: the texture pass owns srand(), so
// sharing that stream would make the field depend on unrelated work.
typedef struct {
    uint32_t s;
} GrRng;

static uint32_t gr_next(GrRng* r) {
    r->s ^= r->s << 13;
    r->s ^= r->s >> 17;
    r->s ^= r->s << 5;
    return r->s;
}

static float gr_randf(GrRng* r, float lo, float hi) {
    return lo + (float)(gr_next(r) & 0xFFFFFF) / (float)0x1000000 * (hi - lo);
}

static float gr_smoothstep(float e0, float e1, float x) {
    float t = (x - e0) / (e1 - e0);
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}

// ---------------------------------------------------------------------------
// Blades
// ---------------------------------------------------------------------------

// One blade: a tapered strip that arcs over. The arc is the whole character --
// a straight blade reads as a spike, and a field of them as a bed of nails.
//
// `lean` is the compass direction the blade bends toward; `phase` is its tuft's
// wind phase; the two colours are the root and tip of its vertical gradient.
static void emit_blade(MeshBuilder* mb, const vec3 root, float lean, float height, float width,
                       float bend, float twist, float phase, const float* col_root,
                       const float* col_tip) {
    vec3 fwd = {cosf(lean), 0.0f, sinf(lean)};
    vec3 up = {0.0f, 1.0f, 0.0f};

    unsigned int base = mb->vcount;

    for (int row = 0; row < BLADE_ROWS; row++) {
        float t = (float)row / (float)(BLADE_ROWS - 1);

        // Arc: rises fast, leans away quadratically, and loses a little height
        // as it does -- a blade that bends over does not stay as tall.
        float rise = height * t * (1.0f - 0.22f * t * t);
        float reach = bend * height * t * t;

        vec3 centre;
        glm_vec3_copy((float*)root, centre);
        glm_vec3_muladds(up, rise, centre);
        glm_vec3_muladds(fwd, reach, centre);

        // Analytic tangent along the blade, for the normal.
        vec3 along;
        glm_vec3_scale(up, height * (1.0f - 0.66f * t * t), along);
        glm_vec3_muladds(fwd, 2.0f * bend * height * t, along);
        glm_vec3_normalize(along);

        // Width axis, twisted slightly along the length so the strip is never
        // perfectly edge-on for its whole height.
        vec3 side;
        glm_vec3_cross(up, fwd, side);
        glm_vec3_normalize(side);
        glm_vec3_rotate(side, twist * t, along);
        glm_vec3_normalize(side);

        vec3 nrm;
        glm_vec3_cross(side, along, nrm);
        glm_vec3_normalize(nrm);

        // Taper to a near-point; a blunt tip is cheaper than degenerate tris.
        float w = width * powf(1.0f - t, 0.7f);
        if (w < width * 0.06f)
            w = width * 0.06f;

        float col[4];
        for (int c = 0; c < 3; c++)
            col[c] = col_root[c] + (col_tip[c] - col_root[c]) * t;
        col[3] = 1.0f;

        // Flex is the wind weight: zero at the root keeps the blade planted.
        float flex = powf(t, 1.3f);

        for (int s = 0; s < 2; s++) {
            float sgn = s == 0 ? -0.5f : 0.5f;
            vec3 pv;
            glm_vec3_copy(centre, pv);
            glm_vec3_muladds(side, sgn * w, pv);
            mb_vertex(mb, pv, nrm, side, s == 0 ? 0.0f : 1.0f, t, phase, flex, col);
        }
    }

    for (int row = 0; row < BLADE_ROWS - 1; row++) {
        unsigned int a = base + (unsigned int)(row * 2);
        unsigned int b = a + 1;
        unsigned int c = a + 2;
        unsigned int d = a + 3;
        mb_tri(mb, a, c, d);
        mb_tri(mb, a, d, b);
    }
}

// A bloom: two quads crossing at right angles, so it reads from any direction
// without needing to face the camera.
static void emit_flower(MeshBuilder* mb, const vec3 at, float size, float phase, float flex,
                        const float* col) {
    vec3 up = {0.0f, 1.0f, 0.0f};
    for (int q = 0; q < 2; q++) {
        float a = q == 0 ? 0.0f : 1.5707963f;
        vec3 side = {cosf(a), 0.0f, sinf(a)};
        // Mostly upward, only leaning with the quad. A bloom faces the sky, and
        // a near-horizontal normal sits close to the mirror angle for a low sun
        // -- whichever flowers happened to land near that alignment caught a
        // hard specular glint and winked as the wind swept them through it.
        vec3 nrm = {-sinf(a) * 0.30f, 1.0f, cosf(a) * 0.30f};
        glm_vec3_normalize(nrm);

        unsigned int base = mb->vcount;
        for (int row = 0; row < 2; row++) {
            for (int s = 0; s < 2; s++) {
                vec3 pv;
                glm_vec3_copy((float*)at, pv);
                glm_vec3_muladds(side, (s == 0 ? -0.5f : 0.5f) * size, pv);
                glm_vec3_muladds(up, (float)row * size, pv);
                mb_vertex(mb, pv, nrm, side, s == 0 ? 0.0f : 1.0f, (float)row, phase, flex,
                          col);
            }
        }
        mb_tri(mb, base, base + 1, base + 3);
        mb_tri(mb, base, base + 3, base + 2);
    }
}

// ---------------------------------------------------------------------------
// Field
// ---------------------------------------------------------------------------

bool grass_build_mesh(const GrassParams* p, Mesh* mesh) {
    if (!p || !mesh || p->radius <= 1.0f || p->density <= 0.0f || p->height <= 0.0f)
        return false;

    GrRng rng = {(uint32_t)p->seed * 2246822519u + 3266489917u};
    if (rng.s == 0)
        rng.s = 1;
    noise_seed((unsigned int)p->seed);

    // Grid pitch from the requested density (tufts per 100 square units).
    float pitch = sqrtf(100.0f / p->density);
    if (pitch < 1.0f)
        pitch = 1.0f;
    int half = (int)(p->radius / pitch) + 1;

    // Reserve for the expected tuft count rather than growing into it.
    int est_tufts = (int)(3.14159f * p->radius * p->radius / (pitch * pitch) * 0.5f);
    if (est_tufts > GRASS_MAX_TUFTS)
        est_tufts = GRASS_MAX_TUFTS;
    size_t est_blades = (size_t)est_tufts * 7;

    MeshBuilder mb;
    if (!mb_init(&mb, est_blades * BLADE_ROWS * 2, est_blades * (BLADE_ROWS - 1) * 6, true))
        return false;

    int tufts = 0;

    for (int gz = -half; gz <= half && tufts < GRASS_MAX_TUFTS; gz++) {
        for (int gx = -half; gx <= half && tufts < GRASS_MAX_TUFTS; gx++) {
            // Jitter off the lattice, or the field reads as a grid.
            float cx = (float)gx * pitch + gr_randf(&rng, -0.5f, 0.5f) * pitch;
            float cz = (float)gz * pitch + gr_randf(&rng, -0.5f, 0.5f) * pitch;

            float d = sqrtf(cx * cx + cz * cz);
            if (d > p->radius || d < p->clear_radius)
                continue;

            // Patchiness: a slow field decides where grass grows at all, so it
            // gathers into islands with bare earth between rather than carpet.
            float patch = noise_perlin3(cx * 0.012f, 0.0f, cz * 0.012f) * 0.5f + 0.5f;
            float threshold = p->patchiness * 0.55f;
            if (patch < threshold)
                continue;
            // Thin toward a patch's edge so it fades out instead of stopping.
            float edge = gr_smoothstep(threshold, threshold + 0.18f, patch);

            // A finer field varies density within a patch.
            float fine = noise_perlin3(cx * 0.06f, 3.0f, cz * 0.06f) * 0.5f + 0.5f;
            float take = edge * (0.55f + 0.45f * fine);
            // Fade the outer rim of the whole field.
            take *= gr_smoothstep(p->radius, p->radius * 0.75f, d);
            if (gr_randf(&rng, 0.0f, 1.0f) > take)
                continue;

            // --- one tuft -------------------------------------------------
            tufts++;
            float phase = gr_randf(&rng, 0.0f, 1.0f);
            int blades = 5 + (int)gr_randf(&rng, 0.0f, 4.99f);
            float tuft_lean = gr_randf(&rng, 0.0f, 6.2831853f);

            // Whole-patch colour drift: some areas greener, some drier.
            float dry = noise_perlin3(cx * 0.02f + 11.0f, 7.0f, cz * 0.02f) * 0.5f + 0.5f;

            for (int i = 0; i < blades; i++) {
                float ox = gr_randf(&rng, -0.55f, 0.55f) * pitch * 0.45f;
                float oz = gr_randf(&rng, -0.55f, 0.55f) * pitch * 0.45f;
                float bx = cx + ox, bz = cz + oz;

                vec3 root = {bx, ground_height_at(bx, bz), bz};

                // Blades splay outward from the tuft's centre.
                float lean = tuft_lean + gr_randf(&rng, -1.1f, 1.1f);
                float hscale = gr_randf(&rng, 0.65f, 1.35f);
                float bend = p->bend * gr_randf(&rng, 0.6f, 1.5f);

                bool seed_head = gr_randf(&rng, 0.0f, 1.0f) < p->seed_head_amount;
                float height = p->height * hscale * (seed_head ? 1.7f : 1.0f);
                float width = p->blade_width * (seed_head ? 0.45f : gr_randf(&rng, 0.8f, 1.25f));
                if (seed_head)
                    bend *= 1.4f; // taller stalks nod over further

                // Fresh green through olive to straw, per blade and per patch.
                float tint = gr_randf(&rng, 0.0f, 1.0f) * 0.6f + dry * 0.4f;
                const float col_root[3] = {0.045f + 0.05f * tint, 0.085f + 0.05f * tint,
                                     0.030f + 0.02f * tint};
                const float col_tip[3] = {0.20f + 0.32f * tint, 0.36f + 0.20f * tint,
                                    0.10f + 0.10f * tint};

                emit_blade(&mb, root, lean, height, width, bend, gr_randf(&rng, -0.5f, 0.5f),
                           phase, col_root, col_tip);

                if (gr_randf(&rng, 0.0f, 1.0f) < p->flower_amount) {
                    // Sit the bloom where this blade's tip ended up, so it
                    // rides the same wind rather than floating beside it.
                    vec3 tip = {bx + cosf(lean) * bend * height,
                                root[1] + height * 0.78f * (1.0f - 0.22f),
                                bz + sinf(lean) * bend * height};
                    const float petals[4][3] = {{0.95f, 0.88f, 0.35f},
                                                {0.92f, 0.55f, 0.62f},
                                                {0.85f, 0.85f, 0.92f},
                                                {0.95f, 0.72f, 0.30f}};
                    int pi = (int)gr_randf(&rng, 0.0f, 3.99f);
                    const float col[4] = {petals[pi][0], petals[pi][1], petals[pi][2], 1.0f};
                    emit_flower(&mb, tip, p->blade_width * 2.6f, phase, 1.0f, col);
                }

                if (!mb.ok)
                    break;
            }
        }
    }

    return mb_transfer(&mb, mesh);
}
