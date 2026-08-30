#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "occlusion.h"

#include "draw_list.h"
#include "mesh.h"
#include "scene.h"

#include "ext/log.h"

// THE CONSERVATISM CONTRACT. Every rounding decision in this file leans one
// way, and together they make a false negative -- a visible thing called
// hidden -- impossible by arithmetic:
//
//   occluder depth     ceil toward far, max over a polygon's clipped vertices
//   occluder coverage  a pixel fills only if its ENTIRE footprint square is
//                      inside every edge of the clipped polygon
//   occludee depth     floor toward near, min over the world box's corners
//   occludee footprint outward to tile boundaries
//   the test           hidden requires item_q >= tile_zmax on EVERY tile, and
//                      a tile with one uncovered pixel is EMPTY and fails all
//
// So stored occluder depth >= true surface depth everywhere it claims
// coverage, tested item depth <= the item's true nearest depth, and
// item_q >= tile_zmax implies the true item stands behind a true surface.
//
// FACES ARE NOT CLASSIFIED. All six of a box's faces rasterise, because the
// per-pixel merge is MIN and a back face is farther than the front face along
// any ray through both -- classification would change no pixel, and at this
// resolution the doubled fill is cheaper than the orientation bugs it invites
// (a mirrored transform flips winding; nothing here has to care).
//
// FIXED-POINT ON PURPOSE: -O2 reassociates float arithmetic and has moved CPU
// results in this tree before (the erosion digest). The floats feeding the
// quantiser can still drift a last bit across builds, which is why the
// fixture keeps every counted mesh away from decision boundaries by asserted
// margins -- but within one build the buffer is bytes, and a probe can digest
// bytes.

// Clip-space guards. W_EPS is light_cluster.c's near-degenerate bound; the
// side planes are clipped OUTSET by SIDE_SLACK rather than exactly, because a
// vertex barely past w = 0 divides to NDC coordinates in the millions and the
// edge functions below would cancel catastrophically at pixel scale. Clipping
// to ~1.05 bounds every coordinate the fill ever sees while cutting nothing
// inside the visible frame.
#define OCC_W_EPS 1e-6f
#define OCC_SIDE_SLACK 1.05f

typedef struct ClipPoly {
    // 4 corners, clipped by up to 6 planes: each clip adds at most one vertex.
    float v[10][4];
    int count;
} ClipPoly;

static void clip_against(ClipPoly* poly, const float plane[4]) {
    ClipPoly out;
    out.count = 0;
    for (int i = 0; i < poly->count; ++i) {
        const float* a = poly->v[i];
        const float* b = poly->v[(i + 1) % poly->count];
        float da = plane[0] * a[0] + plane[1] * a[1] + plane[2] * a[2] + plane[3] * a[3];
        float db = plane[0] * b[0] + plane[1] * b[1] + plane[2] * b[2] + plane[3] * b[3];
        if (da >= 0.0f) {
            memcpy(out.v[out.count++], a, sizeof(float) * 4);
        }
        if ((da >= 0.0f) != (db >= 0.0f)) {
            float t = da / (da - db);
            for (int k = 0; k < 4; ++k)
                out.v[out.count][k] = a[k] + t * (b[k] - a[k]);
            out.count++;
        }
    }
    *poly = out;
}

