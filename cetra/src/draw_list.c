#include <float.h>
#include <stdlib.h>
#include <string.h>

#include "draw_list.h"

#include "animation.h"
#include "ext/log.h"
#include "material.h"
#include "scene.h"
#include "wind.h"

// Never reset, so a stamp taken before a mutation can never compare equal to one
// taken after -- including across a scene teardown and reload.
static uint64_t g_graph_epoch = 1;

uint64_t scene_graph_epoch(void) {
    return g_graph_epoch;
}

void scene_graph_touched(void) {
    g_graph_epoch++;
}

void draw_list_free(DrawList* list) {
    if (!list)
        return;
    free(list->items);
    free(list->gizmos);
    memset(list, 0, sizeof(*list));
}

static bool reserve(DrawList* list, size_t needed) {
    if (list->capacity >= needed)
        return true;
    size_t capacity = list->capacity ? list->capacity : 64;
    while (capacity < needed)
        capacity *= 2;
    DrawItem* items = realloc(list->items, capacity * sizeof(DrawItem));
    if (!items) {
        log_error("Failed to grow draw list to %zu items", capacity);
        return false;
    }
    list->items = items;
    list->capacity = capacity;
    return true;
}

static bool push(DrawList* list, DrawItem item) {
    if (!reserve(list, list->count + 1))
        return false;
    list->items[list->count++] = item;
    list->lane_count[item.lane]++;
    return true;
}

static bool push_gizmo(DrawList* list, SceneNode* node) {
    if (list->gizmo_capacity < list->gizmo_count + 1) {
        size_t capacity = list->gizmo_capacity ? list->gizmo_capacity * 2 : 8;
        SceneNode** gizmos = realloc(list->gizmos, capacity * sizeof(SceneNode*));
        if (!gizmos) {
            log_error("Failed to grow gizmo list to %zu", capacity);
            return false;
        }
        list->gizmos = gizmos;
        list->gizmo_capacity = capacity;
    }
    list->gizmos[list->gizmo_count++] = node;
    return true;
}

// Which pass draws this mesh, and what a pass may assume about it. All of it was
// recomputed per mesh per pass before; none of it depends on the pass.
static void classify(const Mesh* mesh, uint8_t* lane, uint8_t* flags) {
    const Material* mat = mesh->material;
    bool transmissive = mat->transmission > 0.0f;
    // Translucency is IMPLIED by a fractional opacity or a dedicated opacity map
    // on a material that declared no mode -- formats carrying no alpha mode rely
    // on it, and so does anything built in C or overridden from a .cscn. ALPHA_MASK
    // is deliberately not swept in: masked hair with a fractional opacity is
    // still stencilled, not translucent.
    //
    // Derived here rather than written back onto the material at load, which is
    // where it used to live: only the two importers ever called that, so the
    // GUI's opacity slider and .cscn material overrides could put a fractional
    // opacity on a material that stayed in the opaque lane. That was survivable
    // while the lane blended by accident and stopped being so the moment it
    // correctly did not (spec 11.31) -- the slider simply did nothing. As a
    // predicate it cannot be skipped by a caller, and it picks up an opacity map
    // that arrives late from the async loader for free, since the list is
    // rebuilt when the graph changes.
    bool blend = mat->alpha_mode == ALPHA_BLEND ||
                 (mat->alpha_mode == ALPHA_OPAQUE &&
                  (mat->opacity < 1.0f || mat->opacity_tex != NULL));
    bool masked = mat->alpha_mode == ALPHA_MASK;
    // Foliage opts alpha-masked geometry back into casting: leaf cards are
    // centimetres across, so an alpha test resolves them, where hair strands at
    // map-texel scale resolve as streaks or acne either way.
    bool foliage = masked && mat->foliage_shadows && mat->alphaCutoff > 0.0f && mat->albedo_tex;

    *lane = transmissive ? DRAW_LANE_TRANSMISSIVE : (blend ? DRAW_LANE_BLEND : DRAW_LANE_OPAQUE);

    *flags = 0;
    if (masked)
        *flags |= DRAW_ALPHA_MASKED;
    if (foliage)
        *flags |= DRAW_FOLIAGE;
    if (mat->doubleSided)
        *flags |= DRAW_DOUBLE_SIDED;
}

