#include <math.h>
#include <string.h>

// Before any header that reaches gl.h: GLEW must be first in the chain.
#include <GL/glew.h>

#include "roads.h"
#include "scene.h"
#include "engine.h"
#include "material.h"
#include "ubo.h"
#include "ext/log.h"

float roads_polyline_distance_xz(const float (*points)[2], int count, float x, float z) {
    if (!points || count < 2)
        return 1.0e9f;
    float best = 1.0e9f;
    for (int i = 0; i + 1 < count; i++) {
        float ax = points[i][0], az = points[i][1];
        float bx = points[i + 1][0], bz = points[i + 1][1];
        float ex = bx - ax, ez = bz - az;
        float len2 = ex * ex + ez * ez;
        // A repeated point is a zero-length segment; the clamp below would
        // divide by it. Its distance is the endpoint's, which t = 0 gives.
        float t = len2 > 1e-8f ? ((x - ax) * ex + (z - az) * ez) / len2 : 0.0f;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        float dx = x - (ax + ex * t);
        float dz = z - (az + ez * t);
        float d = sqrtf(dx * dx + dz * dz);
        if (d < best)
            best = d;
    }
    return best;
}

// Pack the owner's roads into the std140 mirror. Segments go into one shared
// pool in road order, so a road's descriptor names where its own run starts.
static void roads_pack(const Material* m, GpuRoadsBlock* block) {
    memset(block, 0, sizeof(*block));
    int seg = 0;
    int roads = 0;
    for (int r = 0; r < MATERIAL_MAX_ROADS && r < m->road_count; r++) {
        const MaterialRoad* road = &m->roads[r];
        int pts = road->point_count;
        if (pts > MATERIAL_MAX_ROAD_POINTS)
            pts = MATERIAL_MAX_ROAD_POINTS;
        if (pts < 2 || road->width <= 0.0f)
            continue;
        int count = pts - 1;
        if (seg + count > ROAD_SEGS_MAX)
            count = ROAD_SEGS_MAX - seg;
        if (count <= 0)
            continue;
        // Clamped rather than refused: a layer index past the material's own
        // set is an authoring error the applier already warned about by name,
        // and a road drawn in the last layer is a visible wrong answer where a
        // road silently dropped is not.
        int layer = road->layer;
        if (layer < 0)
            layer = 0;
        if (layer >= m->layer_count)
            layer = m->layer_count - 1;

        block->desc[roads * 2][0] = (float)seg;
        block->desc[roads * 2][1] = (float)count;
        block->desc[roads * 2][2] = road->width * 0.5f;
        block->desc[roads * 2][3] = road->feather > 0.0f ? road->feather : 0.0f;
        block->desc[roads * 2 + 1][0] = (float)layer;
        for (int i = 0; i < count; i++) {
            block->segs[seg + i][0] = road->points[i][0];
            block->segs[seg + i][1] = road->points[i][1];
            block->segs[seg + i][2] = road->points[i + 1][0];
            block->segs[seg + i][3] = road->points[i + 1][1];
        }
        seg += count;
        roads++;
    }
    block->info[0] = roads;
}

void material_roads_ensure(struct Scene* scene, struct Engine* engine) {
    if (!scene || !engine)
        return;

    /*
     * v1 carries ONE road-bearing material per scene: the engine owns a single
     * RoadsBlock, and a buffer per binding point is ubo.h's lifetime contract.
     * The first qualifying material owns the roads; any further one is warned
     * about once and disarmed.
     *
     * This loop is the one owner of roads_armed, so a material that stops
     * qualifying is DISARMED rather than merely skipped -- the shader gates on
     * that uniform, and a stale arm would reshape weights toward a layer whose
     * roads nothing uploaded.
     */
    Material* owner = NULL;
    for (size_t i = 0; i < scene->material_count; i++) {
        Material* m = scene->materials[i];
        if (!m)
            continue;
        // WORLD_XZ only, and not merely because the composite cache is: a road
        // is placed in the world, and a UV1 splat has no world frame to place
        // it in. The authoring path warns by name; this is what makes that
        // warning true rather than advisory.
        bool want = m->road_count > 0 && m->layer_count > 0 &&
                    m->splat_space == SPLAT_SPACE_WORLD_XZ;
        if (want && !owner) {
            owner = m;
            m->roads_armed = true;
            continue;
        }
        if (want) {
            static bool warned_second_owner;
            if (!warned_second_owner) {
                log_warn("roads: material '%s' also carries roads; v1 carries one road-bearing "
                         "material per scene, this one's roads are ignored",
                         m->name ? m->name : "(unnamed)");
                warned_second_owner = true;
            }
        }
        m->roads_armed = false;
    }

    GpuRoadsBlock block;
    if (owner)
        roads_pack(owner, &block);
    else
        memset(&block, 0, sizeof(block));

    // By value, the water-seed idiom: a dirty flag would need every writer to
    // set it, and the writers are a scene file, a CLI schedule and an app
    // building a trail. Packing a kilobyte and comparing it is cheaper than
    // being wrong about who remembered.
    if (memcmp(&block, &engine->roads_shadow, sizeof(block)) == 0)
        return;
    engine->roads_shadow = block;
    if (engine->roads_ubo)
        ubo_upload(engine->roads_ubo, &block, sizeof(block));
}
