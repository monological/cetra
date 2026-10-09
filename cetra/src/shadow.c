
#include <limits.h>
#include <stdio.h> // shadow_rain_probe prints to stdout, like the other probes
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <GL/glew.h>
#include <cglm/cglm.h>

#include "program.h"
#include "uniform.h"
#include "scene.h"
#include "light.h"
#include "mesh.h"
#include "engine.h"
#include "engine_internal.h"
#include "draw_list.h"
#include "intersect.h"
#include "shadow.h"
#include "ies.h"
#include "rain.h"
#include "profiler.h"
#include "texture.h"
#include "render.h"
#include "animation.h"
#include "shader_hook.h"
#include "wind.h"
#include "util.h"
#include "ext/log.h"

// An FBO for depth alone: no colour buffer to read or draw, or it is incomplete.
static void init_depth_fbo(GLuint* fbo) {
    glGenFramebuffers(1, fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// A depth array texture plus the one FBO that renders into its layers. Both
// shadow arrays are built from here: they differ only in size and layer count.
// glTexImage3D, never glTexStorage3D -- that is GL 4.2 and this targets 4.1,
// the constraint material_texture_array.c writes down at its own allocation.
static void init_depth_array(GLuint* tex, GLuint* fbo, int size, int layers) {
    glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_2D_ARRAY, *tex);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT24, size, size, layers, 0,
                 GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    // A white border reads as "nothing occludes here", so a receiver outside
    // the map's footprint stays lit instead of being shadowed by the edge.
    float border_color[] = {1.0f, 1.0f, 1.0f, 1.0f};
    glTexParameterfv(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_BORDER_COLOR, border_color);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

    init_depth_fbo(fbo);
}

static void free_depth_array(GLuint* tex, GLuint* fbo) {
    gl_delete_texture(tex);
    gl_delete_fbo(fbo);
}

// Drop the moment cascades and everything that only exists to fill them. The
// allocation is lazy and rebuilt on demand, so this is both the teardown and the
// resize path; zeroing the built extents is what makes the rebuild trigger.
//
// The FBO and the quad depend on neither size nor layer count, so they survive a
// resize and are torn down only with the system -- a cascade-count change should
// not re-upload a quad.
static void free_msm_arrays(ShadowSystem* system) {
    gl_delete_texture(&system->msm_array);
    gl_delete_texture(&system->msm_scratch);
    system->msm_allocated_layers = 0;
    system->msm_allocated_size = 0;
    system->msm_built = false;
}

static void free_msm_resources(ShadowSystem* system) {
    free_msm_arrays(system);
    gl_delete_fbo(&system->msm_fbo);
    if (system->msm_quad_vao) {
        glDeleteVertexArrays(1, &system->msm_quad_vao);
        glDeleteBuffers(1, &system->msm_quad_vbo);
        system->msm_quad_vao = 0;
        system->msm_quad_vbo = 0;
    }
}

// The transmittance LAYERS live in shadow_map_array and go with it; only the
// accumulation scratch and its quad are owned here.
static void free_tsm_resources(ShadowSystem* system) {
    gl_delete_texture(&system->tsm_scratch);
    gl_delete_fbo(&system->tsm_scratch_fbo);
    if (system->tsm_quad_vao) {
        glDeleteVertexArrays(1, &system->tsm_quad_vao);
        glDeleteBuffers(1, &system->tsm_quad_vbo);
        system->tsm_quad_vao = 0;
        system->tsm_quad_vbo = 0;
    }
    system->tsm_built = false;
}

// Point the FBO at one layer. False means nothing was bound, so the caller must
// not draw -- the FBO is back to 0 and drawing would land in the default
// framebuffer.
static bool bind_depth_layer(GLuint fbo, GLuint tex, int layer) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, tex, 0, layer);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        log_error("Shadow framebuffer incomplete: 0x%x", status);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return false;
    }
    return true;
}

// Bind one layer and clear it for a depth-only pass.
static bool begin_depth_layer(GLuint fbo, GLuint tex, int layer, int size) {
    if (!bind_depth_layer(fbo, tex, layer))
        return false;
    glViewport(0, 0, size, size);
    glClear(GL_DEPTH_BUFFER_BIT);
    return true;
}

// Bind one tile of a layer and, with `clear`, clear that tile alone, which takes the scissor:
// a clear ignores the viewport, so without it a tile's clear wipes every tile beside it.
// Leaves the scissor on for the draw; end_depth_tile turns it off.
static bool begin_depth_tile(GLuint fbo, GLuint tex, int layer, int x, int y, bool clear) {
    if (!bind_depth_layer(fbo, tex, layer))
        return false;
    glEnable(GL_SCISSOR_TEST);
    glScissor(x, y, SHADOW_TILE_SIZE, SHADOW_TILE_SIZE);
    glViewport(x, y, SHADOW_TILE_SIZE, SHADOW_TILE_SIZE);
    if (clear)
        glClear(GL_DEPTH_BUFFER_BIT);
    return true;
}

static void end_depth_tile(void) {
    glDisable(GL_SCISSOR_TEST);
}

ShadowSystem* create_shadow_system(int default_map_size) {
    ShadowSystem* system = malloc(sizeof(ShadowSystem));
    if (!system) {
        log_error("Failed to allocate shadow system");
        return NULL;
    }
    memset(system, 0, sizeof(ShadowSystem));

    system->default_map_size = default_map_size;
    system->directional_count = 0;
    glm_vec3_zero(system->scene_center);
    system->ortho_size = 2000.0f;
    system->near_plane = 1.0f;
    system->far_plane = 7500.0f;
    system->shadow_map_array = 0;
    system->depth_program = NULL;
    system->initialized = false;
    system->enabled = true;
    system->pcss_enabled = false; // library default off; the app opts in
    system->pcss_softness = 1.0f;
    system->msm_enabled = false; // library default off; the app opts in
    system->msm_size = MSM_DEFAULT_SIZE;
    system->msm_blur = MSM_DEFAULT_BLUR;
    system->msm_bleed = MSM_DEFAULT_BLEED;
    system->shadow_distance = 0.0f; // 0 = the camera far clip; see the header
    system->cascade_lambda = 0.75f;
    system->cascade_count = 1; // library default = classic single map; the app opts in
    system->allocated_cascades = 0;
    system->csm_debug = false;
    for (int i = 0; i < MAX_SHADOW_LIGHTS * SHADOW_CASCADES; i++) {
        glm_mat4_identity(system->cascade_matrices[i]);
        glm_vec4_copy((vec4){1.0f, 0.0f, 1.0f, 1.0f}, system->cascade_params[i]);
    }
    for (int i = 0; i < SHADOW_CASCADES; i++) {
        system->cascade_splits[i] = 0.0f;
    }

    for (int i = 0; i < MAX_PUNCTUAL_SHADOW_LAYERS; i++) {
        glm_mat4_identity(system->punctual_matrices[i]);
    }
    system->rain_layer = -1;
    glm_mat4_identity(system->rain_matrix);
    glm_mat4_identity(system->rain_lookup);
    // Past three times the furthest a candle flame's centroid was measured moving from its
    // mean (about a centimetre, mostly up and down), so a flicker never redraws a face and a
    // light carried across a room does.
    system->tile_tolerance = 0.05f;
    system->tile_new_blocks_per_frame = 2;
    system->tile_store_cells = SHADOW_TILE_STORE_CELLS;
    system->tile_store_first = -1;

    system->shadow_bias = 0.005f;

    return system;
}

void free_shadow_system(ShadowSystem* system) {
    if (!system)
        return;

    free_shadow_map_array(system);
    free_depth_array(&system->punctual_map_array, &system->punctual_fbo);
    free_msm_resources(system);
    free_tsm_resources(system);
    if (system->rain_ask_pbo[0])
        glDeleteBuffers(SHADOW_RAIN_ASK_LATENCY, system->rain_ask_pbo);
    gl_delete_fbo(&system->tile_copy_fbo);
    free(system->caster_order);
    free(system->tile_rank);
    free(system->tile_seen);
    free(system->tile_mover_items);

    free(system);
}

int init_shadow_map_array(ShadowSystem* system) {
    if (!system)
        return -1;

    if (system->shadow_map_array != 0)
        return 0;

    // Layers are count-strided (slot * cascade_count + cascade), sized to the
    // ACTIVE cascade count so the classic single-map default never pays the
    // 3x VRAM of a full 9-layer array; a count change rebuilds the array.
    //
    // The transmittance block (spec 11.26) is appended past the cascades when
    // enabled, which is why the array carries it rather than an array of its
    // own: unit 10 is the only sampler there is for a shadow lookup, and the
    // transmittance must be read alongside the occlusion, not instead of it.
    int layers = MAX_SHADOW_LIGHTS * system->cascade_count;
    if (system->tsm_enabled)
        layers += TSM_SLOTS * system->cascade_count * TSM_PARTS;
    init_depth_array(&system->shadow_map_array, &system->cascade_fbo, system->default_map_size,
                     layers);

    system->allocated_cascades = system->cascade_count;
    system->tsm_allocated = system->tsm_enabled;
    system->initialized = true;
    return 0;
}

void free_shadow_map_array(ShadowSystem* system) {
    if (!system)
        return;

    free_depth_array(&system->shadow_map_array, &system->cascade_fbo);
    system->initialized = false;
}

int shadow_live_punctual_layer(const ShadowSystem* system, const struct Light* light) {
    if (!system || !light || !system->enabled || light->shadow_layer < 0 ||
        light->shadow_layer >= system->punctual_layer_count) {
        return -1;
    }
    return light->shadow_layer;
}

int shadow_live_tile(const ShadowSystem* system, const struct Light* light) {
    if (!system || !light || !system->enabled || light->shadow_tile < 0)
        return -1;
    return light->shadow_tile;
}

bool shadow_light_mapped(const ShadowSystem* system, const struct Light* light) {
    return shadow_live_punctual_layer(system, light) >= 0 || shadow_live_tile(system, light) >= 0;
}

// Largest power-of-two edge the VRAM budget affords for `layers` depth layers.
// Halving the size quarters the cost, so this walks down from the ceiling and
// stops at the first size that fits -- and never below the floor, since a map
// too coarse to resolve a silhouette is not worth rendering at all.
static int punctual_size_for(int layers) {
    for (int size = PUNCTUAL_SHADOW_MAX_SIZE; size > PUNCTUAL_SHADOW_MIN_SIZE; size >>= 1) {
        if ((unsigned)layers * (unsigned)size * (unsigned)size * 4u <= PUNCTUAL_SHADOW_VRAM_BUDGET)
            return size;
    }
    return PUNCTUAL_SHADOW_MIN_SIZE;
}

// The edge the punctual array is built at for `light_layers` per-frame layers.
static int punctual_edge_for(int light_layers) {
    return light_layers > 0 ? punctual_size_for(light_layers) : PUNCTUAL_SHADOW_MIN_SIZE;
}

// Grow the punctual array to hold `layers` maps, `light_layers` of them the
// lights'. Demand-driven, like the cascade array: a spot-only scene builds one
// layer rather than the pool ceiling, since every allocated layer is a scene
// traversal per frame.
//
// The size is part of what "grow" means here. A scene that gains a point light
// goes from one layer to seven, and seven layers do not fit at the edge one
// affords -- so the rebuild is triggered by EITHER a larger layer count or a
// size the budget no longer allows, not by the count alone.
//
// The edge comes from the LIGHT layers only. The rain's layer draws into a fixed
// corner, so letting it count would halve a lone spot's resolution the moment it
// started raining; with no light layers at all the edge is the minimum, which is
// all the rain's corner needs.
//
// The cached tiles (spec 13.16) ride past both and do not count toward the edge either: a
// tile is the same size at any edge, so the edge decides only how many fit in a layer.
static void tiles_migrate(ShadowSystem* ss, GLuint old_tex, GLuint old_fbo, int old_edge);
static void rain_migrate(ShadowSystem* ss, GLuint old_tex, GLuint old_fbo, int old_edge);
static int tile_reference_count(const ShadowSystem* ss);
static int tile_shading_views(const ShadowSystem* ss);

static int init_punctual_shadow_array(ShadowSystem* system, int layers, int light_layers) {
    if (layers < 1 || layers > PUNCTUAL_ARRAY_LAYERS + (int)PUNCTUAL_TILE_MAX_LAYERS)
        return -1;

    int size = punctual_edge_for(light_layers);
    if (system->punctual_map_array && system->punctual_allocated_layers >= layers &&
        system->punctual_map_size == size) {
        // The array stays. Its tiles stay too unless the region's base has risen under them,
        // which without a new texture to copy into loses them.
        if (system->tile_held_base != system->tile_base_layer) {
            system->tile_generation++;
            system->tile_held_base = system->tile_base_layer;
        }
        return 0;
    }

    GLuint old_tex = system->punctual_map_array, old_fbo = system->punctual_fbo;
    const int old_size = system->punctual_map_size;
    system->punctual_map_array = 0;
    system->punctual_fbo = 0;
    init_depth_array(&system->punctual_map_array, &system->punctual_fbo, size, layers);
    system->punctual_allocated_layers = layers;
    system->punctual_map_size = size;
    tiles_migrate(system, old_tex, old_fbo, old_size);
    rain_migrate(system, old_tex, old_fbo, old_size);
    free_depth_array(&old_tex, &old_fbo);
    system->tile_held_base = system->tile_base_layer;
    // Both costs, stated rather than assumed: the traversals (every light layer is
    // re-rendered each frame) and the VRAM the budget just spent.
    log_info("Punctual shadow array: %d layer(s) at %d^2 (%.0f MB) -- %d light layer(s) redrawn "
             "every frame",
             layers, size, (double)layers * size * size * 4.0 / (1024.0 * 1024.0), light_layers);
    return 0;
}

// The rain's layer: past every light layer, or the first when shadows are off and no light
// holds one.
static int rain_layer_index(const ShadowSystem* system) {
    return system->enabled ? system->punctual_light_layers : 0;
}

// The rain's map fills the corner of its layer, so a lookup's [0,1] has to land in [0,k] of an
// array k times the map's edge: NDC x -> k x + (k - 1), and the same in y; depth is untouched.
// Folds rain_matrix into rain_lookup for the array's edge as it is, and returns k.
static float rain_fold_lookup(ShadowSystem* ss) {
    const float k = (float)RAIN_OCCLUSION_SIZE / (float)ss->punctual_map_size;
    mat4 corner = GLM_MAT4_IDENTITY_INIT;
    corner[0][0] = k;
    corner[1][1] = k;
    corner[3][0] = k - 1.0f;
    corner[3][1] = k - 1.0f;
    glm_mat4_mul(corner, ss->rain_matrix, ss->rain_lookup);
    return k;
}

// Carry the rain's cover across an array rebuild, into the layer the rain takes in the new
// array, folded for its edge. A rebuild inside a capture's depth pass is followed by the
// capture and not by a rain pass -- a capture takes the depth pass alone -- so a cover left
// behind is read from a layer nothing was drawn into, and every probe of a GI sweep is lit
// under a cover nobody drew, which the volume then keeps.
static void rain_migrate(ShadowSystem* ss, GLuint old_tex, GLuint old_fbo, int old_edge) {
    if (ss->rain_layer < 0 || !old_tex || old_edge <= 0)
        return;
    const int layer = rain_layer_index(ss);
    if (layer >= ss->punctual_allocated_layers) {
        ss->rain_layer = -1;
        return;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, old_fbo);
    glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, old_tex, 0, ss->rain_layer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, ss->punctual_fbo);
    glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, ss->punctual_map_array, 0,
                              layer);
    glBlitFramebuffer(0, 0, RAIN_OCCLUSION_SIZE, RAIN_OCCLUSION_SIZE, 0, 0, RAIN_OCCLUSION_SIZE,
                      RAIN_OCCLUSION_SIZE, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    ss->rain_layer = layer;
    // Its spacing in uv goes as the corner does.
    const float scale = (float)old_edge / (float)ss->punctual_map_size;
    rain_fold_lookup(ss);
    ss->rain_uv_per_metre *= scale;
    ss->rain_cover_spread *= scale;
}

// The punctual array's capacity this frame, which the shadow pass and the rain pass both ask
// for. Two counts would rebuild the array between the passes on every frame it rains, wiping
// the lights' maps -- and every kept tile with them.
static int punctual_capacity(const ShadowSystem* system, const Scene* scene) {
    const int lights_and_rain = rain_layer_index(system) + (rain_active(scene->rain) ? 1 : 0);
    const int tiles = system->tile_layers > 0 ? system->tile_base_layer + system->tile_layers : 0;
    return tiles > lights_and_rain ? tiles : lights_and_rain;
}

static bool begin_punctual_shadow_pass(ShadowSystem* system, int layer) {
    if (layer < 0 || layer >= system->punctual_allocated_layers)
        return false;
    return begin_depth_layer(system->punctual_fbo, system->punctual_map_array, layer,
                             system->punctual_map_size);
}

// caster_index is a LAYER index (slot * cascade_count + cascade)
void begin_shadow_pass(ShadowSystem* system, size_t caster_index) {
    if (!system)
        return;

    if (!system->initialized) {
        if (init_shadow_map_array(system) != 0)
            return;
    }

    // Bound by the array's ALLOCATED layer capacity, not the compile-time
    // ceiling -- a stale layer index should fail here, not at the driver
    if (caster_index >= (size_t)MAX_SHADOW_LIGHTS * (size_t)system->allocated_cascades)
        return;

    begin_depth_layer(system->cascade_fbo, system->shadow_map_array, (int)caster_index,
                      system->default_map_size);
}

void end_shadow_pass(ShadowSystem* system) {
    (void)system;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// Basis up-vector for a light view: world up, unless the light looks along
// it. The threshold and fallback axis define the light-space orientation --
// both fit paths MUST share them or the basis flips with cascade count.
static void light_space_up(const vec3 light_dir, vec3 up) {
    up[0] = 0.0f;
    up[1] = 1.0f;
    up[2] = 0.0f;
    if (fabsf(glm_vec3_dot((float*)light_dir, up)) > 0.99f) {
        up[0] = 1.0f;
        up[1] = 0.0f;
    }
}

void shadow_system_shift_origin(ShadowSystem* system, const vec3 delta) {
    if (!system)
        return;
    glm_vec3_sub(system->scene_center, (float*)delta, system->scene_center);
    // A cached face is drawn about its view, and every view is placed from the body's centre,
    // so it moves with the world unredrawn.
    for (int b = 0; b < system->tile_block_count; ++b) {
        ShadowTileBlock* block = &system->tile_blocks[b];
        glm_vec3_sub(block->centre, (float*)delta, block->centre);
    }
    // And so has every caster in them: an item that goes marks where it was drawn, which is now
    // here. The shift moves no node by its own motion, so nothing else would move these.
    for (size_t i = 0; i < system->tile_seen_count; ++i) {
        AABB* box = &system->tile_seen[i].box;
        glm_vec3_sub(box->min, (float*)delta, box->min);
        glm_vec3_sub(box->max, (float*)delta, box->max);
    }
}

void compute_directional_light_space_matrix(vec3 direction, vec3 scene_center, float ortho_size,
                                            float near_plane, float far_plane, mat4 dest) {
    vec3 light_dir;
    glm_vec3_normalize_to(direction, light_dir);

    vec3 light_pos;
    glm_vec3_scale(light_dir, -far_plane * 0.5f, light_pos);
    glm_vec3_add(light_pos, scene_center, light_pos);

    vec3 up = GLM_VEC3_ZERO_INIT;
    light_space_up(light_dir, up);

    mat4 light_view;
    glm_lookat(light_pos, scene_center, up, light_view);

    mat4 light_projection;
    glm_ortho(-ortho_size, ortho_size, -ortho_size, ortho_size, near_plane, far_plane,
              light_projection);

    glm_mat4_mul(light_projection, light_view, dest);
}

void compute_cascade_light_space_matrix(vec3 direction, const CascadeCamera* cam, float slice_near,
                                        float slice_far, float scene_pad, int map_size,
                                        vec3 snap_anchor, mat4 dest, vec4 out_params) {
    vec3 light_dir;
    glm_vec3_normalize_to(direction, light_dir);

    // Bounding sphere of the view slice: center on the view axis at the
    // radius-minimizing depth, radius from the far corners. Depends only on
    // fov/aspect (or the ortho height) and the split depths, never on camera
    // pose -> the box size is constant per cascade and cannot breathe as the
    // camera moves.
    float zc, radius;
    if (cam->ortho_height > 0.0f) {
        // A parallel slice is a box: centre at its depth midpoint, radius its
        // half-diagonal. No field of view enters, and none could make a
        // frustum sphere fit a box.
        float hy = 0.5f * cam->ortho_height;
        float hx = hy * cam->aspect_ratio;
        float hz = 0.5f * (slice_far - slice_near);
        zc = 0.5f * (slice_near + slice_far);
        radius = sqrtf(hx * hx + hy * hy + hz * hz);
    } else {
        float k = tanf(cam->fov_radians * 0.5f);
        float k2 = k * k * (1.0f + cam->aspect_ratio * cam->aspect_ratio);
        zc = 0.5f * (slice_near + slice_far) * (1.0f + k2);
        if (zc > slice_far)
            zc = slice_far;
        float dz = slice_far - zc;
        radius = sqrtf(dz * dz + slice_far * slice_far * k2);
    }

    vec3 center;
    glm_vec3_scale((float*)cam->forward, zc, center);
    glm_vec3_add(center, (float*)cam->position, center);

    vec3 up = GLM_VEC3_ZERO_INIT;
    light_space_up(light_dir, up);

    // Snap the sphere center to shadow-texel increments in light view space:
    // with the diameter constant, the box then slides in whole texels and
    // shadow edges stay put while the camera translates (Valient)
    //
    // Quantised RELATIVE to the anchor, because floorf(x / texel) * texel is
    // only a texel-accurate operation while x is small enough that a float
    // still resolves one. The offset from the anchor is the small quantity
    // here; the anchor's own coordinate never enters the division.
    mat4 snap_view;
    vec3 origin = {0.0f, 0.0f, 0.0f};
    glm_lookat(origin, light_dir, up, snap_view);
    vec3 center_ls;
    vec3 anchor_ls;
    glm_mat4_mulv3(snap_view, center, 1.0f, center_ls);
    glm_mat4_mulv3(snap_view, snap_anchor, 1.0f, anchor_ls);
    float texel = (2.0f * radius) / (float)map_size;
    center_ls[0] = anchor_ls[0] + floorf((center_ls[0] - anchor_ls[0]) / texel) * texel;
    center_ls[1] = anchor_ls[1] + floorf((center_ls[1] - anchor_ls[1]) / texel) * texel;
    mat4 inv_snap;
    glm_mat4_inv(snap_view, inv_snap);
    glm_mat4_mulv3(inv_snap, center_ls, 1.0f, center);

    // Eye pushed back past the slice by the scene pad so tall geometry
    // OUTSIDE the slice but toward the light still casts into it
    float back = radius + scene_pad;
    vec3 eye;
    glm_vec3_scale(light_dir, -back, eye);
    glm_vec3_add(eye, center, eye);
    mat4 light_view;
    glm_lookat(eye, center, up, light_view);

    float ortho_near = 0.1f;
    float ortho_far = back + radius;
    mat4 light_projection;
    glm_ortho(-radius, radius, -radius, radius, ortho_near, ortho_far, light_projection);
    glm_mat4_mul(light_projection, light_view, dest);

    out_params[0] = 2.0f * radius;
    out_params[1] = ortho_near;
    out_params[2] = ortho_far;
}

void shadow_upload_cascade_uniforms(const ShadowSystem* system, UniformManager* u) {
    if (!system || !u)
        return;

    // At count 1 the layer indices and matrices match the classic path
    // exactly (the byte-identity bridge)
    int cc = system->cascade_count;
    uniform_set_int(u, "cascadeCount", cc);
    vec4 splits = {system->cascade_splits[0], system->cascade_splits[1], system->cascade_splits[2],
                   0.0f};
    uniform_set_vec4(u, "cascadeSplits", splits);

    // Used layers are contiguous from element 0 (layer = slot * cc + c,
    // slots compact), so the per-layer arrays upload as one ranged call
    GLsizei layers = (GLsizei)(system->directional_count * (size_t)cc);
    if (layers <= 0)
        return;
    GLint loc = uniform_location(u, "lightSpaceMatrix[0]");
    if (loc >= 0)
        glUniformMatrix4fv(loc, layers, GL_FALSE, (const GLfloat*)system->cascade_matrices);
    loc = uniform_location(u, "cascadeParams[0]");
    if (loc >= 0)
        glUniform4fv(loc, layers, (const GLfloat*)system->cascade_params);
}

/*
 * The CSM_OUTERMOST_PCF subset, on a caller-chosen unit.
 *
 * csm.glsl says its names "are a contract with ONE C function", and that was true until
 * three consumers needed only the widest cascade and two of them hand-rolled it at the call
 * site rather than take bind_shadow_maps_to_program's punctual, PCSS and MSM state they
 * have no uniforms for. Both copies then drifted the same way: neither uploaded msmEnabled
 * or tsmEnabled, so under --msm they read the depth array while every other surface reads
 * moments, and under --translucent-shadows csmTransmittance short-circuits to 1.0 and a
 * translucent caster casts NOTHING -- which csm.glsl itself calls out as worse than the
 * feature being off.
 *
 * The unit is the parameter because it is the only thing those callers actually needed:
 * water carries cascadePrev1 on SHADOW_MAP_TEXTURE_UNIT, and two sampler TYPES against one
 * image unit is an INVALID_OPERATION at draw.
 *
 * Returns whether anything casts, so the caller publishes its own "no slot" from the same
 * expression that decided the binding rather than from a second one beside it.
 */
bool bind_outermost_cascades_to_program(const ShadowSystem* system, ShaderProgram* program,
                                        int unit) {
    if (!system || !program || !program->uniforms)
        return false;

    UniformManager* u = program->uniforms;
    const bool on = system->enabled;
    const bool directional_on = on && system->directional_count > 0 && system->shadow_map_array;
    // msm_built and tsm_built, never the _enabled flags -- the flag can be on with nothing
    // resolved, and the lookup has to keep working then. Same reasoning as the full binder.
    const bool msm_on = on && system->msm_built;

    // Bound even when nothing casts: a sampler2DArray must resolve to something for the
    // program to be complete.
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D_ARRAY, msm_on ? system->msm_array : system->shadow_map_array);
    uniform_set_int(u, "shadowMaps", unit);
    uniform_set_int(u, "msmEnabled", msm_on ? 1 : 0);
    uniform_set_int(u, "tsmEnabled", (on && system->tsm_built) ? 1 : 0);
    uniform_set_float(u, "msmBleed", system->msm_bleed);
    uniform_set_int(u, "numShadowLights", directional_on ? (int)system->directional_count : 0);
    if (!directional_on)
        return false; // nothing below is read at count 0

    const float texel_size = 1.0f / (float)system->default_map_size;
    uniform_set_vec2(u, "shadowTexelSize", (vec2){texel_size, texel_size});
    uniform_set_float(u, "shadowBias", system->shadow_bias);
    shadow_upload_cascade_uniforms(system, u);
    return true;
}

