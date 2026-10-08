#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "tree_gen.h"
#include "../mesh_builder.h"
#include "../util.h"

// Growth budget. The slider ranges multiply out fast (depth 6 x 5-way splits x
// laterals is millions of branches), so generation stops dead at these caps
// rather than stalling the frame that moved the slider.
#define TG_MAX_BRANCHES 12000
#define TG_MAX_POINTS   150000

// World units covered by one tile of the bark texture, both around the trunk
// and along it -- equal on both axes so texel density stays square.
//
// Sized so a trunk carries only a few tiles around its girth. Tiling finer
// than this is self-defeating: the trunk is a hundred-odd pixels wide on
// screen, so a dozen repeats land several mip levels down and average out to
// flat colour, which is exactly what makes bark read as smooth plastic.
#define TG_BARK_TILE 18.0f

// Spine samples per generation. The trunk carries the most because its curve is
// the one the eye follows; twigs are nearly straight and would only cost verts.
static const int k_points_by_depth[] = {12, 9, 7, 5, 4, 3, 3, 3};

// ---------------------------------------------------------------------------
// Deterministic RNG
//
// Its own generator rather than rand(): the texture pass calls srand() for
// Perlin permutation setup, so sharing libc's stream would make tree shape
// depend on whether textures had been regenerated.
// ---------------------------------------------------------------------------

typedef struct {
    uint32_t s;
} TgRng;

static uint32_t tg_next(TgRng* r) {
    r->s ^= r->s << 13;
    r->s ^= r->s >> 17;
    r->s ^= r->s << 5;
    return r->s;
}

static float tg_randf(TgRng* r, float lo, float hi) {
    return lo + (float)(tg_next(r) & 0xFFFFFF) / (float)0x1000000 * (hi - lo);
}

static float tg_smoothstep(float e0, float e1, float x) {
    float t = (x - e0) / (e1 - e0);
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}

// ---------------------------------------------------------------------------
// Presets
// ---------------------------------------------------------------------------

static const char* const k_preset_names[TREE_PRESET_COUNT] = {
    [TREE_PRESET_BROADLEAF] = "broadleaf", [TREE_PRESET_DEAD] = "dead",
    [TREE_PRESET_SPRUCE] = "spruce",       [TREE_PRESET_FIR] = "fir",
    [TREE_PRESET_SNAG] = "snag",
};

const char* tree_preset_name(TreePreset preset) {
    return preset >= 0 && preset < TREE_PRESET_COUNT ? k_preset_names[preset] : "unknown";
}

bool tree_preset_from_name(const char* name, TreePreset* out) {
    for (int i = 0; name && i < TREE_PRESET_COUNT; i++) {
        if (!strcmp(name, k_preset_names[i])) {
            *out = (TreePreset)i;
            return true;
        }
    }
    return false;
}

void tree_params_preset(TreeParams* p, TreePreset preset, int seed) {
    memset(p, 0, sizeof(*p));
    p->seed = seed;
    p->trunk_length = 125.0f;
    p->twist = 137.5f;
    switch (preset) {
        case TREE_PRESET_SPRUCE:
        case TREE_PRESET_FIR:
        case TREE_PRESET_SNAG:
            p->form = TREE_FORM_EXCURRENT;
            p->trunk_radius = 3.2f;
            p->curve_noise = 0.15f;
            p->angle_variance = 8.0f;
            // Each whorl turned well off the last, so no two tiers line up down the trunk.
            p->twist = 47.0f;
            p->show_leaves = 1;
            p->leaf_size = 14.0f;
            p->leaf_density = 3.5f;
            p->crown_base = 0.12f;
            p->crown_width = 0.32f;
            p->crown_shape = 0.9f;
            p->whorl_spacing = 5.5f;
            p->whorl_size = 5;
            p->branch_pitch = 25.0f;
            p->spray_angle = 55.0f;
            p->droop = 0.5f;
            p->phototropism = 0.35f;
            p->dead_lower = 0.12f;
            p->dead_fraction = 0.12f;
            p->irregularity = 0.7f;
            if (preset == TREE_PRESET_FIR) {
                p->crown_width = 0.28f;
                p->crown_shape = 1.1f;
                p->whorl_spacing = 7.0f;
                p->whorl_size = 4;
                p->branch_pitch = 5.0f;
                p->spray_angle = 70.0f;
                p->droop = 0.25f;
                p->phototropism = 0.5f;
                p->leaf_density = 4.0f;
            } else if (preset == TREE_PRESET_SNAG) {
                p->crown_base = 0.18f;
                p->dead_lower = 0.3f;
                p->dead_fraction = 0.45f;
                p->droop = 0.7f;
                p->leaf_density = 2.2f;
                p->snag = 1.0f;
                p->irregularity = 0.9f;
            }
            break;
        case TREE_PRESET_DEAD:
            // What a dead tree is: branches that sag (droop), wander (curve noise) and no longer
            // straighten back toward the light, spread wide and sparse, and no leaves.
            p->max_depth = 3;
            p->trunk_radius = 8.0f;
            p->branches_per_node = 2;
            p->length_decay = 0.7f;
            p->taper = 0.5f;
            p->branch_angle = 42.0f;
            p->angle_variance = 22.0f;
            p->droop = 0.55f;
            p->curve_noise = 0.7f;
            p->phototropism = 0.1f;
            p->lateral_density = 0.45f;
            p->twig_scale = 0.8f;
            break;
        case TREE_PRESET_BROADLEAF:
        default:
            // A tall, upright habit: a long trunk, branches held closer to vertical, and a stronger
            // pull toward the light, which narrows the crown rather than letting it spread into a
            // ball.
            p->max_depth = 4;
            p->trunk_radius = 9.0f;
            p->branches_per_node = 3;
            p->length_decay = 0.70f;
            p->taper = 0.62f;
            p->branch_angle = 27.0f;
            p->angle_variance = 12.0f;
            p->droop = 0.32f;
            p->curve_noise = 0.4f;
            p->phototropism = 0.45f;
            p->lateral_density = 1.0f;
            p->twig_scale = 1.0f;
            p->show_leaves = 1;
            // A card carries a whole sprig, so it is sized as one and spaced sparsely: the canopy
            // should show its branch structure through the foliage.
            p->leaf_size = 15.0f;
            p->leaf_density = 1.3f;
            break;
    }
}

// ---------------------------------------------------------------------------
// Skeleton growth
// ---------------------------------------------------------------------------

static bool skel_reserve(TreeSkeleton* s, int extra_points) {
    if (s->branch_count == s->branch_cap) {
        int cap = s->branch_cap ? s->branch_cap * 2 : 128;
        Branch* b = safe_realloc(s->branches, (size_t)cap * sizeof(Branch));
        if (!b)
            return false;
        s->branches = b;
        s->branch_cap = cap;
    }
    if (s->point_count + extra_points > s->point_cap) {
        int cap = s->point_cap ? s->point_cap * 2 : 1024;
        while (cap < s->point_count + extra_points)
            cap *= 2;
        BranchPoint* p = safe_realloc(s->points, (size_t)cap * sizeof(BranchPoint));
        if (!p)
            return false;
        s->points = p;
        s->point_cap = cap;
    }
    return true;
}