// Rasterise one convex polygon, given in clip space, into `depth`.
// The single shared path: the context's own buffer, the probe's full-res
// reference twin and the mesh validator all come through here.
static void raster_convex_clip(uint16_t* depth, int w, int h, ClipPoly* poly) {
    // Near plane, then the outset sides. The near clip is what makes a
    // polygon reaching behind the camera contribute exactly its in-front part.
    static const float near_plane[4] = {0.0f, 0.0f, 1.0f, 1.0f}; // z + w >= 0
    clip_against(poly, near_plane);
    if (poly->count < 3)
        return;
    const float planes[4][4] = {
        {1.0f, 0.0f, 0.0f, OCC_SIDE_SLACK},  // x >= -slack * w
        {-1.0f, 0.0f, 0.0f, OCC_SIDE_SLACK}, // x <=  slack * w
        {0.0f, 1.0f, 0.0f, OCC_SIDE_SLACK},
        {0.0f, -1.0f, 0.0f, OCC_SIDE_SLACK},
    };
    for (int p = 0; p < 4; ++p) {
        clip_against(poly, planes[p]);
        if (poly->count < 3)
            return;
    }
    // Guard the divide as well as clip toward it: a vertex ON the w = 0 plane
    // survives the near clip when z is also ~0.
    for (int i = 0; i < poly->count; ++i)
        if (poly->v[i][3] < OCC_W_EPS)
            return;

    // Divide, and take the polygon's ONE conservative depth: the farthest
    // vertex, ceil-quantised. A face is planar, so one depth is honest.
    float px[10], py[10];
    float zmax = -1.0f;
    for (int i = 0; i < poly->count; ++i) {
        float inv_w = 1.0f / poly->v[i][3];
        float ndc_x = poly->v[i][0] * inv_w;
        float ndc_y = poly->v[i][1] * inv_w;
        float ndc_z = poly->v[i][2] * inv_w;
        px[i] = (ndc_x * 0.5f + 0.5f) * (float)w;
        py[i] = (ndc_y * 0.5f + 0.5f) * (float)h;
        if (ndc_z > zmax)
            zmax = ndc_z;
    }
    float dq = ceilf((zmax * 0.5f + 0.5f) * (float)OCCLUSION_DEPTH_MAX);
    if (dq < 0.0f)
        dq = 0.0f;
    if (dq > (float)OCCLUSION_DEPTH_MAX)
        dq = (float)OCCLUSION_DEPTH_MAX;
    uint16_t q = (uint16_t)dq;

    // Winding-normalise: edge normals must point INTO the polygon, whatever
    // orientation projection left it in.
    float area2 = 0.0f;
    for (int i = 0; i < poly->count; ++i) {
        int j = (i + 1) % poly->count;
        area2 += px[i] * py[j] - px[j] * py[i];
    }
    if (fabsf(area2) < 1e-9f)
        return; // edge-on: covers nothing, which is the conservative answer
    float flip = area2 > 0.0f ? 1.0f : -1.0f;

    int x0 = (int)floorf(fminf(fminf(px[0], px[1]), px[2]));
    int x1 = (int)ceilf(px[0]);
    int y0 = (int)floorf(py[0]);
    int y1 = (int)ceilf(py[0]);
    for (int i = 0; i < poly->count; ++i) {
        x0 = (int)fminf((float)x0, floorf(px[i]));
        x1 = (int)fmaxf((float)x1, ceilf(px[i]));
        y0 = (int)fminf((float)y0, floorf(py[i]));
        y1 = (int)fmaxf((float)y1, ceilf(py[i]));
    }
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > w)
        x1 = w;
    if (y1 > h)
        y1 = h;

    // Inner-conservative fill: a pixel fills only when the WORST corner of its
    // footprint square is inside every edge. Evaluating the worst corner is
    // the whole test -- if the corner the edge most disfavours is in, the
    // square is in.
    for (int e = 0; e < poly->count; ++e) {
        int j = (e + 1) % poly->count;
        float ex = -(py[j] - py[e]) * flip; // inward normal
        float ey = (px[j] - px[e]) * flip;
        // Precompute per-edge: offset of the worst footprint corner.
        poly->v[e][0] = ex;
        poly->v[e][1] = ey;
        poly->v[e][2] = px[e];
        poly->v[e][3] = py[e];
    }
    for (int yy = y0; yy < y1; ++yy) {
        uint16_t* row = depth + (size_t)yy * (size_t)w;
        for (int xx = x0; xx < x1; ++xx) {
            int inside = 1;
            for (int e = 0; e < poly->count && inside; ++e) {
                float ex = poly->v[e][0];
                float ey = poly->v[e][1];
                float cx = (float)xx + (ex > 0.0f ? 0.0f : 1.0f);
                float cy = (float)yy + (ey > 0.0f ? 0.0f : 1.0f);
                if (ex * (cx - poly->v[e][2]) + ey * (cy - poly->v[e][3]) < 0.0f)
                    inside = 0;
            }
            if (inside && q < row[xx])
                row[xx] = q;
        }
    }
}