// The rain's cover rides the punctual array (spec 13.9), and is NOT gated on `enabled`:
// switching shadows off does not put a roof over the street.
static void upload_rain_cover(const ShadowSystem* system, UniformManager* u) {
    uniform_set_int(u, "rainOcclusionLayer", system->rain_layer);
    if (system->rain_layer >= 0) {
        uniform_set_mat4(u, "rainOcclusionMatrix", (const float*)system->rain_lookup);
        uniform_set_float(u, "rainCoverSpread", system->rain_cover_spread);
        uniform_set_float(u, "rainUvPerMetre", system->rain_uv_per_metre);
    }
}

void shadow_bind_rain_cover(const ShadowSystem* system, ShaderProgram* program, int unit) {
    if (!program || !program->uniforms)
        return;
    // Pointed at its unit either way, or the sampler sits on unit 0 beside whatever 2D
    // texture the program keeps there.
    uniform_set_int(program->uniforms, "punctualShadowMaps", unit);
    if (!system) {
        uniform_set_int(program->uniforms, "rainOcclusionLayer", -1);
        return;
    }
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D_ARRAY, system->punctual_map_array);
    glActiveTexture(GL_TEXTURE0);
    upload_rain_cover(system, program->uniforms);
}

// Bind whatever this frame's depth pass produced. Call UNCONDITIONALLY: every
// per-light-type gate lives here, so a caller never has to know which types can
// cast. That is deliberate. The gate used to sit at the call site, testing a
// field then named `active_count` -- which counts DIRECTIONAL casters only, a
// fact the name hid. A spot-lit scene with no directional light never reached
// this function, so its map was rendered and never sampled; and turning shadows
// off left spotShadowActive and a stale depth texture bound from the frame
// before. Both were one condition at one call site trying to model four light
// types. Point and area shadows add their own clauses HERE and no caller
// changes.
void bind_shadow_maps_to_program(ShadowSystem* system, ShaderProgram* program) {
    if (!system || !program || !program->uniforms)
        return;

    UniformManager* u = program->uniforms;
    const bool on = system->enabled;
    const bool directional_on = on && system->directional_count > 0;
    const int punctual_on = on ? system->punctual_layer_count : 0;

    // Unit 10 carries the moment cascades when this frame's resolve produced
    // them and the depth cascades otherwise -- MSM replaces what is bound here
    // rather than adding a sampler, which is why it needs none (spec 11.22).
    // Gated on msm_built, not msm_enabled: the flag can be on with nothing
    // resolved (no directional caster, a failed allocation, a bake), and the
    // lookup has to keep working in all three.
    const bool msm_on = on && system->msm_built;
    // The array texture binds even when nothing casts: a sampler2DArray must
    // resolve to something for the program to be complete.
    glActiveTexture(GL_TEXTURE0 + SHADOW_MAP_TEXTURE_UNIT);
    glBindTexture(GL_TEXTURE_2D_ARRAY, msm_on ? system->msm_array : system->shadow_map_array);
    uniform_set_int(u, "shadowMaps", SHADOW_MAP_TEXTURE_UNIT);
    // Ahead of the numShadowLights early return below, deliberately: that return
    // fires whenever no directional light casts, and leaving this behind it
    // would strand the previous frame's value in exactly the scenes that have no
    // cascades to overwrite it.
    uniform_set_int(u, "msmEnabled", msm_on ? 1 : 0);
    // tsm_built, never tsm_enabled: with the flag on but nothing resolved the
    // lookup must read occlusion alone. Uploaded HERE, above the no-directional
    // -caster early return below, for the reason msmEnabled is -- a scene with
    // no cascade caster would otherwise keep a stale 1 and sample layers that
    // were never written.
    uniform_set_int(u, "tsmEnabled", (on && system->tsm_built) ? 1 : 0);
    uniform_set_float(u, "msmBleed", system->msm_bleed);

    // Punctual (perspective) shadow maps on the last unit, so a shadow-casting
    // spot occludes surfaces (e.g. the ball's shadow on the floor). Bound
    // unconditionally for the same reason as the cascade array; the layer count
    // is what decides whether any layer is read, and each light's own base layer
    // rides in its cluster UBO entry.
    glActiveTexture(GL_TEXTURE0 + PUNCTUAL_SHADOW_MAP_TEXTURE_UNIT);
    glBindTexture(GL_TEXTURE_2D_ARRAY, system->punctual_map_array);
    uniform_set_int(u, "punctualShadowMaps", PUNCTUAL_SHADOW_MAP_TEXTURE_UNIT);
    // How many views every cached light with a body was drawn from, and whether they are the
    // kept ones -- read where the body has moved them and blurred to meet -- or the reference's,
    // drawn from the body as it is and exact by their count.
    uniform_set_int(u, "tileViewCount", tile_shading_views(system));
    uniform_set_int(u, "tileViewsKept", tile_reference_count(system) > 0 ? 0 : 1);
    uniform_set_int(u, "punctualShadowCount", punctual_on);
    if (punctual_on > 0) {
        GLint ploc = uniform_location(u, "punctualShadowMatrix[0]");
        if (ploc >= 0)
            glUniformMatrix4fv(ploc, punctual_on, GL_FALSE,
                               (const GLfloat*)system->punctual_matrices);
        uniform_set_float(u, "punctualShadowMapSize", (float)system->punctual_map_size);
    }
    upload_rain_cover(system, u);
    // The kernel rotation, which a cached light's soft edge reads too (spec 13.16), so it is
    // set ahead of the directional early return like the two flags above.
    uniform_set_int(u, "pcssStochastic", system->pcss_stochastic ? 1 : 0);
    uniform_set_int(u, "pcssFrameIndex", system->pcss_frame_index);

    uniform_set_int(u, "numShadowLights", directional_on ? (int)system->directional_count : 0);
    if (!directional_on)
        return; // nothing below is read at count 0

    float texel_size = 1.0f / (float)system->default_map_size;
    uniform_set_vec2(u, "shadowTexelSize", (vec2){texel_size, texel_size});

    // Scalar shadow uniforms are shared across casters; set once
    uniform_set_float(u, "shadowBias", system->shadow_bias);

    // PCSS controls; the per-cascade ortho geometry rides in cascadeParams
    // The exclusion lives here, at the one place that already knows whether the
    // moment path is live. The app used to clear pcss_enabled instead, which
    // was lossy: turning moments off again in the GUI left the field false, so
    // the user got plain PCF back rather than the PCSS they started with.
    uniform_set_int(u, "pcssEnabled", (system->pcss_enabled && !msm_on) ? 1 : 0);
    uniform_set_float(u, "pcssSoftness", system->pcss_softness);

    uniform_set_int(u, "csmDebug", system->csm_debug ? 1 : 0);
    shadow_upload_cascade_uniforms(system, u);
    // (pbr_frag reads each light's CSM slot from its DirLight UBO entry; the
    // old shadowLightIndex[] loop-order indirection is gone)
}

// Which caster set a traversal draws. OPAQUE is the depth pass exactly as it
// was; OPAQUE_TSM is the same pass with the translucent casters withheld,
// because the transmittance map is live to represent them instead; TRANSLUCENT
// is that map's own pass. Three values rather than a pass plus a bool so the
// "flag off is the path that was there" property is visible at the call site.
// RAIN is the rain's cover: OPAQUE, but over what is DRAWN -- a mesh's shadow role
// says what casts for light, and the rain lands on the surfaces the camera sees,
// not on shapes standing in for them. The cover spans the street, so stand-ins
// cut small for a light's reach would only be more draws to it.
// KEPT is a cached light's face, drawn once (spec 13.16): OPAQUE without glass, and without
// anything whose surface moves under its node -- a skinned, morphing or swaying mesh -- since a
// face drawn once would hold that surface wherever it was on that frame; and at level 0 for
// the same reason, since the camera's level is wherever the camera was. A mesh swaying under no
// wind is not moving, and one whose material holds it at rest is still on purpose (spec 13.26):
// both are kept, the second drawn at rest in every kept set, so the face holds the rest pose
// rather than whichever frame it was drawn on. Glass casts nothing because a pane passing nearly
// all the light, drawn solid, puts what stands behind it in full shadow -- a clock's dial behind
// its door -- and the tiles have no transmittance map to say otherwise. KEPT_STILL and KEPT_MOVERS
// split a face drawn every frame, as a copy of its still casters with the movers over it; a
// surface that moves under its node is a mover on every frame (spec 13.18).
typedef enum ShadowCasterSet {
    SHADOW_CASTERS_OPAQUE = 0,
    SHADOW_CASTERS_OPAQUE_TSM,
    SHADOW_CASTERS_TRANSLUCENT,
    SHADOW_CASTERS_RAIN,
    SHADOW_CASTERS_KEPT,
    SHADOW_CASTERS_KEPT_STILL,
    SHADOW_CASTERS_KEPT_MOVERS,
} ShadowCasterSet;

static bool caster_set_kept(ShadowCasterSet set) {
    return set == SHADOW_CASTERS_KEPT || set == SHADOW_CASTERS_KEPT_STILL ||
           set == SHADOW_CASTERS_KEPT_MOVERS;
}

// The kept sets nothing in sways: every caster in them is DRAW_KEPT_STILL, so none moves under
// this scene's wind but one its material holds at rest, which a kept set draws at rest.
static bool caster_set_at_rest(ShadowCasterSet set) {
    return set == SHADOW_CASTERS_KEPT || set == SHADOW_CASTERS_KEPT_STILL;
}

// The wind response a caster is drawn with in `set`: its material's, but none in a kept set for
// a material held at rest in cached shadows (spec 13.26) -- in the overlay too, so a node of
// it that moves is drawn in the pose its store will hold once the node is still.
static float caster_wind_response(ShadowCasterSet set, const Material* mat) {
    return caster_set_kept(set) && mat->cached_shadow_wind == CACHED_SHADOW_WIND_REST
               ? 0.0f
               : mat->wind_response;
}

// Everything about a caster that its MATERIAL decides, for whichever shadow
// program is bound -- the depth or the absorb program, or a hook's variant of
// either. Every uniform here is location-guarded, so the ones belonging to the
// absorb program no-op on the depth program and the two traversals stay one
// function.
static void _upload_shadow_material(UniformManager* u, const Material* mat, bool foliage) {
    uniform_set_int(u, "alphaTested", foliage ? 1 : 0);
    if (foliage) {
        uniform_set_float(u, "alphaCutoff", mat->alphaCutoff);
        // The same UV transform the surface is shaded with. Without it the
        // cutout was sampled at raw TexCoords, so a caster carrying a texture
        // transform cast a shadow of the wrong shape (spec 11.31).
        uniform_set_vec2(u, "uvOffset", (const float*)&mat->uvOffset);
        uniform_set_vec2(u, "uvScale", (const float*)&mat->uvScale);
        uniform_set_float(u, "uvRotation", mat->uvRotation);
    }

    // Transmission scales the coverage: a fully transmissive interface blocks
    // nothing.
    float opacity = mat->opacity;
    if (mat->transmission > 0.0f)
        opacity *= (1.0f - mat->transmission);
    uniform_set_float(u, "tsmOpacity", opacity);
    uniform_set_int(u, "tsmHasAlbedo", mat->albedo_tex ? 1 : 0);

    // Bound whenever one exists, not just for foliage: a sampler left pointing
    // at an empty unit makes some drivers warn even though this shader only
    // reads it under alphaTested.
    if (mat->albedo_tex) {
        uniform_set_int(u, "albedoTex", 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, mat->albedo_tex->id);
    }

    // Wind must displace the caster exactly as the shading pass displaces the
    // surface, or the shadow detaches from what casts it. Its response is set per
    // draw, by caster_wind_response.
    uniform_set_int(u, "uWindMode", mat->wind_mode);
    uniform_set_float(u, "uWindFlutter", mat->wind_flutter);
    // A surface hook's own uniforms (spec 13.29), which its offset and its alpha read here as
    // they do in the shading pass.
    shader_params_upload(&mat->shader_params, u);
}

// Whether this caster set wants this item, from flags the list settled at build.
// Alpha-masked geometry casts NOTHING on the depth path -- at map-texel scale
// hair strands resolve as card-shaped streaks or strand-scale acne either way,
// and their occlusion comes from AO instead. Foliage opts back in, because leaf
// cards are centimetres across and an alpha test resolves them.
static bool caster_set_wants(ShadowCasterSet set, uint8_t lane, uint8_t flags) {
    // The lanes a set takes, named, as the camera passes name theirs, so a lane added later casts
    // nothing until somebody says it should -- the late draw's casts nothing (spec 13.29). A
    // shadow-only mesh stops a light and not the rain.
    const unsigned rain_lanes =
        (1u << DRAW_LANE_OPAQUE) | (1u << DRAW_LANE_BLEND) | (1u << DRAW_LANE_TRANSMISSIVE);
    const unsigned lanes =
        set == SHADOW_CASTERS_RAIN ? rain_lanes : rain_lanes | (1u << DRAW_LANE_SHADOW_ONLY);
    if (!(lanes & (1u << lane)))
        return false;
    if (set != SHADOW_CASTERS_RAIN && (flags & DRAW_NO_CAST))
        return false;
    bool masked_only = (flags & DRAW_ALPHA_MASKED) && !(flags & DRAW_FOLIAGE);
    // What a transmittance map represents instead of a depth map: geometry that
    // casts NOTHING on the depth path (masked without foliage) or casts SOLID
    // where it should not (blend, transmission). A shadow-only mesh casts as
    // opaque, whatever material it was given to be drawable at all.
    bool translucent = masked_only || lane == DRAW_LANE_BLEND || lane == DRAW_LANE_TRANSMISSIVE;

    if (set == SHADOW_CASTERS_TRANSLUCENT)
        return translucent;
    if (masked_only)
        return false;
    if (caster_set_kept(set) && translucent)
        return false;
    // Blend and transmission leave the depth pass ONLY when a transmittance map
    // will receive them. With the flag off this never fires and the depth
    // rendered is the depth that was rendered before, bit for bit.
    return !(set == SHADOW_CASTERS_OPAQUE_TSM && translucent);
}

// Whether a node has moved lately, so a kept face draws it over a copy of its still casters.
static bool tile_node_moves(const ShadowSystem* ss, const SceneNode* node) {
    for (int k = 0; k < ss->tile_mover_count; ++k) {
        if (ss->tile_movers[k] == node->serial)
            return true;
    }
    return false;
}

// Which kept set takes a caster, by how it moves; every other set takes it whatever it does. A
// face drawn whole and kept takes what stands where its node puts it (DRAW_KEPT_STILL, which a
// capture_hidden node is not: a capture's own depth pass draws the face without it, so it is
// drawn over the copy every frame like a mover). Split for a face drawn every frame, the store
// takes what has not moved lately and the overlay the rest -- with every pose, so a pose is only
// ever drawn into a face that is drawn again the next frame.
static bool caster_set_wants_motion(const ShadowSystem* ss, ShadowCasterSet set,
                                    const DrawItem* item) {
    const bool still = (item->flags & DRAW_KEPT_STILL) != 0;
    switch (set) {
        case SHADOW_CASTERS_KEPT:
            return still;
        case SHADOW_CASTERS_KEPT_STILL:
            return still && !tile_node_moves(ss, item->node);
        case SHADOW_CASTERS_KEPT_MOVERS:
            return !still || tile_node_moves(ss, item->node);
        default:
            return true;
    }
}

// Whether `set` draws `item`, by its lane and flags and by how it moves.
static bool caster_set_takes(const ShadowSystem* ss, ShadowCasterSet set, const DrawItem* item) {
    return caster_set_wants(set, item->lane, item->flags) && caster_set_wants_motion(ss, set, item);
}

// Whether two items belong in one span: everything draw_run_key_equal wants
// EXCEPT the level, which the bucket pass sorts within a span rather than
// splitting on. Derived from that function by construction -- temporarily equal
// levels, ask, restore -- so a component added to the key reaches here without
// anyone remembering to come and add it.
static bool _caster_span_member(const DrawItem* head, const DrawItem* next) {
    DrawItem probe = *next;
    probe.lod = head->lod;
    return draw_run_key_equal(head, &probe);
}

// An item's level, clamped into the bucket range.
//
// select_lod already bounds it and the field is unsigned, so this cannot fire
// today -- but the counting pass INDEXES on this value where the code it
// replaced merely compared it, so an out-of-range level goes from a no-op to a
// write past the bucket array. Clamped rather than asserted because this file
// has no runtime asserts and the failure it prevents is a smashed stack.
//
// What a clamp costs if it ever does fire, stated so it is not mistaken for
// free: the item buckets with real level 3 while _ordered_caster_run still
// compares the raw level, so the run breaks anyway. Fragmented runs and no
// signal -- a slower frame, not a wrong one.
static inline int _caster_level(const DrawItem* item) {
    unsigned level = item->lod;
    return level < CETRA_LOD_MAX ? (int)level : CETRA_LOD_MAX - 1;
}