// Interpolate the spine at t in [0,1] along a branch.
static void branch_sample(const TreeSkeleton* s, const Branch* b, float t, vec3 pos, vec3 tan,
                          float* radius, float* arc, float* root_dist) {
    float f = t * (float)(b->num_points - 1);
    int i = (int)f;
    if (i < 0)
        i = 0;
    if (i > b->num_points - 2)
        i = b->num_points - 2;
    float frac = f - (float)i;
    const BranchPoint* p0 = &s->points[b->first_point + i];
    const BranchPoint* p1 = &s->points[b->first_point + i + 1];
    glm_vec3_lerp((float*)p0->pos, (float*)p1->pos, frac, pos);
    glm_vec3_lerp((float*)p0->tangent, (float*)p1->tangent, frac, tan);
    glm_vec3_normalize(tan);
    *radius = p0->radius + (p1->radius - p0->radius) * frac;
    *arc = p0->arc + (p1->arc - p0->arc) * frac;
    *root_dist = p0->root_dist + (p1->root_dist - p0->root_dist) * frac;
}

// Any unit vector perpendicular to `v`.
static void perp_to(const vec3 v, vec3 out) {
    vec3 ref = {0.0f, 1.0f, 0.0f};
    if (fabsf(v[1]) > 0.9f) {
        ref[0] = 1.0f;
        ref[1] = 0.0f;
    }
    glm_vec3_cross((float*)ref, (float*)v, out);
    glm_vec3_normalize(out);
}

// A branch before it grows: where and which way it starts, its size, the three pulls that bend
// it, and what it carries on from its parent.
typedef struct BranchSeed {
    int parent; // index into branches[], -1 for the trunk
    int depth;  // its generation, which also sets how many spine points it gets
    vec3 origin, dir;
    float length, base_r, tip_r;
    float droop, phototropism, curve_noise;
    float root_dist0, parent_phase, uv_v0;
    bool terminal, bears_leaves;
} BranchSeed;

// Add one branch and integrate its spine. Its index, or -1 when the skeleton is full.
static int add_branch(TreeSkeleton* s, const BranchSeed* bs, TgRng* rng) {
    if (s->branch_count >= TG_MAX_BRANCHES || s->point_count >= TG_MAX_POINTS)
        return -1;

    int num_points = k_points_by_depth[bs->depth < 8 ? bs->depth : 7];
    if (!skel_reserve(s, num_points))
        return -1;

    int bi = s->branch_count++;
    int first_point = s->point_count;
    s->point_count += num_points;

    float phase = tg_randf(rng, 0.0f, 1.0f);

    {
        Branch* b = &s->branches[bi];
        b->parent = bs->parent;
        b->depth = bs->depth;
        b->first_point = first_point;
        b->num_points = num_points;
        b->base_radius = bs->base_r;
        b->tip_radius = bs->tip_r;
        b->length = bs->length;
        b->phase = phase;
        b->parent_phase = bs->parent_phase;
        b->uv_v0 = bs->uv_v0;
        b->is_terminal = bs->terminal;
        b->bears_leaves = bs->bears_leaves;
        b->bark_segs = 0;
        b->vigor = 1.0f;
        float circ = 2.0f * (float)M_PI * bs->base_r;
        int tiles = (int)(circ / TG_BARK_TILE + 0.5f);
        b->uv_tiles_u = tiles < 1 ? 1 : tiles;
    }

    // Integrate the spine. Direction accumulates three pulls: gravity drags it
    // down (weighted against radius, so limbs hold their line while twigs
    // sag), phototropism turns the outer part back up, and noise keeps any two
    // branches from tracing the same arc.
    vec3 d, pos;
    glm_vec3_copy((float*)bs->dir, d);
    glm_vec3_normalize(d);
    glm_vec3_copy((float*)bs->origin, pos);

    float step = bs->length / (float)(num_points - 1);
    float arc = 0.0f;

    for (int i = 0; i < num_points; i++) {
        float t = (float)i / (float)(num_points - 1);
        BranchPoint* bp = &s->points[first_point + i];
        glm_vec3_copy(pos, bp->pos);
        glm_vec3_copy(d, bp->tangent);
        bp->radius = bs->base_r + (bs->tip_r - bs->base_r) * powf(t, 0.85f);
        bp->arc = arc;
        bp->root_dist = bs->root_dist0 + arc;

        if (i == num_points - 1)
            break;

        glm_vec3_muladds(d, step, pos);
        arc += step;

        vec3 grav = {0.0f, -1.0f, 0.0f};
        glm_vec3_muladds(grav, bs->droop * step * 0.08f / (1.0f + bp->radius), d);

        vec3 up = {0.0f, 1.0f, 0.0f};
        glm_vec3_muladds(up, bs->phototropism * step * 0.06f * t * t, d);

        vec3 jitter = {tg_randf(rng, -1.0f, 1.0f), tg_randf(rng, -1.0f, 1.0f) * 0.3f,
                       tg_randf(rng, -1.0f, 1.0f)};
        glm_vec3_muladds(jitter, bs->curve_noise * step * 0.03f, d);

        glm_vec3_normalize(d);
    }
    return bi;
}