static void transform_point(mat4 view_proj, mat4 transform, const float* p, float out[4]) {
    vec4 world = {p[0], p[1], p[2], 1.0f};
    if (transform) {
        vec4 tmp;
        glm_mat4_mulv(transform, world, tmp);
        glm_vec4_copy(tmp, world);
    }
    vec4 clip;
    glm_mat4_mulv(view_proj, world, clip);
    memcpy(out, clip, sizeof(float) * 4);
}

static bool eye_inside_box(const vec3 eye, const vec3 box_min, const vec3 box_max,
                           mat4 transform) {
    // In OBJECT space, where the box is axis-aligned whatever the node did.
    // The margin makes "on the surface" count as inside: skipping is the safe
    // direction, and an eye grazing a face has its near plane inside the box.
    vec3 e;
    if (transform) {
        mat4 inv;
        glm_mat4_inv(transform, inv);
        glm_mat4_mulv3(inv, (float*)eye, 1.0f, e);
    } else {
        glm_vec3_copy((float*)eye, e);
    }
    const float margin = 1e-3f;
    for (int k = 0; k < 3; ++k)
        if (e[k] < box_min[k] - margin || e[k] > box_max[k] + margin)
            return false;
    return true;
}

void occlusion_rasterize_box_into(uint16_t* depth, int w, int h, mat4 view_proj, const vec3 eye,
                                  const vec3 box_min, const vec3 box_max, mat4 transform) {
    if (!depth || w <= 0 || h <= 0)
        return;
    if (eye_inside_box(eye, box_min, box_max, transform))
        return;

    float clip[8][4];
    for (int c = 0; c < 8; ++c) {
        const float p[3] = {(c & 1) ? box_max[0] : box_min[0], (c & 2) ? box_max[1] : box_min[1],
                      (c & 4) ? box_max[2] : box_min[2]};
        transform_point(view_proj, transform, p, clip[c]);
    }
    // Corner indices of the six faces; orientation is irrelevant (see the
    // header comment: min-merge makes classification a no-op).
    static const int faces[6][4] = {
        {0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3},
    };
    for (int f = 0; f < 6; ++f) {
        ClipPoly poly;
        poly.count = 4;
        for (int i = 0; i < 4; ++i)
            memcpy(poly.v[i], clip[faces[f][i]], sizeof(float) * 4);
        raster_convex_clip(depth, w, h, &poly);
    }
}

void occlusion_rasterize_mesh_into(uint16_t* depth, int w, int h, mat4 view_proj,
                                   const struct Mesh* mesh, mat4 transform) {
    if (!depth || !mesh || !mesh->vertices || !mesh->indices)
        return;
    for (size_t t = 0; t + 2 < mesh->index_count; t += 3) {
        ClipPoly poly;
        poly.count = 3;
        for (int i = 0; i < 3; ++i) {
            const float* p = &mesh->vertices[(size_t)mesh->indices[t + (size_t)i] * 3];
            transform_point(view_proj, transform, p, poly.v[i]);
        }
        raster_convex_clip(depth, w, h, &poly);
    }
}