// The casters this layer wants, compacted out of the draw list and reordered so
// they batch. Returns how many, into ss->caster_order.
//
// The camera path forms runs over CONTIGUOUS list entries, so one culled item or
// one LOD change ends a batch. That rule is load-bearing there -- the opaque lane
// is depth-sorted and the late lanes are order-dependent -- and it is not here:
// a depth map resolves by comparison, so the order casters arrive in cannot
// change it.
//
// TWO OF THE THREE CALLERS ARE DEPTH. The third is the TSM absorbance walk,
// which runs with the depth test off and additive blending into fp32, so what
// holds there is commutativity and not exact reassociation -- reordering moves
// the last bits of a sum, in the texels more than one translucent caster
// reaches. That is accepted, not overlooked. Anything reordered here on a
// stronger premise than "commutative up to fp32 rounding" has to answer for
// that pass first.
//
// Two steps, and neither needs a comparison sort. COMPACT drops everything the
// layer does not want, so a rejected caster no longer splits its neighbours.
// Then GROUP stable-partitions each maximal same-mesh span by LOD level, which
// is where the fragmentation actually lives: levels are chosen from the CAMERA
// (draw_list.h), so instances of one prototype form concentric rings around it
// and every ring boundary crossed in graph order used to end a run, even where
// this layer sees them identically. Measured on apps/forest, the level half is
// 94% of the win and the compaction 6% -- culled items arrive in long runs
// because the scatter is Morton-ordered, so they cost few splits, where the
// rings cut ACROSS that curve and cost thousands.
//
// GROUPING IS CONFINED TO SPANS THE GRAPH ALREADY MADE ADJACENT, which is a
// weaker thing than it sounds and is worth knowing before relying on it: a
// scene that interleaves two prototypes has spans of length one and gets
// nothing from this half. apps/forest buys the adjacency itself by parenting
// every instance under a global per-prototype group. Grouping the whole list by
// (mesh id, level) once at build time would remove that precondition and the
// per-layer repetition with it; it moves zero draws on the one app that has
// the precondition, so it is booked rather than built.
//
// Stable, and by BUCKET rather than by key: a counting pass is linear where a
// sort is not, and it copies indices rather than DrawItems. Mesh pointers are
// compared for EQUALITY here, never for order -- an address is not stable
// across runs and `submit-exact` asserts two runs agree.
static size_t _build_caster_order(ShadowSystem* ss, const DrawList* list, ShadowCasterSet set,
                                  bool group, const CullView* cull, SubmitStats* stats) {
    // Twice the list: the upper half is the counting pass's output buffer. One
    // allocation and one count rather than a second of each.
    size_t want = list->count * 2;
    if (want > ss->caster_order_alloc) {
        size_t* grown = realloc(ss->caster_order, want * sizeof(size_t));
        if (!grown) {
            // Returning 0 alone would draw an empty map into a layer that was
            // just cleared to "nothing occludes" -- a fully lit cascade, which
            // renders as a plausible frame. Say so instead.
            log_error("Shadow: could not size the caster order to %zu; layer unshadowed", want);
            return 0;
        }
        ss->caster_order = grown;
        ss->caster_order_alloc = want;
    }

    // The movers a face draws over its copy, in list order, where this pass has listed them;
    // every other set, and a pass that has not, looks at the whole list.
    const bool listed = set == SHADOW_CASTERS_KEPT_MOVERS && ss->tile_mover_list == list;
    const size_t scan = listed ? ss->tile_mover_item_count : list->count;
    size_t n = 0;
    for (size_t s = 0; s < scan; ++s) {
        const size_t i = listed ? ss->tile_mover_items[s] : s;
        const DrawItem* item = &list->items[i];
        if (!listed && !caster_set_takes(ss, set, item))
            continue;
        if (stats)
            stats->meshes_seen++;
        if (!draw_item_visible(item, cull)) {
            if (stats)
                stats->meshes_culled++;
            continue;
        }
        ss->caster_order[n++] = i;
    }

    // One same-mesh span at a time: count each level, prefix-sum the counts into
    // bucket heads, scatter, copy back. Linear in the span.
    //
    // The obvious in-place form -- sweep once per level, rotating each match
    // down to the write head -- is NOT linear. Its move count is exactly the
    // INVERSION COUNT of the level sequence, so a span of m costs up to 3m²/8,
    // and clustering the input lowers the constant without touching the order.
    // It survived apps/forest only because that app's spans are ~435 long;
    // merging its twenty prototypes into one would have cost 20x for identical
    // content, which is a perverse price on the thing you do to batch better.
    //
    // Skipped entirely when the caller cannot batch: the order it produces is
    // read only through _ordered_caster_run, and with instancing off every run
    // is one item whatever order they arrive in. That also keeps
    // --no-instancing a single-variable control, which matters because the
    // reorder moves the TSM absorbance sum (see the header).
    if (!group)
        return n;

    // Inside the loop, where n > 0 is established: `caster_order + count` on an
    // empty list is NULL + 0, which nothing here would dereference but which C
    // does not define.
    size_t span_start = 0;
    size_t* scatter = ss->caster_order + list->count;
    while (span_start < n) {
        // A span is items draw_run_key_equal would accept but for their LEVEL,
        // which the bucket pass below is about to sort them by. Spelling it as
        // `.mesh ==` here would put a third copy of that key in the tree -- and
        // this one is the copy that matters, because a key that gains a
        // component while the partition still groups on two produces runs that
        // are merely not maximal. Nothing fails; the pass just gets slower, and
        // submit-exact compares two runs of one build so it cannot see it.
        const DrawItem* first = &list->items[ss->caster_order[span_start]];
        size_t span_end = span_start + 1;
        while (span_end < n && _caster_span_member(first, &list->items[ss->caster_order[span_end]]))
            span_end++;

        size_t span = span_end - span_start;
        if (span > 1) {
            // One past the top level, so the prefix sum below can write the
            // running total for level L into head[L + 1] before shifting.
            size_t head[CETRA_LOD_MAX + 1] = {0};
            for (size_t i = span_start; i < span_end; ++i)
                head[_caster_level(&list->items[ss->caster_order[i]]) + 1]++;
            for (int level = 0; level < CETRA_LOD_MAX; ++level)
                head[level + 1] += head[level];
            for (size_t i = span_start; i < span_end; ++i) {
                size_t idx = ss->caster_order[i];
                scatter[head[_caster_level(&list->items[idx])]++] = idx;
            }
            memcpy(&ss->caster_order[span_start], scatter, span * sizeof(size_t));
        }
        span_start = span_end;
    }
    return n;
}

// How many casters from `first` one draw can carry, over the GROUPED order:
// same geometry, same level, capped at a chunk. Visibility and set membership
// were settled when the order was built, so this asks neither.
static size_t _ordered_caster_run(const DrawList* list, const size_t* order, size_t count,
                                  size_t first) {
    const DrawItem* head = &list->items[order[first]];
    size_t n = 1;
    while (first + n < count && n < UBO_INSTANCE_MAX) {
        const DrawItem* next = &list->items[order[first + n]];
        if (!draw_run_key_equal(head, next))
            break;
        n++;
    }
    return n;
}

// The program a caster is drawn with: the pass's, or its hook's variant of it where the hook
// changes where the caster is or where it is cut (spec 13.29) -- which classify settles as
// DRAW_HOOKED_CASTER. A hook with no such variant, or one that would not build, casts as the
// pass's.
static ShaderProgram* _caster_program(const ShadowSystem* ss, const DrawItem* item, bool absorb) {
    ShaderProgram* base = absorb ? ss->tsm_absorb_program : ss->depth_program;
    if (!(item->flags & DRAW_HOOKED_CASTER))
        return base;
    const ShaderHook* hook = item->mesh->material->shader_hook;
    ShaderProgram* hooked = absorb ? hook->shadow_absorb : hook->shadow_depth;
    return hooked ? hooked : base;
}

// What every caster in one layer shares, onto `program` as the layer first draws through it. The
// displacement inputs carry the frame's one render clock, which the shading pass reads too --
// that is what keeps a swaying caster and its shadow in lockstep rather than merely close.
static void _prime_caster_program(ShaderProgram* program, const Engine* engine, const Scene* scene,
                                  const float* matrix) {
    UniformManager* u = program->uniforms;
    uniform_set_mat4(u, "lightSpaceMatrix", matrix);
    uniform_set_int(u, "albedoTex", 0);
    engine_upload_displacement_uniforms(engine, scene, u);
}

// One layer's casters, through `matrix`, with the depth program or with `absorb` the translucent
// absorb program.
//
// The layer's own volume decides visibility, so a rejected caster contributed
// nothing to it -- the matrix IS the clip volume, and nothing here enables
// GL_DEPTH_CLAMP, so anything the test rejects the rasterizer would have
// clipped. That is what makes the map bit-identical rather than merely close,
// and it holds whatever fit produced the matrix: the padded cascade slices, the
// outermost whole-scene fit, and the punctual faces that have no pad at all.
//
// Enabling depth clamp on this pass -- the usual remedy for a caster between the
// light and the near plane -- would break that, and would do it silently:
// casters the test drops would then have contributed.
static void _draw_shadow_items(ShadowSystem* ss, const DrawList* list, bool absorb,
                               SubmitState* state, ShadowCasterSet set, const CullView* cull,
                               const Engine* engine, const Scene* scene, const float* matrix) {
    if (!ss || !list || !engine)
        return;

    SubmitStats* stats = profiler_submit(engine->profiler);
    InstanceChunk chunk;

    // Loop-invariant, so it is settled once rather than re-asked per draw -- and
    // it is what decides whether the order is worth grouping at all.
    const ShaderProgram* base = absorb ? ss->tsm_absorb_program : ss->depth_program;
    const bool batching = engine->instancing_enabled && engine->instance_ubo && base->instanced;

    size_t count = _build_caster_order(ss, list, set, batching, cull, stats);
    const size_t* order = ss->caster_order;

    // A layer primes each program as it first draws through it, so the tracker must forget the
    // one the last layer left bound: a matrix written to it unbound would land on whatever was,
    // while its cache recorded it as held.
    submit_state_reset(state);
    for (size_t pos = 0; pos < count; ++pos) {
        size_t idx = order[pos];
        const DrawItem* item = &list->items[idx];

        {
            const SceneNode* node = item->node;
            Mesh* mesh = item->mesh;
            Material* mat = mesh->material;
            bool foliage = (item->flags & DRAW_FOLIAGE) != 0;
            ShaderProgram* drawn = _caster_program(ss, item, absorb);
            UniformManager* u = drawn->uniforms;
            if (submit_use_program(state, drawn->id))
                _prime_caster_program(drawn, engine, scene, matrix);

            // The depth stage reads InstanceBlock, so casters batch here even
            // when the same mesh cannot batch on the camera path. A run shares
            // one pose by construction -- the pose is in the run key -- so one
            // upload of bone matrices serves every instance in it. Gated on the
            // program all the same, so this follows the shader rather than
            // restating what it does.
            size_t run = batching ? _ordered_caster_run(list, order, count, pos) : 1;
            // No shading transforms: this stage reads uInstModel and nothing
            // else, so the rest of the block is bytes it cannot look at.
            if (run > 1)
                instance_chunk_upload_ordered(engine->instance_ubo, &chunk, list, order, count, pos,
                                              run, false);

            // Only for a draw carrying one object, for the reason
            // _submit_item's own guard records. Note the grouped order is not
            // node order, so a node holding several meshes no longer uploads
            // this once -- the value cache absorbs the repeat, and the meshes
            // are in different spans by then anyway.
            if (run == 1)
                uniform_set_mat4(u, "model", (const float*)node->global_transform);

            // Everything the material decides, uploaded once per material
            // rather than once per mesh -- a canopy of leaf cards is hundreds
            // of meshes sharing one of these.
            if (submit_take_material(state, mat)) {
                _upload_shadow_material(u, mat, foliage);
                if (stats)
                    stats->material_switches++;
            }

            // Per draw rather than with the material block, which is uploaded once
            // across every set while one material may sway in one set and rest in
            // a kept one.
            uniform_set_float(u, "uWindResponse", caster_wind_response(set, mat));
            // Per mesh, because it is the mesh's own wind range: where along Y the
            // mask ramps from anchored to free.
            wind_upload_mesh(mesh, u);
            // Per mesh for the same reason the shading pass sets it per mesh:
            // whether COLOR_0 exists is geometry, not material. The cutout
            // multiplies it in, so a caster carrying its alpha there rather than
            // in the albedo map used to cast as though it were solid.
            uniform_set_int(u, "vertexColorExists", mesh->colors ? 1 : 0);

            // Skin animated meshes so they cast animated shadows
            render_update_skinning_uniforms(drawn, mesh, item->pose);

            // A two-sided card has no back face, so culling either way would
            // drop it from the map entirely.
            //
            // Excluded on the translucent pass, which runs with culling off
            // for EVERY mesh so a closed volume accumulates both its surfaces.
            // Restoring it here would re-enable culling for the rest of that
            // cascade's traversal, halving the absorbance of every translucent
            // mesh drawn after the first two-sided one -- transmittance would
            // come out as its square root, and which meshes were affected
            // would depend on scene-graph order.
            bool two_sided = (item->flags & DRAW_DOUBLE_SIDED) && set != SHADOW_CASTERS_TRANSLUCENT;
            // The camera's level, not one chosen for this light: see DrawItem. A kept
            // face takes level 0, since the camera's is wherever the camera was.
            DrawItem level0 = *item;
            level0.lod = 0;
            submit_draw_run(state, u, caster_set_kept(set) ? &level0 : item, run, two_sided, stats);
            pos += run - 1;
        }
    }
}

// The fog's volumetric spot: v1 scatters exactly one cone into a beam, and it
// is the first spot in the scene. Only the fog publish selects a light this
// way -- the depth pass gives every shadow-casting spot its own layer -- so
// what this picks is which beam is volumetric, not which spot casts.
static const Light* scene_first_spot_light(const Scene* scene) {
    if (!scene || !scene->lights)
        return NULL;
    for (size_t i = 0; i < scene->light_count; i++) {
        const Light* l = scene->lights[i];
        if (l && l->type == LIGHT_SPOT)
            return l;
    }
    return NULL;
}

// One perspective light-space matrix: eye at `pos`, looking down `dir_in`, rolled so `up` is
// up in the map. All three punctual types reduce to this; only the fov and the axes differ.
static void compute_perspective_light_space_up(const vec3 pos, const vec3 dir_in, const vec3 up,
                                               float fov, float near_plane, float far_plane,
                                               mat4 dest) {
    vec3 dir;
    glm_vec3_normalize_to((float*)dir_in, dir);
    vec3 target;
    glm_vec3_add((float*)pos, dir, target);
    mat4 view, proj;
    glm_lookat((float*)pos, target, (float*)up, view);
    glm_perspective(fov, 1.0f, near_plane, far_plane, proj);
    glm_mat4_mul(proj, view, dest);
}

// The same, rolled to world up, or to +X when it looks along that.
static void compute_perspective_light_space(const vec3 pos, const vec3 dir_in, float fov,
                                            float near_plane, float far_plane, mat4 dest) {
    vec3 dir;
    glm_vec3_normalize_to((float*)dir_in, dir);
    vec3 up = GLM_VEC3_ZERO_INIT;
    light_space_up(dir, up);
    compute_perspective_light_space_up(pos, dir, up, fov, near_plane, far_plane, dest);
}

// A cube's six faces, in the +X -X +Y -Y +Z -Z order that include/cube_face.glsl selects by
// dominant axis, and the up each is rolled to: world up, or +X for the two looking along it,
// light_space_up's choice. For a per-frame map that order is the whole contract, since
// everything else about a face is in its matrix; a cached face's basis is restated in
// include/tile_lookup.glsl, which projects without one.
static const vec3 PUNCTUAL_CUBE_DIR[6] = {{1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f},
                                          {0.0f, 1.0f, 0.0f}, {0.0f, -1.0f, 0.0f},
                                          {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f}};