static void grow_branch(TreeSkeleton* s, const TreeParams* p, int parent_idx, const vec3 origin,
                        const vec3 dir, float length, float base_r, int depth, float root_dist0,
                        float parent_phase, float uv_v0, TgRng* rng) {
    if (depth > p->max_depth || base_r < 0.05f || length < 0.4f)
        return;

    bool terminal = (depth == p->max_depth);
    // A terminal branch narrows to almost nothing and gets a pointed cap;
    // an internal one stops at the taper so its children can pick up the radius.
    float tip_r = terminal ? base_r * 0.12f : base_r * p->taper;

    BranchSeed bs = {.parent = parent_idx,
                     .depth = depth,
                     .length = length,
                     .base_r = base_r,
                     .tip_r = tip_r,
                     .droop = p->droop,
                     .phototropism = p->phototropism,
                     .curve_noise = p->curve_noise,
                     .root_dist0 = root_dist0,
                     .parent_phase = parent_phase,
                     .uv_v0 = uv_v0,
                     .terminal = terminal,
                     .bears_leaves = depth >= p->max_depth - 1};
    glm_vec3_copy((float*)origin, bs.origin);
    glm_vec3_copy((float*)dir, bs.dir);
    int bi = add_branch(s, &bs, rng);
    if (bi < 0 || terminal)
        return;
    const int first_point = s->branches[bi].first_point;
    const int num_points = s->branches[bi].num_points;
    const float phase = s->branches[bi].phase;

    float twist_rad = glm_rad(p->twist);
    float angle_rad = glm_rad(p->branch_angle);
    float var_rad = glm_rad(p->angle_variance);

    // --- Tip split -------------------------------------------------------
    // Radii follow da Vinci's rule (the children's cross-sections sum to the
    // parent's), which is what keeps a joint from looking like a pipe fitting.
    int k = p->branches_per_node;
    if (k > 5)
        k = 5;
    if (k < 1)
        k = 1;

    float w[5];
    float wsum = 0.0f;
    int leader = 0;
    for (int i = 0; i < k; i++) {
        w[i] = 1.0f + tg_randf(rng, -0.25f, 0.25f);
        wsum += w[i];
        if (w[i] > w[leader])
            leader = i;
    }

    vec3 tip_pos, tip_tan;
    glm_vec3_copy(s->points[first_point + num_points - 1].pos, tip_pos);
    glm_vec3_copy(s->points[first_point + num_points - 1].tangent, tip_tan);

    vec3 fu = GLM_VEC3_ZERO_INIT, fv = GLM_VEC3_ZERO_INIT;
    perp_to(tip_tan, fu);
    glm_vec3_cross(tip_tan, fu, fv);
    glm_vec3_normalize(fv);

    float child_uv_v0 = uv_v0 + length / TG_BARK_TILE;

    for (int i = 0; i < k; i++) {
        float cr = tip_r * sqrtf(w[i] / wsum);
        if (cr > tip_r * 0.95f)
            cr = tip_r * 0.95f;

        float az =
            twist_rad + 2.0f * (float)M_PI * (float)i / (float)k + tg_randf(rng, -0.2f, 0.2f);
        float tilt = angle_rad + tg_randf(rng, -var_rad, var_rad);
        // One child continues the parent's line. Without a leader every split
        // is a symmetric fork and the tree reads as a shrub, not a tree.
        if (i == leader)
            tilt *= 0.4f;

        vec3 side, cd;
        glm_vec3_scale(fu, cosf(az), side);
        glm_vec3_muladds(fv, sinf(az), side);
        glm_vec3_scale(tip_tan, cosf(tilt), cd);
        glm_vec3_muladds(side, sinf(tilt), cd);
        glm_vec3_normalize(cd);

        float clen = length * p->length_decay * (1.0f + tg_randf(rng, -0.15f, 0.15f));
        if (depth + 1 == p->max_depth)
            clen *= p->twig_scale;

        // Start the child inside its parent so the collar below emerges from
        // solid wood instead of hanging off the end of an open tube.
        vec3 corigin;
        glm_vec3_copy(tip_pos, corigin);
        glm_vec3_muladds(cd, -0.6f * tip_r, corigin);

        grow_branch(s, p, bi, corigin, cd, clen, cr, depth + 1, root_dist0 + length, phase,
                    child_uv_v0, rng);
    }

    // --- Lateral branches ------------------------------------------------
    // Side limbs along the parent's length, spaced by the golden angle the way
    // real phyllotaxis does, so successive laterals never stack in a plane.
    // Only structural generations grow them; twigs would explode the count.
    if (depth < p->max_depth - 1) {
        int n_lat = (int)(length * p->lateral_density / 10.0f);
        if (n_lat > 4)
            n_lat = 4;

        for (int j = 0; j < n_lat; j++) {
            float t =
                0.35f + 0.55f * ((float)j + 0.5f) / (float)n_lat + tg_randf(rng, -0.04f, 0.04f);
            if (t < 0.3f)
                t = 0.3f;
            if (t > 0.92f)
                t = 0.92f;

            vec3 spos = GLM_VEC3_ZERO_INIT, stan = GLM_VEC3_ZERO_INIT;
            float sr, sarc, sroot;
            branch_sample(s, &s->branches[bi], t, spos, stan, &sr, &sarc, &sroot);

            vec3 lu = GLM_VEC3_ZERO_INIT, lv = GLM_VEC3_ZERO_INIT;
            perp_to(stan, lu);
            glm_vec3_cross(stan, lu, lv);
            glm_vec3_normalize(lv);

            float az = twist_rad + (float)j * 2.39996f + tg_randf(rng, -0.25f, 0.25f);
            float tilt = angle_rad * 1.3f + tg_randf(rng, -var_rad, var_rad);
            if (tilt > glm_rad(88.0f))
                tilt = glm_rad(88.0f);

            vec3 side, cd;
            glm_vec3_scale(lu, cosf(az), side);
            glm_vec3_muladds(lv, sinf(az), side);
            glm_vec3_scale(stan, cosf(tilt), cd);
            glm_vec3_muladds(side, sinf(tilt), cd);
            glm_vec3_normalize(cd);

            float cr = sr * tg_randf(rng, 0.35f, 0.5f);
            if (cr > sr * 0.7f)
                cr = sr * 0.7f;

            // Shorter toward the tip, which is what tapers the crown into a
            // cone rather than a cylinder of equal-length limbs.
            float clen = length * p->length_decay * (1.0f - 0.45f * t);

            vec3 corigin;
            glm_vec3_copy(spos, corigin);
            glm_vec3_muladds(cd, -0.6f * sr, corigin);

            grow_branch(s, p, bi, corigin, cd, clen, cr, depth + 1, sroot, phase,
                        uv_v0 + sarc / TG_BARK_TILE, rng);
        }
    }
}

// One branch of a conifer, off the trunk `height` up it, out toward azimuth `az` and `pitch`
// below level. A conifer's branches are terminal, so they are the whole of its wood but the
// trunk; their needles are painted on the cards.
static void excurrent_branch(TreeSkeleton* s, const TreeParams* p, int trunk, float trunk_len,
                             float height, float az, float pitch, float length, float vigor,
                             float sag, TgRng* rng) {
    if (length < 0.4f)
        return;
    vec3 pos = GLM_VEC3_ZERO_INIT, tan = GLM_VEC3_ZERO_INIT;
    float r, arc, root;
    const Branch* tb = &s->branches[trunk];
    branch_sample(s, tb, height / trunk_len, pos, tan, &r, &arc, &root);

    const bool dead = vigor <= 0.0f;
    // Thicker as it is longer, and slight beside the trunk: a conifer's branches are a fraction
    // of its stem, and most of what shows of them is needles.
    const float br = fminf(r * 0.35f, fmaxf(0.06f, 0.022f * length));
    BranchSeed bs = {.parent = trunk,
                     // Five spine points: enough for the sag and the turn up at the tip.
                     .depth = 3,
                     .length = length,
                     .base_r = br,
                     .tip_r = br * 0.12f,
                     // Dead wood is stiff: it holds its line and wanders, where a live branch
                     // sags under its needles and turns its tip back up.
                     .droop = p->droop * sag * (dead ? 0.4f : 1.0f),
                     .phototropism = dead ? 0.0f : p->phototropism,
                     .curve_noise = p->curve_noise * (dead ? 2.5f : 1.0f),
                     .root_dist0 = root,
                     .parent_phase = tb->phase,
                     .uv_v0 = tb->uv_v0 + arc / TG_BARK_TILE,
                     .terminal = true,
                     .bears_leaves = true};
    const float cp = cosf(pitch);
    vec3 dir = {cosf(az) * cp, -sinf(pitch), sinf(az) * cp};
    glm_vec3_copy(dir, bs.dir);
    // Started inside the trunk, so its collar comes out of solid wood.
    glm_vec3_copy(pos, bs.origin);
    glm_vec3_muladds(dir, -0.6f * r, bs.origin);

    int bi = add_branch(s, &bs, rng);
    if (bi < 0)
        return;
    // Mostly hidden by its own needles, so a few sides are all its bark needs.
    s->branches[bi].bark_segs = 5;
    s->branches[bi].vigor = vigor;
}

/*
 * The excurrent form, a conifer, after ez-tree's evergreen (Greenheck, MIT): one trunk to the
 * top, and branches up it whose length falls with height, so the crown is a cone. Unlike
 * ez-tree's, the branches come in WHORLS, a tier of several at one height as a spruce or a fir
 * grows them, each tier turned off the last. The crown's lowest branches are dead and some
 * others die or brown, and a snag's top is broken off. Every random draw is made here, as in
 * the recursive form, so any meshing reads the one skeleton.
 */
// Kink the trunk where its leader was once knocked aside: above each kink the trunk is shifted
// sideways over a short stretch and grows on parallel, an S-bend rather than an even wander.
static void kink_trunk(TreeSkeleton* s, int trunk, float length, float irregularity, TgRng* rng) {
    const Branch* tb = &s->branches[trunk];
    BranchPoint* pt = &s->points[tb->first_point];
    const int n = tb->num_points;
    const int kinks = 1 + (int)tg_randf(rng, 0.0f, 1.99f);
    for (int k = 0; k < kinks; k++) {
        const float at = tg_randf(rng, 0.25f, 0.85f);
        const float az = tg_randf(rng, 0.0f, 2.0f * (float)M_PI);
        const float shift = irregularity * length * tg_randf(rng, 0.01f, 0.035f);
        for (int i = 0; i < n; i++) {
            const float w = tg_smoothstep(at - 0.06f, at + 0.06f, (float)i / (float)(n - 1));
            pt[i].pos[0] += cosf(az) * shift * w;
            pt[i].pos[2] += sinf(az) * shift * w;
        }
    }
    for (int i = 0; i < n; i++) {
        const int a = i > 0 ? i - 1 : 0, b = i < n - 1 ? i + 1 : n - 1;
        glm_vec3_sub(pt[b].pos, pt[a].pos, pt[i].tangent);
        glm_vec3_normalize(pt[i].tangent);
    }
}

