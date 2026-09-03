#include <math.h>
#include <stdio.h>
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
// FACES ARE CLASSIFIED, and the near plane is why. A first version rasterised
// all six on the argument that min-merge makes a back face harmless -- true
// along any ray that passes through both faces, and FALSE once the near plane
// clips the front face away: the renderer then shows THROUGH the opened shell
// (front near-clipped, interior backface-culled) while an unclassified raster
// still stood the back face across the frame. Measured on the portal fixture
// with the eye a near-plane's depth from the wall: 118,794 px of visible scene
// culled, 24.7% of the frame. Only faces whose OUTWARD side holds the eye
// rasterise, so the raster keeps exactly the region the renderer keeps.
//
// Classification also SUBSUMES the camera-inside-a-box skip: from inside, no
// face is front-facing and the box covers nothing -- a theorem here where an
// explicit inside test was a second mechanism to keep right. Orientation is
// winding-agnostic (the outward normal is fixed against the opposite corner),
// so a mirrored transform still needs no care. Edge-on faces classify as back
// and cover nothing, which is the conservative side.
//
// FIXED-POINT ON PURPOSE: -O2 reassociates float arithmetic and has moved CPU
// results in this tree before (the erosion digest). The floats feeding the
// quantiser can still drift a last bit across builds, which is why the
// fixture keeps every counted mesh away from decision boundaries by asserted
// margins -- but within one build the buffer is bytes, and a probe can digest
// bytes.
//
// And the conservatism is exact only up to FLOAT rounding in the fill's edge
// functions -- the rounding directions are all chosen safely, but the edge
// tests themselves are float, so the fill requires OCC_FILL_EPS of slack
// rather than bare non-negativity. Shrinking coverage is the safe direction;
// without the bias, cameras in the elongated near-wedge regime produced fills
// the 4x reference refused (probe violations=4, from float cancellation, not
// from any rounding choice).