// Projected size at which a level gives way to the next, as the ratio of a
// mesh's world radius to its distance from the eye -- so it is a screen-size
// ladder, independent of scene units, and a big object holds detail further out
// than a small one at the same distance.
//
// FOV is deliberately not in it. Folding it in would make a zoom re-pick every
// level in the scene at once, which is visible as the whole frame changing
// silhouette; holding the ladder in world terms costs a little detail at narrow
// FOV and never pops en masse.
static const float LOD_SWITCH[] = {0.045f, 0.022f, 0.011f};

// Sized off the literals, then checked against the cap. Declaring it
// [CETRA_LOD_MAX - 1] instead would zero-pad when the cap is raised, and a
// threshold of 0 is one no projected size ever falls below -- so the new levels
// would be unreachable and nothing would say so.
_Static_assert(sizeof(LOD_SWITCH) / sizeof(LOD_SWITCH[0]) == CETRA_LOD_MAX - 1,
               "LOD_SWITCH needs one threshold per level below the top");

// Where a mesh sits in the world and how big it is, from its IMPORT bound --
// undisplaced, and at bind pose for a skinned mesh. `out_radius` may be NULL
// for a caller that only wants the centre.
//
// That is deliberately NOT the bound the culler tests (_item_bounds), and the
// two answer different questions: culling needs the envelope the geometry can
// reach, where LOD and depth ordering want the size and place of the geometry
// itself. Feeding them the envelope would pick a level from a grass blade's
// sway rather than from the blade, which on apps/tree is an 8-unit margin on a
// mesh far smaller than that.
//
// What the split costs, stated so nobody has to rediscover it: a skinned mesh
// depth-sorts from its BIND centre, which a pose can move far from where it
// draws. That is early-Z efficiency, not correctness -- the opaque lane is
// order-independent apart from coplanar ties -- and LOD is unaffected either
// way, since lod.c refuses skinned meshes a chain at all.
//
// Zeroed because they are out-params of a call in another translation unit,
// which static analysis reads as a use before write.
static void item_world_bounds(const Mesh* mesh, const SceneNode* node, vec3 out_centre,
                              float* out_radius) {
    vec3 world_min = {0.0f, 0.0f, 0.0f}, world_max = {0.0f, 0.0f, 0.0f};
    aabb_transform((float*)mesh->aabb.min, (float*)mesh->aabb.max,
                   (vec4*)node->global_transform, world_min, world_max);
    glm_vec3_add(world_min, world_max, out_centre);
    glm_vec3_scale(out_centre, 0.5f, out_centre);
    if (out_radius) {
        vec3 extent;
        glm_vec3_sub(world_max, world_min, extent);
        *out_radius = glm_vec3_norm(extent) * 0.5f;
    }
}

// Level for this item, from its own bounds. Zero whenever the chain is absent
// or selection is off, which is what makes --no-lod reach the pre-chain frame.
static uint8_t select_lod(const Mesh* mesh, const SceneNode* node, const LodSelect* lod) {
    if (!lod || !lod->enabled || mesh->lod_levels <= 1)
        return 0;

    // Zeroed for the same reason the bounds inside item_world_bounds are: an
    // out-param filled by a callee reads as a use before write to cppcheck.
    vec3 world_centre = {0.0f, 0.0f, 0.0f};
    float radius = 0.0f;
    item_world_bounds(mesh, node, world_centre, &radius);

    float distance = glm_vec3_distance((float*)lod->eye, world_centre);
    if (distance < 1e-4f)
        return 0;

    float projected = (radius / distance) * (lod->bias > 0.0f ? lod->bias : 1.0f);
    uint8_t level = 0;
    while (level < CETRA_LOD_MAX - 1 && projected < LOD_SWITCH[level])
        level++;
    if (level >= mesh->lod_levels)
        level = (uint8_t)(mesh->lod_levels - 1);
    return level;
}