static const vec3 PUNCTUAL_CUBE_UP[6] = {{0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
                                         {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f},
                                         {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};

// Layers one light needs, 0 for a light the pool does not draw: a point light's cube is six 2D
// faces, a panel's the first five of the same cube in its own frame (spec 13.27) -- the sixth
// would look behind it, where it lights nothing -- and a spot's a single perspective map.
static int punctual_layers_for(const Light* light) {
    switch (light->type) {
        case LIGHT_POINT:
            return 6;
        case LIGHT_AREA:
            return 5;
        case LIGHT_SPOT:
            return 1;
        default:
            return 0;
    }
}

// Fill a light's layers with its light-space matrices, in the layer order its
// consumer selects by, and return how many it wrote (so the render loop bounds
// itself off this rather than re-deriving the point/else split).
//
// All three share the system's own scene-scaled near/far, not a fixed range. A
// hardcoded near of 1.0 puts the whole of any room-sized scene INSIDE the near
// plane -- nothing reaches the map and the light silently stops casting, which
// is not a subtle degradation but a total one. Apps already scale
// near_plane/far_plane off the scene radius for the cascades; a punctual light
// in the same scene has no reason to disagree with them.
static int compute_punctual_matrices(const Light* light, const ShadowSystem* ss,
                                     const IesProfile* profile, mat4* dest) {
    const float near_p = ss->near_plane, far_p = ss->far_plane;

    switch (light->type) {
        case LIGHT_POINT:
        case LIGHT_AREA: {
            // A cube in the light's frame: the world's for a point light, and for a panel the
            // one light_cluster.c ships and ltcPanel shades by, the width axis r = u x n, its up
            // u and its normal n as x, y and z -- the frame panelCubeFace
            // (include/cube_face.glsl) selects in. Every face is rolled within the frame, so its
            // square frustum covers exactly the directions it is chosen for, which world up
            // would not on a panel turned off the world's axes. A single map down a panel's
            // normal reached 60 degrees off it and read everything past that as lit, walls
            // included (spec 10.4). A panel's faces end at its range where it has one, as a
            // cached light's do: it lights nothing past it, and nothing past it can stand
            // between it and what it lights.
            mat3 frame = GLM_MAT3_IDENTITY_INIT;
            float face_far = far_p;
            if (light->type == LIGHT_AREA) {
                light_emission_frame(light, frame[2], frame[1]);
                glm_vec3_cross(frame[1], frame[2], frame[0]);
                if (light->range > 0.0f)
                    face_far = fminf(far_p, light->range);
            }
            const int faces = punctual_layers_for(light);
            for (int f = 0; f < faces; f++) {
                vec3 dir = GLM_VEC3_ZERO_INIT, up = GLM_VEC3_ZERO_INIT;
                glm_mat3_mulv(frame, (float*)PUNCTUAL_CUBE_DIR[f], dir);
                glm_mat3_mulv(frame, (float*)PUNCTUAL_CUBE_UP[f], up);
                compute_perspective_light_space_up(light->global_position, dir, up, glm_rad(90.0f),
                                                   near_p, face_far, dest[f]);
            }
            break;
        }
        default: {
            // A spot's fov is its own cone, plus a margin so the outer edge is not
            // clipped by the frustum it is supposed to fill.
            //
            // A PROFILED spot's cone is dead -- the profile replaced it -- so the
            // frustum is fitted to the profile's angular support instead. Real IES
            // skirts routinely reach past the authored cone, and fitting the cone
            // anyway would leave that skirt lit and unshadowed, which reads as a
            // light passing through walls.
            float half_angle = acosf(light->outerCutOff); // outerCutOff = cos(half-angle)
            if (profile)
                half_angle = glm_rad(profile->support_deg);
            float fov = 2.0f * half_angle * 1.15f;
            if (fov > glm_rad(175.0f))
                fov = glm_rad(175.0f);
            compute_perspective_light_space(light->global_position, light->direction, fov, near_p,
                                            far_p, dest[0]);
            break;
        }
    }

    return punctual_layers_for(light);
}

// The body both depth-pass loops share, once a layer is bound: walk the scene into it through
// one light-space matrix.
static void draw_shadow_layer(ShadowSystem* ss, const Scene* scene, const DrawList* list,
                              mat4 matrix, SubmitState* state, ShadowCasterSet set,
                              const Engine* engine) {
    // The matrix IS the layer's coverage volume, and Gribb-Hartmann does not
    // care whether it is ortho or perspective -- so cascades and punctual faces
    // cull through the same six planes.
    Frustum layer_frustum;
    frustum_extract_from_vp(matrix, &layer_frustum);
    CullView cull = render_cull_view(engine, scene, &layer_frustum);
    // Culled by the box of the pose drawn, which in these sets no wind moves.
    if (caster_set_at_rest(set))
        cull.wind = NULL;
    _draw_shadow_items(ss, list, false, state, set, &cull, engine, scene, (const float*)matrix);
    end_shadow_pass(ss);
}

/*
 * Cached point- and area-light shadows (specs 13.16 and 13.27). A panel is cached as a point
 * light at its centre: six world-axis faces, one view, no body.
 *
 * A cached light's faces are tiles of the punctual array past the per-frame layers and the
 * rain's, each drawn once from where the light was and kept until something says it is
 * wrong: a view moved past tile_tolerance, its planes changed, or the region lost its
 * contents (tile_generation). Cells count from layer 0 across the whole array, row-major
 * within a layer, so a light's first cell is the one number its lookup needs. The edge
 * decides only how many tiles fit in a layer: a tile is the same size at any edge, so what
 * it holds never depends on how many per-frame lights the scene has.
 *
 * A light with a body is drawn from several VIEWS spread over it, not one. One cube map
 * records the nearest surface in each direction from ONE point, so it cannot tell a point that
 * point cannot see from a solid one: under a candle's rim, every point the flame's middle
 * cannot see reads as hidden from the whole flame, though its top sees over the rim. Each view
 * is exact for its own point, and the lookup averages them.
 */

// Whether a light asks to keep its shadow in tiles and is of a type that can: a point light, or
// a panel, cached from its centre over the six world-axis faces a point light's are (spec
// 13.27). Not a spot: its cone cosines carry a cached light's segment.
static bool light_wants_tiles(const Light* light) {
    return light->cast_shadows && light->shadow_cache &&
           (light->type == LIGHT_POINT || light->type == LIGHT_AREA);
}

// Whether a light's shadow goes in tiles rather than the per-frame pool. Its range is where
// its faces end, so a cached light with none stays in the pool.
bool shadow_light_takes_tiles(const Light* light) {
    return light_wants_tiles(light) && light->range > 0.0f;
}

static int tiles_per_layer(int edge) {
    const int per_row = edge / SHADOW_TILE_SIZE;
    return per_row * per_row;
}

// Where a cell is: its layer, and its corner in texels.
static void tile_cell_at(int cell, int edge, int* layer, int* x, int* y) {
    const int per_row = edge / SHADOW_TILE_SIZE;
    const int within = cell % (per_row * per_row);
    *layer = cell / (per_row * per_row);
    *x = (within % per_row) * SHADOW_TILE_SIZE;
    *y = (within / per_row) * SHADOW_TILE_SIZE;
}

static int tile_block_first_cell(const ShadowSystem* ss, int edge, int block) {
    return ss->tile_base_layer * tiles_per_layer(edge) + ss->tile_blocks[block].first;
}

static int tile_store_first_cell(const ShadowSystem* ss, int edge) {
    return ss->tile_base_layer * tiles_per_layer(edge) + ss->tile_store_first;
}

// The reference's view count, 0 when it is off.
static int tile_reference_count(const ShadowSystem* ss) {
    return ss->tile_reference < 0                           ? 0
           : ss->tile_reference > SHADOW_TILE_REFERENCE_MAX ? SHADOW_TILE_REFERENCE_MAX
                                                            : ss->tile_reference;
}

// A panel has none, whatever it carries: its body is its rectangle, which a capsule along its
// normal is not.
static bool light_has_body(const Light* light) {
    return light->type != LIGHT_AREA &&
           (light->source_radius > 0.0f || light->source_length > 0.0f);
}

// The views every light with a body is drawn and shaded from this frame, kept or the
// reference's -- one count for all of them, which the lookup is told rather than packed per
// light.
static int tile_shading_views(const ShadowSystem* ss) {
    const int reference = tile_reference_count(ss);
    if (reference > 0)
        return reference;
    return ss->tile_views > 0 && ss->tile_views < SHADOW_TILE_VIEWS ? ss->tile_views
                                                                    : SHADOW_TILE_VIEWS;
}

// The views a light's block is drawn from: the shading count spread over a body, its centre
// alone for a light with none.
static int tile_views_for(const ShadowSystem* ss, const Light* light) {
    return light_has_body(light) ? tile_shading_views(ss) : 1;
}

// A light's body as it is now: its centre, its segment end to end, its radius.
static void tile_body_now(const Light* light, vec3 centre, vec3 segment, float* radius) {
    const bool body = light_has_body(light);
    glm_vec3_copy((float*)light->global_position, centre);
    glm_vec3_scale((float*)light->direction, body ? light->source_length : 0.0f, segment);
    *radius = body ? light->source_radius : 0.0f;
}

// View m of `count` over a body: the centre for m = 0, else u stratified along the segment and
// a point of the ball from R3. punctual_tiles.glsl's tileBodyPoint places the same points, so
// the lookup finds each view where it was drawn from.
static void tile_body_point(const vec3 centre, const vec3 segment, float radius, int m, int count,
                            vec3 dest) {
    glm_vec3_copy((float*)centre, dest);
    if (m == 0)
        return;
    const float u = ((float)m - 0.5f) / (float)(count - 1);
    const float i = (float)(m - 1);
    const float qx = 0.5f + i * SHADOW_TILE_R3_X, qy = 0.5f + i * SHADOW_TILE_R3_Y,
                qz = 0.5f + i * SHADOW_TILE_R3_Z;
    const float q[3] = {qx - floorf(qx), qy - floorf(qy), qz - floorf(qz)};
    const float rho = radius * cbrtf(q[0]);
    const float cos_t = 1.0f - 2.0f * q[1];
    const float sin_t = sqrtf(fmaxf(1.0f - cos_t * cos_t, 0.0f));
    const float phi = 2.0f * GLM_PIf * q[2];
    const vec3 ball = {sin_t * cosf(phi), cos_t, sin_t * sinf(phi)};
    glm_vec3_muladds((float*)segment, u - 0.5f, dest);
    glm_vec3_muladds((float*)ball, rho, dest);
}

// View v's origin in a block.
static void tile_view_origin(const ShadowTileBlock* block, int view, vec3 dest) {
    tile_body_point(block->centre, block->segment, block->radius, view, block->views, dest);
}

// A cached light's planes: the far is its range, the near what it states or a fraction of
// the range.
static void tile_planes(const Light* light, float* near_plane, float* far_plane) {
    *far_plane = light->range;
    *near_plane = light->shadow_near > 0.0f && light->shadow_near < light->range
                      ? light->shadow_near
                      : SHADOW_TILE_NEAR_RATIO * light->range;
}

static bool tile_face_in(uint64_t mask, int face) {
    return (mask >> face) & 1u;
}

// Every face of `views` views. The reference's may be more than a mask holds, and it marks
// none, so all of them is what it gets.
static uint64_t tile_faces_of(int views) {
    return 6 * views >= 64 ? ~0ull : (1ull << (6 * views)) - 1ull;
}

// Set a block's views over the light's body as it is now, with the light's planes, in the
// region as it is now.
static void tile_block_place(const ShadowSystem* ss, ShadowTileBlock* block, int views) {
    block->views = views;
    // A face fewer views no longer have is drawn by nothing, and would see a mover for good.
    block->dynamic &= tile_faces_of(views);
    tile_body_now(block->light, block->centre, block->segment, &block->radius);
    tile_planes(block->light, &block->near_plane, &block->far_plane);
    block->generation = ss->tile_generation;
}

// Whether every face of every view a block was drawn with is in.
static bool tile_block_whole(const ShadowTileBlock* block) {
    return block->views > 0 && block->valid == tile_faces_of(block->views);
}

// Whether a block's views have drifted past the tolerance from where the light's body would
// place them now -- the light moved, or its body turned or stretched. A light that follows has
// none: the two points come from the same arithmetic on the same inputs, so a light that has not
// moved is exactly where it was drawn.
static bool tile_views_drifted(const ShadowSystem* ss, const ShadowTileBlock* block) {
    const float tolerance = block->light->shadow_follow ? 0.0f : ss->tile_tolerance;
    vec3 centre = GLM_VEC3_ZERO_INIT, segment = GLM_VEC3_ZERO_INIT;
    float radius = 0.0f;
    tile_body_now(block->light, centre, segment, &radius);
    for (int v = 0; v < block->views; ++v) {
        vec3 drawn = GLM_VEC3_ZERO_INIT, now = GLM_VEC3_ZERO_INIT;
        tile_view_origin(block, v, drawn);
        tile_body_point(centre, segment, radius, v, block->views, now);
        if (glm_vec3_distance(drawn, now) > tolerance)
            return true;
    }
    return false;
}

// A cached face's light-space matrix: from `origin` down face `face` (+X -X +Y -Y +Z -Z),
// across the face's 90 degrees plus the guard band each side, over [near, far].
static void shadow_tile_face_matrix(const vec3 origin, int face, float near_plane, float far_plane,
                                    mat4 dest) {
    const float fov = 2.0f * atanf(1.0f / SHADOW_TILE_INNER);
    compute_perspective_light_space_up(origin, PUNCTUAL_CUBE_DIR[face], PUNCTUAL_CUBE_UP[face], fov,
                                       near_plane, far_plane, dest);
}

// Face `face` of a block -- counted across its views, six a view -- from its view's origin
// over the block's planes.
static void tile_block_face_matrix(const ShadowTileBlock* block, int face, mat4 dest) {
    vec3 origin = GLM_VEC3_ZERO_INIT;
    tile_view_origin(block, face / 6, origin);
    shadow_tile_face_matrix(origin, face % 6, block->near_plane, block->far_plane, dest);
}

// A light's block, or -1.
static int tile_block_of(const ShadowSystem* ss, const Light* light) {
    for (int b = 0; b < ss->tile_block_count; ++b) {
        if (ss->tile_blocks[b].light == light)
            return b;
    }
    return -1;
}

// A block holding nothing, at `first` for `cells`.
static ShadowTileBlock tile_block_blank(int first, int cells) {
    ShadowTileBlock block = {.first = first, .cells = cells};
    memset(block.store_cell, -1, sizeof(block.store_cell));
    return block;
}

// Cells the region reaches, which is what sizes it: the blocks and the store pool are only ever
// appended, so whichever came last ends furthest.
static int tile_cells_used(const ShadowSystem* ss) {
    int used = 0;
    if (ss->tile_block_count > 0) {
        const ShadowTileBlock* last = &ss->tile_blocks[ss->tile_block_count - 1];
        used = last->first + last->cells;
    }
    if (ss->tile_store_first >= 0 && ss->tile_store_first + SHADOW_TILE_STORE_CELLS > used)
        used = ss->tile_store_first + SHADOW_TILE_STORE_CELLS;
    return used;
}

// A block no light holds with room for `cells`, the first that fits, else a new one past the
// last; -1 when the lights' budget, which the store pool is on top of, has no room. A freed
// block keeps its cells, so a light that needs no more reuses them where they are.
static int tile_block_take(ShadowSystem* ss, int cells) {
    for (int b = 0; b < ss->tile_block_count; ++b) {
        const ShadowTileBlock* block = &ss->tile_blocks[b];
        if (!block->light && block->cells >= cells)
            return b;
    }
    const int first = tile_cells_used(ss);
    const int pool = ss->tile_store_first >= 0 ? SHADOW_TILE_STORE_CELLS : 0;
    if (ss->tile_block_count >= (int)SHADOW_TILE_MAX_BLOCKS ||
        first - pool + cells > (int)SHADOW_TILE_MAX_CELLS)
        return -1;
    const int b = ss->tile_block_count++;
    ss->tile_blocks[b] = tile_block_blank(first, cells);
    return b;
}

// Give back face `f`'s store cell, which then holds nothing.
static void tile_store_release(ShadowTileBlock* block, int f) {
    block->store_cell[f] = -1;
    block->stored &= ~(1ull << f);
}

// Give a block's cells back, and its store cells, keeping where they are.
static void tile_block_free(ShadowTileBlock* block) {
    *block = tile_block_blank(block->first, block->cells);
}

// How much nearer a light without a block must be than one holding one to take it, in metres:
// a camera between two candles would otherwise trade one block between them on every step, and
// each trade redraws every face.
#define TILE_STREAM_MARGIN 2.0f

// How far a light's reach is from the camera: 0 inside its range.
static float tile_light_distance(const Light* light, const vec3 eye) {
    return fmaxf(glm_vec3_distance((float*)light->global_position, (float*)eye) - light->range,
                 0.0f);
}

static int tile_rank_order(const void* a, const void* b) {
    const ShadowTileRank* x = a;
    const ShadowTileRank* y = b;
    if (x->distance != y->distance)
        return x->distance < y->distance ? -1 : 1;
    return x->order - y->order;
}

// The cached lights, nearest the camera first, ties to scene order, each with the block it
// held last frame. False on out of memory, with nothing ranked.
static bool tiles_rank(ShadowSystem* ss, const Engine* engine, const Scene* scene) {
    ss->tile_rank_count = 0;
    if (!grow_array((void**)&ss->tile_rank, &ss->tile_rank_capacity, scene->light_count,
                    sizeof(ShadowTileRank), 16))
        return false;
    vec3 eye = {0.0f, 0.0f, 0.0f};
    if (engine->camera)
        glm_vec3_copy(engine->camera->position, eye);
    for (size_t i = 0; i < scene->light_count; ++i) {
        Light* light = scene->lights[i];
        if (!light || !shadow_light_takes_tiles(light))
            continue;
        ss->tile_rank[ss->tile_rank_count++] = (ShadowTileRank){
            .light = light,
            .distance = tile_light_distance(light, eye),
            .order = (int)i,
            .block = tile_block_of(ss, light),
        };
    }
    qsort(ss->tile_rank, ss->tile_rank_count, sizeof(ShadowTileRank), tile_rank_order);
    return true;
}

// Steps `*k` through the ranked lights more than the margin farther than the one ranked `r`,
// farthest first, starting from tile_rank_count; false once there are no more. What a nearer
// light may take from them, a block or a store cell, they give up.
static bool tile_rank_farther(const ShadowSystem* ss, size_t r, size_t* k) {
    if (*k <= r + 1)
        return false;
    --*k;
    return ss->tile_rank[*k].distance > ss->tile_rank[r].distance + TILE_STREAM_MARGIN;
}

// The ranked light holding a block with room for `cells` that the light ranked `r` may take it
// from, the farthest, or -1 when none is.
static int tile_rank_to_evict(const ShadowSystem* ss, size_t r, int cells) {
    for (size_t k = ss->tile_rank_count; tile_rank_farther(ss, r, &k);) {
        const ShadowTileRank* far = &ss->tile_rank[k];
        if (far->block >= 0 && ss->tile_blocks[far->block].cells >= cells)
            return (int)k;
    }
    return -1;
}

void shadow_tiles_update(ShadowSystem* ss, const Engine* engine, const Scene* scene) {
    if (!ss || !ss->enabled || !engine || !scene)
        return;
    ss->tile_held = 0;
    if (!tiles_rank(ss, engine, scene))
        return;

    // A light keeps its block by identity from frame to frame, which is what lets its faces be
    // kept, until a light clearly nearer needs it and the budget has no other room. The first
    // frame to place any places every block it can, since that is the load; after it at most
    // tile_new_blocks_per_frame a frame, since a light placed draws all its faces at once.
    const bool opening = ss->tile_block_count == 0;
    int placed = 0;
    // The fewest cells eviction has found no room for: a farther light's limit is only larger,
    // so one needing as many finds none either.
    int unplaceable = INT_MAX;
    bool held[SHADOW_TILE_MAX_BLOCKS] = {false};
    const Light* full = NULL;
    for (size_t r = 0; r < ss->tile_rank_count; ++r) {
        ShadowTileRank* rank = &ss->tile_rank[r];
        // A light whose shape now wants more views than its block holds takes a bigger one.
        const int cells = 6 * tile_views_for(ss, rank->light);
        if (rank->block >= 0 && ss->tile_blocks[rank->block].cells < cells) {
            tile_block_free(&ss->tile_blocks[rank->block]);
            rank->block = -1;
        }
        if (rank->block < 0) {
            if (!opening && ss->tile_new_blocks_per_frame > 0 &&
                placed >= ss->tile_new_blocks_per_frame)
                continue;
            int b = tile_block_take(ss, cells);
            if (b < 0 && cells < unplaceable) {
                const int f = tile_rank_to_evict(ss, r, cells);
                if (f >= 0) {
                    b = ss->tile_rank[f].block;
                    tile_block_free(&ss->tile_blocks[b]);
                    ss->tile_rank[f].block = -1;
                } else {
                    unplaceable = cells;
                }
            }
            if (b < 0) {
                if (!full)
                    full = rank->light;
                continue;
            }
            ss->tile_blocks[b].light = rank->light;
            rank->block = b;
            placed++;
        }
        held[rank->block] = true;
        ss->tile_held++;
    }
    // And the blocks of lights no longer cached.
    for (int b = 0; b < ss->tile_block_count; ++b) {
        if (ss->tile_blocks[b].light && !held[b])
            tile_block_free(&ss->tile_blocks[b]);
    }
    if (full && !ss->tile_full_warned) {
        log_warn("Cached shadow tiles full (%u faces in %u MB): '%s' and any cached light "
                 "farther from the camera will not cast",
                 (unsigned)SHADOW_TILE_MAX_CELLS, PUNCTUAL_TILE_VRAM_BUDGET / (1024u * 1024u),
                 full->name ? full->name : "unnamed light");
    }
    ss->tile_full_warned = full != NULL;
}

void shadow_tiles_stream_print(const ShadowSystem* ss, int frame) {
    if (!ss)
        return;
    printf("stream tiles frame=%d lights=%zu\n", frame, ss->tile_rank_count);
    for (size_t r = 0; r < ss->tile_rank_count; ++r) {
        const ShadowTileRank* rank = &ss->tile_rank[r];
        printf("stream tile rank=%zu light=%s dist=%.2f block=%d whole=%d\n", r,
               rank->light->name ? rank->light->name : "unnamed", (double)rank->distance,
               rank->block,
               rank->block >= 0 && tile_block_whole(&ss->tile_blocks[rank->block]) ? 1 : 0);
    }
    fflush(stdout);
}

bool shadow_tiles_cover(const ShadowSystem* ss, const AABB* box) {
    if (!ss || !ss->enabled)
        return true;
    for (size_t r = 0; r < ss->tile_rank_count; ++r) {
        const ShadowTileRank* rank = &ss->tile_rank[r];
        const Light* light = rank->light;
        if (aabb_dist_sq(box, light->global_position) > light->range * light->range)
            continue;
        // A held block of a light that emits is drawn whole by the capture's own depth pass;
        // one that does not emit yet is drawn when it does.
        if (rank->block < 0 ||
            !(tile_block_whole(&ss->tile_blocks[rank->block]) || light->intensity > 0.0f))
            return false;
    }
    return true;
}

// Lay the region out for the edge the array is about to be built at: its base past every
// per-frame layer and the rain's, and layers enough for every block. Moving the base moves
// every tile -- carried across when the array is rebuilt for it, lost when it is not -- so it
// only ever rises; but it leaves room for the rain only once it has rained, since at the
// largest edge a layer held for rain that never falls is 64 MB of nothing.
static void tiles_layout(ShadowSystem* ss, const Scene* scene, int light_layers) {
    const int base = light_layers + (rain_active(scene->rain) ? 1 : 0);
    if (base > ss->tile_base_layer)
        ss->tile_base_layer = base;
    const int per_layer = tiles_per_layer(punctual_edge_for(light_layers));
    ss->tile_layers = (tile_cells_used(ss) + per_layer - 1) / per_layer;
}

// Copy one tile: from cell `src` of `read_tex`, `src_edge` texels across, read through
// `read_fbo`, to cell `dst` of `draw_tex`, `dst_edge` across, drawn through `draw_fbo`. Two
// framebuffers, since a blit reads one and draws the other.
static void tile_blit(GLuint read_fbo, GLuint read_tex, int src, int src_edge, GLuint draw_fbo,
                      GLuint draw_tex, int dst, int dst_edge) {
    int sl, sx, sy, dl, dx, dy;
    tile_cell_at(src, src_edge, &sl, &sx, &sy);
    tile_cell_at(dst, dst_edge, &dl, &dx, &dy);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
    glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, read_tex, 0, sl);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fbo);
    glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, draw_tex, 0, dl);
    glBlitFramebuffer(sx, sy, sx + SHADOW_TILE_SIZE, sy + SHADOW_TILE_SIZE, dx, dy,
                      dx + SHADOW_TILE_SIZE, dy + SHADOW_TILE_SIZE, GL_DEPTH_BUFFER_BIT,
                      GL_NEAREST);
}

