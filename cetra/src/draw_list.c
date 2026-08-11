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

// The material decides which pass draws a mesh and what the depth pass may do
// with it. Both were recomputed per mesh per pass before; neither depends on
// the pass.
static void classify(const Material* mat, uint8_t* lane, uint8_t* flags) {
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
    if (mat->wind_response > 0.0f)
        *flags |= DRAW_UNBOUNDED;
}

// Depth-first, children left to right, a node's meshes before its gizmo --
// the order the two recursive walks produced between them.
static bool append_node(DrawList* list, SceneNode* node) {
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

        DrawItem item = {.mesh = mesh, .node = node};
        classify(mesh->material, &item.lane, &item.flags);
        if (mesh->is_skinned)
            item.flags |= DRAW_UNBOUNDED;
        if (!push(list, item))
            return false;
    }

    if (node->show_xyz && node->xyz_shader_program) {
        if (!push_gizmo(list, node))
            return false;
    }

    for (size_t i = 0; i < node->children_count; ++i) {
        if (!append_node(list, node->children[i]))
            return false;
    }
    return true;
}

bool draw_list_build(DrawList* list, Scene* scene, uint64_t stamp) {
    if (!list || !scene)
        return false;
    if (list->valid && list->stamp == stamp)
        return true;

    list->count = 0;
    list->gizmo_count = 0;
    memset(list->lane_count, 0, sizeof(list->lane_count));
    list->valid = false;
    if (!append_node(list, scene->root_node))
        return false;

    list->stamp = stamp;
    list->valid = true;
    return true;
}
