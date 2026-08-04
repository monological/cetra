
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
#include "shadow.h"
#include "render.h"
#include "animation.h"
#include "wind.h"
#include "util.h"
#include "ext/log.h"

// A depth array texture plus the one FBO that renders into its layers. Both
// shadow arrays are built from here: they differ only in size and layer count.
// glTexImage3D, never glTexStorage3D -- that is GL 4.2 and this targets 4.1,
// the constraint mask_array.c writes down at its own allocation.
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

    glGenFramebuffers(1, fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void free_depth_array(GLuint* tex, GLuint* fbo) {
    gl_delete_texture(tex);
    gl_delete_fbo(fbo);
}

// Point the FBO at one layer and clear it for a depth-only pass. False means
// nothing was bound, so the caller must not draw -- the FBO is back to 0 and
// drawing would land in the default framebuffer.
static bool begin_depth_layer(GLuint fbo, GLuint tex, int layer, int size) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, tex, 0, layer);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        log_error("Shadow framebuffer incomplete: 0x%x", status);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return false;
    }

    glViewport(0, 0, size, size);
    glClear(GL_DEPTH_BUFFER_BIT);
    return true;
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
    system->ortho_size = 2000.0f;
    system->near_plane = 1.0f;
    system->far_plane = 7500.0f;
    system->shadow_map_array = 0;
    system->depth_program = NULL;
    system->initialized = false;
    system->enabled = true;
    system->pcss_enabled = false; // library default off; the app opts in
    system->pcss_softness = 1.0f;
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

    system->shadow_bias = 0.005f;

    return system;
}

void free_shadow_system(ShadowSystem* system) {
    if (!system)
        return;

    free_shadow_map_array(system);
    free_depth_array(&system->punctual_map_array, &system->punctual_fbo);

    free(system);
}

int init_shadow_map_array(ShadowSystem* system) {
    if (!system)
        return -1;

    if (system->shadow_map_array != 0)
        return 0;

    // Layers are count-strided (slot * cascade_count + cascade), sized to the
    // ACTIVE cascade count so the classic single-map default never pays the
    // 3x VRAM of a full 9-layer array; a count change rebuilds the array
    init_depth_array(&system->shadow_map_array, &system->cascade_fbo,
                     system->default_map_size, MAX_SHADOW_LIGHTS * system->cascade_count);

    system->allocated_cascades = system->cascade_count;
    system->initialized = true;
    return 0;
}

void free_shadow_map_array(ShadowSystem* system) {
    if (!system)
        return;

    free_depth_array(&system->shadow_map_array, &system->cascade_fbo);
    system->initialized = false;
}

// Largest power-of-two edge the VRAM budget affords for `layers` depth layers.
// Halving the size quarters the cost, so this walks down from the ceiling and
// stops at the first size that fits -- and never below the floor, since a map
// too coarse to resolve a silhouette is not worth rendering at all.
static int punctual_size_for(int layers) {
    for (int size = PUNCTUAL_SHADOW_MAX_SIZE; size > PUNCTUAL_SHADOW_MIN_SIZE; size >>= 1) {
        if ((unsigned)layers * (unsigned)size * (unsigned)size * 4u <=
            PUNCTUAL_SHADOW_VRAM_BUDGET)
            return size;
    }
    return PUNCTUAL_SHADOW_MIN_SIZE;
}