// Carry the kept tiles from an array being replaced into its replacement, each under the new
// layout -- the region's new base, the new edge's tiles per layer -- so a rebuild for one
// more light layer, or a new edge, does not cost every cached light its faces in one frame.
static void tiles_migrate(ShadowSystem* ss, GLuint old_tex, GLuint old_fbo, int old_edge) {
    if (!old_tex || old_edge <= 0)
        return;
    const int new_edge = ss->punctual_map_size;
    for (int b = 0; b < ss->tile_block_count; ++b) {
        ShadowTileBlock* block = &ss->tile_blocks[b];
        // A store's copies are not carried: the faces they serve are rebuilt every frame
        // anyway, so drawing the still casters once more costs a frame's churn, once.
        block->stored = 0;
        if (!block->light || block->generation != ss->tile_generation)
            continue;
        const int src = ss->tile_held_base * tiles_per_layer(old_edge) + block->first;
        const int dst = ss->tile_base_layer * tiles_per_layer(new_edge) + block->first;
        for (int f = 0; f < 6 * block->views; ++f) {
            if (tile_face_in(block->valid, f))
                tile_blit(old_fbo, old_tex, src + f, old_edge, ss->punctual_fbo,
                          ss->punctual_map_array, dst + f, new_edge);
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// What a box does to the kept faces that see it, any of: they are drawn again; they are drawn
// as their still casters with the movers over them, every frame until one is drawn with no
// moving caster in it; their store's copy of the still casters is drawn again, because it held
// one that has started to move.
enum { TILE_BOX_INVALIDATE = 1, TILE_BOX_DYNAMIC = 2, TILE_BOX_UNSTORE = 4 };

static void tiles_mark_box(ShadowSystem* ss, const vec3 box_min, const vec3 box_max,
                           unsigned marks) {
    mat4 identity = GLM_MAT4_IDENTITY_INIT;
    vec3 middle = GLM_VEC3_ZERO_INIT;
    glm_vec3_center((float*)box_min, (float*)box_max, middle);
    const float half = 0.5f * glm_vec3_distance((float*)box_min, (float*)box_max);
    for (int b = 0; b < ss->tile_block_count; ++b) {
        ShadowTileBlock* block = &ss->tile_blocks[b];
        if (!block->light || block->views <= 0)
            continue;
        // A box past every view's far plane, with the body's reach, is in none of its faces.
        const float reach = block->far_plane + glm_vec3_norm(block->segment) + block->radius;
        if (glm_vec3_distance(middle, block->centre) > reach + half)
            continue;
        for (int f = 0; f < 6 * block->views; ++f) {
            // The face's own volume, which is exactly what it drew: nothing outside it is in
            // the tile to go stale.
            mat4 matrix = GLM_MAT4_IDENTITY_INIT;
            tile_block_face_matrix(block, f, matrix);
            Frustum volume;
            frustum_extract_from_vp(matrix, &volume);
            if (!frustum_test_aabb_transformed(&volume, (float*)box_min, (float*)box_max, identity))
                continue;
            const uint64_t bit = 1ull << f;
            block->touched |= bit;
            if (marks & TILE_BOX_DYNAMIC)
                block->dynamic |= bit;
            if (marks & (TILE_BOX_UNSTORE | TILE_BOX_INVALIDATE))
                block->stored &= ~bit;
            if (marks & TILE_BOX_INVALIDATE)
                block->valid &= ~bit;
        }
    }
}

// Every drawn face of every cached light, drawn over its copy: for a moving caster with no
// bound, which may be anywhere, as draw_item_visible takes it.
static void tiles_mark_all_dynamic(ShadowSystem* ss) {
    for (int b = 0; b < ss->tile_block_count; ++b) {
        ShadowTileBlock* block = &ss->tile_blocks[b];
        if (!block->light || block->views <= 0)
            continue;
        block->touched |= tile_faces_of(block->views);
        block->dynamic |= tile_faces_of(block->views);
    }
}

// Let go of the movers that have held still for SHADOW_TILE_MOVER_HOLD frames. A face that
// drew one over its store is then wrong whole -- the store left it out -- so every face drawn
// that way is drawn whole again, the one that stopped where it stopped, and every store's copy
// is drawn again before it is next used; the movers still moving mark theirs again at once.
static void tiles_expire_movers(ShadowSystem* ss) {
    int kept = 0;
    for (int k = 0; k < ss->tile_mover_count; ++k) {
        if (ss->tile_frame - ss->tile_mover_moved[k] > SHADOW_TILE_MOVER_HOLD)
            continue;
        ss->tile_movers[kept] = ss->tile_movers[k];
        ss->tile_mover_moved[kept++] = ss->tile_mover_moved[k];
    }
    if (kept == ss->tile_mover_count)
        return;
    ss->tile_mover_count = kept;
    for (int b = 0; b < ss->tile_block_count; ++b) {
        ShadowTileBlock* block = &ss->tile_blocks[b];
        if (!block->light)
            continue;
        block->valid &= ~block->dynamic;
        block->dynamic = 0;
        block->stored = 0;
    }
}

// What a kept face draws an item by (ShadowTileSeen), all but where: tile_seen_place says that.
static ShadowTileSeen tile_seen_of(const DrawItem* item) {
    ShadowTileSeen seen = {.node = item->node->serial,
                           .mesh = item->mesh->id,
                           .upload = item->mesh->upload_count,
                           .lane = item->lane,
                           .flags = item->flags,
                           .kept = caster_set_wants(SHADOW_CASTERS_KEPT, item->lane, item->flags)};
    const Material* mat = item->mesh->material;
    uint64_t hash = FNV1A64_BASIS;
    if (item->flags & DRAW_FOLIAGE) {
        const uintptr_t albedo = (uintptr_t)mat->albedo_tex;
        hash = fnv1a64(hash, &mat->alphaCutoff, sizeof(mat->alphaCutoff));
        hash = fnv1a64(hash, mat->uvOffset, sizeof(mat->uvOffset));
        hash = fnv1a64(hash, mat->uvScale, sizeof(mat->uvScale));
        hash = fnv1a64(hash, &mat->uvRotation, sizeof(mat->uvRotation));
        hash = fnv1a64(hash, &albedo, sizeof(albedo));
    }
    if (item->flags & DRAW_HOOKED_CASTER) {
        hash = fnv1a64(hash, &mat->shader_hook->id, sizeof(mat->shader_hook->id));
        hash = fnv1a64(hash, &mat->shader_params, sizeof(mat->shader_params));
    }
    seen.shape = hash == FNV1A64_BASIS ? 0 : hash;
    const ShaderHook* hook = shader_hook_live(mat->shader_hook);
    seen.offset_bound = hook && hook->offset ? hook->offset_bound : 0.0f;
    return seen;
}

// Where a kept face draws an item: its bound, posed and displaced, at its node now. A caster the
// kept faces take with no bound may be anywhere, and is marked so.
static void tile_seen_place(ShadowTileSeen* seen, const DrawItem* item, const CullView* view) {
    if (!seen->kept)
        return;
    AABB box;
    seen->unbounded = !draw_item_bounds(item, view, &box);
    if (!seen->unbounded)
        aabb_transform(box.min, box.max, (vec4*)item->node->global_transform, seen->box.min,
                       seen->box.max);
}

// The faces a seen caster was drawn in, drawn again, its store's copy with them. False for one
// with no bound, whose faces could be any of them.
static bool tiles_mark_seen(ShadowSystem* ss, const ShadowTileSeen* seen) {
    if (!seen->kept)
        return true;
    if (seen->unbounded)
        return false;
    tiles_mark_box(ss, seen->box.min, seen->box.max, TILE_BOX_INVALIDATE | TILE_BOX_UNSTORE);
    return true;
}

// The list as the kept faces see it this frame, into `out` (count items).
static void tiles_seen_build(ShadowTileSeen* out, const DrawList* list, const CullView* view) {
    for (size_t i = 0; list && i < list->count; ++i) {
        out[i] = tile_seen_of(&list->items[i]);
        tile_seen_place(&out[i], &list->items[i], view);
    }
}

// Keep the list as the kept faces saw it this frame. On out of memory nothing is kept, and the
// next frame draws every face again. A caster with no bound is kept as one, so only a change to
// it draws every face again, and not every frame it stands there.
static void tiles_seen_record(ShadowSystem* ss, const DrawList* list, const CullView* view) {
    const size_t count = list ? list->count : 0;
    ss->tile_seen_count = 0;
    if (!grow_array((void**)&ss->tile_seen, &ss->tile_seen_capacity, count, sizeof(ShadowTileSeen),
                    64))
        return;
    tiles_seen_build(ss->tile_seen, list, view);
    ss->tile_seen_count = count;
}

// List the items every face drawn over its copy takes, once the movers are settled, so a face
// reads the few it draws rather than asking the whole list. Unlisted on out of memory, which
// leaves each face asking the whole list.
static void tiles_list_mover_items(ShadowSystem* ss, const DrawList* list) {
    const size_t count = list ? list->count : 0;
    ss->tile_mover_item_count = 0;
    if (!list || !grow_array((void**)&ss->tile_mover_items, &ss->tile_mover_item_capacity, count,
                             sizeof(size_t), 64))
        return;
    for (size_t i = 0; i < count; ++i) {
        if (caster_set_takes(ss, SHADOW_CASTERS_KEPT_MOVERS, &list->items[i]))
            ss->tile_mover_items[ss->tile_mover_item_count++] = i;
    }
    ss->tile_mover_list = list;
}

// Whether the list holds the items the kept faces last saw, each where it was.
static bool tiles_seen_same_items(const ShadowSystem* ss, const DrawList* list) {
    const size_t count = list ? list->count : 0;
    if (count != ss->tile_seen_count)
        return false;
    for (size_t i = 0; i < count; ++i) {
        if (list->items[i].node->serial != ss->tile_seen[i].node ||
            list->items[i].mesh->id != ss->tile_seen[i].mesh)
            return false;
    }
    return true;
}

// Whether a kept face would draw `now` otherwise than it drew `then`: in or out of what the faces
// keep, between still and moving, or a cut-out or a hook changed.
static bool tile_seen_looks_differ(const ShadowTileSeen* then, const ShadowTileSeen* now) {
    return now->lane != then->lane || now->flags != then->flags || now->shape != then->shape;
}

// One item a kept face would draw otherwise, or from geometry uploaded again: the faces it was
// drawn in and the faces it is in now, drawn again. `then` was drawn where it was; `now` is
// placed here. False when it had or has no bound.
static bool tiles_mark_redrawn(ShadowSystem* ss, const ShadowTileSeen* then, ShadowTileSeen* now,
                               const DrawItem* item, const CullView* view) {
    tile_seen_place(now, item, view);
    ShadowTileSeen was = *then;
    // The bound as the faces last drew it, when that was wider: a hook whose offset reached
    // further left its shadow out there.
    if (was.offset_bound > now->offset_bound)
        aabb_expand(&was.box, was.offset_bound - now->offset_bound);
    const bool was_placed = tiles_mark_seen(ss, &was);
    return tiles_mark_seen(ss, now) && was_placed;
}

// Draw again, where it stands and where it stood, every item a kept face would draw otherwise
// than it did: a material moves a caster in or out of what the faces keep, or between still and
// moving, or changes its cut-out or its hook, with nothing in the graph changing -- its opacity,
// its shadow role, its wind, its cachedShadowWind -- and a face drawn before would keep it as it
// was, or leave it out for good. False when one has no bound, for which every face is drawn again.
static bool tiles_mark_changed_looks(ShadowSystem* ss, const DrawList* list, const CullView* view) {
    for (size_t i = 0; list && i < list->count; ++i) {
        const DrawItem* item = &list->items[i];
        ShadowTileSeen now = tile_seen_of(item);
        ShadowTileSeen* then = &ss->tile_seen[i];
        if (!tile_seen_looks_differ(then, &now))
            continue;
        if (!tiles_mark_redrawn(ss, then, &now, item, view))
            return false;
        *then = now;
        ss->tile_seen_changes++;
    }
    return true;
}

// The key a graph change matches items by: node serial, then mesh id, then where in the list.
typedef struct TileSeenKey {
    uint64_t node;
    unsigned mesh;
    size_t index;
} TileSeenKey;

// Which item comes first, by node serial and then mesh id; 0 = the same item.
static int tile_seen_item_order(const TileSeenKey* x, const TileSeenKey* y) {
    if (x->node != y->node)
        return x->node < y->node ? -1 : 1;
    if (x->mesh != y->mesh)
        return x->mesh < y->mesh ? -1 : 1;
    return 0;
}

// For qsort: by item, and a node drawing one mesh twice by where each stands in its list.
static int tile_seen_key_order(const void* a, const void* b) {
    const TileSeenKey* x = a;
    const TileSeenKey* y = b;
    const int order = tile_seen_item_order(x, y);
    if (order)
        return order;
    return x->index < y->index ? -1 : (x->index > y->index ? 1 : 0);
}

static TileSeenKey* tile_seen_keys(const ShadowTileSeen* seen, size_t count) {
    TileSeenKey* keys = malloc((count ? count : 1) * sizeof(TileSeenKey));
    if (!keys)
        return NULL;
    for (size_t i = 0; i < count; ++i)
        keys[i] = (TileSeenKey){seen[i].node, seen[i].mesh, i};
    qsort(keys, count, sizeof(TileSeenKey), tile_seen_key_order);
    return keys;
}

// Whether `serial` names a node among `keys`, sorted.
static bool tile_seen_keys_hold(const TileSeenKey* keys, size_t count, uint64_t serial) {
    size_t lo = 0, hi = count;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (keys[mid].node < serial)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < count && keys[lo].node == serial;
}

/*
 * A change to which items the list holds (spec 13.38), answered item by item rather than by
 * drawing every face again: the items are matched by node serial and mesh id, so one that only
 * moved along the list is the same item, and a node freed and another created at its address
 * are two. Each item the list no longer holds draws again the faces it was drawn in; each it
 * holds anew, the faces it is in now; each held still but drawn otherwise, or its mesh uploaded
 * again, both. A mover whose node is gone is let go, its last box among those drawn again. An
 * item no kept face takes, before or after, marks nothing. False when it cannot answer -- out of
 * memory, or a caster with no bound among those it would mark -- for which every face is drawn
 * again.
 */
static bool tiles_mark_graph_change(ShadowSystem* ss, const DrawList* list, const CullView* view) {
    const size_t count = list ? list->count : 0;
    ShadowTileSeen* now = malloc((count ? count : 1) * sizeof(ShadowTileSeen));
    TileSeenKey* then_keys = tile_seen_keys(ss->tile_seen, ss->tile_seen_count);
    TileSeenKey* now_keys = NULL;
    if (now && then_keys) {
        tiles_seen_build(now, list, view);
        now_keys = tile_seen_keys(now, count);
    }
    if (!now_keys) {
        free(now);
        free(then_keys);
        return false;
    }
    bool placed = true;
    size_t a = 0, b = 0;
    while (a < ss->tile_seen_count || b < count) {
        const int order = a == ss->tile_seen_count ? 1
                          : b == count ? -1
                                       : tile_seen_item_order(&then_keys[a], &now_keys[b]);
        if (order < 0) {
            placed = tiles_mark_seen(ss, &ss->tile_seen[then_keys[a++].index]) && placed;
        } else if (order > 0) {
            placed = tiles_mark_seen(ss, &now[now_keys[b++].index]) && placed;
        } else {
            const ShadowTileSeen* then = &ss->tile_seen[then_keys[a++].index];
            const ShadowTileSeen* held = &now[now_keys[b++].index];
            if (tile_seen_looks_differ(then, held) || then->upload != held->upload) {
                placed = tiles_mark_seen(ss, then) && placed;
                placed = tiles_mark_seen(ss, held) && placed;
                if (tile_seen_looks_differ(then, held))
                    ss->tile_seen_changes++;
            }
        }
    }
    if (!placed) {
        free(now);
        free(then_keys);
        free(now_keys);
        return false;
    }
    int kept = 0;
    for (int k = 0; k < ss->tile_mover_count; ++k) {
        if (!tile_seen_keys_hold(now_keys, count, ss->tile_movers[k]))
            continue;
        ss->tile_movers[kept] = ss->tile_movers[k];
        ss->tile_mover_moved[kept++] = ss->tile_mover_moved[k];
    }
    ss->tile_mover_count = kept;
    free(then_keys);
    free(now_keys);
    free(ss->tile_seen);
    ss->tile_seen = now;
    ss->tile_seen_count = count;
    ss->tile_seen_capacity = count ? count : 1;
    return true;
}

// What the kept faces hold that is no longer true, every frame. An item the list no longer
// holds, or holds anew, draws again the faces it was in or is in (tiles_mark_graph_change); one
// a kept face would now draw otherwise is drawn again where it was and where it is
// (tiles_mark_changed_looks). Every face is drawn again only when neither can answer. A KEPT
// caster whose node moves becomes a MOVER, and every face that sees it, where it was and where
// it is, is drawn from then on as a copy of its still casters with the movers over them
// (render_shadow_movers). A surface that moves under its node -- skinned, swaying, morphing --
// needs no place among the movers: it moves every frame and is in no store, so it marks where
// it is now, and a face it has left is still marked from the frame it was there, which draws it
// once more without it. It marks on the frame the faces are drawn again too, or each would be
// drawn whole without it and keep its shadow out for a frame. `frame` is the engine's, so the
// hold counts frames and not depth passes, which a burst of captures multiplies. A whole frame
// with no note -- no block held, a reference drawn, shadows off -- leaves the record behind
// whatever moved in it, so the first frame after one draws every face again.
static void tiles_note_changes(ShadowSystem* ss, const Engine* engine, const Scene* scene,
                               uint64_t frame) {
    const bool resumed = ss->tile_noted && frame > ss->tile_frame + 1;
    ss->tile_noted = true;
    ss->tile_frame = frame;
    for (int b = 0; b < ss->tile_block_count; ++b)
        ss->tile_blocks[b].touched = 0;
    const CullView view = render_cull_view(engine, scene, NULL);
    const DrawList* list = scene->draw_list;
    const uint64_t epoch = scene_graph_epoch();
    const bool same = epoch == ss->tile_epoch && tiles_seen_same_items(ss, list);
    ss->tile_epoch = epoch;
    if (!resumed && (same ? tiles_mark_changed_looks(ss, list, &view)
                          : tiles_mark_graph_change(ss, list, &view))) {
        tiles_expire_movers(ss);
    } else {
        tiles_seen_record(ss, list, &view);
        ss->tile_generation++;
        ss->tile_mover_count = 0;
        for (int b = 0; b < ss->tile_block_count; ++b) {
            ss->tile_blocks[b].dynamic = 0;
            ss->tile_blocks[b].stored = 0;
        }
    }
    // Where each caster that moves is now, so the faces it is in are the ones drawn again if it
    // goes: kept up only while the seen list stands for this frame's list.
    ShadowTileSeen* seen = ss->tile_seen_count == (list ? list->count : 0) ? ss->tile_seen : NULL;
    for (size_t i = 0; list && i < list->count; ++i) {
        const DrawItem* item = &list->items[i];
        const SceneNode* node = item->node;
        if (!caster_set_wants(SHADOW_CASTERS_KEPT, item->lane, item->flags))
            continue;
        vec3 lo = GLM_VEC3_ZERO_INIT, hi = GLM_VEC3_ZERO_INIT;
        // The surface as drawn, posed and displaced: the import box is the bind pose, which a
        // curled body or a swung tail leaves, and a mover is drawn with its wind.
        AABB box;
        if (!(item->flags & DRAW_KEPT_STILL)) {
            if (!draw_item_bounds(item, &view, &box)) {
                tiles_mark_all_dynamic(ss);
                continue;
            }
            aabb_transform(box.min, box.max, (vec4*)node->global_transform, lo, hi);
            tiles_mark_box(ss, lo, hi, TILE_BOX_DYNAMIC);
            if (seen) {
                glm_vec3_copy(lo, seen[i].box.min);
                glm_vec3_copy(hi, seen[i].box.max);
                seen[i].unbounded = false;
            }
            continue;
        }
        if (memcmp(node->global_transform, node->prev_global_transform, sizeof(mat4)) == 0)
            continue;
        if (!draw_item_bounds(item, &view, &box)) {
            tiles_mark_all_dynamic(ss);
            continue;
        }
        int k = 0;
        while (k < ss->tile_mover_count && ss->tile_movers[k] != node->serial)
            ++k;
        unsigned marks = TILE_BOX_DYNAMIC;
        if (k == ss->tile_mover_count) {
            // A new mover was drawn into the stores as still; with no room for another, the
            // faces that see it are simply drawn again, every frame it moves.
            if (k < SHADOW_TILE_MAX_MOVERS) {
                ss->tile_movers[ss->tile_mover_count++] = node->serial;
                marks = TILE_BOX_DYNAMIC | TILE_BOX_UNSTORE;
            } else {
                marks = TILE_BOX_INVALIDATE;
            }
        }
        if (k < SHADOW_TILE_MAX_MOVERS)
            ss->tile_mover_moved[k] = ss->tile_frame;
        for (int when = 0; when < 2; ++when) {
            aabb_transform(
                box.min, box.max,
                when ? (vec4*)node->global_transform : (vec4*)node->prev_global_transform, lo, hi);
            tiles_mark_box(ss, lo, hi, marks);
        }
        // The loop's last pass leaves lo and hi where it stands now.
        if (seen) {
            glm_vec3_copy(lo, seen[i].box.min);
            glm_vec3_copy(hi, seen[i].box.max);
            seen[i].unbounded = false;
        }
    }
    tiles_list_mover_items(ss, list);
}

// Whether a face's volume reaches the camera's view. All eight of its corners outside one of
// the view's planes means nothing the camera sees can sample the face; anything else might, so
// it is drawn.
static bool tile_face_in_view(const Frustum* view, mat4 face_matrix) {
    mat4 inverse;
    glm_mat4_inv(face_matrix, inverse);
    vec4 corner[8];
    glm_frustum_corners(inverse, corner);
    for (int p = 0; p < 6; ++p) {
        const float* pl = view->planes[p];
        int outside = 0;
        for (int c = 0; c < 8; ++c)
            outside +=
                pl[0] * corner[c][0] + pl[1] * corner[c][1] + pl[2] * corner[c][2] + pl[3] < 0.0f;
        if (outside == 8)
            return false;
    }
    return true;
}

// The camera's view this frame.
static void tile_camera_view(const Engine* engine, Frustum* view) {
    mat4 view_proj;
    glm_mat4_mul((vec4*)engine->projection_matrix, (vec4*)engine->view_matrix, view_proj);
    frustum_extract_from_vp(view_proj, view);
}

// Whether none of a block's faces show the light as it is now, so all are drawn again from where
// it stands: the region lost its contents, the light's planes or its view count changed, or its
// views drifted.
static bool tile_block_stale(const ShadowSystem* ss, const ShadowTileBlock* block) {
    float near_p, far_p;
    tile_planes(block->light, &near_p, &far_p);
    return ss->tile_refresh || block->generation != ss->tile_generation ||
           block->near_plane != near_p || block->far_plane != far_p ||
           block->views != tile_views_for(ss, block->light) || tile_views_drifted(ss, block);
}

// The faces of a block render_shadow_movers draws this frame: those that see a mover, but a face
// out of the camera's view keeps the copy it has, which nothing on screen reads -- unless it
// holds nothing yet, or inside a capture, which keeps what it sees.
static uint64_t tile_mover_faces_drawn(const ShadowSystem* ss, const Engine* engine,
                                       const Frustum* view, const ShadowTileBlock* block) {
    if (!block->light || !block->dynamic || block->views <= 0 ||
        block->generation != ss->tile_generation || !(block->light->intensity > 0.0f))
        return 0;
    const bool every = !engine->camera || engine->capture_kind != SCENE_CAPTURE_NONE;
    uint64_t drawn = 0;
    for (int f = 0; f < 6 * block->views; ++f) {
        if (!tile_face_in(block->dynamic, f))
            continue;
        mat4 matrix = GLM_MAT4_IDENTITY_INIT;
        tile_block_face_matrix(block, f, matrix);
        if (every || !tile_face_in(block->valid, f) || tile_face_in_view(view, matrix))
            drawn |= 1ull << f;
    }
    return drawn;
}

// The last face of a block among `faces` holding a store cell, -1 for none.
static int tile_store_holder(const ShadowTileBlock* block, uint64_t faces) {
    for (int f = 6 * block->views; f-- > 0;) {
        if (block->store_cell[f] >= 0 && tile_face_in(faces, f))
            return f;
    }
    return -1;
}

// Take face `f`'s store cell from it, which then goes without.
static int tile_store_reclaim(ShadowTileBlock* block, int f) {
    const int cell = block->store_cell[f];
    tile_store_release(block, f);
    return cell;
}

// A store cell for a face of the light ranked `r`, drawn this frame: a free one; else one held
// by a face not drawn this frame, the farthest light's first; else one held by the farthest
// light more than the margin farther, whose face goes without. -1 when there is none.
static int tile_store_take(ShadowSystem* ss, const uint64_t* drawn, uint64_t* free_cells,
                           size_t r) {
    if (*free_cells) {
        const int cell = __builtin_ctzll(*free_cells);
        *free_cells &= *free_cells - 1ull;
        return cell;
    }
    for (size_t k = ss->tile_rank_count; k-- > 0;) {
        const int b = ss->tile_rank[k].block;
        const int f = b >= 0 ? tile_store_holder(&ss->tile_blocks[b], ~drawn[b]) : -1;
        if (f >= 0)
            return tile_store_reclaim(&ss->tile_blocks[b], f);
    }
    for (size_t k = ss->tile_rank_count; tile_rank_farther(ss, r, &k);) {
        const int b = ss->tile_rank[k].block;
        const int f = b >= 0 ? tile_store_holder(&ss->tile_blocks[b], ~0ull) : -1;
        if (f >= 0)
            return tile_store_reclaim(&ss->tile_blocks[b], f);
    }
    return -1;
}

// Give each face drawn this frame over a copy of its still casters a cell of the store pool to
// keep them in (spec 13.26), nearest light first. A face keeps the cell it holds while it is not
// drawn that way, so a mover coming back finds the copy still there, until a face that is drawn
// needs the cell. A face left without one is drawn whole, which is said once each time the
// camera's pass runs out. A block drawn again from where its light stands this frame takes none
// and keeps none: its copy would be drawn and thrown away in the same pass. Before the region is
// laid out, so the pool is in the array the frame it opens.
static void tiles_take_stores(ShadowSystem* ss, const Engine* engine) {
    int usable = ss->tile_store_cells < SHADOW_TILE_STORE_CELLS ? ss->tile_store_cells
                                                                : SHADOW_TILE_STORE_CELLS;
    if (usable < 0)
        usable = 0;
    uint64_t free_cells = usable >= 64 ? ~0ull : (1ull << usable) - 1ull;
    Frustum view;
    tile_camera_view(engine, &view);
    uint64_t drawn[SHADOW_TILE_MAX_BLOCKS];
    for (int b = 0; b < ss->tile_block_count; ++b) {
        ShadowTileBlock* block = &ss->tile_blocks[b];
        const bool stale = block->light && tile_block_stale(ss, block);
        drawn[b] = stale ? 0 : tile_mover_faces_drawn(ss, engine, &view, block);
        // What each face holds: kept if it is a cell of the pool as large as it now is, under a
        // view the block still has, and no other face's.
        for (int f = 0; f < 6 * SHADOW_TILE_VIEWS; ++f) {
            const int cell = block->store_cell[f];
            if (cell < 0)
                continue;
            if (!block->light || stale || f >= 6 * block->views || !tile_face_in(free_cells, cell))
                tile_store_release(block, f);
            else
                free_cells &= ~(1ull << cell);
        }
    }
    const Light* refused = NULL;
    for (size_t r = 0; r < ss->tile_rank_count; ++r) {
        const int b = ss->tile_rank[r].block;
        if (b < 0)
            continue;
        ShadowTileBlock* block = &ss->tile_blocks[b];
        for (int f = 0; f < 6 * block->views; ++f) {
            if (!tile_face_in(drawn[b], f) || block->store_cell[f] >= 0)
                continue;
            const int cell = usable > 0 ? tile_store_take(ss, drawn, &free_cells, r) : -1;
            if (cell < 0) {
                if (!refused)
                    refused = ss->tile_rank[r].light;
                continue;
            }
            if (ss->tile_store_first < 0)
                ss->tile_store_first = tile_cells_used(ss);
            block->store_cell[f] = (int8_t)cell;
        }
    }
    // The camera's pass alone says so (spec 13.32). A capture's draws every face a mover reaches,
    // in view or not, so it runs out where the camera's does not, and a latch the two shared was
    // spent by one and re-armed by the other every frame a capture ran.
    if (engine->capture_kind != SCENE_CAPTURE_NONE)
        return;
    if (refused && !ss->tile_store_warned) {
        log_warn("Cached shadow stores full (%d faces): '%s' and any farther cached light draw the "
                 "faces their moving casters reach whole, every frame",
                 usable, refused->name ? refused->name : "unnamed light");
    }
    ss->tile_store_warned = refused != NULL;
}

// Draw one face, through `matrix`, into cell `cell` of the array -- counted from layer 0 --
// over what the cell holds unless `clear`. False when the tile could not be bound, so nothing
// was drawn.
static bool draw_tile_cell(ShadowSystem* ss, const Engine* engine, const Scene* scene,
                           SubmitState* state, int cell, mat4 matrix, ShadowCasterSet set,
                           bool clear) {
    int layer, x, y;
    tile_cell_at(cell, ss->punctual_map_size, &layer, &x, &y);
    if (!begin_depth_tile(ss->punctual_fbo, ss->punctual_map_array, layer, x, y, clear))
        return false;
    draw_shadow_layer(ss, scene, scene->draw_list, matrix, state, set, engine);
    end_depth_tile();
    return true;
}

// One face with everything a kept face casts -- its still casters, then what moves over them --
// for a face that is drawn again the next frame, since a pose is in it.
static bool draw_tile_cell_all(ShadowSystem* ss, const Engine* engine, const Scene* scene,
                               SubmitState* state, int cell, mat4 matrix) {
    return draw_tile_cell(ss, engine, scene, state, cell, matrix, SHADOW_CASTERS_KEPT_STILL,
                          true) &&
           draw_tile_cell(ss, engine, scene, state, cell, matrix, SHADOW_CASTERS_KEPT_MOVERS,
                          false);
}

// The framebuffer a tile is read through while the punctual one draws its copy.
static GLuint tile_copy_fbo(ShadowSystem* ss) {
    if (!ss->tile_copy_fbo)
        init_depth_fbo(&ss->tile_copy_fbo);
    return ss->tile_copy_fbo;
}

// Point a cached light's lookup at block `b`.
static void publish_tile_block(ShadowSystem* ss, int b) {
    ss->tile_blocks[b].light->shadow_tile = tile_block_first_cell(ss, ss->punctual_map_size, b);
}

// Point each cached light's lookup at its block once it is whole.
static void publish_shadow_tiles(ShadowSystem* ss) {
    for (int b = 0; b < ss->tile_block_count; ++b) {
        if (ss->tile_blocks[b].light && tile_block_whole(&ss->tile_blocks[b]))
            publish_tile_block(ss, b);
    }
}

bool shadow_tile_lookup(const ShadowSystem* system, const Light* light, ShadowTileLookup* out) {
    const int first = shadow_live_tile(system, light);
    const int b = first >= 0 ? tile_block_of(system, light) : -1;
    if (b < 0)
        return false;
    const ShadowTileBlock* block = &system->tile_blocks[b];
    out->first = first;
    glm_vec3_copy((float*)block->centre, out->centre);
    glm_vec3_copy((float*)block->segment, out->segment);
    out->radius = block->radius;
    out->near_plane = block->near_plane;
    return true;
}

// Draw what the cached lights' blocks are missing. A light is not drawn until it emits: a
// candle's light sits at the wick until its flame has burned a frame, and a face drawn from
// there is a face drawn from the wrong place. The loading screen moves between faces: the frame
// the tiles open draws every face of every light that holds a block.
static void render_shadow_tiles(ShadowSystem* ss, Engine* engine, const Scene* scene,
                                SubmitState* state) {
    const int edge = ss->punctual_map_size;
    for (int b = 0; b < ss->tile_block_count; ++b) {
        ShadowTileBlock* block = &ss->tile_blocks[b];
        if (!block->light)
            continue;
        if (tile_block_stale(ss, block)) {
            block->valid = 0;
            block->stored = 0;
        }
        if (tile_block_whole(block) || !(block->light->intensity > 0.0f))
            continue;
        if (!block->valid) {
            tile_block_place(ss, block, tile_views_for(ss, block->light));
            // This frame's marks were taken against the faces as they stood, so none of them
            // may let a face go as holding no moving caster.
            block->touched = tile_faces_of(block->views);
        }
        // A face that sees a mover is rebuilt every frame by render_shadow_movers instead.
        const uint64_t missing = tile_faces_of(block->views) & ~block->valid & ~block->dynamic;
        for (int f = 0; f < 6 * block->views; ++f) {
            if (!tile_face_in(missing, f))
                continue;
            mat4 matrix = GLM_MAT4_IDENTITY_INIT;
            tile_block_face_matrix(block, f, matrix);
            if (draw_tile_cell(ss, engine, scene, state, tile_block_first_cell(ss, edge, b) + f,
                               matrix, SHADOW_CASTERS_KEPT, true)) {
                block->valid |= 1ull << f;
                ss->tile_faces_drawn++;
            }
            loading_screen_tick(engine);
        }
    }
}

// Whether any kept face sees a mover.
static bool tiles_any_dynamic(const ShadowSystem* ss) {
    for (int b = 0; b < ss->tile_block_count; ++b) {
        if (ss->tile_blocks[b].light && ss->tile_blocks[b].dynamic)
            return true;
    }
    return false;
}

// The kept faces that see a mover, every frame (tile_mover_faces_drawn): each is a copy of its
// store cell, which holds the still casters alone and is drawn only when it does not, with the
// movers drawn over the copy. What a pendulum costs a frame is then a copy and its own draws,
// where drawing the face whole costs every caster in the room. A face without a cell is drawn
// whole, still casters and movers both. A face drawn when no moving caster reached it this frame
// holds nothing that moves, so it is kept from then on: without that, every face a cat ever
// walked through would be drawn again every frame.
static void render_shadow_movers(ShadowSystem* ss, Engine* engine, const Scene* scene,
                                 SubmitState* state) {
    const int edge = ss->punctual_map_size;
    Frustum view;
    tile_camera_view(engine, &view);
    for (int b = 0; b < ss->tile_block_count; ++b) {
        ShadowTileBlock* block = &ss->tile_blocks[b];
        const uint64_t drawn = tile_mover_faces_drawn(ss, engine, &view, block);
        for (int f = 0; f < 6 * block->views; ++f) {
            if (!tile_face_in(drawn, f))
                continue;
            mat4 matrix = GLM_MAT4_IDENTITY_INIT;
            tile_block_face_matrix(block, f, matrix);
            const int cell = tile_block_first_cell(ss, edge, b) + f;
            bool over_copy = false;
            if (block->store_cell[f] >= 0) {
                const int stored = tile_store_first_cell(ss, edge) + block->store_cell[f];
                if (!tile_face_in(block->stored, f) &&
                    draw_tile_cell(ss, engine, scene, state, stored, matrix,
                                   SHADOW_CASTERS_KEPT_STILL, true))
                    block->stored |= 1ull << f;
                over_copy = tile_face_in(block->stored, f);
                if (over_copy)
                    tile_blit(tile_copy_fbo(ss), ss->punctual_map_array, stored, edge,
                              ss->punctual_fbo, ss->punctual_map_array, cell, edge);
            }
            if (over_copy ? draw_tile_cell(ss, engine, scene, state, cell, matrix,
                                           SHADOW_CASTERS_KEPT_MOVERS, false)
                          : draw_tile_cell_all(ss, engine, scene, state, cell, matrix)) {
                block->valid |= 1ull << f;
                if (!tile_face_in(block->touched, f))
                    block->dynamic &= ~(1ull << f);
                if (over_copy)
                    ss->mover_faces_copied++;
                else
                    ss->mover_faces_whole++;
            }
            // The first frames draw every store cell besides: the loading screen moves between.
            loading_screen_tick(engine);
        }
    }
}

// --tile-reference: every cached light drawn this frame from points of its body as it is now,
// with everything a kept face casts, and published; the lookup places the same points. Its views
// fill the block's faces but are not the block's kept views, so leaving the reference redraws.
static void render_shadow_reference(ShadowSystem* ss, const Engine* engine, const Scene* scene,
                                    SubmitState* state) {
    const int edge = ss->punctual_map_size;
    for (int b = 0; b < ss->tile_block_count; ++b) {
        ShadowTileBlock* block = &ss->tile_blocks[b];
        if (!block->light || !(block->light->intensity > 0.0f))
            continue;
        tile_block_place(ss, block, tile_views_for(ss, block->light));
        block->valid = 0;
        bool whole = true;
        for (int f = 0; f < 6 * block->views; ++f) {
            mat4 matrix = GLM_MAT4_IDENTITY_INIT;
            tile_block_face_matrix(block, f, matrix);
            whole &= draw_tile_cell_all(ss, engine, scene, state,
                                        tile_block_first_cell(ss, edge, b) + f, matrix);
            ss->tile_faces_drawn++;
        }
        if (whole)
            publish_tile_block(ss, b);
        block->views = 0;
    }
}

// One layer, three passes: depth -> moments into msm_array, then blur
// msm_array -> scratch and scratch -> msm_array, one axis each. Ending in
// msm_array is why there is no handle swap and no fourth copy. Both source and
// destination are the same layer index, since this array mirrors the cascades
// one for one.
// One draw. `mode` doubles as the sampler unit -- the two sources are distinct
// uniforms on distinct units, so neither pass rebinds the other's texture.
static void msm_pass(ShadowSystem* ss, UniformManager* mu, int layer, GLuint dst, GLuint src,
                     int mode, float dx, float dy) {
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, dst, 0, layer);
    uniform_set_int(mu, "mode", mode);
    uniform_set_vec2(mu, "blurStep", (vec2){dx, dy});
    glActiveTexture(GL_TEXTURE0 + mode);
    glBindTexture(GL_TEXTURE_2D_ARRAY, src);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void msm_build_layer(ShadowSystem* ss, UniformManager* mu, int layer) {
    uniform_set_int(mu, "layer", layer);
    msm_pass(ss, mu, layer, ss->msm_array, ss->shadow_map_array, 0, 0.0f, 0.0f);
    // At spacing 0 every blur tap lands on the same texel, so the two passes
    // would copy the layer twice for nothing. Skipping them is what makes the
    // default free rather than merely harmless -- and is why msm_scratch is not
    // allocated at all until a blur asks for it.
    if (ss->msm_blur <= 0.0f)
        return;
    // Texels of the NARROWEST cascade, with the wider ones scaled down to match
    // its world width.
    //
    // A fixed texel spacing would be a different world-space softness in every
    // cascade -- cascade 0 is a near view-slice fit, the outermost is the whole
    // scene -- so the penumbra would step at each seam, which is the artifact
    // PCSS divides by frustumWidth to avoid (see lightSizeUV in pbr_frag).
    // Denominating in world units instead is the obvious fix and the wrong one:
    // the filter is 5 fixed taps and combs past ~1 texel, so the widest cascade
    // sets a ceiling that leaves the useful range a sliver either side of 0.02.
    // Scaling from the narrowest cascade gives equal world width AND keeps every
    // cascade under the comb limit whenever the knob is <= 1.
    const float texel = 1.0f / (float)ss->msm_allocated_size;
    const float near_width = ss->cascade_params[layer - (layer % ss->cascade_count)][0];
    const float width = ss->cascade_params[layer][0];
    const float step = width > 0.0f ? ss->msm_blur * texel * (near_width / width) : 0.0f;
    msm_pass(ss, mu, layer, ss->msm_scratch, ss->msm_array, 1, step, 0.0f);
    msm_pass(ss, mu, layer, ss->msm_array, ss->msm_scratch, 1, 0.0f, step);
}

// Derive the filterable moment cascades from the depth cascades the pass above
// just rendered. False means nothing usable was produced and the lookup must
// stay on the depth array -- which is why the caller stores the result rather
// than re-reading msm_enabled.
//
// Deliberately reads the finished depth array instead of writing moments during
// the depth pass. That keeps the depth pass, its polygon offset, its front-face
// culling and its colour-less FBO byte-identical between --msm and the default,
// so the off path is the path that was there before rather than a rebuild of it.
static bool shadow_build_msm(ShadowSystem* ss, Engine* engine) {
    const int layers = (int)ss->directional_count * ss->cascade_count;
    if (!ss->msm_enabled || !ss->shadow_map_array || layers <= 0) {
        // Hand the VRAM back when the feature is switched off, but keep the FBO
        // and quad: the capture path clears msm_enabled around every cube face
        // (render.c), so tying those to the flag would rebuild them per face.
        if (ss->msm_array)
            free_msm_arrays(ss);
        return false;
    }

    if (!ss->msm_program)
        ss->msm_program = engine_find_program(engine, "msm_resolve");
    if (!ss->msm_program || !ss->msm_program->uniforms)
        return false;

    // Never above the depth array it resolves FROM. The 2x2 gather in the shader
    // is a downsample, so at parity it degenerates to a half-texel shift and
    // above it to an upsample -- more memory for strictly less information.
    int size = ss->msm_size;
    if (size > ss->default_map_size) {
        log_warn("Moment cascade %d^2 exceeds the depth map; clamping to %d^2", size,
                 ss->default_map_size);
        size = ss->default_map_size;
        ss->msm_size = size;
    }
    // Only when a blur is asked for. The scratch exists solely as the separable
    // filter's ping target, and the default blur is 0, so allocating it eagerly
    // was doubling the feature's resting cost for a texture no draw touches.
    const bool want_scratch = ss->msm_blur > 0.0f;
    // GROW-ONLY on the layer count. It matters because the caster count
    // OSCILLATES: each sky body clears cast_shadows as it sets, so a running
    // day/night cycle walks this 1 -> 2 -> 1 twice a day and was freeing and
    // rebuilding a 24-48 MB RGBA16F array every time.
    //
    // The depth array is NOT the precedent, though it looks like one: it is
    // sized by MAX_SHADOW_LIGHTS, the compile-time ceiling, so no per-frame
    // value reaches it and it has no shrink to refuse. This is the only
    // allocation in the file keyed on a count that varies per frame, which is
    // why it is the only one that needed the rule.
    //
    // Surplus layers are inert, not merely unused: the resolve loop below runs
    // to `layers`, and the shader bounds its reads by numShadowLights, which is
    // the same live count. Neither ever addresses one.
    //
    // The SIZE keeps its exact test -- a size change really does need a new
    // texture, and it shrinks only when a user lowers --msm-size, which is not
    // a per-frame event.
    if (ss->msm_allocated_layers < layers || ss->msm_allocated_size != size ||
        (want_scratch && !ss->msm_scratch)) {
        const bool resized = ss->msm_allocated_layers < layers || ss->msm_allocated_size != size;
        if (resized)
            free_msm_arrays(ss);
        // The HIGH-WATER MARK, not the live count, and it is what makes the
        // grow-only test above honest. The block is also entered on the
        // scratch clause -- switching a blur on mid-run -- which can happen
        // while the count is BELOW what the array already holds. Allocating
        // and recording the live count there would size the scratch smaller
        // than its array and write the capacity down, so the next crossing
        // would take the grow branch and perform exactly the free-and-rebuild
        // this test exists to delete.
        const int want_layers =
            layers > ss->msm_allocated_layers ? layers : ss->msm_allocated_layers;
        if (!ss->msm_array)
            ss->msm_array =
                create_texture_2d_array_float(size, size, want_layers, GL_RGBA16F, GL_RGBA);
        if (want_scratch && !ss->msm_scratch)
            ss->msm_scratch =
                create_texture_2d_array_float(size, size, want_layers, GL_RGBA16F, GL_RGBA);
        if (!ss->msm_array || (want_scratch && !ss->msm_scratch)) {
            log_error("Failed to allocate moment shadow cascades");
            free_msm_arrays(ss);
            return false;
        }
        ss->msm_allocated_layers = want_layers;
        ss->msm_allocated_size = size;
        // What is actually resident, not what a blurred configuration would
        // cost, and stated as the amount --msm adds ON TOP of the depth cascades.
        log_info("Moment shadow cascades: %d layer(s) at %d^2 (%.0f MB%s, on top of the depth "
                 "array)",
                 layers, size,
                 (want_scratch ? 2.0 : 1.0) * layers * (double)size * size * 8.0 /
                     (1024.0 * 1024.0),
                 want_scratch ? " incl. blur scratch" : "");
    }

    // The caller restored the viewport immediately before this and every path
    // into the depth pass leaves the FBO at 0, so neither is queried back.
    GLboolean blend_was = glIsEnabled(GL_BLEND);
    GLboolean cull_was = glIsEnabled(GL_CULL_FACE);
    GLboolean depth_was = glIsEnabled(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);

    if (ss->msm_quad_vao == 0)
        create_fullscreen_quad_vao(&ss->msm_quad_vao, &ss->msm_quad_vbo);
    if (ss->msm_fbo == 0)
        glGenFramebuffers(1, &ss->msm_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, ss->msm_fbo);
    glViewport(0, 0, size, size);
    glUseProgram(ss->msm_program->id);
    UniformManager* mu = ss->msm_program->uniforms;
    uniform_set_int(mu, "srcDepth", 0);
    uniform_set_int(mu, "srcMoments", 1);
    // Bound once for the whole pass: the quad never changes, and unit 0 always
    // holds the depth array -- only unit 1 alternates, inside msm_pass.
    glBindVertexArray(ss->msm_quad_vao);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, ss->shadow_map_array);

    // Completeness is only meaningful once a layer is attached, and a driver that
    // rejects RGBA16F as a layered colour attachment would otherwise fail
    // silently -- this is the first non-float-32 array target in the codebase.
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, ss->msm_array, 0, 0);
    bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (!ok) {
        log_error("Moment shadow FBO incomplete; falling back to the depth cascades");
    } else {
        for (int layer = 0; layer < layers; layer++)
            msm_build_layer(ss, mu, layer);
    }

    glBindVertexArray(0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (blend_was)
        glEnable(GL_BLEND);
    if (cull_was)
        glEnable(GL_CULL_FACE);
    if (depth_was)
        glEnable(GL_DEPTH_TEST);
    // Both units, not just 0: a blurred build leaves the scratch on unit 1, and
    // this file already records drivers complaining about samplers left pointed
    // at array targets they no longer own.
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    return ok;
}

// Translucent shadow maps (spec 11.26). Two layers per cascade, for slot 0:
//
//   part 0  transmittance  exp(-b0), from absorbances summed additively
//   part 1  nearest translucent depth, so a receiver IN FRONT of a caster is
//           not darkened by it
//
// Absorbance is summed rather than transmittance multiplied because a sum is
// what a GL_ONE/GL_ONE blend can do, and -log(1-a) turns a product into one --
// the same primitive mboit.glsl uses, for the same reason. The exponential is
// taken ONCE, at the resolve, so what the array holds is a transmittance: that
// is what makes the shadow lookup's PCF box over these layers exact, where
// averaging absorbance would carry Jensen's bias.
// Acquire everything the transmittance pass needs and answer, ONCE, whether it
// will run this frame. Split from the build because the depth pass has to know
// the answer BEFORE it decides which casters to withhold, and it cannot know it
// from `tsm_enabled`: a shader that failed to compile or an allocation that
// failed leaves the request true and the map empty, so the casters come out of
// the depth pass and nothing represents them -- glass casting NOTHING, which is
// worse than the black shadow this feature exists to replace.
static bool shadow_tsm_prepare(ShadowSystem* ss, Engine* engine) {
    if (!ss->tsm_enabled || !ss->tsm_allocated || ss->shadow_map_array == 0 ||
        ss->directional_count == 0)
        return false;
    // Excluded by the moment path, at the one place that knows it is live --
    // the same home, and for the same reason, as the PCSS exclusion below.
    // Under --msm unit 10 carries msm_array, which has directional_count*cc
    // layers and no transmittance block at all, so the lookup would index past
    // its end and read a mean depth as a transmittance.
    if (ss->msm_enabled)
        return false;

    if (!ss->tsm_absorb_program)
        ss->tsm_absorb_program = engine_find_program(engine, "shadow_absorb");
    if (!ss->tsm_resolve_program)
        ss->tsm_resolve_program = engine_find_program(engine, "tsm_resolve");
    if (!ss->tsm_absorb_program || !ss->tsm_resolve_program)
        return false;

    const int size = ss->default_map_size;
    const int cc = ss->cascade_count;

    if (ss->tsm_scratch == 0) {
        // R32F and not R16F: a dense groom sums many strand absorbances into
        // one texel, and -log(1-a) is unbounded as a approaches 1. The shared
        // helper's LINEAR filter is moot -- tsm_resolve_frag reads this with
        // texelFetch, which never filters.
        ss->tsm_scratch = create_texture_2d_float(size, size, GL_R32F, GL_RED, NULL);
        if (ss->tsm_scratch == 0)
            return false;
        glGenFramebuffers(1, &ss->tsm_scratch_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, ss->tsm_scratch_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ss->tsm_scratch,
                               0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            log_error("Translucent shadow scratch framebuffer incomplete");
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            gl_delete_texture(&ss->tsm_scratch);
            gl_delete_fbo(&ss->tsm_scratch_fbo);
            return false;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        log_info(
            "Translucent shadows: %d layer(s) at %d^2 appended to the depth array "
            "(%.0f MB incl. a %.0f MB accumulation scratch)",
            TSM_SLOTS * cc * TSM_PARTS, size,
            (TSM_SLOTS * cc * TSM_PARTS * (double)size * size * 4.0 + size * (double)size * 4.0) /
                (1024.0 * 1024.0),
            size * (double)size * 4.0 / (1024.0 * 1024.0));
    }
    if (ss->tsm_quad_vao == 0)
        create_fullscreen_quad_vao(&ss->tsm_quad_vao, &ss->tsm_quad_vbo);
    return true;
}

// Draw the transmittance layers. Callable only when shadow_tsm_prepare returned
// true, so every resource here is known to exist.
static bool shadow_build_tsm(ShadowSystem* ss, const Engine* engine, const Scene* scene) {
    const int size = ss->default_map_size;
    const int cc = ss->cascade_count;
    bool ok = true;

    GLboolean blend_was = glIsEnabled(GL_BLEND);
    GLboolean cull_was = glIsEnabled(GL_CULL_FACE);
    GLboolean depth_was = glIsEnabled(GL_DEPTH_TEST);
    // The clear COLOUR is saved too, and it is not fussiness: the scene clear
    // reads whatever this leaves behind, so an unrestored black here turns a
    // sky-lit frame black and moves 100% of its pixels. Measured that way once.
    GLfloat clear_was[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear_was);
    // The blend FUNCTION as well as the enable. Restoring only the enable
    // leaves GL_ONE/GL_ONE installed for whatever blends next, which is not a
    // shadow bug at all -- it desaturates the whole frame, and it reads exactly
    // like an exposure shift while the shadows look right.
    GLint blend_src_rgb, blend_dst_rgb, blend_src_a, blend_dst_a;
    glGetIntegerv(GL_BLEND_SRC_RGB, &blend_src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &blend_dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &blend_src_a);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &blend_dst_a);

    for (int c = 0; c < cc; ++c) {
        const float* matrix = (const float*)ss->cascade_matrices[c];
        // Same volume the depth layer culls against; both TSM walks below write
        // the layers this cascade owns.
        Frustum tsm_frustum;
        frustum_extract_from_vp(ss->cascade_matrices[c], &tsm_frustum);
        CullView tsm_cull = render_cull_view(engine, scene, &tsm_frustum);
        SubmitState state = {0};

        // --- accumulate absorbance -----------------------------------------
        // No depth buffer here at all, so no test and no offset: every
        // translucent fragment along the ray must contribute, which is the
        // opposite of what a depth pass wants.
        glBindFramebuffer(GL_FRAMEBUFFER, ss->tsm_scratch_fbo);
        glViewport(0, 0, size, size);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        _draw_shadow_items(ss, scene->draw_list, true, &state, SHADOW_CASTERS_TRANSLUCENT,
                           &tsm_cull, engine, scene, matrix);
        glDisable(GL_BLEND);

        // --- resolve to transmittance --------------------------------------
        // The depth clear is 1.0, which is exactly "fully transmitting", so a
        // cascade with no translucent caster needs no special case and the
        // array's white CLAMP_TO_BORDER agrees with it outside the footprint.
        // A failed layer bind leaves every LATER layer uncleared, holding
        // undefined glTexImage3D content that the lookup would read as a
        // transmittance -- so this reports failure rather than returning true
        // over a partial build, exactly as shadow_build_msm does.
        if (!begin_depth_layer(ss->cascade_fbo, ss->shadow_map_array, TSM_LAYER(cc, c, 0), size)) {
            ok = false;
            break;
        }
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_ALWAYS);
        glDepthMask(GL_TRUE);
        glUseProgram(ss->tsm_resolve_program->id);
        uniform_set_int(ss->tsm_resolve_program->uniforms, "absorbance", 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, ss->tsm_scratch);
        draw_fullscreen_quad(ss->tsm_quad_vao);
        glBindTexture(GL_TEXTURE_2D, 0);

        // --- nearest translucent depth -------------------------------------
        // The ordinary depth program and the ordinary depth test: the hardware
        // min-blends for free, so this layer needs no shader of its own.
        if (!begin_depth_layer(ss->cascade_fbo, ss->shadow_map_array, TSM_LAYER(cc, c, 1), size)) {
            ok = false;
            break;
        }
        glDepthFunc(GL_LESS);
        // The fullscreen resolve above bound its own program and its own VAO; the walk forgets
        // what it tracked, so it binds its own again.
        _draw_shadow_items(ss, scene->draw_list, false, &state, SHADOW_CASTERS_TRANSLUCENT,
                           &tsm_cull, engine, scene, matrix);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(0);
    glClearColor(clear_was[0], clear_was[1], clear_was[2], clear_was[3]);
    glBlendFuncSeparate((GLenum)blend_src_rgb, (GLenum)blend_dst_rgb, (GLenum)blend_src_a,
                        (GLenum)blend_dst_a);
    glDepthFunc(GL_LESS);
    if (blend_was)
        glEnable(GL_BLEND);
    else
        glDisable(GL_BLEND);
    if (cull_was)
        glEnable(GL_CULL_FACE);
    else
        glDisable(GL_CULL_FACE);
    if (depth_was)
        glEnable(GL_DEPTH_TEST);
    else
        glDisable(GL_DEPTH_TEST);
    return ok;
}

void render_shadow_depth_pass(Engine* engine, Scene* scene) {
    if (!engine || !scene || !scene->shadow_system)
        return;

    // This pass runs BEFORE the camera passes, so it is usually the one that
    // flattens the graph; the stamp makes the camera pass reuse what this built
    // rather than build a second time -- unless the app mutated the graph in
    // between, which the epoch half of the stamp catches.
    engine_build_draw_list((Engine*)engine, scene);

    ShadowSystem* ss = scene->shadow_system;

    // Classify every light in one pass before any GL. Both shadow indices are
    // reset for EVERY light, including -1: a light that stops casting must not
    // keep pointing at a slot another light now owns. Directionals are counted
    // (capped, since the cascade arrays are sized by MAX_SHADOW_LIGHTS) and get
    // their slot assigned later by the cascade fit; the punctual types get a
    // layer here. This is punctual DEMAND, not supply -- punctual_layer_count
    // only rises to cover layers the pass actually renders below, so a failure
    // anywhere between here and there leaves the shader's bound at 0.
    ss->directional_count = 0;
    ss->punctual_layer_count = 0;
    ss->punctual_light_layers = 0;
    // Cleared up front so every early return below leaves the lookup on the
    // depth array. Like punctual_layer_count above, this is what the pass
    // actually produced, not what was asked for.
    ss->msm_built = false;
    // Same latch, same reason: every early return below must leave the lookup
    // reading occlusion alone rather than an array that was never filled.
    ss->tsm_built = false;
    ss->tsm_live = false;
    ss->tile_faces_drawn = 0;
    ss->mover_faces_copied = 0;
    ss->mover_faces_whole = 0;
    ss->tile_mover_list = NULL;
    int punctual_needed = 0;
    const Light* pool_overflow = NULL;
    const Light* dir_overflow = NULL;
    const Light* rangeless = NULL;

    for (size_t i = 0; i < scene->light_count; ++i) {
        Light* light = scene->lights[i];
        if (!light)
            continue;
        light->shadow_map_index = -1;
        light->shadow_layer = -1;
        light->shadow_tile = -1;
        if (!light->cast_shadows || shadow_light_takes_tiles(light))
            continue;
        if (light_wants_tiles(light) && !rangeless)
            rangeless = light;

        if (light->type == LIGHT_DIRECTIONAL) {
            if (ss->directional_count < MAX_SHADOW_LIGHTS)
                ss->directional_count++;
            else if (!dir_overflow)
                dir_overflow = light;
            continue;
        }
        const int want = punctual_layers_for(light);
        if (want == 0)
            continue;

        // A light takes all its faces or none: a cube short of one is a light with
        // holes in it, which is worse than one that does not cast. A later light
        // needing fewer may still fit.
        if (punctual_needed + want > MAX_PUNCTUAL_SHADOW_LAYERS) {
            if (!pool_overflow)
                pool_overflow = light;
            continue;
        }
        light->shadow_layer = punctual_needed;
        punctual_needed += want;
    }
    ss->punctual_light_layers = punctual_needed;

    // Latched so a misconfigured scene reports once rather than every frame,
    // and re-arms if the overflow clears. Named, because the alternative is a
    // light that silently stops casting and reads as a shading bug.
    if (pool_overflow && !ss->punctual_pool_warned) {
        log_warn("Punctual shadow pool full (%d layers): '%s' will not cast, nor will any later "
                 "caster needing more layers than are left",
                 MAX_PUNCTUAL_SHADOW_LAYERS,
                 pool_overflow->name ? pool_overflow->name : "unnamed light");
    }
    ss->punctual_pool_warned = pool_overflow != NULL;

    // Same latch for the cascade slots, which run out one light sooner than the
    // UBO does: it carries LC_MAX_DIR_LIGHTS directionals and there are only
    // MAX_SHADOW_LIGHTS slots. The overflowing light still lights the scene at
    // full strength -- the shader leaves its shadow term at 1.0 -- so without
    // this it is indistinguishable from one that never asked to cast. Which
    // light loses is scene->lights order, so the name is the whole point.
    if (dir_overflow && !ss->dir_slot_warned) {
        log_warn(
            "Directional shadow slots full (%d casters): '%s' and any further caster will not cast",
            MAX_SHADOW_LIGHTS, dir_overflow->name ? dir_overflow->name : "unnamed light");
    }
    ss->dir_slot_warned = dir_overflow != NULL;

    // A cached light with no range has no far plane for its faces, so it is drawn into the
    // pool every frame instead -- which still casts, and costs a traversal a face a frame.
    if (rangeless && !ss->tile_range_warned) {
        log_warn("'%s' asks for a cached shadow and has no range; drawn every frame instead",
                 rangeless->name ? rangeless->name : "unnamed light");
    }
    ss->tile_range_warned = rangeless != NULL;

    // The blocks shadow_tiles_update assigned this frame, before any capture: every depth pass
    // of the frame draws the same ones, and the region is laid out for them before any is drawn,
    // so every cell is in the array it is drawn into.
    const int tiled = ss->tile_held;
    if (tiled > 0 && tile_reference_count(ss) == 0) {
        tiles_note_changes(ss, engine, scene, engine->total_frames);
        tiles_take_stores(ss, engine);
    }
    if (ss->tile_block_count > 0)
        tiles_layout(ss, scene, punctual_needed);

    // Clamp the runtime count into the compile-time ceiling: the splits and
    // matrix arrays are sized by SHADOW_CASCADES and the count is writable
    // from the GUI. Cascade fitting needs the camera; without one, fall
    // back to the classic scene-fit single map.
    if (ss->cascade_count < 1)
        ss->cascade_count = 1;
    if (ss->cascade_count > SHADOW_CASCADES)
        ss->cascade_count = SHADOW_CASCADES;
    if (!engine->camera)
        ss->cascade_count = 1;

    // Grow the array when the cascade count exceeds its layer capacity. A
    // larger array serves any smaller count (layers stride by the runtime
    // count from 0), so shrinking never reallocates -- this keeps the
    // probe's capture-time force to count 1 realloc-free.
    if (ss->initialized && ss->allocated_cascades < ss->cascade_count) {
        free_shadow_map_array(ss);
    }
    // The transmittance block changes the array's layer COUNT, so toggling it
    // rebuilds. Not grow-only like the cascades: turning it off should hand the
    // VRAM back, and it is the largest array the renderer allocates.
    if (ss->initialized && ss->tsm_allocated != ss->tsm_enabled) {
        free_shadow_map_array(ss);
        // The scratch is NOT part of the array, so the line above does not
        // reach it -- without this, turning the feature off hands back the
        // layers and keeps a 16 MB accumulation target until shutdown.
        free_tsm_resources(ss);
    }

    // Always initialize the shadow map array texture (needed for sampler2DArray in shader)
    if (!ss->initialized) {
        if (init_shadow_map_array(ss) != 0)
            return;
    }

    // Early exit if nothing casts - but the array texture is already initialized.
    // A punctual caster keeps the pass alive even with no directional casters.
    if (ss->directional_count == 0 && punctual_needed == 0 && tiled == 0)
        return;

    // Now get the depth program for shadow rendering
    if (!ss->depth_program) {
        ss->depth_program = engine_get_program(engine, "shadow_depth");
        if (!ss->depth_program) {
            return;
        }
    }

    // Cascade split depths (count > 1): the practical lambda mix of
    // logarithmic (resolution where the eye is) and uniform (coverage)
    // splits over [camera near, shadow distance]
    int cc = ss->cascade_count;
    CascadeCamera cam = {0};
    if (cc > 1) {
        const Camera* camera = engine->camera;
        glm_vec3_copy((float*)camera->position, cam.position);
        // World-space view forward from the view matrix's third row
        cam.forward[0] = -engine->view_matrix[0][2];
        cam.forward[1] = -engine->view_matrix[1][2];
        cam.forward[2] = -engine->view_matrix[2][2];
        glm_vec3_normalize(cam.forward);
        cam.fov_radians = camera->fov_radians;
        cam.aspect_ratio = camera->aspect_ratio;
        cam.ortho_height = camera_ortho_height(camera);

        float cam_near = camera->near_clip;
        // Unset DERIVES NOTHING, and that is a measurement rather than caution.
        // Deriving it from ortho_size was tried: it takes forest's cascade 1
        // from a 967.8-unit box to 255.9 against the outermost's 1000, which is
        // the separation this field exists for -- and it regresses `pillar-msm`
        // from 0.0000 to 0.1757, light leaking into a thin caster's band.
        //
        // The cause is not the slicing, it is the RATIO the fit ends up with.
        // compute_cascade_light_space_matrix spends [0.1, 2*radius + scene_pad]
        // of depth on a box 2*radius wide, so depth-per-unit-of-extent is
        // 1 + scene_pad/(2*radius) -- and scene_pad is far_plane * 0.5, fixed.
        // Tightening a slice therefore shrinks BOTH, but not equally: on
        // dir_shadow that ratio goes 1.96 to 12.7, and four moments cannot hold
        // the second. Sizing the pad to what can actually cast into the slice
        // is the prerequisite for any tightening default. Until then this is a
        // knob an app sets against its own content.
        const float fallback_dist = fminf(ss->far_plane, camera->far_clip);
        float shadow_dist = ss->shadow_distance > 0.0f
                                ? fminf(ss->shadow_distance, camera->far_clip)
                                : fallback_dist;
        // Only the SET arm can be rescued -- the unset one already is the
        // fallback, so re-assigning it there would be a no-op guarding nothing.
        // A split ladder that starts at or below the near plane makes
        // powf(dist/near, t) collapse to zero and takes the texel snap's
        // 2*radius/map_size with it.
        if (ss->shadow_distance > 0.0f && shadow_dist <= cam_near) {
            log_error("Shadow: shadow_distance %.3f is inside the near plane %.3f; using %.3f",
                      (double)ss->shadow_distance, (double)cam_near, (double)fallback_dist);
            shadow_dist = fallback_dist;
        }
        // Nothing writes cascade_lambda yet, so this cannot fire today -- but it
        // is a public field, and outside [0,1] the blend EXTRAPOLATES: a split
        // can land below its predecessor while the last stays pinned to
        // shadow_dist, which hands the fit a slice whose far is behind its near
        // and centres a cascade box behind the camera.
        const float lambda = glm_clamp(ss->cascade_lambda, 0.0f, 1.0f);
        for (int c = 0; c < cc; c++) {
            float t = (float)(c + 1) / (float)cc;
            float uniform_split = cam_near + (shadow_dist - cam_near) * t;
            float log_split = cam_near * powf(shadow_dist / cam_near, t);
            ss->cascade_splits[c] = lambda * log_split + (1.0f - lambda) * uniform_split;
        }
    }

    // Fit each caster's cascades. Count 1 takes the classic scene-fit path
    // VERBATIM (the byte-identity bridge); count > 1 fits each view slice's
    // bounding sphere with texel snapping.
    size_t slot = 0;
    for (size_t i = 0; i < scene->light_count && slot < MAX_SHADOW_LIGHTS; ++i) {
        Light* light = scene->lights[i];
        if (!light)
            continue;

        if (light->type == LIGHT_DIRECTIONAL && light->cast_shadows) {
            if (cc == 1) {
                compute_directional_light_space_matrix(light->direction, ss->scene_center,
                                                       ss->ortho_size, ss->near_plane,
                                                       ss->far_plane, ss->cascade_matrices[slot]);
                ss->cascade_params[slot][0] = 2.0f * ss->ortho_size;
                ss->cascade_params[slot][1] = ss->near_plane;
                ss->cascade_params[slot][2] = ss->far_plane;
            } else {
                float slice_near = engine->camera->near_clip;
                // The scene pad covers casters toward the light outside the
                // slice; the legacy fit's eye sat at far/2, reuse that scale
                float scene_pad = ss->far_plane * 0.5f;
                float legacy_width = 2.0f * ss->ortho_size;
                // Camera-fit cascades sharpen the near slices; the OUTERMOST
                // cascade is the classic scene-fit map, camera-independent
                // and complete for every caster in the scene by construction.
                // Anything the tight frustum-fit boxes clip falls back to it,
                // so no shadow can ever end at a boundary that moves with the
                // camera -- worst case equals the classic single-map look.
                for (int c = 0; c < cc - 1; c++) {
                    int layer = (int)slot * cc + c;
                    vec4* params = &ss->cascade_params[layer];
                    compute_cascade_light_space_matrix(light->direction, &cam, slice_near,
                                                       ss->cascade_splits[c], scene_pad,
                                                       ss->default_map_size, ss->scene_center,
                                                       ss->cascade_matrices[layer], *params);
                    slice_near = ss->cascade_splits[c];
                }
                int last = (int)slot * cc + (cc - 1);
                compute_directional_light_space_matrix(light->direction, ss->scene_center,
                                                       ss->ortho_size, ss->near_plane,
                                                       ss->far_plane, ss->cascade_matrices[last]);
                ss->cascade_params[last][0] = legacy_width;
                ss->cascade_params[last][1] = ss->near_plane;
                ss->cascade_params[last][2] = ss->far_plane;
            }
            light->shadow_map_index = (int)slot;
            slot++;
        }
    }

    GLint prev_viewport[4];
    glGetIntegerv(GL_VIEWPORT, prev_viewport);

    // One depth policy for every shadow map, cascade and punctual alike: store
    // the surface NEAREST the light (back-face cull) and pay for acne with a
    // slope-scaled polygon offset plus the receiver-side bias in the lookups.
    //
    // Far-side storage cannot be biased into correctness, for the reason spec
    // 10.3 measured on the punctual path: a caster's far side near its
    // silhouette -- and everywhere near a ground contact -- rasterizes as
    // slivers seen almost edge-on. A sliver misses texel centres, those texels
    // keep the clear value, and a comparison against "nothing here" reads lit.
    // No bias of either sign reaches a texel with no data in it. On the
    // cascade path the cost was a resting sphere losing its shadow almost
    // entirely (the dir_shadow_fixture hole gate).
    //
    // The cascades held out on far-side the longest because an earlier flip
    // was tried with the polygon offset ALONE and correctly reverted: an
    // ortho map over a whole scene puts every receiver into its own map, and
    // at low sun the ground answered with banded acne no offset could cover.
    // The receiver-plane bias in the cascade lookup is the other half of the
    // remedy; the two land as a pair, and the fixture's acne gate at 10
    // degrees is the regression guard for exactly that history.
    glCullFace(GL_BACK);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(SHADOW_DEPTH_SLOPE_BIAS, SHADOW_DEPTH_CONSTANT_BIAS);

    // Resolved HERE, after the caster classification that tells it whether any
    // directional light casts, and before the first draw that depends on it.
    // Withholding a translucent caster from the depth pass is legal only when
    // the transmittance map will actually exist to represent it, and only for
    // the slots that HAVE one -- TSM_SLOTS is 1, so casters 1 and 2 keep the
    // solid shadow they have always cast rather than losing it for nothing.
    ss->tsm_live = shadow_tsm_prepare(ss, engine);

    // Tracking starts AFTER tsm_prepare, which resolves programs by name and
    // builds a quad VAO. Claiming state across a callee that binds is how the
    // tracker goes wrong, and ordering costs nothing where a remembered reset
    // would have to be remembered.
    SubmitState state = {0};

    profiler_scope_begin_if(engine->profiler, ss->directional_count > 0, "shadow cascades");
    for (size_t i = 0; i < ss->directional_count; ++i) {
        const ShadowCasterSet set =
            (ss->tsm_live && i < TSM_SLOTS) ? SHADOW_CASTERS_OPAQUE_TSM : SHADOW_CASTERS_OPAQUE;
        for (int c = 0; c < cc; ++c) {
            size_t layer = i * (size_t)cc + (size_t)c;
            begin_shadow_pass(ss, layer);
            draw_shadow_layer(ss, scene, scene->draw_list, ss->cascade_matrices[layer], &state, set,
                              engine);
        }
    }
    profiler_scope_end(engine->profiler);

    // Punctual maps (for surface shadows + the volumetric beam), one layer per
    // caster. Reuses the allocation (a no-op once it is large enough), the
    // bound depth program, and the depth policy set above.
    const bool punctual_ready =
        (punctual_needed > 0 || tiled > 0) &&
        init_punctual_shadow_array(ss, punctual_capacity(ss, scene), punctual_needed) == 0;
    if (punctual_ready && punctual_needed > 0) {
        profiler_scope_begin(engine->profiler, "shadow punctual");
        for (size_t i = 0; i < scene->light_count; ++i) {
            Light* light = scene->lights[i];
            if (!light || light->shadow_layer < 0)
                continue;

            int layers = compute_punctual_matrices(
                light, ss, ies_library_at(scene->ies_library, light->ies_profile),
                &ss->punctual_matrices[light->shadow_layer]);

            // One scene traversal per layer -- this loop is the frame cost the
            // pool ceiling caps, and a point light pays six times a spot's.
            for (int f = 0; f < layers; f++) {
                int layer = light->shadow_layer + f;
                if (!begin_punctual_shadow_pass(ss, layer))
                    continue;
                // Deliberately NOT opaque_set: the transmittance map covers
                // the directional cascades only (unit 15 has no room for a
                // second lookup), so withholding translucent casters here
                // would take away the solid shadow without replacing it.
                draw_shadow_layer(ss, scene, scene->draw_list, ss->punctual_matrices[layer], &state,
                                  SHADOW_CASTERS_OPAQUE, engine);
                // Layers are handed out in increasing order, so the last one
                // drawn is the bound the shader needs
                ss->punctual_layer_count = layer + 1;
            }
        }
        profiler_scope_end(engine->profiler);
    }

    // The cached lights' tiles, under the same program and depth policy.
    if (punctual_ready && tiled > 0 && tile_reference_count(ss) > 0) {
        render_shadow_reference(ss, engine, scene, &state);
    } else if (punctual_ready && tiled > 0) {
        profiler_scope_begin(engine->profiler, "shadow tiles");
        render_shadow_tiles(ss, engine, scene, &state);
        profiler_scope_end(engine->profiler);
        profiler_scope_begin_if(engine->profiler, tiles_any_dynamic(ss), "shadow movers");
        render_shadow_movers(ss, engine, scene, &state);
        profiler_scope_end(engine->profiler);
        publish_shadow_tiles(ss);
    }

    glDisable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(0.0f, 0.0f);
    glUseProgram(0);
    // The walk stopped unbinding after each draw, so the last caster's is still
    // bound. Released here for the same reason the scene passes release theirs:
    // a pass should not hand its successor state it never asked for.
    glBindVertexArray(0);

    // Ahead of the viewport restore, so the one restore below covers the resolve
    // too rather than the resolve querying back a value this function already
    // holds. It disables cull for its own draws, so the cull face this pass set
    // and never saved cannot reach it either.
    // Gated on the same flag the callee's first early-return clause tests. Its
    // other two clauses (no array, no layers) are not visible from here, so a
    // frame that trips those still files a zero row -- the alternative is
    // copying a three-clause guard that would drift, which it already did once.
    profiler_scope_begin_if(engine->profiler, ss->msm_enabled, "shadow msm");
    ss->msm_built = shadow_build_msm(ss, engine);
    profiler_scope_end(engine->profiler);

    profiler_scope_begin_if(engine->profiler, ss->tsm_live, "shadow tsm");
    ss->tsm_built = ss->tsm_live && shadow_build_tsm(ss, engine, scene);
    profiler_scope_end(engine->profiler);

    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
}

// RAIN_OCCLUSION_REACH and RAIN_EXPOSED_BIAS: the map's depth range and the lookup's
// bias, shared with rain_occlusion.glsl, which has to invert the one and apply the other.
// A tower above and a cellar below both fit in the reach, and 500 m in 24 bits is 30
// microns a step, so it costs no resolution worth having.
#include "../shaders/include/rain_constants.glsl"

// Where a world point falls on the rain's map: its uv over the map and its depth along the
// rain, each 0..1. The map fills the layer's corner, so the unfolded matrix addresses it
// directly. False off the map, which is open sky.
static bool rain_map_point(const ShadowSystem* ss, const vec3 point, float* u, float* v, float* z) {
    vec4 p;
    glm_mat4_mulv((vec4*)ss->rain_matrix, (vec4){point[0], point[1], point[2], 1.0f}, p);
    *u = p[0] * 0.5f + 0.5f;
    *v = p[1] * 0.5f + 0.5f;
    *z = p[2] * 0.5f + 0.5f;
    return *u >= 0.0f && *u < 1.0f && *v >= 0.0f && *v < 1.0f;
}

// Whether rain reaches a point at depth `z` where the map holds `map`: rainOpenAt's test.
static bool rain_map_open(float z, float map) {
    return z <= map + RAIN_EXPOSED_DEPTH_BIAS;
}

// Whether a slot of the ring has been issued long enough ago to answer.
static bool rain_ask_answered(const ShadowSystem* ss) {
    return ss->rain_ask_passes > SHADOW_RAIN_ASK_LATENCY;
}

/*
 * The cover at the asked point: retire the slot issued SHADOW_RAIN_ASK_LATENCY passes ago, then
 * queue this pass's texel into it. No fence, for water's surface query's reason: mapping a slot
 * whose read has not landed stalls rather than answering early, so the latency decides how
 * often that happens and never what the answer is. The point's own depth is kept per slot, so
 * a listener that moved between is answered for where it was.
 *
 * Read through the punctual framebuffer, which the map was just drawn through with the layer
 * still attached.
 */
static void _rain_ask_pass(ShadowSystem* ss) {
    if (!ss->rain_ask_set || ss->rain_layer < 0 || !ss->punctual_map_array)
        return;
    if (!ss->rain_ask_pbo[0]) {
        glGenBuffers(SHADOW_RAIN_ASK_LATENCY, ss->rain_ask_pbo);
        for (int i = 0; i < SHADOW_RAIN_ASK_LATENCY; i++) {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, ss->rain_ask_pbo[i]);
            glBufferData(GL_PIXEL_PACK_BUFFER, sizeof(float), NULL, GL_STREAM_READ);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }
    const int slot = (int)(ss->rain_ask_passes % SHADOW_RAIN_ASK_LATENCY);
    if (ss->rain_ask_passes >= SHADOW_RAIN_ASK_LATENCY) {
        // Off the map, or a map texel with nothing in it, is open sky.
        float map = 1.0f;
        if (ss->rain_ask_issued_valid[slot]) {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, ss->rain_ask_pbo[slot]);
            const float* px =
                glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, sizeof(float), GL_MAP_READ_BIT);
            if (px) {
                map = *px;
                glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            }
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        }
        ss->rain_ask_open = rain_map_open(ss->rain_ask_issued_depth[slot], map) ? 1.0f : 0.0f;
    }

    float u, v;
    const bool inside =
        rain_map_point(ss, ss->rain_ask_point, &u, &v, &ss->rain_ask_issued_depth[slot]);
    ss->rain_ask_issued_valid[slot] = inside;
    if (inside) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, ss->punctual_fbo);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, ss->rain_ask_pbo[slot]);
        glReadPixels((GLint)(u * (float)RAIN_OCCLUSION_SIZE),
                     (GLint)(v * (float)RAIN_OCCLUSION_SIZE), 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT,
                     NULL);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }
    ss->rain_ask_passes++;
}