static void grow_excurrent(TreeSkeleton* s, const TreeParams* p, TgRng* rng) {
    const float L = p->trunk_length, R = p->trunk_radius;
    const float irr = p->irregularity;
    // A snag's top is gone: the trunk ends at the break, where the wood is still thick, and
    // gets the blunt cap of a branch that goes on rather than a point.
    const float broken = p->snag > 0.0f ? 0.45f * p->snag * tg_randf(rng, 0.6f, 1.0f) : 0.0f;
    // A dead leader: the trunk stops a little short in a dead spike, and the branches under it
    // turn up to take its place.
    const bool forked = broken <= 0.0f && tg_randf(rng, 0.0f, 1.0f) < 0.3f * irr;
    const float trunk_len = L * (1.0f - broken) * (forked ? tg_randf(rng, 0.82f, 0.9f) : 1.0f);
    // The side it grew toward the light from, fuller than the rest.
    const float light_az = tg_randf(rng, 0.0f, 2.0f * (float)M_PI);
    const float lopsided = 0.35f * irr * tg_randf(rng, 0.3f, 1.0f);
    const float top_r = R * 0.04f;
    const float tip_r = broken > 0.0f ? R + (top_r - R) * powf(1.0f - broken, 0.85f) : top_r;

    BranchSeed trunk = {.parent = -1,
                        .depth = 0,
                        .dir = {0.0f, 1.0f, 0.0f},
                        .length = trunk_len,
                        .base_r = R,
                        .tip_r = tip_r,
                        .curve_noise = p->curve_noise,
                        .terminal = broken <= 0.0f};
    const int ti = add_branch(s, &trunk, rng);
    if (ti < 0)
        return;
    if (irr > 0.0f)
        kink_trunk(s, ti, trunk_len, irr, rng);

    // A break is not a cut: splinters stand up round its rim.
    if (broken > 0.0f) {
        const Branch tb = s->branches[ti];
        const BranchPoint top = s->points[tb.first_point + tb.num_points - 1];
        const int splinters = 4 + (int)tg_randf(rng, 0.0f, 2.99f);
        for (int i = 0; i < splinters; i++) {
            const float az = tg_randf(rng, 0.0f, 2.0f * (float)M_PI);
            const float tilt = glm_rad(tg_randf(rng, 5.0f, 25.0f));
            const float base_r = tip_r * tg_randf(rng, 0.25f, 0.45f);
            BranchSeed sp = {.parent = ti,
                             .depth = 4,
                             .length = tip_r * tg_randf(rng, 1.5f, 5.0f),
                             .base_r = base_r,
                             .tip_r = base_r * 0.1f,
                             .curve_noise = p->curve_noise,
                             .root_dist0 = trunk_len,
                             .parent_phase = tb.phase,
                             .uv_v0 = tb.uv_v0 + trunk_len / TG_BARK_TILE,
                             .terminal = true};
            const vec3 out = {cosf(az), 0.0f, sinf(az)};
            glm_vec3_copy((float*)top.pos, sp.origin);
            glm_vec3_muladds((float*)out, tip_r * tg_randf(rng, 0.3f, 0.8f), sp.origin);
            vec3 dir = {out[0] * sinf(tilt), cosf(tilt), out[2] * sinf(tilt)};
            glm_vec3_copy(dir, sp.dir);
            const int si = add_branch(s, &sp, rng);
            if (si >= 0) {
                s->branches[si].bark_segs = 4;
                s->branches[si].vigor = 0.0f;
            }
        }
    }

    // Measured on the unbroken tree, so a snag keeps the long low branches it grew.
    const float crown0 = p->crown_base * L;
    const float crown_span = fmaxf(L - crown0, 1e-3f);
    const float longest = p->crown_width * L;
    const float twist = glm_rad(p->twist), var = glm_rad(p->angle_variance);
    const float low_pitch = glm_rad(p->branch_pitch), top_pitch = glm_rad(-40.0f);
    const float spacing = fmaxf(p->whorl_spacing, 0.5f);

    // The lowest branches, shaded out long ago: dead stubs up the bare trunk.
    const int stubs = (int)(crown0 / spacing * 0.5f);
    for (int i = 0; i < stubs; i++) {
        const float h = tg_randf(rng, 0.1f, 1.0f) * crown0;
        const float az = tg_randf(rng, 0.0f, 2.0f * (float)M_PI);
        const float len = longest * tg_randf(rng, 0.06f, 0.16f);
        excurrent_branch(s, p, ti, trunk_len, h, az, low_pitch + tg_randf(rng, -var, var), len,
                         0.0f, 1.0f, rng);
    }

    int whorl = 0;
    for (float h = crown0 + tg_randf(rng, 0.0f, 0.5f) * spacing; h < trunk_len - 0.4f * spacing;
         h += spacing *
              fmaxf(0.3f, 1.0f + tg_randf(rng, -0.2f, 0.2f) + irr * tg_randf(rng, -0.25f, 0.4f)),
               whorl++) {
        const float u = (h - crown0) / crown_span;
        float len = longest * powf(fmaxf(1.0f - u, 0.0f), p->crown_shape);
        int n = p->whorl_size + (int)floorf(tg_randf(rng, -1.0f, 2.0f));
        if (n < 1)
            n = 1;
        // Now and then a tier is stunted, or gone, which leaves a band you can see through.
        const float fate = tg_randf(rng, 0.0f, 1.0f);
        if (fate < 0.07f * irr)
            continue;
        if (fate < 0.2f * irr)
            len *= tg_randf(rng, 0.35f, 0.6f);
        const float az0 = (float)whorl * twist + tg_randf(rng, -0.3f, 0.3f);
        // Low branches leave below level and the top ones rise toward the leader.
        const float pitch = low_pitch + (top_pitch - low_pitch) * powf(u, 1.3f);
        // What dies: the crown's lowest tiers, and on a snag the last stretch under the break.
        const bool zone_dead = u < p->dead_lower || (broken > 0.0f && h > trunk_len - 0.06f * L);
        for (int k = 0; k < n; k++) {
            const float az = az0 + 2.0f * (float)M_PI * (float)k / (float)n +
                             tg_randf(rng, -0.35f, 0.35f) * (float)M_PI / (float)n;
            const float roll = tg_randf(rng, 0.0f, 1.0f);
            const float gone = tg_randf(rng, 0.0f, 1.0f);
            float vigor = 1.0f;
            if (zone_dead)
                vigor = 0.0f;
            else if (roll < p->dead_fraction)
                vigor = roll < 0.5f * p->dead_fraction ? 0.0f : 0.5f;
            // Lost to a storm, a deer or the dark: a branch that is simply not there.
            if (gone < 0.15f * irr)
                continue;
            float vary = 1.0f + irr * tg_randf(rng, -0.35f, 0.35f);
            if (tg_randf(rng, 0.0f, 1.0f) < 0.05f * irr)
                vary *= 1.4f;
            const float toward_light = 1.0f + lopsided * cosf(az - light_az);
            // A dead branch has mostly snapped off short.
            const float blen = len * tg_randf(rng, 0.8f, 1.15f) * vary * toward_light *
                               (vigor > 0.0f ? 1.0f : tg_randf(rng, 0.3f, 0.75f));
            const float sag = 1.0f + irr * tg_randf(rng, -0.4f, 0.5f);
            excurrent_branch(s, p, ti, trunk_len, h, az, pitch + tg_randf(rng, -var, var), blen,
                             vigor, sag, rng);
        }
    }

    // The dead leader's replacements: one or two branches just under it, turned up steeply.
    if (forked) {
        const int leaders = 1 + (int)tg_randf(rng, 0.0f, 1.99f);
        for (int i = 0; i < leaders; i++) {
            const float h = trunk_len - L * tg_randf(rng, 0.03f, 0.08f);
            const float az = tg_randf(rng, 0.0f, 2.0f * (float)M_PI);
            const float pitch = -glm_rad(tg_randf(rng, 55.0f, 72.0f));
            excurrent_branch(s, p, ti, trunk_len, h, az, pitch, L * tg_randf(rng, 0.12f, 0.2f),
                             1.0f, 0.25f, rng);
        }
    }
}