// Depth-first, children left to right, a node's meshes before its gizmo --
// the order the two recursive walks produced between them.
static bool append_node(DrawList* list, SceneNode* node, const LodSelect* lod) {
    if (!node)
        return true;

    for (size_t i = 0; i < node->mesh_count; ++i) {
        Mesh* mesh = node->meshes ? node->meshes[i] : NULL;
        // The same three the draw path refused: no geometry to bind, or no
        // program to bind it with. Refused here so a consumer can assume every
        // item is drawable.
        if (!mesh || !mesh->material || mesh->vao == 0)
            continue;
        if (!mesh->material->shader_program || !mesh->material->shader_program->uniforms)
            continue;

        DrawItem item = {.mesh = mesh, .node = node, .lod = select_lod(mesh, node, lod)};
        classify(mesh, &item.lane, &item.flags);
        if (!push(list, item))
            return false;
    }

    if (node->show_xyz && node->xyz_shader_program) {
        if (!push_gizmo(list, node))
            return false;
    }

    for (size_t i = 0; i < node->children_count; ++i) {
        if (!append_node(list, node->children[i], lod))
            return false;
    }
    return true;
}

bool draw_list_build(DrawList* list, Scene* scene, uint64_t stamp, const LodSelect* lod) {
    if (!list || !scene)
        return false;
    if (list->valid && list->stamp == stamp)
        return true;

    list->count = 0;
    list->gizmo_count = 0;
    memset(list->lane_count, 0, sizeof(list->lane_count));
    list->valid = false;
    if (!append_node(list, scene->root_node, lod))
        return false;

    list->stamp = stamp;
    list->valid = true;
    return true;
}

// The object-space box the geometry this item draws actually occupies, which is
// mesh->aabb only for a mesh that neither sways nor poses.
//
// Returns false when no bound can be established, which the caller reads as
// visible. That is the honest answer rather than a fallback: culling on a bound
// the geometry can leave drops something on screen, and there is no test in the
// corpus that would catch it.
// The box a skinned mesh's POSE occupies, in object space: each bone's own
// bind-space vertices under that bone's matrix, plus the vertices no bone
// claims. Left EMPTY (min > max) when nothing contributed, which is the same
// sentinel the per-bone boxes carry.
static void _posed_bounds(const Mesh* mesh, const AnimationState* pose, vec3 out_min,
                          vec3 out_max) {
    glm_vec3_fill(out_min, FLT_MAX);
    glm_vec3_fill(out_max, -FLT_MAX);

    size_t bones = mesh->bone_aabb_count < pose->active_bone_count ? mesh->bone_aabb_count
                                                                   : pose->active_bone_count;
    for (size_t b = 0; b < bones; ++b) {
        const AABB* box = &mesh->bone_aabb[b];
        if (box->min[0] > box->max[0])
            continue; // no vertex binds this bone, and FLT_MAX cannot be transformed
        // The casts are cglm's const-incorrectness, the same one
        // item_world_bounds carries: it reads these and declares them non-const
        // anyway.
        vec3 lo = {0.0f, 0.0f, 0.0f}, hi = {0.0f, 0.0f, 0.0f};
        aabb_transform((float*)box->min, (float*)box->max, (vec4*)pose->bone_matrices[b], lo, hi);
        glm_vec3_minv(out_min, lo, out_min);
        glm_vec3_maxv(out_max, hi, out_max);
    }
    // Already in this space, so no transform -- and an empty one unions to a
    // no-op, which is why it needs no flag saying whether it is there.
    glm_vec3_minv(out_min, (float*)mesh->bone_rest_aabb.min, out_min);
    glm_vec3_maxv(out_max, (float*)mesh->bone_rest_aabb.max, out_max);
}

