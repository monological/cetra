#include <stdlib.h>
#include <string.h>

#include "draw_list.h"

#include "ext/log.h"
#include "material.h"
#include "scene.h"

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
    bool blend = mat->alpha_mode == ALPHA_BLEND;
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
    // Neither is bounded by mesh->aabb: calculate_aabb runs once at import, so
    // a skinned mesh carries bind-pose bounds, and wind displacement is computed
    // in the shader after the fact.
    if (mesh->is_skinned || mat->wind_response > 0.0f)
        *flags |= DRAW_UNBOUNDED;
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

// Level for this item, from its own bounds. Zero whenever the chain is absent
// or selection is off, which is what makes --no-lod reach the pre-chain frame.
static uint8_t select_lod(const Mesh* mesh, const SceneNode* node, const LodSelect* lod) {
    if (!lod || !lod->enabled || mesh->lod_levels <= 1)
        return 0;

    // The same world bound the culler tests against, from the same function, so
    // "how big is this mesh in the world" has one definition rather than a
    // second one here free to drift from it.
    // Zeroed because they are out-params of a call in another translation unit,
    // which static analysis reads as a use before write.
    vec3 world_min = {0.0f, 0.0f, 0.0f}, world_max = {0.0f, 0.0f, 0.0f};
    vec3 world_centre, extent;
    aabb_transform((float*)mesh->aabb.min, (float*)mesh->aabb.max,
                   (vec4*)node->global_transform, world_min, world_max);
    glm_vec3_add(world_min, world_max, world_centre);
    glm_vec3_scale(world_centre, 0.5f, world_centre);
    glm_vec3_sub(world_max, world_min, extent);
    float radius = glm_vec3_norm(extent) * 0.5f;

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

bool draw_item_visible(const DrawItem* item, const Frustum* frustum) {
    if (!frustum || (item->flags & DRAW_UNBOUNDED))
        return true;
    return frustum_test_aabb_transformed(frustum, item->mesh->aabb.min, item->mesh->aabb.max,
                                         item->node->global_transform);
}

bool draw_run_can_join(const DrawItem* head, const DrawItem* next, const Frustum* frustum) {
    return next->mesh == head->mesh && next->lod == head->lod && draw_item_visible(next, frustum);
}