// Grow the punctual array to hold `layers` maps. Demand-driven, like the
// cascade array: a spot-only scene builds one layer rather than the pool
// ceiling, since every allocated layer is a scene traversal per frame.
//
// The size is part of what "grow" means here. A scene that gains a point light
// goes from one layer to seven, and seven layers do not fit at the edge one
// affords -- so the rebuild is triggered by EITHER a larger layer count or a
// size the budget no longer allows, not by the count alone.
static int init_punctual_shadow_array(ShadowSystem* system, int layers) {
    if (layers < 1 || layers > MAX_PUNCTUAL_SHADOW_LAYERS)
        return -1;

    int size = punctual_size_for(layers);
    if (system->punctual_map_array && system->punctual_allocated_layers >= layers &&
        system->punctual_map_size == size)
        return 0;

    free_depth_array(&system->punctual_map_array, &system->punctual_fbo);
    init_depth_array(&system->punctual_map_array, &system->punctual_fbo, size, layers);
    system->punctual_allocated_layers = layers;
    system->punctual_map_size = size;
    // Both costs, stated rather than assumed: the traversals (every layer is
    // re-rendered each frame) and the VRAM the budget just spent.
    log_info("Punctual shadow array: %d layer(s) at %d^2 (%.0f MB) -- %d scene traversal(s)/frame",
             layers, size, (double)layers * size * size * 4.0 / (1024.0 * 1024.0), layers);
    return 0;
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
                                        float slice_far, float scene_pad, int map_size, mat4 dest,
                                        vec4 out_params) {
    vec3 light_dir;
    glm_vec3_normalize_to(direction, light_dir);

    // Bounding sphere of the view slice: center on the view axis at the
    // radius-minimizing depth, radius from the far corners. Depends only on
    // fov/aspect and the split depths, never on camera pose -> the box size
    // is constant per cascade and cannot breathe as the camera moves.
    float k = tanf(cam->fov_radians * 0.5f);
    float k2 = k * k * (1.0f + cam->aspect_ratio * cam->aspect_ratio);
    float zc = 0.5f * (slice_near + slice_far) * (1.0f + k2);
    if (zc > slice_far)
        zc = slice_far;
    float dz = slice_far - zc;
    float radius = sqrtf(dz * dz + slice_far * slice_far * k2);

    vec3 center;
    glm_vec3_scale((float*)cam->forward, zc, center);
    glm_vec3_add(center, (float*)cam->position, center);

    vec3 up = GLM_VEC3_ZERO_INIT;
    light_space_up(light_dir, up);

    // Snap the sphere center to shadow-texel increments in light view space:
    // with the diameter constant, the box then slides in whole texels and
    // shadow edges stay put while the camera translates (Valient)
    mat4 snap_view;
    vec3 origin = {0.0f, 0.0f, 0.0f};
    glm_lookat(origin, light_dir, up, snap_view);
    vec3 center_ls;
    glm_mat4_mulv3(snap_view, center, 1.0f, center_ls);
    float texel = (2.0f * radius) / (float)map_size;
    center_ls[0] = floorf(center_ls[0] / texel) * texel;
    center_ls[1] = floorf(center_ls[1] / texel) * texel;
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

    // The array texture binds even when nothing casts: a sampler2DArray must
    // resolve to something for the program to be complete.
    glActiveTexture(GL_TEXTURE0 + SHADOW_MAP_TEXTURE_UNIT);
    glBindTexture(GL_TEXTURE_2D_ARRAY, system->shadow_map_array);
    uniform_set_int(u, "shadowMaps", SHADOW_MAP_TEXTURE_UNIT);

    // Punctual (perspective) shadow maps on the last unit, so a shadow-casting
    // spot occludes surfaces (e.g. the ball's shadow on the floor). Bound
    // unconditionally for the same reason as the cascade array; the layer count
    // is what decides whether any layer is read, and each light's own base layer
    // rides in its cluster UBO entry.
    glActiveTexture(GL_TEXTURE0 + PUNCTUAL_SHADOW_MAP_TEXTURE_UNIT);
    glBindTexture(GL_TEXTURE_2D_ARRAY, system->punctual_map_array);
    uniform_set_int(u, "punctualShadowMaps", PUNCTUAL_SHADOW_MAP_TEXTURE_UNIT);
    uniform_set_int(u, "punctualShadowCount", punctual_on);
    if (punctual_on > 0) {
        GLint ploc = uniform_location(u, "punctualShadowMatrix[0]");
        if (ploc >= 0)
            glUniformMatrix4fv(ploc, punctual_on, GL_FALSE,
                               (const GLfloat*)system->punctual_matrices);
        uniform_set_float(u, "punctualShadowMapSize", (float)system->punctual_map_size);
    }

    uniform_set_int(u, "numShadowLights", directional_on ? (int)system->directional_count : 0);
    if (!directional_on)
        return; // nothing below is read at count 0

    float texel_size = 1.0f / (float)system->default_map_size;
    GLint loc = uniform_location(u, "shadowTexelSize");
    if (loc >= 0)
        glUniform2f(loc, texel_size, texel_size);

    // Scalar shadow uniforms are shared across casters; set once
    uniform_set_float(u, "shadowBias", system->shadow_bias);

    // PCSS controls; the per-cascade ortho geometry rides in cascadeParams
    uniform_set_int(u, "pcssEnabled", system->pcss_enabled ? 1 : 0);
    uniform_set_float(u, "pcssSoftness", system->pcss_softness);
    uniform_set_int(u, "pcssStochastic", system->pcss_stochastic ? 1 : 0);
    uniform_set_int(u, "pcssFrameIndex", system->pcss_frame_index);

    uniform_set_int(u, "csmDebug", system->csm_debug ? 1 : 0);
    shadow_upload_cascade_uniforms(system, u);
    // (pbr_frag reads each light's CSM slot from its DirLight UBO entry; the
    // old shadowLightIndex[] loop-order indirection is gone)
}