void shadow_render_rain_layer(Engine* engine, Scene* scene) {
    if (!engine || !scene || !scene->shadow_system)
        return;
    ShadowSystem* ss = scene->shadow_system;
    ss->rain_layer = -1;
    const Rain* rain = scene->rain;
    if (!rain_active(rain) || !engine->camera || !(rain->occlusion_extent > 0.0f)) {
        // No map, so no answer -- and the ring starts over, so the first answer once it rains
        // again is not a slot issued before it stopped.
        ss->rain_ask_passes = 0;
        return;
    }

    const int layer = rain_layer_index(ss);
    if (init_punctual_shadow_array(ss, punctual_capacity(ss, scene), layer) != 0)
        return;
    if (!ss->depth_program) {
        ss->depth_program = engine_get_program(engine, "shadow_depth");
        if (!ss->depth_program)
            return;
    }
    // A stamp compare when the shadow pass built it; the build when shadows are off.
    engine_build_draw_list(engine, scene);

    /*
     * Looking along the rain, from the origin, so the camera's position in that view
     * can be SNAPPED to the texel grid before the box is placed around it. The grid is
     * then fixed in the world rather than riding the camera, and cover does not crawl
     * along a roof edge as the player walks.
     */
    vec3 dir = GLM_VEC3_ZERO_INIT, up = GLM_VEC3_ZERO_INIT;
    glm_vec3_copy((float*)rain->travel, dir);
    light_space_up(dir, up);
    mat4 view, proj;
    glm_lookat((vec3){0.0f, 0.0f, 0.0f}, dir, up, view);
    vec4 c;
    const float* eye = engine->camera->position;
    glm_mat4_mulv(view, (vec4){eye[0], eye[1], eye[2], 1.0f}, c);
    const float half = 0.5f * rain->occlusion_extent;
    const float texel = rain->occlusion_extent / (float)RAIN_OCCLUSION_SIZE;
    const float cx = floorf(c[0] / texel) * texel;
    const float cy = floorf(c[1] / texel) * texel;
    glm_ortho(cx - half, cx + half, cy - half, cy + half, -c[2] - RAIN_OCCLUSION_REACH,
              -c[2] + RAIN_OCCLUSION_REACH, proj);
    glm_mat4_mul(proj, view, ss->rain_matrix);
    const float k = rain_fold_lookup(ss);
    // The softness in metres, as lookup uv: a fraction of the extent, carried into the corner.
    //
    // NEVER MORE THAN A TEXEL between taps. Wider, each row of the 3x3 finds the same thin
    // occluder at a different offset, and a lamp arm narrower than a texel -- a one-texel line
    // in the map -- leaves three dry stripes on the pavement a tap-spacing apart. A softer edge
    // than a texel buys needs a filtered map rather than wider taps.
    const float texel_uv = 1.0f / (float)ss->punctual_map_size;
    ss->rain_uv_per_metre = k / rain->occlusion_extent;
    ss->rain_cover_spread =
        fminf(fmaxf(rain->occlusion_softness, 0.0f) * ss->rain_uv_per_metre, texel_uv);

    GLint prev_viewport[4];
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    profiler_scope_begin(engine->profiler, "rain occlusion");
    // The shadow pass's depth policy, for its reason: store the surface nearest the
    // sky and let the lookup's bias absorb the rest.
    glCullFace(GL_BACK);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(RAIN_MAP_SLOPE_BIAS, RAIN_MAP_CONSTANT_BIAS);
    SubmitState state = {0};
    if (begin_depth_layer(ss->punctual_fbo, ss->punctual_map_array, layer, ss->punctual_map_size)) {
        glViewport(0, 0, RAIN_OCCLUSION_SIZE, RAIN_OCCLUSION_SIZE);
        // Glass is in the opaque set, and should be: a glass roof keeps the rain off.
        draw_shadow_layer(ss, scene, scene->draw_list, ss->rain_matrix, &state, SHADOW_CASTERS_RAIN,
                          engine);
        ss->rain_layer = layer;
    }
    glDisable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(0.0f, 0.0f);
    glUseProgram(0);
    glBindVertexArray(0);
    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
    _rain_ask_pass(ss);
    profiler_scope_end(engine->profiler);
}