void occlusion_begin(OcclusionContext* context, mat4 view_proj, const vec3 eye) {
    if (!context)
        return;
    glm_mat4_copy(view_proj, context->view_proj);
    glm_vec3_copy((float*)eye, context->eye);
    // Every byte 0xFF is every pixel OCCLUSION_DEPTH_EMPTY.
    memset(context->depth, 0xFF, sizeof(context->depth));
    context->occluder_count = 0;
    context->tested = 0;
    context->culled = 0;
    context->active = false;
}

void occlusion_add_box(OcclusionContext* context, const vec3 box_min, const vec3 box_max,
                       mat4 transform) {
    if (!context)
        return;
    if (context->occluder_count >= OCCLUSION_MAX_OCCLUDERS) {
        if (!context->warned_overflow) {
            log_warn("occlusion: more than %d occluders; extras dropped",
                     OCCLUSION_MAX_OCCLUDERS);
            context->warned_overflow = true;
        }
        return;
    }
    if (eye_inside_box(context->eye, box_min, box_max, transform))
        return; // skipped boxes do not count toward the frame's set
    context->occluder_count++;
    occlusion_rasterize_box_into(&context->depth[0][0], OCCLUSION_W, OCCLUSION_H,
                                 context->view_proj, context->eye, box_min, box_max, transform);
}

void occlusion_finish(OcclusionContext* context) {
    if (!context)
        return;
    context->active = false;
    for (int ty = 0; ty < OCCLUSION_TILES_Y; ++ty) {
        for (int tx = 0; tx < OCCLUSION_TILES_X; ++tx) {
            uint16_t zmax = 0;
            for (int y = 0; y < OCCLUSION_TILE_H; ++y) {
                const uint16_t* row = &context->depth[ty * OCCLUSION_TILE_H + y][tx * OCCLUSION_TILE_W];
                for (int x = 0; x < OCCLUSION_TILE_W; ++x)
                    if (row[x] > zmax)
                        zmax = row[x];
            }
            context->tile_zmax[ty][tx] = zmax;
            if (zmax != OCCLUSION_DEPTH_EMPTY)
                context->active = true;
        }
    }
}

bool occlusion_active(const OcclusionContext* context) {
    return context && context->active;
}

bool occlusion_test_aabb(const OcclusionContext* context, const vec3 world_min,
                         const vec3 world_max) {
    if (!context || !context->active)
        return false;
    // cglm's API is not const-correct; the cast keeps mat4's alignment where a
    // pointer-to-array cast would shed it.
    OcclusionContext* mut = (OcclusionContext*)context;

    float ndc_min[3] = {1e30f, 1e30f, 1e30f};
    float ndc_max[3] = {-1e30f, -1e30f, -1e30f};
    for (int c = 0; c < 8; ++c) {
        const float p[3] = {(c & 1) ? world_max[0] : world_min[0],
                      (c & 2) ? world_max[1] : world_min[1],
                      (c & 4) ? world_max[2] : world_min[2]};
        float clip[4];
        transform_point(mut->view_proj, NULL, p, clip);
        // A box touching the near plane or reaching behind the camera cannot
        // be culled -- the conservative bail, light_cluster.c's idiom.
        if (clip[3] < OCC_W_EPS || clip[2] + clip[3] <= 0.0f)
            return false;
        float inv_w = 1.0f / clip[3];
        for (int k = 0; k < 3; ++k) {
            float v = clip[k] * inv_w;
            if (v < ndc_min[k])
                ndc_min[k] = v;
            if (v > ndc_max[k])
                ndc_max[k] = v;
        }
    }

    // Nearest depth, floored toward near.
    float fq = floorf((ndc_min[2] * 0.5f + 0.5f) * (float)OCCLUSION_DEPTH_MAX);
    if (fq < 0.0f)
        fq = 0.0f;
    if (fq > (float)OCCLUSION_DEPTH_MAX)
        fq = (float)OCCLUSION_DEPTH_MAX;
    uint16_t item_q = (uint16_t)fq;

    // Outward-rounded footprint, in tiles. Clamping to the buffer is safe:
    // whatever hangs past NDC +-1 is outside the frustum, which the frustum
    // test owns.
    int px0 = (int)floorf((ndc_min[0] * 0.5f + 0.5f) * (float)OCCLUSION_W);
    int px1 = (int)ceilf((ndc_max[0] * 0.5f + 0.5f) * (float)OCCLUSION_W);
    int py0 = (int)floorf((ndc_min[1] * 0.5f + 0.5f) * (float)OCCLUSION_H);
    int py1 = (int)ceilf((ndc_max[1] * 0.5f + 0.5f) * (float)OCCLUSION_H);
    int tx0 = px0 < 0 ? 0 : px0 / OCCLUSION_TILE_W;
    int ty0 = py0 < 0 ? 0 : py0 / OCCLUSION_TILE_H;
    int tx1 = (px1 - 1) / OCCLUSION_TILE_W;
    int ty1 = (py1 - 1) / OCCLUSION_TILE_H;
    if (tx0 >= OCCLUSION_TILES_X || ty0 >= OCCLUSION_TILES_Y || tx1 < 0 || ty1 < 0)
        return false; // wholly off-buffer: nothing here may claim it hidden
    if (tx1 >= OCCLUSION_TILES_X)
        tx1 = OCCLUSION_TILES_X - 1;
    if (ty1 >= OCCLUSION_TILES_Y)
        ty1 = OCCLUSION_TILES_Y - 1;

    for (int ty = ty0; ty <= ty1; ++ty) {
        for (int tx = tx0; tx <= tx1; ++tx) {
            uint16_t zmax = context->tile_zmax[ty][tx];
            if (zmax == OCCLUSION_DEPTH_EMPTY || item_q < zmax)
                return false;
        }
    }
    return true;
}