static bool _item_bounds(const DrawItem* item, const CullView* view, vec3 out_min, vec3 out_max) {
    const Mesh* mesh = item->mesh;
    glm_vec3_copy((float*)mesh->aabb.min, out_min);
    glm_vec3_copy((float*)mesh->aabb.max, out_max);

    // A skinned mesh's import AABB describes its BIND pose, which is a
    // different shape from the one being drawn -- so it is replaced rather than
    // widened. The three cases are the three the shader takes:
    //
    //   no live pose      -> render_update_skinning_uniforms sets skinned = 0
    //                        and the mesh draws at bind, so the import AABB is
    //                        EXACT here rather than a fallback
    //   pose is this rig  -> the posed union
    //   pose is some      -> a foreign rig's matrices are about to be uploaded
    //   other rig            for this mesh; nothing here can bound that, and
    //                        saying so is better than bounding it wrongly
    if (mesh->is_skinned && view->pose && view->pose->active_bone_count > 0) {
        if (mesh->skeleton != view->pose->skeleton || !mesh->bone_aabb)
            return false;
        _posed_bounds(mesh, view->pose, out_min, out_max);
        if (out_min[0] > out_max[0])
            return false; // a skinned mesh that binds nothing
    }

    // Wind is added in OBJECT space (object_position.glsl) and the caller
    // transforms this box afterwards, so the margin lands in the same space the
    // displacement does and picks up the node's scale exactly as it does.
    float margin = wind_max_offset(view->wind, mesh->material->wind_response,
                                   mesh->material->wind_mode, mesh->wind_flex_max,
                                   mesh->wind_leaf_max);
    if (margin > 0.0f) {
        glm_vec3_subs(out_min, margin, out_min);
        glm_vec3_adds(out_max, margin, out_max);
    }
    return true;
}

bool draw_item_visible(const DrawItem* item, const CullView* view) {
    if (!view || !view->frustum)
        return true;
    // Zeroed for the same reason item_world_bounds zeroes its out-params:
    // static analysis reads a write through a pointer as a use before write.
    vec3 lo = {0.0f, 0.0f, 0.0f}, hi = {0.0f, 0.0f, 0.0f};
    if (!_item_bounds(item, view, lo, hi))
        return true;
    return frustum_test_aabb_transformed(view->frustum, lo, hi, item->node->global_transform);
}

bool draw_run_can_join(const DrawItem* head, const DrawItem* next, const CullView* view) {
    return next->mesh == head->mesh && next->lod == head->lod && draw_item_visible(next, view);
}

// Distance from `eye` to an item's world-space bound centre, through the same
// helper select_lod uses -- so the two consumers of "where is this mesh" cannot
// disagree with each other, whatever they both differ from.
static float item_distance(const DrawItem* item, const vec3 eye) {
    vec3 centre = {0.0f, 0.0f, 0.0f};
    item_world_bounds(item->mesh, item->node, centre, NULL);
    return glm_vec3_distance((float*)eye, centre);
}

// The sort key, packed so one integer compare orders the whole thing:
// [63:48] depth bucket  [47:28] material  [27:4] mesh  [3:0] level.
//
// Mesh above level is what the batcher needs; level last because it only ever
// splits a run that already agreed on everything else.
//
// MATERIAL IS NOT E5's THIRD LIMB AND DOES NOT DELIVER IT. This comment used to
// claim that putting material above mesh made two meshes sharing a material land
// adjacent, so the block uploaded once for both. Measured on apps/forest, that
// is backwards: material switches go 4 -> 106 with the sort on, because the
// depth bucket sits ABOVE material and shatters material coherence before this
// field is ever consulted -- and Morton-grouped graph order already had nearly
// perfect coherence to lose. The field orders WITHIN a bucket and nothing more.
// It is kept because grouping inside a bucket is still the right tiebreak, not
// because it buys what E5 wanted; that limb needs material above depth, which is
// a different sort with a different measurement behind it.
typedef struct SortKey {
    // One slot, two phases: the first pass stores the distance, because the
    // bucket needs a range not known until every item has been measured; the
    // second overwrites it with the packed key. A union rather than two fields
    // so the struct stays the size qsort has to move.
    union {
        float dist;
        uint64_t key;
    };
    // What makes the order TOTAL. Without it every instance of one prototype at
    // one level in one bucket ties, which is the DOMINANT case in a scatter --
    // and qsort is not stable, so their order would be whatever the pivots did.
    // That is observable: masked materials still blend, and coplanar surfaces
    // break their depth tie by draw order. It would also differ across libc,
    // which is exactly the run-to-run instability Mesh.id was added to avoid.
    // Ties resolve to graph order, which is what every golden was baked from.
    uint32_t index;
    DrawItem item;
} SortKey;