void tree_skeleton_build(TreeSkeleton* skel, const TreeParams* p) {
    tree_skeleton_free(skel);

    TgRng rng = {(uint32_t)p->seed * 2654435761u + 1u};
    if (rng.s == 0)
        rng.s = 1;

    vec3 origin = {0.0f, 0.0f, 0.0f};
    vec3 up = {0.0f, 1.0f, 0.0f};
    if (p->form == TREE_FORM_EXCURRENT)
        grow_excurrent(skel, p, &rng);
    else
        grow_branch(skel, p, -1, origin, up, p->trunk_length, p->trunk_radius, 0, 0.0f, 0.0f, 0.0f,
                    &rng);

    skel->max_root_dist = 1.0f;
    for (int i = 0; i < skel->point_count; i++) {
        if (skel->points[i].root_dist > skel->max_root_dist)
            skel->max_root_dist = skel->points[i].root_dist;
    }
}

void tree_skeleton_free(TreeSkeleton* skel) {
    if (!skel)
        return;
    free(skel->branches);
    free(skel->points);
    memset(skel, 0, sizeof(*skel));
}

// ---------------------------------------------------------------------------
// Bark meshing
// ---------------------------------------------------------------------------

// Wind phase for a point: the branch's own phase, eased in from the parent's
// over the first quarter of the arc so a joint has no phase discontinuity to
// tear along.
static float branch_phase_at(const Branch* b, float t_arc) {
    return b->parent_phase + (b->phase - b->parent_phase) * tg_smoothstep(0.0f, 0.25f, t_arc);
}

// Ring segments for a branch. Thick wood needs enough of them to resolve its
// flutes; twigs are a few pixels wide and would only cost vertices. Shared by
// the sweep and the vertex census, which must agree exactly.
static int branch_segs(const Branch* b) {
    if (b->bark_segs > 0)
        return b->bark_segs;
    int segs = 6 + (int)ceilf(b->base_radius * 2.0f);
    return segs > 26 ? 26 : segs;
}

// Bark cross-section profile: how far the surface departs from a circle at
// angle `a`, as a signed fraction in [-1,1], plus its derivative in `a`.
//
// A trunk is not a cylinder -- it has flutes and ridges running up it, and a
// normal map cannot express that because it never touches the silhouette. The
// harmonics use whole-number frequencies so the profile closes exactly at the
// seam, and each one drifts slowly along the branch so the flutes wander
// rather than running dead straight.
static float bark_profile(float a, float arc, float ph0, float ph1, float ph2, float* d_da) {
    const float f0 = 3.0f, f1 = 5.0f, f2 = 8.0f;
    const float w0 = 0.55f, w1 = 0.30f, w2 = 0.15f; // sum to 1
    float t0 = a * f0 + ph0 + arc * 0.020f;
    float t1 = a * f1 + ph1 - arc * 0.014f;
    float t2 = a * f2 + ph2 + arc * 0.030f;
    if (d_da)
        *d_da = w0 * f0 * cosf(t0) + w1 * f1 * cosf(t1) + w2 * f2 * cosf(t2);
    return w0 * sinf(t0) + w1 * sinf(t1) + w2 * sinf(t2);
}