static void _render_shadow_node(SceneNode* node, ShaderProgram* program, GLuint* current_program) {
    if (!node)
        return;

    if (node->meshes && node->mesh_count > 0) {
        if (*current_program != program->id) {
            glUseProgram(program->id);
            *current_program = program->id;
        }

        uniform_set_mat4(program->uniforms, "model", (const float*)node->global_transform);

        for (size_t i = 0; i < node->mesh_count; ++i) {
            Mesh* mesh = node->meshes[i];
            if (!mesh || mesh->vao == 0)
                continue;

            Material* mat = mesh->material;
            UniformManager* u = program->uniforms;

            // Alpha-masked materials (hair cards) are excluded from the
            // shadow map entirely: at map-texel scale (millimeters) their
            // strands can neither cast nor receive cleanly — solid-quad
            // casting draws card-shaped streaks, alpha-tested casting
            // draws strand-scale acne on whatever is beneath. Their
            // occlusion comes from the AO texture and SSAO instead.
            //
            // Foliage opts back in (material.h foliage_shadows): leaf cards
            // are centimeters across, so an alpha test resolves them cleanly,
            // and the dappled canopy shadow they cast is the reason the
            // surface exists at all.
            bool masked = mat && mat->alpha_mode == ALPHA_MASK;
            bool foliage =
                masked && mat->foliage_shadows && mat->alphaCutoff > 0.0f && mat->albedo_tex;
            if (masked && !foliage)
                continue;

            uniform_set_int(u, "alphaTested", foliage ? 1 : 0);
            if (foliage)
                uniform_set_float(u, "alphaCutoff", mat->alphaCutoff);
            // Bind whenever one exists, not just for foliage: a sampler left
            // pointing at an empty unit makes some drivers warn even though
            // this shader only reads it under alphaTested.
            if (mat && mat->albedo_tex) {
                uniform_set_int(u, "albedoTex", 0);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, mat->albedo_tex->id);
            }

            // Wind must displace the caster exactly as the shading pass
            // displaces the surface, or the shadow detaches from what casts it.
            uniform_set_float(u, "uWindResponse", mat ? mat->wind_response : 0.0f);
            uniform_set_int(u, "uWindMode", mat ? mat->wind_mode : 0);
            uniform_set_float(u, "uWindMaskMinY", mesh->aabb.min[1]);
            uniform_set_float(u, "uWindMaskMaxY", mesh->aabb.max[1]);

            // Skin animated meshes so they cast animated shadows
            render_update_skinning_uniforms(program, mesh);

            // A two-sided card has no back face, so culling either way would
            // drop it from the map entirely.
            bool two_sided = mat && mat->doubleSided;
            if (two_sided)
                glDisable(GL_CULL_FACE);

            glBindVertexArray(mesh->vao);
            glDrawElements(mesh->draw_mode, mesh->index_count, GL_UNSIGNED_INT, 0);
            glBindVertexArray(0);

            if (two_sided)
                glEnable(GL_CULL_FACE);
        }
    }

    for (size_t i = 0; i < node->children_count; i++) {
        _render_shadow_node(node->children[i], program, current_program);
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

// One perspective light-space matrix: eye at `pos`, looking down `dir_in`. All
// three punctual types reduce to this; only the fov and the axis differ.
static void compute_perspective_light_space(const vec3 pos, const vec3 dir_in, float fov,
                                            float near_plane, float far_plane, mat4 dest) {
    vec3 dir;
    glm_vec3_normalize_to((float*)dir_in, dir);
    vec3 up = GLM_VEC3_ZERO_INIT;
    light_space_up(dir, up);
    vec3 target;
    glm_vec3_add((float*)pos, dir, target);
    mat4 view, proj;
    glm_lookat((float*)pos, target, up, view);
    glm_perspective(fov, 1.0f, near_plane, far_plane, proj);
    glm_mat4_mul(proj, view, dest);
}

// A point light's six faces, in the +X -X +Y -Y +Z -Z order that
// include/punctual_shadow.glsl selects by dominant axis. That order is the whole
// contract between the two files; everything else about a face is in its matrix.
static const vec3 PUNCTUAL_CUBE_DIR[6] = {{1.0f, 0.0f, 0.0f},  {-1.0f, 0.0f, 0.0f},
                                          {0.0f, 1.0f, 0.0f},  {0.0f, -1.0f, 0.0f},
                                          {0.0f, 0.0f, 1.0f},  {0.0f, 0.0f, -1.0f}};

// A panel emits into a full hemisphere and one perspective map cannot cover
// 180 degrees, so an area light's shadow is an approximation with a chosen
// reach: wide enough that a receiver across a room is inside it, narrow enough
// that the texels are not spread to uselessness. Outside the cone the white
// border reads as lit, which is the safe direction to be wrong -- a missing
// shadow rather than a spurious one.
#define AREA_SHADOW_FOV glm_rad(120.0f)

// Layers one light needs: a point light's cube is six 2D faces, everything
// else is a single perspective map.
static int punctual_layers_for(const Light* light) {
    return light->type == LIGHT_POINT ? 6 : 1;
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
static int compute_punctual_matrices(const Light* light, const ShadowSystem* ss, mat4* dest) {
    const float near_p = ss->near_plane, far_p = ss->far_plane;

    switch (light->type) {
    case LIGHT_POINT:
        for (int f = 0; f < 6; f++) {
            compute_perspective_light_space(light->global_position, PUNCTUAL_CUBE_DIR[f],
                                            glm_rad(90.0f), near_p, far_p, dest[f]);
        }
        break;
    case LIGHT_AREA: {
        // Down the panel normal. A degenerate authored direction falls back to
        // -Y, matching what light_cluster.c builds the panel's own frame from,
        // so the shadow and the lit rectangle can never disagree about which
        // way the panel faces.
        vec3 dir;
        glm_vec3_copy((float*)light->direction, dir);
        if (glm_vec3_norm(dir) < 1e-6f)
            glm_vec3_copy((vec3){0.0f, -1.0f, 0.0f}, dir);
        compute_perspective_light_space(light->global_position, dir, AREA_SHADOW_FOV, near_p, far_p,
                                        dest[0]);
        break;
    }
    default: {
        // A spot's fov is its own cone, plus a margin so the outer edge is not
        // clipped by the frustum it is supposed to fill.
        float fov = 2.0f * acosf(light->outerCutOff) * 1.15f; // outerCutOff = cos(half-angle)
        if (fov > glm_rad(175.0f))
            fov = glm_rad(175.0f);
        compute_perspective_light_space(light->global_position, light->direction, fov, near_p, far_p,
                                        dest[0]);
        break;
    }
    }

    return punctual_layers_for(light);
}

// The body both depth-pass loops share, once a layer is bound: aim the depth
// program at one light-space matrix and walk the scene into it.
static void draw_shadow_layer(ShadowSystem* ss, SceneNode* root, const float* matrix,
                              GLuint* current_program) {
    uniform_set_mat4(ss->depth_program->uniforms, "lightSpaceMatrix", matrix);
    _render_shadow_node(root, ss->depth_program, current_program);
    end_shadow_pass(ss);
}

void render_shadow_depth_pass(Engine* engine, Scene* scene) {
    if (!engine || !scene || !scene->shadow_system)
        return;

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
    int punctual_needed = 0;
    const Light* pool_overflow = NULL;
    vec3 scene_center = {0.0f, 0.0f, 0.0f};

    for (size_t i = 0; i < scene->light_count; ++i) {
        Light* light = scene->lights[i];
        if (!light)
            continue;
        light->shadow_map_index = -1;
        light->shadow_layer = -1;
        if (!light->cast_shadows)
            continue;

        if (light->type == LIGHT_DIRECTIONAL) {
            if (ss->directional_count < MAX_SHADOW_LIGHTS)
                ss->directional_count++;
            continue;
        }
        if (light->type != LIGHT_SPOT && light->type != LIGHT_POINT &&
            light->type != LIGHT_AREA)
            continue;

        // A point light takes its six faces or none: five faces is a light with
        // holes in it, which is worse than one that does not cast.
        int want = punctual_layers_for(light);
        if (punctual_needed + want > MAX_PUNCTUAL_SHADOW_LAYERS) {
            if (!pool_overflow)
                pool_overflow = light;
            continue;
        }
        light->shadow_layer = punctual_needed;
        punctual_needed += want;
    }

    // Latched so a misconfigured scene reports once rather than every frame,
    // and re-arms if the overflow clears. Named, because the alternative is a
    // light that silently stops casting and reads as a shading bug.
    if (pool_overflow && !ss->punctual_pool_warned) {
        log_warn("Punctual shadow pool full (%d layers): '%s' and any further caster will not cast",
                 MAX_PUNCTUAL_SHADOW_LAYERS,
                 pool_overflow->name ? pool_overflow->name : "unnamed light");
    }
    ss->punctual_pool_warned = pool_overflow != NULL;

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

    // Always initialize the shadow map array texture (needed for sampler2DArray in shader)
    if (!ss->initialized) {
        if (init_shadow_map_array(ss) != 0)
            return;
    }

    // Early exit if nothing casts - but the array texture is already initialized.
    // A punctual caster keeps the pass alive even with no directional casters.
    if (ss->directional_count == 0 && punctual_needed == 0)
        return;

    // Now get the depth program for shadow rendering
    if (!ss->depth_program) {
        ss->depth_program = get_engine_shader_program_by_name(engine, "shadow_depth");
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

        const float lambda = 0.75f;
        float cam_near = camera->near_clip;
        float shadow_dist = fminf(ss->far_plane, camera->far_clip);
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
                compute_directional_light_space_matrix(light->direction, scene_center,
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
                    compute_cascade_light_space_matrix(
                        light->direction, &cam, slice_near, ss->cascade_splits[c], scene_pad,
                        ss->default_map_size, ss->cascade_matrices[layer], *params);
                    slice_near = ss->cascade_splits[c];
                }
                int last = (int)slot * cc + (cc - 1);
                compute_directional_light_space_matrix(light->direction, scene_center,
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

    GLuint current_program = 0;
    glUseProgram(ss->depth_program->id);
    current_program = ss->depth_program->id;

    // Wind globals for the whole pass (per-mesh response/mask ride along in the
    // node walk). render_time is the frame's single render clock, which the
    // shading pass reads too -- that is what keeps a swaying caster and its
    // shadow in lockstep rather than merely close.
    wind_upload_to_program(scene->wind, ss->depth_program->uniforms);
    uniform_set_float(ss->depth_program->uniforms, "time", (float)engine->render_time);

    for (size_t i = 0; i < ss->directional_count; ++i) {
        for (int c = 0; c < cc; ++c) {
            size_t layer = i * (size_t)cc + (size_t)c;
            begin_shadow_pass(ss, layer);
            draw_shadow_layer(ss, scene->root_node, (const float*)ss->cascade_matrices[layer],
                              &current_program);
        }
    }

    // Punctual maps (for surface shadows + the volumetric beam), one layer per
    // caster. Reuses the allocation (a no-op once it is large enough), the
    // bound depth program, and the depth policy set above.
    if (punctual_needed > 0 && init_punctual_shadow_array(ss, punctual_needed) == 0) {
        for (size_t i = 0; i < scene->light_count; ++i) {
            Light* light = scene->lights[i];
            if (!light || light->shadow_layer < 0)
                continue;

            int layers =
                compute_punctual_matrices(light, ss, &ss->punctual_matrices[light->shadow_layer]);

            // One scene traversal per layer -- this loop is the frame cost the
            // pool ceiling caps, and a point light pays six times a spot's.
            for (int f = 0; f < layers; f++) {
                int layer = light->shadow_layer + f;
                if (!begin_punctual_shadow_pass(ss, layer))
                    continue;
                draw_shadow_layer(ss, scene->root_node, (const float*)ss->punctual_matrices[layer],
                                  &current_program);
                // Layers are handed out in increasing order, so the last one
                // drawn is the bound the shader needs
                ss->punctual_layer_count = layer + 1;
            }
        }
    }

    glDisable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(0.0f, 0.0f);
    glUseProgram(0);

    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
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
        glm_vec3_normalize_to((float*)sp->direction, fx->fog_spot_dir);
        glm_vec3_scale((float*)sp->color, sp->intensity, fx->fog_spot_color);
        // Same packing the clustered path uses (light_cluster.c): x = 1/range^2
        // for getDistanceAtt's window, yz unused. The beam and the pool it casts
        // read one falloff.
        fx->fog_spot_atten[0] = sp->range > 0.0f ? 1.0f / (sp->range * sp->range) : 0.0f;
        fx->fog_spot_atten[1] = 0.0f;
        fx->fog_spot_atten[2] = 0.0f;
        fx->fog_spot_cos_inner = sp->cutOff;
        fx->fog_spot_cos_outer = sp->outerCutOff;
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

    // Publishing count 0 with a zero array handle is the single "no
    // shadowed in-scatter" state consumers rely on: a nonzero count
    // guarantees the map array and every slot below it are valid.
    if (!ss || !ss->enabled || ss->directional_count == 0 || !ss->shadow_map_array || !scene->lights) {
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