static int sort_key_cmp(const void* a, const void* b) {
    const SortKey* x = a;
    const SortKey* y = b;
    // Not (ka - kb): the difference of two uint64 wraps, and a comparator that
    // wraps sorts arbitrarily rather than wrongly-but-consistently.
    if (x->key != y->key)
        return x->key < y->key ? -1 : 1;
    return x->index < y->index ? -1 : 1;
}

bool draw_list_sort_lane(DrawList* dst, const DrawList* src, uint8_t lane, const vec3 eye) {
    if (!dst || !src)
        return false;

    dst->count = 0;
    dst->gizmo_count = 0;
    memset(dst->lane_count, 0, sizeof(dst->lane_count));

    size_t n = src->lane_count[lane];
    if (n == 0) {
        dst->valid = true;
        return true;
    }

    SortKey* keys = malloc(n * sizeof(SortKey));
    if (!keys)
        return false;

    // Two passes over the lane: the first measures the depth range so the
    // buckets span what is actually drawn rather than the far plane. A scene
    // occupying a tenth of the frustum would otherwise land in three buckets and
    // sort no better than not sorting at all.
    size_t count = 0;
    float near_d = 0.0f, far_d = 0.0f;
    for (size_t i = 0; i < src->count; ++i) {
        if (src->items[i].lane != lane)
            continue;
        float d = item_distance(&src->items[i], eye);
        if (count == 0 || d < near_d)
            near_d = d;
        if (count == 0 || d > far_d)
            far_d = d;
        keys[count].item = src->items[i];
        keys[count].index = (uint32_t)count;
        keys[count].dist = d;
        count++;
    }

    float span = far_d - near_d;
    // Everything at one depth: every bucket would be 0 anyway, and dividing by
    // the span would not be defined.
    float scale = span > 1e-4f ? (float)(DRAW_SORT_DEPTH_BUCKETS - 1) / span : 0.0f;

    for (size_t i = 0; i < count; ++i) {
        // Clamped BEFORE the integer conversion, not after. (uint64_t) of a NaN
        // or of an out-of-range float is undefined (C11 6.3.1.4) -- it saturates
        // to 0 on arm64 and to 0x8000... on x86-64 -- and a NaN reaches here from
        // any node whose transform went bad, at which point `scale` is 0 and
        // EVERY item takes the conversion. `!(t > 0)` catches NaN where `t < 0`
        // would not.
        float t = (keys[i].dist - near_d) * scale;
        if (!(t > 0.0f))
            t = 0.0f;
        else if (t > (float)(DRAW_SORT_DEPTH_BUCKETS - 1))
            t = (float)(DRAW_SORT_DEPTH_BUCKETS - 1);
        uint64_t bucket = (uint64_t)t;

        // NOTE: masked items are sorted along with everything else, and that is
        // no longer nearly free. It cost six figures of pixels while the opaque
        // lane still blended -- reordering two overlapping leaf cards changed
        // what each composited against -- and 11.31 stopped that lane blending.
        // What remains is the coplanar tie: two surfaces at exactly equal depth
        // both pass and the later one wins, so draw order still decides between
        // them. Raiden moves 31 px, cornell_box 1.
        //
        // Holding them out was measured separately and is worse on both counts:
        // it wins no GPU time and still moves the image.
        const Material* mat = keys[i].item.mesh->material;
        uint64_t mat_id = mat ? (uint64_t)mat->id : 0;
        // 20 bits of material and 24 of mesh: a scene with more than a million
        // materials or sixteen million meshes would alias two of them into one
        // group, which costs a material upload and never a wrong picture.
        keys[i].key = (bucket << 48) | ((mat_id & 0xFFFFF) << 28) |
                      (((uint64_t)keys[i].item.mesh->id & 0xFFFFFF) << 4) |
                      ((uint64_t)keys[i].item.lod & 0xF);
    }

    qsort(keys, count, sizeof(SortKey), sort_key_cmp);

    for (size_t i = 0; i < count; ++i) {
        if (!push(dst, keys[i].item)) {
            free(keys);
            return false;
        }
    }
    free(keys);
    dst->valid = true;
    return true;
}