void shadow_rain_cover_ask(ShadowSystem* ss, const vec3 point) {
    if (!ss)
        return;
    glm_vec3_copy((float*)point, ss->rain_ask_point);
    ss->rain_ask_set = true;
}

bool shadow_rain_cover_answer(const ShadowSystem* ss, float* open) {
    if (!ss || !rain_ask_answered(ss))
        return false;
    if (open)
        *open = ss->rain_ask_open;
    return true;
}

// A greyscale PPM of the map, stretched over the depths it actually holds: the whole
// scene spans a few metres of a 500 m range, which is flat grey unstretched. Cleared
// texels -- nothing there -- stay white.
static void _write_rain_map(const float* depth, int n, const char* path) {
    float lo = 1.0f, hi = 0.0f;
    for (int i = 0; i < n * n; i++) {
        if (depth[i] < 1.0f) {
            lo = fminf(lo, depth[i]);
            hi = fmaxf(hi, depth[i]);
        }
    }
    FILE* f = fopen(path, "wb");
    if (!f) {
        log_warn("rain-probe: cannot write '%s'", path);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", n, n);
    const float span = hi > lo ? hi - lo : 1.0f;
    for (int y = n - 1; y >= 0; y--) { // GL rows run bottom-up
        for (int x = 0; x < n; x++) {
            const float d = depth[y * n + x];
            const unsigned char g =
                d >= 1.0f ? 255 : (unsigned char)(40.0f + 200.0f * (d - lo) / span);
            const unsigned char px[3] = {g, g, g};
            fwrite(px, 1, 3, f);
        }
    }
    fclose(f);
}

void shadow_rain_probe(const ShadowSystem* ss, const vec3* points, int count,
                       const char* image_path) {
    if (!ss || ss->rain_layer < 0 || !ss->punctual_map_array) {
        printf("rain-probe cover present=0\n");
        return;
    }
    const int n = RAIN_OCCLUSION_SIZE;
    float* depth = malloc((size_t)n * (size_t)n * sizeof(float));
    if (!depth)
        return;
    glBindFramebuffer(GL_FRAMEBUFFER, ss->punctual_fbo);
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, ss->punctual_map_array, 0,
                              ss->rain_layer);
    glReadPixels(0, 0, n, n, GL_DEPTH_COMPONENT, GL_FLOAT, depth);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    printf("rain-probe cover present=1 layer=%d size=%d edge=%d bias=%.9g\n", ss->rain_layer, n,
           ss->punctual_map_size, (double)RAIN_EXPOSED_DEPTH_BIAS);
    for (int i = 0; i < count; i++) {
        float u, v, z;
        const bool inside = rain_map_point(ss, points[i], &u, &v, &z);
        printf("rain-probe exposure x=%.9g y=%.9g z=%.9g inside=%d", (double)points[i][0],
               (double)points[i][1], (double)points[i][2], inside ? 1 : 0);
        if (inside) {
            const float map = depth[(int)(v * (float)n) * n + (int)(u * (float)n)];
            printf(" map=%.9g depth=%.9g exposed=%d", (double)map, (double)z,
                   rain_map_open(z, map) ? 1 : 0);
        }
        printf("\n");
    }
    if (image_path)
        _write_rain_map(depth, n, image_path);
    free(depth);
}