static void sweep_branch(MeshBuilder* mb, const TreeSkeleton* s, const Branch* b) {
    int segs = branch_segs(b);
    int ring_verts = segs + 1; // duplicated seam vertex so u can reach its last tile
    float slope = (b->tip_radius - b->base_radius) / (b->length > 1e-4f ? b->length : 1e-4f);

    unsigned int first_ring = mb->vcount;

    // Per-branch profile phases, so no two branches share a cross-section.
    float ph0 = b->phase * 6.2831853f;
    float ph1 = ph0 * 1.7f + 1.3f;
    float ph2 = ph0 * 2.3f + 2.7f;

    // Parallel-transport frame: rotating the previous ring's normal by the
    // twist between consecutive tangents keeps the rings from spinning around
    // a curved spine (which would shear the bark texture).
    vec3 N = GLM_VEC3_ZERO_INIT;
    perp_to(s->points[b->first_point].tangent, N);

    for (int i = 0; i < b->num_points; i++) {
        const BranchPoint* bp = &s->points[b->first_point + i];
        vec3 T;
        glm_vec3_copy((float*)bp->tangent, T);

        if (i > 0) {
            vec3 Tprev, axis;
            glm_vec3_copy((float*)s->points[b->first_point + i - 1].tangent, Tprev);
            glm_vec3_cross(Tprev, T, axis);
            float sn = glm_vec3_norm(axis);
            if (sn > 1e-6f) {
                glm_vec3_scale(axis, 1.0f / sn, axis);
                glm_vec3_rotate(N, atan2f(sn, glm_vec3_dot(Tprev, T)), axis);
            }
        }
        // Re-orthogonalize against drift from the incremental rotations.
        glm_vec3_muladds(T, -glm_vec3_dot(N, T), N);
        glm_vec3_normalize(N);

        vec3 B;
        glm_vec3_cross(T, N, B);
        glm_vec3_normalize(B);

        float t_arc = bp->arc / (b->length > 1e-4f ? b->length : 1e-4f);
        // Branch collar: every branch swells where it leaves its parent, and
        // the trunk flares harder still into its root buttress.
        float flare = b->depth == 0 ? 1.0f + 0.85f * (1.0f - tg_smoothstep(0.0f, 0.15f, t_arc))
                                    : 1.0f + 0.30f * (1.0f - tg_smoothstep(0.0f, 0.22f, t_arc));
        float r = bp->radius * flare;

        // How far the cross-section departs from a circle. Thick wood carries
        // pronounced flutes; twigs are nearly round. The base of the trunk
        // deepens further still, which is what reads as buttress roots.
        float ridge_amp = 0.04f + 0.07f * tg_smoothstep(1.0f, 8.0f, b->base_radius);
        if (b->depth == 0)
            ridge_amp *= 1.0f + 1.6f * (1.0f - tg_smoothstep(0.0f, 0.30f, t_arc));

        float phase = branch_phase_at(b, t_arc);
        float flex = powf(bp->root_dist / s->max_root_dist, 1.2f);
        float v = b->uv_v0 + bp->arc / TG_BARK_TILE;

        for (int j = 0; j <= segs; j++) {
            float a = 2.0f * (float)M_PI * (float)j / (float)segs;
            float ca = cosf(a), sa = sinf(a);

            vec3 radial, tangential, vpos, nrm, tng;
            glm_vec3_scale(N, ca, radial);
            glm_vec3_muladds(B, sa, radial);
            // d(radial)/da
            glm_vec3_scale(N, -sa, tangential);
            glm_vec3_muladds(B, ca, tangential);

            float dprofile_da = 0.0f;
            float profile = bark_profile(a, bp->arc, ph0, ph1, ph2, &dprofile_da);
            float rr = r * (1.0f + ridge_amp * profile);
            float dr_da = r * ridge_amp * dprofile_da;

            glm_vec3_copy((float*)bp->pos, vpos);
            glm_vec3_muladds(radial, rr, vpos);

            // Exact surface normal for the fluted sweep: cross the two surface
            // derivatives. Keeping the plain radial normal here would light the
            // trunk as a smooth cylinder that merely happens to have a bumpy
            // outline -- the flutes would only show in silhouette.
            //   dP/da = tangential * rr + radial * dr/da
            //   dP/ds = T           + radial * taper slope
            vec3 dPda, dPds;
            glm_vec3_scale(tangential, rr, dPda);
            glm_vec3_muladds(radial, dr_da, dPda);
            glm_vec3_copy(T, dPds);
            glm_vec3_muladds(radial, slope, dPds);

            glm_vec3_cross(dPda, dPds, nrm);
            glm_vec3_normalize(nrm);

            // Tangent follows +u (around the circumference), re-orthogonalized
            // against the real normal so the bark normal map stays square.
            glm_vec3_copy(tangential, tng);
            glm_vec3_muladds(nrm, -glm_vec3_dot(tng, nrm), tng);
            glm_vec3_normalize(tng);

            mb_vertex(mb, vpos, nrm, tng, (float)j / (float)segs * (float)b->uv_tiles_u, v, phase,
                      flex, NULL);
        }
    }

    for (int i = 0; i < b->num_points - 1; i++) {
        for (int j = 0; j < segs; j++) {
            unsigned int a = first_ring + (unsigned int)(i * ring_verts + j);
            unsigned int bb = a + 1;
            unsigned int c = a + (unsigned int)ring_verts;
            unsigned int d = c + 1;
            mb_tri(mb, a, bb, d);
            mb_tri(mb, a, d, c);
        }
    }

    // Close the tip with a cone. Terminal twigs get a long point; internal ones
    // get a stub that their children hide, there only so a gap at the joint
    // never shows the inside of the tube.
    const BranchPoint* last = &s->points[b->first_point + b->num_points - 1];
    vec3 apex;
    glm_vec3_copy((float*)last->pos, apex);
    glm_vec3_muladds((float*)last->tangent, b->tip_radius * (b->is_terminal ? 2.0f : 0.6f), apex);

    vec3 an = GLM_VEC3_ZERO_INIT, at = GLM_VEC3_ZERO_INIT;
    glm_vec3_copy((float*)last->tangent, an);
    perp_to(an, at);

    float t_arc_tip = 1.0f;
    unsigned int apex_i = mb_vertex(
        mb, apex, an, at, 0.5f * (float)b->uv_tiles_u, b->uv_v0 + last->arc / TG_BARK_TILE,
        branch_phase_at(b, t_arc_tip), powf(last->root_dist / s->max_root_dist, 1.2f), NULL);

    unsigned int last_ring = first_ring + (unsigned int)((b->num_points - 1) * ring_verts);
    for (int j = 0; j < segs; j++) {
        mb_tri(mb, last_ring + (unsigned int)j, last_ring + (unsigned int)j + 1, apex_i);
    }
}

/*
 * The wind leans a vegetation mesh by its vertices' height through the mesh's OWN height range
 * (the shader's uWindMaskMinY/MaxY, from the mesh's bounds). A conifer's needles start at the
 * crown's base where its wood starts at the ground, so measured apart the two would lean by
 * different amounts at the same height and a gust would slide the sprays along their branches.
 * Both meshes carry the same two unreferenced vertices, at the bottom and the top of the whole
 * tree, so they measure one range.
 */
#define TG_WIND_ANCHORS 2

static void excurrent_wind_anchors(MeshBuilder* mb, const TreeSkeleton* s, const TreeParams* p) {
    float y0 = 0.0f, y1 = 0.0f;
    for (int i = 0; i < s->point_count; i++) {
        y0 = fminf(y0, s->points[i].pos[1]);
        y1 = fmaxf(y1, s->points[i].pos[1]);
    }
    // The sprays reach past the wood by up to a card's length.
    y0 -= p->leaf_size;
    y1 += p->leaf_size;
    const vec3 n = {0.0f, 1.0f, 0.0f}, t = {1.0f, 0.0f, 0.0f};
    const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    mb_vertex(mb, (vec3){0.0f, y0, 0.0f}, n, t, 0.0f, 0.0f, 0.0f, 0.0f, white);
    mb_vertex(mb, (vec3){0.0f, y1, 0.0f}, n, t, 0.0f, 0.0f, 0.0f, 0.0f, white);
}

bool tree_mesh_bark(const TreeSkeleton* skel, const TreeParams* p, Mesh* mesh) {
    if (!skel || skel->branch_count == 0 || !mesh)
        return false;
    const bool excurrent = p->form == TREE_FORM_EXCURRENT;

    // Census first: one exact reservation beats reallocating through 40k verts.
    size_t vres = excurrent ? TG_WIND_ANCHORS : 0, ires = 0;
    for (int i = 0; i < skel->branch_count; i++) {
        const Branch* b = &skel->branches[i];
        int segs = branch_segs(b);
        vres += (size_t)b->num_points * (size_t)(segs + 1) + 1;
        ires += (size_t)(b->num_points - 1) * (size_t)segs * 6 + (size_t)segs * 3;
    }

    MeshBuilder mb;
    if (!mb_init(&mb, vres, ires, false))
        return false;

    for (int i = 0; i < skel->branch_count; i++)
        sweep_branch(&mb, skel, &skel->branches[i]);
    if (excurrent)
        excurrent_wind_anchors(&mb, skel, p);

    return mb_transfer(&mb, mesh);
}

// ---------------------------------------------------------------------------
// Leaf meshing
// ---------------------------------------------------------------------------

