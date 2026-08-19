
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
#include "intersect.h"
#include "shadow.h"
#include "light_cluster.h"
#include "profiler.h"
#include "texture.h"
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
    system->msm_enabled = false; // library default off; the app opts in
    system->msm_size = MSM_DEFAULT_SIZE;
    system->msm_blur = MSM_DEFAULT_BLUR;
    system->msm_bleed = MSM_DEFAULT_BLEED;
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
    free_msm_resources(system);
    free_tsm_resources(system);

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
    init_depth_array(&system->shadow_map_array, &system->cascade_fbo,
                     system->default_map_size, layers);

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
    uniform_set_int(u, "pcssStochastic", system->pcss_stochastic ? 1 : 0);
    uniform_set_int(u, "pcssFrameIndex", system->pcss_frame_index);

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
typedef enum ShadowCasterSet {
    SHADOW_CASTERS_OPAQUE = 0,
    SHADOW_CASTERS_OPAQUE_TSM,
    SHADOW_CASTERS_TRANSLUCENT,
} ShadowCasterSet;

// Everything about a caster that its MATERIAL decides, for whichever of the two
// depth programs is bound. Every uniform here is location-guarded, so the ones
// belonging to the absorb program no-op on the depth program and the two
// traversals stay one function.
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
    // surface, or the shadow detaches from what casts it.
    uniform_set_float(u, "uWindResponse", mat->wind_response);
    uniform_set_int(u, "uWindMode", mat->wind_mode);
}

// Whether this caster set wants this item, from flags the list settled at build.
// Alpha-masked geometry casts NOTHING on the depth path -- at map-texel scale
// hair strands resolve as card-shaped streaks or strand-scale acne either way,
// and their occlusion comes from AO instead. Foliage opts back in, because leaf
// cards are centimetres across and an alpha test resolves them.
static bool caster_set_wants(ShadowCasterSet set, uint8_t lane, uint8_t flags) {
    bool masked_only = (flags & DRAW_ALPHA_MASKED) && !(flags & DRAW_FOLIAGE);
    // What a transmittance map represents instead of a depth map: geometry that
    // casts NOTHING on the depth path (masked without foliage) or casts SOLID
    // where it should not (blend, transmission -- i.e. any non-opaque lane).
    bool translucent = masked_only || lane != DRAW_LANE_OPAQUE;

    if (set == SHADOW_CASTERS_TRANSLUCENT)
        return translucent;
    if (masked_only)
        return false;
    // Blend and transmission leave the depth pass ONLY when a transmittance map
    // will receive them. With the flag off this never fires and the depth
    // rendered is the depth that was rendered before, bit for bit.
    return !(set == SHADOW_CASTERS_OPAQUE_TSM && translucent);
}

// How many casters from `first` one draw can carry: same geometry, all wanted
// by this set, all visible, capped at a chunk. Same contiguity rule as the
// camera path -- a skipped item ends the run, because the batch submits
// whatever the chunk holds.
static size_t _visible_caster_run(const DrawList* list, size_t first, ShadowCasterSet set,
                                  const CullView* cull) {
    const DrawItem* head = &list->items[first];
    size_t n = 1;
    while (first + n < list->count && n < UBO_INSTANCE_MAX) {
        const DrawItem* next = &list->items[first + n];
        if (!caster_set_wants(set, next->lane, next->flags) ||
            !draw_run_can_join(head, next, cull))
            break;
        n++;
    }
    return n;
}