void shadow_tiles_probe(const ShadowSystem* ss, const Scene* scene) {
    if (!ss || !scene) {
        printf("tiles-probe present=0\n");
        return;
    }
    const int edge = ss->punctual_map_size;
    const int store_cells = ss->tile_store_first >= 0 ? SHADOW_TILE_STORE_CELLS : 0;
    int store_used = 0;
    for (int b = 0; b < ss->tile_block_count; ++b) {
        for (int f = 0; ss->tile_blocks[b].light && f < 6 * SHADOW_TILE_VIEWS; ++f)
            store_used += ss->tile_blocks[b].store_cell[f] >= 0 ? 1 : 0;
    }
    printf("tiles-probe region base=%d layers=%d edge=%d per_layer=%d allocated=%d "
           "generation=%u blocks=%d cells=%d faces_drawn=%d movers=%d mover_faces_drawn=%d "
           "mover_faces_copied=%d mover_faces_whole=%d store_cells=%d store_used=%d "
           "kept_changes=%d reference=%d\n",
           ss->tile_base_layer, ss->tile_layers, edge, edge > 0 ? tiles_per_layer(edge) : 0,
           ss->punctual_allocated_layers, ss->tile_generation, ss->tile_block_count,
           tile_cells_used(ss), ss->tile_faces_drawn, ss->tile_mover_count,
           ss->mover_faces_copied + ss->mover_faces_whole, ss->mover_faces_copied,
           ss->mover_faces_whole, store_cells, store_used, ss->tile_seen_changes,
           tile_reference_count(ss));
    for (int b = 0; b < ss->tile_block_count; ++b) {
        const ShadowTileBlock* block = &ss->tile_blocks[b];
        if (!block->light)
            continue;
        const Light* light = block->light;
        int kept = 0, dynamic = 0, stored = 0;
        for (int f = 0; f < 6 * block->views; ++f) {
            kept += tile_face_in(block->valid, f) ? 1 : 0;
            dynamic += tile_face_in(block->dynamic, f) ? 1 : 0;
            stored += block->store_cell[f] >= 0 ? 1 : 0;
        }
        printf(
            "tiles-probe block=%d light=%s stored_faces=%d dynamic=%d first=%d cells=%d views=%d "
            "faces=%d whole=%d published=%d radius=%.9g length=%.9g axis=%.9g,%.9g,%.9g "
            "centre=%.9g,%.9g,%.9g segment=%.9g,%.9g,%.9g drawn_radius=%.9g drift=%.9g "
            "near=%.9g far=%.9g current=%d\n",
            b, light->name ? light->name : "unnamed", stored, dynamic,
            tile_block_first_cell(ss, edge, b), block->cells, block->views, kept,
            tile_block_whole(block) ? 1 : 0, light->shadow_tile, (double)light->source_radius,
            (double)light->source_length, (double)light->direction[0], (double)light->direction[1],
            (double)light->direction[2], (double)block->centre[0], (double)block->centre[1],
            (double)block->centre[2], (double)block->segment[0], (double)block->segment[1],
            (double)block->segment[2], (double)block->radius,
            (double)glm_vec3_distance((float*)block->centre, (float*)light->global_position),
            (double)block->near_plane, (double)block->far_plane,
            block->generation == ss->tile_generation ? 1 : 0);
        for (int v = 0; v < block->views && edge > 0; ++v) {
            vec3 origin = GLM_VEC3_ZERO_INIT;
            tile_view_origin(block, v, origin);
            printf("tiles-probe   view=%d origin=%.9g,%.9g,%.9g", v, (double)origin[0],
                   (double)origin[1], (double)origin[2]);
            for (int f = 0; f < 6; ++f) {
                int layer, x, y;
                tile_cell_at(tile_block_first_cell(ss, edge, b) + 6 * v + f, edge, &layer, &x, &y);
                printf(" face%d=%d:%d:%d", f, layer, x, y);
            }
            printf("\n");
        }
    }
}

bool shadow_tiles_map(const ShadowSystem* ss, const Light* light, const char* path) {
    const int b = ss && light ? tile_block_of(ss, light) : -1;
    if (b < 0 || !ss->punctual_map_array || !tile_block_whole(&ss->tile_blocks[b])) {
        log_warn("tile-map: '%s' has no complete block of cached faces",
                 light && light->name ? light->name : "(none)");
        return false;
    }
    const ShadowTileBlock* block = &ss->tile_blocks[b];
    const int n = SHADOW_TILE_SIZE;
    const int faces = 6 * block->views;
    float* depth = malloc((size_t)faces * (size_t)n * (size_t)n * sizeof(float));
    if (!depth)
        return false;
    const int edge = ss->punctual_map_size;
    glBindFramebuffer(GL_FRAMEBUFFER, ss->punctual_fbo);
    for (int f = 0; f < faces; ++f) {
        int layer, x, y;
        tile_cell_at(tile_block_first_cell(ss, edge, b) + f, edge, &layer, &x, &y);
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, ss->punctual_map_array, 0,
                                  layer);
        glReadPixels(x, y, n, n, GL_DEPTH_COMPONENT, GL_FLOAT, depth + (size_t)f * n * n);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    FILE* out = fopen(path, "wb");
    if (!out) {
        log_warn("tile-map: cannot write '%s'", path);
        free(depth);
        return false;
    }
    // Grey by metres along the face's axis, which a perspective depth is not: linearised
    // through the block's own planes, so every face reads on one scale. Two rows a view, the
    // first view on top.
    const float np = block->near_plane, fp = block->far_plane;
    fprintf(out, "P6\n%d %d\n255\n", 3 * n, 2 * block->views * n);
    for (int row = 0; row < 2 * block->views; ++row) {
        for (int y = n - 1; y >= 0; --y) { // GL rows run bottom-up
            for (int col = 0; col < 3; ++col) {
                const float* face = depth + (size_t)(row * 3 + col) * n * n;
                for (int x = 0; x < n; ++x) {
                    const float d = face[y * n + x];
                    const float z_ndc = 2.0f * d - 1.0f;
                    const float dist = 2.0f * np * fp / (fp + np - z_ndc * (fp - np));
                    const unsigned char g =
                        d >= 1.0f ? 255 : (unsigned char)(40.0f + 200.0f * fminf(dist / fp, 1.0f));
                    const unsigned char px[3] = {g, g, g};
                    fwrite(px, 1, 3, out);
                }
            }
        }
    }
    fclose(out);
    free(depth);
    return true;
}

// Flatten this frame's shadow casters + their lights into postfx's fog block.
// The consumer indexes its POSTFX_FOG_MAX_LIGHTS-sized arrays by the count
// published here, so the two slot capacities must never diverge.
_Static_assert(POSTFX_FOG_MAX_LIGHTS == MAX_SHADOW_LIGHTS,
               "postfx caster mirror must match the shadow slot count");
_Static_assert(POSTFX_FOG_CASCADES == SHADOW_CASCADES,
               "postfx cascade mirror must match the shadow cascade count");
_Static_assert(SHADOW_CASCADES <= 4, "cascadeSplits packs the split depths into a vec4");

void shadow_publish_to_postfx(const Scene* scene, PostFX* fx) {
    if (!fx)
        return;

    // Volumetric spot (the flashlight): the fog scatters its cone into a beam
    // shaft. Same light the depth pass renders, so in-scatter and shadow agree.
    // Published even with the shadow system off (an unshadowed beam still works).
    const Light* sp = scene_first_spot_light(scene);
    fx->fog_spot_enabled = sp != NULL;
    if (sp) {
        glm_vec3_copy((float*)sp->global_position, fx->fog_spot_pos);
        glm_vec3_scale((float*)sp->color, sp->intensity, fx->fog_spot_color);
        // Same packing the clustered path uses (light_cluster.c): x = 1/range^2
        // for getDistanceAtt's window, yz unused. The beam and the pool it casts
        // read one falloff.
        fx->fog_spot_atten[0] = sp->range > 0.0f ? 1.0f / (sp->range * sp->range) : 0.0f;
        fx->fog_spot_atten[1] = 0.0f;
        fx->fog_spot_atten[2] = 0.0f;
        fx->fog_spot_cos_inner = sp->cutOff;
        fx->fog_spot_cos_outer = sp->outerCutOff;
        // ...and the same for the angular half: a profiled spot's shaft has to
        // read the profile the floor pool reads, through the same frame, or the
        // beam and the pool turn the lamp different ways.
        fx->fog_spot_ies_profile = sp->ies_profile;
        light_emission_frame(sp, fx->fog_spot_dir, fx->fog_spot_up);
    }

    // The population no shadow map can serve: point and spot lights holding no
    // LIVE map, a layer or a cached tile. Counted BEFORE the directional early-out below,
    // because a scene lit only by practicals has no directional and this is the
    // whole reason it still needs a contact-shadow pass (spec 11.56). The cull
    // radius is the same test the cluster build applies -- a light that never
    // reaches epsilon is not a light the march can shadow.
    //
    // A SCENE count, not a visible one: the march itself walks the cluster list,
    // so a scene whose practicals are all off screen arms a pass that answers 1
    // everywhere. Counting the visible ones means reading the cluster build's own
    // tally, which a reflection-probe capture face overwrites with its own
    // frustum -- a bigger coupling than the frame it saves.
    fx->cs_mapless_lights = 0;
    for (size_t i = 0; scene && i < scene->light_count; i++) {
        const Light* l = scene->lights[i];
        if (!l || (l->type != LIGHT_POINT && l->type != LIGHT_SPOT))
            continue;
        if (!shadow_light_mapped(scene->shadow_system, l) && light_cull_radius(l) != 0.0f) {
            fx->cs_mapless_lights++;
        }
    }

    // Spot shadow (Phase 2): occludes the beam by geometry. Published
    // independently of the directional early-out below (works even with no
    // directional casters). Gated on `enabled` as well as on the light having a
    // layer: the depth pass does not run at all when the master switch is off,
    // so every index it maintains keeps the value it had when it last ran.
    fx->fog_spot_shadowed = false;
    fx->fog_punctual_shadow_maps = 0;
    ShadowSystem* ss = scene ? scene->shadow_system : NULL;
    if (ss && ss->enabled && sp && sp->shadow_layer >= 0 &&
        sp->shadow_layer < ss->punctual_layer_count && ss->punctual_map_array) {
        glm_mat4_copy((vec4*)ss->punctual_matrices[sp->shadow_layer], fx->fog_spot_light_space);
        fx->fog_punctual_shadow_maps = ss->punctual_map_array;
        fx->fog_spot_shadow_layer = sp->shadow_layer;
        fx->fog_spot_shadowed = true;
    }
    // The rain's cover rides the same array (spec 13.9), and like the surfaces' it is NOT
    // gated on `enabled`: switching shadows off does not put a roof over the street.
    fx->rain_cover_layer = -1;
    if (ss && ss->rain_layer >= 0 && ss->punctual_map_array) {
        fx->rain_cover_layer = ss->rain_layer;
        glm_mat4_copy(ss->rain_lookup, fx->rain_cover_matrix);
        fx->fog_punctual_shadow_maps = ss->punctual_map_array;
    }
    // And a cached light's faces (spec 13.16), which the medium finds through the lights
    // block. Asked of the same function the packing asks, so the array is bound for exactly
    // the lights that carry the marker the fog branches on.
    for (size_t i = 0; ss && ss->punctual_map_array && i < scene->light_count; i++) {
        if (shadow_live_tile(ss, scene->lights[i]) >= 0) {
            fx->fog_punctual_shadow_maps = ss->punctual_map_array;
            break;
        }
    }

    // Publishing count 0 with a zero array handle is the single "no
    // shadowed in-scatter" state consumers rely on: a nonzero count
    // guarantees the map array and every slot below it are valid.
    if (!ss || !ss->enabled || ss->directional_count == 0 || !ss->shadow_map_array ||
        !scene->lights) {
        fx->fog_light_count = 0;
        fx->fog_cascade_count = 1;
        fx->fog_shadow_map_array = 0;
        return;
    }

    int cc = ss->cascade_count;
    for (size_t i = 0; i < scene->light_count; i++) {
        Light* light = scene->lights[i];
        if (!light || light->shadow_map_index < 0 ||
            light->shadow_map_index >= POSTFX_FOG_MAX_LIGHTS) {
            continue;
        }
        int slot = light->shadow_map_index;
        // Layers stride by the runtime count (slot indices at count 1)
        for (int c = 0; c < cc; c++) {
            int layer = slot * cc + c;
            glm_mat4_copy(ss->cascade_matrices[layer], fx->fog_light_space[layer]);
        }
        glm_vec3_normalize_to(light->direction, fx->fog_light_dir[slot]);
        glm_vec3_scale(light->color, light->intensity, fx->fog_light_color[slot]);
    }
    fx->fog_light_count = (int)ss->directional_count;
    fx->fog_cascade_count = cc;
    fx->fog_shadow_map_array = ss->shadow_map_array;
    fx->fog_shadow_bias = ss->shadow_bias;
}