// One leaf card: a quad split down the middle so the mid-rib can crease and the
// tip row can droop. Flat cards read as stickers from every angle but head-on.
// `fold` is how far the crease stands proud, and `tip_droop` how far the tip
// hangs, each a fraction of the card's width and length.
static void emit_leaf_card(MeshBuilder* mb, const vec3 attach, const vec3 L, const vec3 Nl,
                           const vec3 S, float len, float width, float fold, float tip_droop,
                           float phase, float flex, const float* rgba, int variant, bool mirror,
                           const vec3 canopy_center) {
    unsigned int base = mb->vcount;
    vec3 down = {0.0f, -1.0f, 0.0f};
    const float inv_variants = 1.0f / (float)TG_LEAF_VARIANTS;

    for (int row = 0; row < 2; row++) {
        float vv = (float)row;
        for (int col = 0; col < 3; col++) {
            float uu = ((float)col - 1.0f) * 0.5f;

            vec3 pv;
            glm_vec3_copy((float*)attach, pv);
            glm_vec3_muladds((float*)L, vv * len, pv);
            glm_vec3_muladds((float*)S, uu * width, pv);
            // Mid-rib fold: the spine of the leaf stands proud of its edges.
            glm_vec3_muladds((float*)Nl, (0.5f - fabsf(uu)) * width * fold, pv);
            // Tip droop.
            glm_vec3_muladds(down, vv * len * tip_droop, pv);

            vec3 nrm = GLM_VEC3_ZERO_INIT, tng = GLM_VEC3_ZERO_INIT;
            glm_vec3_copy((float*)Nl, nrm);
            glm_vec3_muladds((float*)S, uu * 0.6f, nrm);
            glm_vec3_normalize(nrm);

            // Canopy-volume normal. Shading each card by its own plane normal
            // is what makes a canopy read as confetti: a thousand cards catch
            // the light a thousand ways. Bending the normal toward the vector
            // out of the canopy centre makes the crown shade as one soft body,
            // lit on the sunward side and falling into shadow behind -- the
            // single biggest lighting difference in vegetation rendering.
            vec3 vol;
            glm_vec3_sub(pv, (float*)canopy_center, vol);
            if (glm_vec3_norm(vol) > 1e-4f) {
                glm_vec3_normalize(vol);
                glm_vec3_lerp(nrm, vol, 0.5f, nrm);
                glm_vec3_normalize(nrm);
            }

            // Re-orthogonalize the tangent against the bent normal so the
            // normal map now perturbs around the volume normal.
            glm_vec3_copy((float*)S, tng);
            glm_vec3_muladds(nrm, -glm_vec3_dot(tng, nrm), tng);
            glm_vec3_normalize(tng);

            // UV0.v runs stem (0) to tip (1): the leaf texture points that way,
            // and the wind shader pivots flutter about v = 0. U is remapped into
            // this card's cluster cell in the atlas.
            // Mirroring doubles the distinct arrangements for free -- with only
            // the raw variant count, the repeated sprig is easy to spot.
            float cell_u = mirror ? 0.5f - uu : uu + 0.5f;
            float u_tex = ((float)variant + cell_u) * inv_variants;
            mb_vertex(mb, pv, nrm, tng, u_tex, vv, phase, flex, rgba);
        }
    }

    for (int col = 0; col < 2; col++) {
        unsigned int a = base + (unsigned int)col;
        unsigned int b = a + 1;
        unsigned int c = base + 3 + (unsigned int)col;
        unsigned int d = c + 1;
        mb_tri(mb, a, b, d);
        mb_tri(mb, a, d, c);
    }
}

/*
 * A conifer's needles: sprays along each branch, one each side in turn, angled out in the
 * branch's own plane as a spruce's side shoots are, and one more off its tip. Each spray is a
 * crossed pair of cards, the flat one and one stood on edge across it, so a tree seen from
 * the side shows needles and not a stack of slivers. A dead branch keeps a few bare twigs, and
 * a browning one sprays mostly browned.
 */
static bool mesh_sprays(const TreeSkeleton* skel, const TreeParams* p, Mesh* mesh) {
    TgRng rng = {(uint32_t)p->seed * 747796405u + 2891336453u};
    if (rng.s == 0)
        rng.s = 1;

    MeshBuilder mb;
    if (!mb_init(&mb, 4096, 4096, true))
        return false;

    const vec3 up = {0.0f, 1.0f, 0.0f};
    const float L = p->trunk_length, crown0 = p->crown_base * L;
    const float crown_span = fmaxf(L - crown0, 1e-3f);
    const float longest = fmaxf(p->crown_width * L, 1e-3f);
    const float spray = glm_rad(p->spray_angle), var = glm_rad(p->angle_variance);
    // Some trees in a wood are full and some thin, and any branch has bare stretches.
    const float fullness = 1.0f - 0.35f * p->irregularity * tg_randf(&rng, 0.0f, 1.0f);
    const float gaps = 0.15f * p->irregularity;

    for (int i = 0; i < skel->branch_count && mb.ok; i++) {
        const Branch* b = &skel->branches[i];
        if (b->depth == 0 || !b->bears_leaves)
            continue;
        const bool dead = b->vigor <= 0.0f;
        const int n = (int)(b->length * p->leaf_density / 10.0f * (dead ? 0.35f : fullness));

        for (int j = 0; j <= n && mb.ok; j++) {
            if (j < n && tg_randf(&rng, 0.0f, 1.0f) < gaps)
                continue;
            const bool tip = j == n;
            const float t =
                tip ? 1.0f : 0.15f + 0.8f * ((float)j + tg_randf(&rng, 0.2f, 0.8f)) / (float)n;

            vec3 spos = GLM_VEC3_ZERO_INIT, stan = GLM_VEC3_ZERO_INIT;
            float sr, sarc, sroot;
            branch_sample(skel, b, t, spos, stan, &sr, &sarc, &sroot);

            // The branch's own plane: along it, and level across it.
            vec3 side;
            glm_vec3_cross(stan, (float*)up, side);
            if (glm_vec3_norm(side) < 1e-3f)
                perp_to(stan, side);
            glm_vec3_normalize(side);

            const float a = tip ? 0.0f : spray + tg_randf(&rng, -var, var);
            const float sgn = (j & 1) ? 1.0f : -1.0f;
            vec3 L_dir;
            glm_vec3_scale(stan, cosf(a), L_dir);
            glm_vec3_muladds(side, sgn * sinf(a), L_dir);
            // A spray hangs a little below the shoot it grows from.
            L_dir[1] -= 0.12f;
            glm_vec3_normalize(L_dir);

            // The flat card, its normal up off the spray, and its pair stood on edge.
            vec3 n1, s1, n2, s2;
            glm_vec3_copy((float*)up, n1);
            glm_vec3_muladds(L_dir, -glm_vec3_dot((float*)up, L_dir), n1);
            if (glm_vec3_norm(n1) < 1e-3f)
                perp_to(L_dir, n1);
            glm_vec3_normalize(n1);
            glm_vec3_cross(n1, L_dir, s1);
            glm_vec3_normalize(s1);
            glm_vec3_copy(s1, n2);
            glm_vec3_cross(n2, L_dir, s2);
            glm_vec3_normalize(s2);

            const float len = p->leaf_size * tg_randf(&rng, 0.75f, 1.2f) * (1.0f - 0.35f * t);
            const float width = len * 0.9f;

            const float roll = tg_randf(&rng, 0.0f, 1.0f);
            int cell;
            if (dead)
                cell = TG_SPRAY_BARE_CELL;
            else if (roll < (b->vigor < 1.0f ? 0.7f : 0.3f * p->dead_fraction))
                cell = TG_SPRAY_LIVE_CELLS + (int)tg_randf(&rng, 0.0f, (float)TG_SPRAY_DEAD_CELLS);
            else
                cell = (int)tg_randf(&rng, 0.0f, (float)TG_SPRAY_LIVE_CELLS);
            if (cell > TG_SPRAY_BARE_CELL)
                cell = TG_SPRAY_BARE_CELL;
            const bool mirror = tg_randf(&rng, 0.0f, 1.0f) < 0.5f;

            // Baked occlusion, as the broadleaf's: a spray deep against the trunk sees little
            // sky against one out at the crown's edge, and the crown is darker low down.
            const float hu = glm_clamp((spos[1] - crown0) / crown_span, 0.0f, 1.0f);
            const float reach = longest * powf(1.0f - hu, p->crown_shape) + 1e-3f;
            const float out = sqrtf(spos[0] * spos[0] + spos[2] * spos[2]) / reach;
            float ao = 0.45f + 0.55f * tg_smoothstep(0.1f, 1.0f, out);
            ao *= 0.7f + 0.3f * hu;
            const float bright = tg_randf(&rng, 0.85f, 1.15f);
            const float warm = tg_randf(&rng, -0.06f, 0.06f);
            const float tint[4] = {ao * bright * (1.0f + warm), ao * bright,
                                   ao * bright * (1.0f - warm), 1.0f};

            float ph = branch_phase_at(b, t) + tg_randf(&rng, 0.0f, 0.3f);
            ph -= floorf(ph);
            const float flex = powf(sroot / skel->max_root_dist, 1.2f);

            // The crown shades as one body round the trunk's axis at this height.
            const vec3 centre = {0.0f, spos[1], 0.0f};
            emit_leaf_card(&mb, spos, L_dir, n1, s1, len, width, 0.06f, 0.1f, ph, flex, tint, cell,
                           mirror, centre);
            emit_leaf_card(&mb, spos, L_dir, n2, s2, len, width, 0.06f, 0.1f, ph, flex, tint, cell,
                           !mirror, centre);
        }
    }
    excurrent_wind_anchors(&mb, skel, p);
    return mb_transfer(&mb, mesh);
}