// Clip-space guards. W_EPS is light_cluster.c's near-degenerate bound; the
// side planes are clipped OUTSET by SIDE_SLACK rather than exactly, because a
// vertex barely past w = 0 divides to NDC coordinates in the millions and the
// edge functions below would cancel catastrophically at pixel scale. Clipping
// to ~1.05 bounds every coordinate the fill ever sees while cutting nothing
// inside the visible frame.
#define OCC_W_EPS 1e-6f
#define OCC_SIDE_SLACK 1.05f
// Pixel-space slack the fill's edge test must clear -- see the float caveat in
// the header. Edge-function magnitudes are bounded by the outset clip
// (coordinates <= ~1.05 * buffer size), so 1e-3 dwarfs the cancellation error
// while costing far less than one pixel of coverage.
#define OCC_FILL_EPS 1e-3f

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

    float bx0 = px[0], bx1 = px[0], by0 = py[0], by1 = py[0];
    for (int i = 1; i < poly->count; ++i) {
        bx0 = fminf(bx0, px[i]);
        bx1 = fmaxf(bx1, px[i]);
        by0 = fminf(by0, py[i]);
        by1 = fmaxf(by1, py[i]);
    }
    int x0 = (int)floorf(bx0), x1 = (int)ceilf(bx1);
    int y0 = (int)floorf(by0), y1 = (int)ceilf(by1);
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > w)
        x1 = w;
    if (y1 > h)
        y1 = h;

    // Inner-conservative fill: a pixel fills only when the WORST corner of its
    // footprint square clears every edge by OCC_FILL_EPS -- if the corner the
    // edge most disfavours is in, the square is in.
    float ex[10], ey[10];
    for (int e = 0; e < poly->count; ++e) {
        int j = (e + 1) % poly->count;
        ex[e] = -(py[j] - py[e]) * flip; // inward normal
        ey[e] = (px[j] - px[e]) * flip;
    }
    for (int yy = y0; yy < y1; ++yy) {
        uint16_t* row = depth + (size_t)yy * (size_t)w;
        for (int xx = x0; xx < x1; ++xx) {
            int inside = 1;
            for (int e = 0; e < poly->count && inside; ++e) {
                float cx = (float)xx + (ex[e] > 0.0f ? 0.0f : 1.0f);
                float cy = (float)yy + (ey[e] > 0.0f ? 0.0f : 1.0f);
                if (ex[e] * (cx - px[e]) + ey[e] * (cy - py[e]) < OCC_FILL_EPS)
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

void occlusion_rasterize_box_into(uint16_t* depth, int w, int h, mat4 view_proj, const vec4 eye,
                                  const vec3 box_min, const vec3 box_max, mat4 transform) {
    if (!depth || w <= 0 || h <= 0)
        return;

    vec3 world[8];
    float clip[8][4];
    for (int c = 0; c < 8; ++c) {
        vec3 p = {(c & 1) ? box_max[0] : box_min[0], (c & 2) ? box_max[1] : box_min[1],
                  (c & 4) ? box_max[2] : box_min[2]};
        if (transform)
            glm_mat4_mulv3(transform, p, 1.0f, world[c]);
        else
            glm_vec3_copy(p, world[c]);
        vec4 hw = {world[c][0], world[c][1], world[c][2], 1.0f};
        vec4 hc;
        glm_mat4_mulv(view_proj, hw, hc);
        memcpy(clip[c], hc, sizeof(float) * 4);
    }

    // Corner indices of the six faces, each with one corner of the OPPOSITE
    // face -- what fixes the outward normal without trusting winding, so a
    // mirrored transform needs no care. Only front faces rasterise; the header
    // says why that is correctness, not perf.
    static const int faces[6][5] = {
        {0, 1, 3, 2, 4}, {4, 6, 7, 5, 0}, {0, 4, 5, 1, 2},
        {2, 3, 7, 6, 0}, {0, 2, 6, 4, 1}, {1, 5, 7, 3, 0},
    };
    for (int f = 0; f < 6; ++f) {
        const int* fc = faces[f];
        vec3 e1, e2, n, to_opp, to_eye;
        glm_vec3_sub(world[fc[1]], world[fc[0]], e1);
        glm_vec3_sub(world[fc[2]], world[fc[0]], e2);
        glm_vec3_cross(e1, e2, n);
        glm_vec3_sub(world[fc[4]], world[fc[0]], to_opp);
        if (glm_vec3_dot(n, to_opp) > 0.0f)
            glm_vec3_negate(n);
        // to_eye = eye.xyz - eye.w * p: the vector to a finite eye, or the
        // one direction every face sees an eye at infinity along.
        glm_vec3_scale(world[fc[0]], eye[3], to_eye);
        glm_vec3_sub((float*)eye, to_eye, to_eye);
        // <= skips edge-on and degenerate faces too: both cover nothing,
        // which is the conservative side.
        if (glm_vec3_dot(n, to_eye) <= 0.0f)
            continue;

        ClipPoly poly;
        poly.count = 4;
        for (int i = 0; i < 4; ++i)
            memcpy(poly.v[i], clip[fc[i]], sizeof(float) * 4);
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

void occlusion_begin(OcclusionContext* context, mat4 view_proj, const vec4 eye) {
    if (!context)
        return;
    glm_mat4_copy(view_proj, context->view_proj);
    glm_vec4_copy((float*)eye, context->eye);
    // Every byte 0xFF is every pixel OCCLUSION_DEPTH_EMPTY.
    memset(context->depth, 0xFF, sizeof(context->depth));
    context->occluder_count = 0;
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
    // No inside-a-box skip: from inside, classification rejects every face and
    // the box covers nothing on its own -- recording it is harmless and keeps
    // the probe's replay exactly the frame's set.
    OcclusionBoxRec* rec = &context->boxes[context->occluder_count++];
    glm_vec3_copy((float*)box_min, rec->box_min);
    glm_vec3_copy((float*)box_max, rec->box_max);
    rec->transformed = transform != NULL;
    if (transform)
        glm_mat4_copy(transform, rec->transform);
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

// The occludee half BOTH tests share: the box's conservatively nearest
// quantised depth and its NDC rect, or false when it touches the near plane or
// reaches behind the camera and may not be considered at all (the conservative
// bail, light_cluster.c's idiom). Shared by design rather than by copy -- the
// probe verifies the hierarchy DOWNSTREAM of this point, and a drifted second
// copy of the prologue would have it validating a different question.
static bool project_occludee(mat4 view_proj, const vec3 world_min, const vec3 world_max,
                             float ndc_min[3], float ndc_max[3], uint16_t* item_q) {
    for (int k = 0; k < 3; ++k) {
        ndc_min[k] = 1e30f;
        ndc_max[k] = -1e30f;
    }
    for (int c = 0; c < 8; ++c) {
        const float p[3] = {(c & 1) ? world_max[0] : world_min[0],
                            (c & 2) ? world_max[1] : world_min[1],
                            (c & 4) ? world_max[2] : world_min[2]};
        float clip[4];
        transform_point(view_proj, NULL, p, clip);
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
    *item_q = (uint16_t)fq;
    return true;
}

bool occlusion_test_aabb(const OcclusionContext* context, const vec3 world_min,
                         const vec3 world_max) {
    if (!context || !context->active)
        return false;
    // cglm's API is not const-correct; the cast keeps mat4's alignment where a
    // pointer-to-array cast would shed it.
    OcclusionContext* mut = (OcclusionContext*)context;

    float ndc_min[3], ndc_max[3];
    uint16_t item_q;
    if (!project_occludee(mut->view_proj, world_min, world_max, ndc_min, ndc_max, &item_q))
        return false;

    // Outward-rounded footprint, in tiles. Clamping to the buffer is safe:
    // whatever hangs past NDC +-1 is outside the frustum, which the frustum
    // test owns.
    int px0 = (int)floorf((ndc_min[0] * 0.5f + 0.5f) * (float)OCCLUSION_W);
    int px1 = (int)ceilf((ndc_max[0] * 0.5f + 0.5f) * (float)OCCLUSION_W);
    int py0 = (int)floorf((ndc_min[1] * 0.5f + 0.5f) * (float)OCCLUSION_H);
    int py1 = (int)ceilf((ndc_max[1] * 0.5f + 0.5f) * (float)OCCLUSION_H);
    // Wholly off-buffer: nothing here may claim it hidden. Checked on the
    // PIXEL bounds, before the divisions -- C truncates toward zero, so
    // (px1 - 1) / TILE_W reads 0 for px1 anywhere in [-6, 0] and a box off
    // the left edge would otherwise be tested against tile column 0.
    if (px1 <= 0 || py1 <= 0 || px0 >= OCCLUSION_W || py0 >= OCCLUSION_H)
        return false;
    int tx0 = px0 < 0 ? 0 : px0 / OCCLUSION_TILE_W;
    int ty0 = py0 < 0 ? 0 : py0 / OCCLUSION_TILE_H;
    int tx1 = (px1 - 1) / OCCLUSION_TILE_W;
    int ty1 = (py1 - 1) / OCCLUSION_TILE_H;
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

void occlusion_cull_list(const OcclusionContext* context, struct DrawList* list,
                         const struct CullView* view) {
    if (!context || !list || !context->active)
        return;
    for (size_t i = 0; i < list->count; ++i) {
        DrawItem* item = &list->items[i];
        // WRITTEN both ways, not only set: the pass is then idempotent, so an
        // embedder driving render_current_scene twice in one frame (same stamp,
        // no rebuild) gets each camera's own answer instead of the OR of both.
        item->occluded = 0;
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
        if (occlusion_test_aabb(context, world_min, world_max))
            item->occluded = 1;
    }
}

// --- the probe --------------------------------------------------------------

// Reference resolution: 4x the mask in each axis. Finer only sharpens the
// reference; the property being checked (hierarchical-hidden implies
// reference-hidden) is one-directional, so the exact factor is not load-bearing.
#define OCC_REF_W (OCCLUSION_W * 4)
#define OCC_REF_H (OCCLUSION_H * 4)
#define OCC_SWEEP_COUNT 2048
#define OCC_SWEEP_SEED 1226u
#define OCC_PROBE_VIOLATION_CAP 8

// Per-pixel exact test against the reference buffer: the shared occludee
// prologue (project_occludee -- shared by design, see its comment), with the
// tile fold and the outward footprint replaced by the pixels themselves. What
// the twin does NOT share is exactly the hierarchy it exists to check.
static bool ref_test_aabb(const uint16_t* depth, mat4 view_proj, const vec3 world_min,
                          const vec3 world_max) {
    float ndc_min[3], ndc_max[3];
    uint16_t item_q;
    if (!project_occludee(view_proj, world_min, world_max, ndc_min, ndc_max, &item_q))
        return false;

    int px0 = (int)floorf((ndc_min[0] * 0.5f + 0.5f) * (float)OCC_REF_W);
    int px1 = (int)ceilf((ndc_max[0] * 0.5f + 0.5f) * (float)OCC_REF_W);
    int py0 = (int)floorf((ndc_min[1] * 0.5f + 0.5f) * (float)OCC_REF_H);
    int py1 = (int)ceilf((ndc_max[1] * 0.5f + 0.5f) * (float)OCC_REF_H);
    if (px0 < 0)
        px0 = 0;
    if (py0 < 0)
        py0 = 0;
    if (px1 > OCC_REF_W)
        px1 = OCC_REF_W;
    if (py1 > OCC_REF_H)
        py1 = OCC_REF_H;
    if (px0 >= px1 || py0 >= py1)
        return false;
    for (int y = py0; y < py1; ++y) {
        const uint16_t* row = depth + (size_t)y * OCC_REF_W;
        for (int x = px0; x < px1; ++x)
            if (row[x] == OCCLUSION_DEPTH_EMPTY || item_q < row[x])
                return false;
    }
    return true;
}

// The interior contract, per flagged item: the box may never claim coverage
// the triangles do not back. Two violation kinds, BOTH judged against the
// pixel's 3x3 mesh neighbourhood rather than the pixel alone, because the
// conservative fill leaves a seam along each quad's diagonal that a box face
// does not have -- a seam pixel reads as uncovered, or as covered by a farther
// face showing through the slit, and either way its neighbours carry the true
// surface. `poke` is a box pixel whose whole neighbourhood is EMPTY (a real
// poke-out is a region; a seam is a line). `near` is a box pixel nearer than
// the NEAREST depth anywhere in the neighbourhood, past quantisation slack.
static void probe_proxies(OcclusionContext* context, const DrawList* list) {
    const size_t ref_px = sizeof(uint16_t) * OCC_REF_W * OCC_REF_H;
    uint16_t* box_buf = malloc(ref_px);
    uint16_t* mesh_buf = malloc(ref_px);
    if (!box_buf || !mesh_buf) {
        free(box_buf);
        free(mesh_buf);
        return;
    }
    for (size_t i = 0; i < list->count; ++i) {
        const DrawItem* item = &list->items[i];
        if (!(item->flags & DRAW_OCCLUDER))
            continue;
        memset(box_buf, 0xFF, ref_px);
        memset(mesh_buf, 0xFF, ref_px);
        occlusion_rasterize_box_into(box_buf, OCC_REF_W, OCC_REF_H, context->view_proj,
                                     context->eye, item->mesh->aabb.min, item->mesh->aabb.max,
                                     item->node->global_transform);
        occlusion_rasterize_mesh_into(mesh_buf, OCC_REF_W, OCC_REF_H, context->view_proj,
                                      item->mesh, item->node->global_transform);
        size_t covered = 0, poke = 0, near_viol = 0;
        for (int y = 0; y < OCC_REF_H; ++y) {
            for (int x = 0; x < OCC_REF_W; ++x) {
                uint16_t b = box_buf[(size_t)y * OCC_REF_W + x];
                if (b == OCCLUSION_DEPTH_EMPTY)
                    continue;
                covered++;
                uint16_t nearest = OCCLUSION_DEPTH_EMPTY;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= OCC_REF_W || ny >= OCC_REF_H)
                            continue;
                        uint16_t m = mesh_buf[(size_t)ny * OCC_REF_W + nx];
                        if (m < nearest)
                            nearest = m;
                    }
                }
                if (nearest == OCCLUSION_DEPTH_EMPTY)
                    poke++;
                else if (b + 4 < nearest)
                    near_viol++;
            }
        }
        printf("occlusion-probe proxy pixels=%zu poke=%zu near=%zu name=%s\n", covered, poke,
               near_viol, item->node->name ? item->node->name : "?");
    }
    free(box_buf);
    free(mesh_buf);
}

// A frustum-distributed world box off the LCG: an NDC point unprojected, given
// a half-size. Deterministic by seed, printed in the header so a failure names
// its reproduction.
static uint32_t lcg_next(uint32_t* state) {
    *state = *state * 1664525u + 1013904223u;
    return *state;
}

static float lcg_unit(uint32_t* state) {
    return (float)(lcg_next(state) >> 8) / 16777216.0f;
}

void occlusion_probe_print(const OcclusionContext* context, struct Scene* scene,
                           const struct CullView* view) {
    if (!context || !scene || !view) {
        printf("occlusion-probe header available=0 reason=nocontext\n");
        return;
    }
    if (context->occluder_count == 0) {
        printf("occlusion-probe header available=0 reason=nooccluders\n");
        return;
    }
    if (!context->active) {
        printf("occlusion-probe header available=0 reason=inactive\n");
        return;
    }
    OcclusionContext* mut = (OcclusionContext*)context;

    uint16_t* ref = malloc(sizeof(uint16_t) * OCC_REF_W * OCC_REF_H);
    if (!ref) {
        printf("occlusion-probe header available=0 reason=alloc\n");
        return;
    }
    memset(ref, 0xFF, sizeof(uint16_t) * OCC_REF_W * OCC_REF_H);
    // Replay the ACCEPTED set -- not a re-gather, which could drift from what
    // the frame actually rasterised.
    for (int i = 0; i < context->occluder_count; ++i) {
        OcclusionBoxRec* rec = &mut->boxes[i];
        occlusion_rasterize_box_into(ref, OCC_REF_W, OCC_REF_H, mut->view_proj, context->eye,
                                     rec->box_min, rec->box_max,
                                     rec->transformed ? rec->transform : NULL);
    }

    DrawList* list = &scene->draw_list;
    printf("occlusion-probe header available=1 w=%d h=%d ref_w=%d ref_h=%d occluders=%d "
           "items=%zu sweep=%d seed=%u\n",
           OCCLUSION_W, OCCLUSION_H, OCC_REF_W, OCC_REF_H, context->occluder_count, list->count,
           OCC_SWEEP_COUNT, OCC_SWEEP_SEED);

    // Scene items: hier is the bit the last frame set; ref is the exact test
    // over the same world box.
    size_t hidden_hier = 0, hidden_ref = 0;
    int violations = 0;
    for (size_t i = 0; i < list->count; ++i) {
        const DrawItem* item = &list->items[i];
        AABB box;
        if (!draw_item_bounds(item, view, &box))
            continue;
        vec3 wmin = {0.0f, 0.0f, 0.0f}, wmax = {0.0f, 0.0f, 0.0f};
        aabb_transform(box.min, box.max, (vec4*)item->node->global_transform, wmin, wmax);
        bool hier = item->occluded != 0;
        bool r = ref_test_aabb(ref, mut->view_proj, wmin, wmax);
        hidden_hier += hier ? 1 : 0;
        hidden_ref += r ? 1 : 0;
        if (hier && !r) {
            violations++;
            if (violations <= OCC_PROBE_VIOLATION_CAP)
                printf("occlusion-probe violation where=item i=%zu name=%s\n", i,
                       item->node->name ? item->node->name : "?");
        }
        printf("occlusion-probe item hier=%d ref=%d name=%s\n", hier ? 1 : 0, r ? 1 : 0,
               item->node->name ? item->node->name : "?");
    }

    probe_proxies(mut, list);

    // The seeded sweep: frustum-distributed boxes neither authored nor drawn,
    // so the two tests are compared where no fixture geometry shaped the
    // answer.
    uint32_t rng = OCC_SWEEP_SEED;
    mat4 inv_vp;
    glm_mat4_inv(mut->view_proj, inv_vp);
    int sweep_ref = 0, sweep_hier = 0, sweep_viol = 0;
    for (int i = 0; i < OCC_SWEEP_COUNT; ++i) {
        // A ray through a uniform NDC point, walked at LINEAR view depth --
        // uniform NDC z piles everything onto the far plane and starves the
        // sweep of hidden boxes, which is a vacuous instrument (measured:
        // 13 hidden of 2048). Two unprojected anchors per ray, lerped.
        float nx = lcg_unit(&rng) * 1.9f - 0.95f;
        float ny = lcg_unit(&rng) * 1.9f - 0.95f;
        float t = 0.02f + lcg_unit(&rng) * 0.9f;
        float u = lcg_unit(&rng);
        // The far anchor is the far PLANE (NDC z exactly 1): perspective z is
        // so nonlinear that NDC 0.9 unprojects barely past the near field, and
        // a sweep anchored there never reaches behind anything (measured: 0
        // hidden of 2048). Lerping world positions between these two anchors is
        // what makes t linear in world depth.
        vec4 near_ndc = {nx, ny, -0.5f, 1.0f};
        vec4 far_ndc = {nx, ny, 1.0f, 1.0f};
        vec4 a4, b4;
        glm_mat4_mulv(inv_vp, near_ndc, a4);
        glm_mat4_mulv(inv_vp, far_ndc, b4);
        if (fabsf(a4[3]) < OCC_W_EPS || fabsf(b4[3]) < OCC_W_EPS)
            continue;
        vec3 a = {a4[0] / a4[3], a4[1] / a4[3], a4[2] / a4[3]};
        vec3 b = {b4[0] / b4[3], b4[1] / b4[3], b4[2] / b4[3]};
        vec3 centre;
        glm_vec3_lerp(a, b, t, centre);
        // Box size grows with distance from the viewer: from the eye under
        // perspective, from the ray's own near anchor under an orthographic
        // camera, whose eye is a direction with no position to measure from.
        float dist = context->eye[3] > 0.0f ? glm_vec3_distance((float*)context->eye, centre)
                                            : glm_vec3_distance(a, centre);
        float half = (0.01f + 0.15f * u) * dist;
        vec3 wmin = {centre[0] - half, centre[1] - half, centre[2] - half};
        vec3 wmax = {centre[0] + half, centre[1] + half, centre[2] + half};
        bool hier = occlusion_test_aabb(context, wmin, wmax);
        bool r = ref_test_aabb(ref, mut->view_proj, wmin, wmax);
        sweep_ref += r ? 1 : 0;
        sweep_hier += hier ? 1 : 0;
        if (hier && !r) {
            sweep_viol++;
            if (sweep_viol <= OCC_PROBE_VIOLATION_CAP)
                printf("occlusion-probe violation where=sweep i=%d min=%.3f,%.3f,%.3f "
                       "max=%.3f,%.3f,%.3f\n",
                       i, (double)wmin[0], (double)wmin[1], (double)wmin[2], (double)wmax[0],
                       (double)wmax[1], (double)wmax[2]);
        }
    }
    printf("occlusion-probe sweep boxes=%d ref_hidden=%d hier_hidden=%d violations=%d "
           "catch=%.4f\n",
           OCC_SWEEP_COUNT, sweep_ref, sweep_hier, sweep_viol,
           sweep_ref > 0 ? (double)sweep_hier / (double)sweep_ref : 0.0);
    printf("occlusion-probe summary items=%zu hidden_hier=%zu hidden_ref=%zu violations=%d\n",
           list->count, hidden_hier, hidden_ref, violations);
    free(ref);
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