void occlusion_cull_list(OcclusionContext* context, struct DrawList* list,
                         const struct CullView* view) {
    if (!context || !list || !context->active)
        return;
    for (size_t i = 0; i < list->count; ++i) {
        DrawItem* item = &list->items[i];
        // The same box the frustum test uses, margins and pose included.
        // Unboundable means visible AND never occlusion-cull.
        AABB box;
        if (!draw_item_bounds(item, view, &box))
            continue;
        // Zeroed because they are out-params of a call in another translation
        // unit, which static analysis reads as a use before write.
        vec3 world_min = {0.0f, 0.0f, 0.0f}, world_max = {0.0f, 0.0f, 0.0f};
        aabb_transform(box.min, box.max, (vec4*)item->node->global_transform, world_min,
                       world_max);
        context->tested++;
        if (occlusion_test_aabb(context, world_min, world_max)) {
            item->occluded = 1;
            context->culled++;
        }
    }
}

const uint16_t* occlusion_depth_pixels(const OcclusionContext* context, int* w, int* h) {
    if (w)
        *w = OCCLUSION_W;
    if (h)
        *h = OCCLUSION_H;
    return context ? &context->depth[0][0] : NULL;
}

const uint16_t* occlusion_tiles(const OcclusionContext* context, int* tx, int* ty) {
    if (tx)
        *tx = OCCLUSION_TILES_X;
    if (ty)
        *ty = OCCLUSION_TILES_Y;
    return context ? &context->tile_zmax[0][0] : NULL;
}

int occlusion_occluder_count(const OcclusionContext* context) {
    return context ? context->occluder_count : 0;
}

size_t occlusion_tested_count(const OcclusionContext* context) {
    return context ? context->tested : 0;
}

size_t occlusion_culled_count(const OcclusionContext* context) {
    return context ? context->culled : 0;
}

OcclusionContext* create_occlusion_context(void) {
    OcclusionContext* context = calloc(1, sizeof(OcclusionContext));
    if (!context) {
        log_error("Failed to allocate occlusion context");
        return NULL;
    }
    return context;
}

void free_occlusion_context(OcclusionContext* context) {
    if (!context)
        return;
    free(context);
}