bool tree_mesh_leaves(const TreeSkeleton* skel, const TreeParams* p, Mesh* mesh) {
    if (!skel || !mesh || !p->show_leaves || p->leaf_density <= 0.0f || p->leaf_size <= 0.0f)
        return false;
    if (p->form == TREE_FORM_EXCURRENT)
        return mesh_sprays(skel, p, mesh);

    TgRng rng = {(uint32_t)p->seed * 747796405u + 2891336453u};
    if (rng.s == 0)
        rng.s = 1;

    MeshBuilder mb;
    if (!mb_init(&mb, 4096, 4096, true))
        return false;

    vec3 up = {0.0f, 1.0f, 0.0f};

    // Canopy bounds, for the baked occlusion below. Alpha-masked surfaces are
    // deliberately skipped by screen-space AO (the strand tangle it was written
    // for is derivative noise), so without this the canopy would be lit as
    // evenly on the inside as on the outside and read as a flat green cloud.
    vec3 canopy_min = {1e9f, 1e9f, 1e9f}, canopy_max = {-1e9f, -1e9f, -1e9f};
    for (int i = 0; i < skel->branch_count; i++) {
        const Branch* b = &skel->branches[i];
        if (!b->bears_leaves)
            continue;
        for (int k = 0; k < b->num_points; k++) {
            const float* q = skel->points[b->first_point + k].pos;
            for (int c = 0; c < 3; c++) {
                if (q[c] < canopy_min[c])
                    canopy_min[c] = q[c];
                if (q[c] > canopy_max[c])
                    canopy_max[c] = q[c];
            }
        }
    }
    vec3 canopy_center;
    glm_vec3_add(canopy_min, canopy_max, canopy_center);
    glm_vec3_scale(canopy_center, 0.5f, canopy_center);
    float canopy_r = 0.5f * glm_vec3_distance(canopy_min, canopy_max);
    if (canopy_r < 1e-3f)
        canopy_r = 1.0f;
    float canopy_h = canopy_max[1] - canopy_min[1];
    if (canopy_h < 1e-3f)
        canopy_h = 1.0f;

    for (int i = 0; i < skel->branch_count; i++) {
        const Branch* b = &skel->branches[i];
        if (!b->bears_leaves)
            continue;

        // Leaves ride the outer stretch of a branch; the bare inner length is
        // what makes a canopy look like it has structure holding it up.
        float span = b->length * 0.6f;
        int n = (int)(span * p->leaf_density / 10.0f);
        int tip_cluster = 2;

        for (int j = 0; j < n + tip_cluster; j++) {
            bool at_tip = j >= n;
            float t = at_tip ? tg_randf(&rng, 0.92f, 1.0f)
                             : 0.4f + 0.6f * ((float)j + 0.5f) / (float)(n > 0 ? n : 1) +
                                   tg_randf(&rng, -0.05f, 0.05f);
            if (t < 0.0f)
                t = 0.0f;
            if (t > 1.0f)
                t = 1.0f;

            vec3 spos = GLM_VEC3_ZERO_INIT, stan = GLM_VEC3_ZERO_INIT;
            float sr, sarc, sroot;
            branch_sample(skel, b, t, spos, stan, &sr, &sarc, &sroot);

            vec3 fu = GLM_VEC3_ZERO_INIT, fv = GLM_VEC3_ZERO_INIT;
            perp_to(stan, fu);
            glm_vec3_cross(stan, fu, fv);
            glm_vec3_normalize(fv);

            float az = tg_randf(&rng, 0.0f, 2.0f * (float)M_PI);
            vec3 radial;
            glm_vec3_scale(fu, cosf(az), radial);
            glm_vec3_muladds(fv, sinf(az), radial);
            glm_vec3_normalize(radial);

            vec3 attach;
            glm_vec3_copy(spos, attach);
            glm_vec3_muladds(radial, sr * 1.05f, attach);

            // Growth axis: outward off the twig, angled along it, then pulled
            // toward the light and jittered so no two sit parallel.
            vec3 L;
            glm_vec3_copy(radial, L);
            glm_vec3_muladds(stan, 0.5f, L);
            glm_vec3_normalize(L);
            vec3 tmp;
            glm_vec3_lerp(L, up, 0.35f, tmp);
            glm_vec3_copy(tmp, L);
            vec3 jit = {tg_randf(&rng, -1.0f, 1.0f), tg_randf(&rng, -1.0f, 1.0f),
                        tg_randf(&rng, -1.0f, 1.0f)};
            glm_vec3_muladds(jit, 0.35f, L);
            glm_vec3_normalize(L);

            // Blade normal: up, projected off the growth axis, then rolled.
            vec3 Nl;
            glm_vec3_copy(up, Nl);
            glm_vec3_muladds(L, -glm_vec3_dot(up, L), Nl);
            if (glm_vec3_norm(Nl) < 1e-3f)
                perp_to(L, Nl);
            glm_vec3_normalize(Nl);
            glm_vec3_rotate(Nl, tg_randf(&rng, -0.6f, 0.6f), L);
            glm_vec3_normalize(Nl);

            vec3 S;
            glm_vec3_cross(Nl, L, S);
            glm_vec3_normalize(S);

            float scale = tg_randf(&rng, 0.7f, 1.3f);
            float len = p->leaf_size * scale;
            float width = len * 0.62f;

            float ph = branch_phase_at(b, t) + tg_randf(&rng, 0.0f, 0.3f);
            ph -= floorf(ph);
            float flex = powf(sroot / skel->max_root_dist, 1.2f);

            // Baked canopy occlusion: leaves buried near the middle of the
            // crown see little sky, leaves on the rim see most of it, and the
            // underside of the crown is darker than the top.
            float radial_d = glm_vec3_distance(attach, canopy_center) / canopy_r;
            float ao = 0.45f + 0.55f * tg_smoothstep(0.15f, 0.95f, radial_d);
            float hy = (attach[1] - canopy_min[1]) / canopy_h;
            ao *= 0.75f + 0.25f * hy;

            // Per-leaf variation on top, so no two neighbours share a tone.
            float warm = tg_randf(&rng, -0.14f, 0.14f);
            float bright = tg_randf(&rng, 0.82f, 1.18f);
            const float tint[4] = {ao * bright * (1.0f + warm), ao * bright * (1.0f - warm * 0.35f),
                                   ao * bright * (1.0f - warm), 1.0f};

            int variant = (int)(tg_randf(&rng, 0.0f, (float)TG_LEAF_VARIANTS));
            if (variant >= TG_LEAF_VARIANTS)
                variant = TG_LEAF_VARIANTS - 1;
            bool mirror = tg_randf(&rng, 0.0f, 1.0f) < 0.5f;

            emit_leaf_card(&mb, attach, L, Nl, S, len, width, 0.25f, 0.15f, ph, flex, tint, variant,
                           mirror, canopy_center);

            if (!mb.ok)
                break;
        }
    }

    return mb_transfer(&mb, mesh);
}