static void _draw_shadow_items(const DrawList* list, ShaderProgram* program, SubmitState* state,
                               ShadowCasterSet set, const CullView* cull, const Engine* engine) {
    if (!list || !engine)
        return;

    SubmitStats* stats = profiler_submit(engine->profiler);
    InstanceChunk chunk;

    for (size_t idx = 0; idx < list->count; ++idx) {
        const DrawItem* item = &list->items[idx];
        if (!caster_set_wants(set, item->lane, item->flags))
            continue;

        if (stats)
            stats->meshes_seen++;
        // The layer's own volume, so a rejected caster contributed nothing to
        // it -- the matrix IS the clip volume, and nothing here enables
        // GL_DEPTH_CLAMP, so anything this rejects the rasterizer would have
        // clipped. That is what makes the map bit-identical rather than merely
        // close, and it holds whatever fit produced the matrix: the padded
        // cascade slices, the outermost whole-scene fit, and the punctual faces
        // that have no pad at all.
        //
        // Enabling depth clamp on this pass -- the usual remedy for a caster
        // between the light and the near plane -- would break that, and would
        // do it silently: casters this test drops would then have contributed.
        if (!draw_item_visible(item, cull)) {
            if (stats)
                stats->meshes_culled++;
            continue;
        }

        {
            const SceneNode* node = item->node;
            Mesh* mesh = item->mesh;
            Material* mat = mesh->material;
            UniformManager* u = program->uniforms;
            bool foliage = (item->flags & DRAW_FOLIAGE) != 0;

            // The depth stage reads InstanceBlock, so casters batch here even
            // when the same mesh cannot batch on the camera path: skinning is
            // a single global pose, which every instance of one mesh shares by
            // definition. Gated on the program all the same, so this follows
            // the shader rather than restating what it does.
            size_t run = 1;
            if (engine->instancing_enabled && engine->instance_ubo && program->instanced)
                run = _visible_caster_run(list, idx, set, cull);
            if (stats)
                stats->meshes_seen += run - 1;
            // No shading transforms: this stage reads uInstModel and nothing
            // else, so the rest of the block is bytes it cannot look at.
            if (run > 1)
                instance_chunk_upload(engine->instance_ubo, &chunk, list, idx, run, false);

            // Per node, and the list is in node order, so consecutive meshes of
            // one node still upload it once.
            uniform_set_mat4(u, "model", (const float*)node->global_transform);

            // Everything the material decides, uploaded once per material
            // rather than once per mesh -- a canopy of leaf cards is hundreds
            // of meshes sharing one of these.
            if (submit_take_material(state, mat)) {
                _upload_shadow_material(u, mat, foliage);
                if (stats)
                    stats->material_switches++;
            }

            // Per mesh, because it is the mesh's own bounds: where along Y the
            // cloth mask ramps from anchored to free.
            uniform_set_float(u, "uWindMaskMinY", mesh->aabb.min[1]);
            uniform_set_float(u, "uWindMaskMaxY", mesh->aabb.max[1]);
            // Per mesh for the same reason the shading pass sets it per mesh:
            // whether COLOR_0 exists is geometry, not material. The cutout
            // multiplies it in, so a caster carrying its alpha there rather than
            // in the albedo map used to cast as though it were solid.
            uniform_set_int(u, "vertexColorExists", mesh->colors ? 1 : 0);

            // Skin animated meshes so they cast animated shadows
            render_update_skinning_uniforms(program, mesh);

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
            bool two_sided =
                (item->flags & DRAW_DOUBLE_SIDED) && set != SHADOW_CASTERS_TRANSLUCENT;
            // The camera's level, not one chosen for this light: see DrawItem.
            submit_draw_run(state, u, item, run, two_sided, stats);
            idx += run - 1;
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
static void draw_shadow_layer(ShadowSystem* ss, const Scene* scene, const DrawList* list,
                              mat4 matrix, SubmitState* state, ShadowCasterSet set,
                              const Engine* engine) {
    uniform_set_mat4(ss->depth_program->uniforms, "lightSpaceMatrix", (const float*)matrix);
    // The matrix IS the layer's coverage volume, and Gribb-Hartmann does not
    // care whether it is ortho or perspective -- so cascades and punctual faces
    // cull through the same six planes.
    Frustum layer_frustum;
    frustum_extract_from_vp(matrix, &layer_frustum);
    CullView cull = render_cull_view(engine, scene, &layer_frustum);
    _draw_shadow_items(list, ss->depth_program, state, set, &cull, engine);
    end_shadow_pass(ss);
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
        ss->msm_program = get_engine_shader_program_by_name(engine, "msm_resolve");
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
    if (ss->msm_allocated_layers != layers || ss->msm_allocated_size != size ||
        (want_scratch && !ss->msm_scratch)) {
        const bool resized = ss->msm_allocated_layers != layers || ss->msm_allocated_size != size;
        if (resized)
            free_msm_arrays(ss);
        if (!ss->msm_array)
            ss->msm_array = create_texture_2d_array_float(size, size, layers, GL_RGBA16F, GL_RGBA);
        if (want_scratch && !ss->msm_scratch)
            ss->msm_scratch =
                create_texture_2d_array_float(size, size, layers, GL_RGBA16F, GL_RGBA);
        if (!ss->msm_array || (want_scratch && !ss->msm_scratch)) {
            log_error("Failed to allocate moment shadow cascades");
            free_msm_arrays(ss);
            return false;
        }
        ss->msm_allocated_layers = layers;
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
        ss->tsm_absorb_program = get_engine_shader_program_by_name(engine, "shadow_absorb");
    if (!ss->tsm_resolve_program)
        ss->tsm_resolve_program = get_engine_shader_program_by_name(engine, "tsm_resolve");
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
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               ss->tsm_scratch, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            log_error("Translucent shadow scratch framebuffer incomplete");
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            gl_delete_texture(&ss->tsm_scratch);
            gl_delete_fbo(&ss->tsm_scratch_fbo);
            return false;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        log_info("Translucent shadows: %d layer(s) at %d^2 appended to the depth array "
                 "(%.0f MB incl. a %.0f MB accumulation scratch)",
                 TSM_SLOTS * cc * TSM_PARTS, size,
                 (TSM_SLOTS * cc * TSM_PARTS * (double)size * size * 4.0 +
                  size * (double)size * 4.0) /
                     (1024.0 * 1024.0),
                 size * (double)size * 4.0 / (1024.0 * 1024.0));
    }
    if (ss->tsm_quad_vao == 0)
        create_fullscreen_quad_vao(&ss->tsm_quad_vao, &ss->tsm_quad_vbo);
    return true;
}

// Draw the transmittance layers. Callable only when shadow_tsm_prepare returned
// true, so every resource here is known to exist.
static bool shadow_build_tsm(ShadowSystem* ss, const Engine* engine, Scene* scene) {
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

        submit_use_program(&state, ss->tsm_absorb_program->id);
        UniformManager* au = ss->tsm_absorb_program->uniforms;
        uniform_set_mat4(au, "lightSpaceMatrix", matrix);
        uniform_set_int(au, "albedoTex", 0);
        wind_upload_to_program(scene->wind, au);
        uniform_set_float(au, "time", (float)engine->render_time);
        _draw_shadow_items(&scene->draw_list, ss->tsm_absorb_program, &state,
                           SHADOW_CASTERS_TRANSLUCENT, &tsm_cull, engine);
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
        // The fullscreen resolve above bound its own program and its own VAO,
        // so nothing the walk below tracked is still true.
        submit_state_reset(&state);
        submit_use_program(&state, ss->depth_program->id);
        uniform_set_mat4(ss->depth_program->uniforms, "lightSpaceMatrix", matrix);
        _draw_shadow_items(&scene->draw_list, ss->depth_program, &state,
                           SHADOW_CASTERS_TRANSLUCENT, &tsm_cull, engine);
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
    // Cleared up front so every early return below leaves the lookup on the
    // depth array. Like punctual_layer_count above, this is what the pass
    // actually produced, not what was asked for.
    ss->msm_built = false;
    // Same latch, same reason: every early return below must leave the lookup
    // reading occlusion alone rather than an array that was never filled.
    ss->tsm_built = false;
    ss->tsm_live = false;
    int punctual_needed = 0;
    const Light* pool_overflow = NULL;
    const Light* dir_overflow = NULL;
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
            else if (!dir_overflow)
                dir_overflow = light;
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

    // Same latch for the cascade slots, which run out one light sooner than the
    // UBO does: it carries LC_MAX_DIR_LIGHTS directionals and there are only
    // MAX_SHADOW_LIGHTS slots. The overflowing light still lights the scene at
    // full strength -- the shader leaves its shadow term at 1.0 -- so without
    // this it is indistinguishable from one that never asked to cast. Which
    // light loses is scene->lights order, so the name is the whole point.
    if (dir_overflow && !ss->dir_slot_warned) {
        log_warn("Directional shadow slots full (%d casters): '%s' and any further caster will not cast",
                 MAX_SHADOW_LIGHTS,
                 dir_overflow->name ? dir_overflow->name : "unnamed light");
    }
    ss->dir_slot_warned = dir_overflow != NULL;

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

    glUseProgram(ss->depth_program->id);

    // Wind globals for the whole pass (per-mesh response/mask ride along in the
    // node walk). render_time is the frame's single render clock, which the
    // shading pass reads too -- that is what keeps a swaying caster and its
    // shadow in lockstep rather than merely close.
    wind_upload_to_program(scene->wind, ss->depth_program->uniforms);
    uniform_set_float(ss->depth_program->uniforms, "time", (float)engine->render_time);

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
    submit_use_program(&state, ss->depth_program->id);


    profiler_scope_begin_if(engine->profiler, ss->directional_count > 0, "shadow cascades");
    for (size_t i = 0; i < ss->directional_count; ++i) {
        const ShadowCasterSet set = (ss->tsm_live && i < TSM_SLOTS)
                                        ? SHADOW_CASTERS_OPAQUE_TSM
                                        : SHADOW_CASTERS_OPAQUE;
        for (int c = 0; c < cc; ++c) {
            size_t layer = i * (size_t)cc + (size_t)c;
            begin_shadow_pass(ss, layer);
            draw_shadow_layer(ss, scene, &scene->draw_list, ss->cascade_matrices[layer], &state,
                              set, engine);
        }
    }
    profiler_scope_end(engine->profiler);

    // Punctual maps (for surface shadows + the volumetric beam), one layer per
    // caster. Reuses the allocation (a no-op once it is large enough), the
    // bound depth program, and the depth policy set above.
    if (punctual_needed > 0 && init_punctual_shadow_array(ss, punctual_needed) == 0) {
        profiler_scope_begin(engine->profiler, "shadow punctual");
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
                // Deliberately NOT opaque_set: the transmittance map covers
                // the directional cascades only (unit 15 has no room for a
                // second lookup), so withholding translucent casters here
                // would take away the solid shadow without replacing it.
                draw_shadow_layer(ss, scene, &scene->draw_list, ss->punctual_matrices[layer],
                                  &state, SHADOW_CASTERS_OPAQUE, engine);
                // Layers are handed out in increasing order, so the last one
                // drawn is the bound the shader needs
                ss->punctual_layer_count = layer + 1;
            }
        }
        profiler_scope_end(engine->profiler);
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

    // The population no shadow map can serve: point and spot lights holding no
    // punctual layer. Counted BEFORE the directional early-out below, because a
    // scene lit only by practicals has no directional and this is the whole
    // reason it still needs a contact-shadow pass (spec 11.56). The cull radius
    // is the same test the cluster build applies -- a light that never reaches
    // epsilon is not a light the march can shadow.
    fx->cs_mapless_lights = 0;
    for (size_t i = 0; scene && i < scene->light_count; i++) {
        const Light* l = scene->lights[i];
        if (!l || (l->type != LIGHT_POINT && l->type != LIGHT_SPOT))
            continue;
        if (l->shadow_layer < 0 && light_cull_radius(l) != 0.0f)
            fx->cs_mapless_lights++;
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
