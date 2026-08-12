
#include <stdlib.h>
#include <string.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <cglm/cglm.h>

#include "animation.h"
#include "ext/log.h"
#include "scene.h"
#include "sky.h"
#include "wind.h"
#include "gi_volume.h"
#include "program.h"
#include "uniform.h"
#include "shader.h"
#include "mesh.h"
#include "material.h"
#include "mask_array.h"
#include "light.h"
#include "camera.h"
#include "common.h"
#include "engine.h"
#include "render.h"
#include "profiler.h"
#include "light_cluster.h"
#include "util.h"
#include "shadow.h"
#include "intersect.h"
#include "particle_system.h" // scene-attached particle systems (auto-rendered)

// The 0-15 fragment texture-unit budget is one global resource whose slots are
// declared across common.h (material) + shadow.h + ibl.h (engine). Pin the whole
// ordered map here so a change to any one #define that would collide with its
// neighbour fails the build (the queried GL_MAX_TEXTURE_IMAGE_UNITS is 16, so the
// top unit must stay < 16 -- A3 relocated brdfLUT/skybox off units 16/17).
_Static_assert(TEXUNIT_CLEARCOAT_NORMAL < TEXUNIT_HEIGHT && TEXUNIT_HEIGHT < TEXUNIT_EMISSIVE,
               "POM height unit must sit between clearcoat-normal and emissive");
_Static_assert(TEXUNIT_SCENE_COLOR < TEXUNIT_LTC && TEXUNIT_LTC < TEXUNIT_SHEEN,
               "LTC table-array unit must sit between scene-color and sheen");
_Static_assert(TEXUNIT_MATERIAL_MAX < IBL_CHARLIE_TEXTURE_UNIT &&
                   IBL_CHARLIE_TEXTURE_UNIT < SHADOW_MAP_TEXTURE_UNIT,
               "Charlie sheen env unit must sit between the material units and the shadow array");
_Static_assert(SHADOW_MAP_TEXTURE_UNIT < IBL_IRRADIANCE_TEXTURE_UNIT,
               "shadow unit overlaps the IBL irradiance unit");
_Static_assert(IBL_IRRADIANCE_TEXTURE_UNIT < IBL_PREFILTER_TEXTURE_UNIT,
               "IBL irradiance overlaps prefilter unit");
_Static_assert(IBL_PREFILTER_TEXTURE_UNIT < IBL_BRDF_LUT_TEXTURE_UNIT,
               "IBL prefilter overlaps brdfLUT unit");
_Static_assert(IBL_BRDF_LUT_TEXTURE_UNIT < IBL_SKYBOX_TEXTURE_UNIT,
               "IBL brdfLUT overlaps skybox unit");
_Static_assert(IBL_SKYBOX_TEXTURE_UNIT < 16,
               "engine texture units exceed GL_MAX_TEXTURE_IMAGE_UNITS (16)");
// The GI atlas deliberately shares the skybox's number: units are per PROGRAM,
// and pbr_frag -- the only program that samples the atlas -- has never sampled
// the skybox cube. Asserted as equality so a future move of either one has to
// come here and decide whether the sharing still holds.
_Static_assert(GI_ATLAS_TEXTURE_UNIT == IBL_SKYBOX_TEXTURE_UNIT,
               "GI atlas unit is the skybox unit reused; pbr_frag samples neither cube");

// Global animation state for skinned mesh rendering (set via set_render_animation_state)
static AnimationState* g_current_animation_state = NULL;

void set_render_animation_state(AnimationState* state) {
    g_current_animation_state = state;
}

AnimationState* get_render_animation_state(void) {
    return g_current_animation_state;
}

void render_update_skinning_uniforms(ShaderProgram* program, const Mesh* mesh) {
    if (!program || !program->uniforms)
        return;

    UniformManager* u = program->uniforms;

    if (mesh && mesh->is_skinned && g_current_animation_state &&
        g_current_animation_state->active_bone_count > 0) {
        uniform_set_int(u, "skinned", 1);

        GLsizei count = (GLsizei)g_current_animation_state->active_bone_count;

        // Upload bone matrices
        GLint loc = uniform_location(u, "boneMatrices[0]");
        if (loc >= 0) {
            glUniformMatrix4fv(loc, count, GL_FALSE,
                               (const GLfloat*)g_current_animation_state->bone_matrices);
        }

        // Previous-frame bones for skinned motion vectors (TAA), packed once per
        // frame by animation_snapshot_prev_pose. Absent on programs without the
        // uniform (e.g. the shadow depth pass) -> skipped.
        GLint prevLoc = uniform_location(u, "uPrevBoneRows[0]");
        if (prevLoc >= 0) {
            glUniform4fv(prevLoc, count * 3, g_current_animation_state->prev_bone_rows);
        }
    } else {
        uniform_set_int(u, "skinned", 0);
    }
}

// Scatter profiles and the blur's reach, for pre-integrated skin (§11.13).
//
// pbr_frag needs both because it takes up exactly the slack the screen-space
// blur cannot deliver: the blur caps its base scatter radius, so past a certain
// closeness it falls short of the authored world radius. `sssMaxScatterPerDepth`
// is the world scatter the blur can still reach per unit of view depth, which
// since §11.14 IS the cap -- it is already expressed in those units, so there is
// no conversion to get wrong and no projection to read.
//
// Uploaded per program switch rather than per material because
// _update_program_material_uniforms has no Engine, and widening its signature
// to reach postfx would be a far larger blast radius than one call here.
static void _upload_skin_preint_uniforms(const Engine* engine, UniformManager* u) {
    const PostFX* fx = engine->postfx;
    if (!fx)
        return;
    GLint prof_loc = uniform_location(u, "sssProfiles[0]");
    if (prof_loc >= 0 && fx->sss_profile_count > 0)
        glUniform4fv(prof_loc, fx->sss_profile_count, (const GLfloat*)fx->sss_profiles);

    // Zero when the blur will not run: the shader reads this as "the
    // screen-space pass will deliver nothing", so pre-integration takes the
    // whole authored width instead of the shortfall. Without it, the deficit is
    // computed against a blur that is not there and the falloff comes out
    // narrower the more the absent blur would have covered.
    //
    // Gated on sss_this_frame rather than sss_enabled, because those differ:
    // the pass also needs a scene that actually carries subsurface and a PBR
    // frame mode (engine.c). Reading the toggle instead leaves the same defect
    // reachable through a debug render mode.
    float reach = engine->sss_this_frame
                      ? postfx_sss_max_sigma_per_depth(fx, engine->projection_matrix)
                      : 0.0f;
    uniform_set_float(u, "sssMaxScatterPerDepth", reach);
}

// `a2c_capable` is whether the current target has MSAA samples for
// alpha-to-coverage to dither into. It gates only the coverage path -- whether
// the material is masked at all is uploaded separately, because the shadow and
// GTAO rules key off the material and must not move with the AA mode.
void _update_program_material_uniforms(ShaderProgram* program, Material* material,
                                       bool a2c_capable) {
    if (!program || !program->uniforms || !material)
        return;

    UniformManager* u = program->uniforms;
    bool masked = material->alpha_mode == ALPHA_MASK;

    uniform_set_vec3(u, "albedo", (const float*)&material->albedo);
    vec3 emissive_hdr;
    glm_vec3_scale(material->emissive, material->emissive_strength, emissive_hdr);
    // The shader treats a black factor with an emissive texture as "use the
    // texture as-is"; substitute the bare strength so it still applies there
    if (material->emissive_tex && glm_vec3_norm2(material->emissive) < 1e-8f) {
        glm_vec3_fill(emissive_hdr, material->emissive_strength);
    }
    uniform_set_vec3(u, "emissiveFactor", (const float*)&emissive_hdr);
    uniform_set_float(u, "metallic", material->metallic);
    uniform_set_float(u, "roughness", material->roughness);
    uniform_set_float(u, "ao", material->ao);
    uniform_set_float(u, "materialOpacity", material->opacity);
    uniform_set_float(u, "alphaCutoff", material->alphaCutoff);
    uniform_set_int(u, "alphaToCoverage", masked && a2c_capable ? 1 : 0);
    uniform_set_int(u, "alphaMasked", masked ? 1 : 0);
    uniform_set_int(u, "foliageShadows", material->foliage_shadows ? 1 : 0);
    uniform_set_float(u, "normalScale", material->normalScale);
    uniform_set_float(u, "aoStrength", material->aoStrength);
    uniform_set_float(u, "ior", material->ior);
    uniform_set_float(u, "transmission", material->transmission);
    uniform_set_float(u, "transmissionThickness", material->thickness);
    uniform_set_float(u, "filmThickness", material->filmThickness);
    uniform_set_float(u, "clearcoat", material->clearcoat);
    uniform_set_float(u, "clearcoatRoughness", material->clearcoat_roughness);
    uniform_set_float(u, "specularFactor", material->specular_factor);
    uniform_set_vec3(u, "specularColorFactor", (const float*)&material->specular_color_factor);
    uniform_set_vec3(u, "sheenColorFactor", (const float*)&material->sheen_color_factor);
    uniform_set_float(u, "sheenRoughnessFactor", material->sheen_roughness_factor);
    // Unconditional, like uWindResponse below: the gate is per material, so a
    // material that leaves it 0 must still upload the 0 or it inherits whichever
    // anisotropic material happened to draw before it.
    uniform_set_float(u, "anisotropy", material->anisotropy);
    uniform_set_float(u, "parallaxScale", material->parallax_scale); // POM depth (0 = off)
    uniform_set_float(u, "subsurface", material->subsurface);        // SSS strength (0 = off)
    uniform_set_vec3(u, "subsurfaceColor", (const float*)&material->subsurface_color);
    uniform_set_int(u, "sssProfileIndex", material->subsurface_profile); // scatter-profile slot
    uniform_set_float(u, "curvatureScale", material->curvature_scale);   // pre-integrated skin
    uniform_set_vec2(u, "uvOffset", (const float*)&material->uvOffset);
    uniform_set_vec2(u, "uvScale", (const float*)&material->uvScale);
    uniform_set_float(u, "uvRotation", material->uvRotation);
    // Wind response (0 = rigid). Uploaded per material switch, so non-cloth
    // materials reset it to 0 and the shader early-outs for them. The mask
    // bounds are per-mesh (uploaded in the draw loop from the mesh's AABB).
    uniform_set_float(u, "uWindResponse", material->wind_response);
    uniform_set_int(u, "uWindMode", material->wind_mode);

    // Dedicated (native-resolution) sampler units. The scalar masks
    // (roughness/metallic/ao/opacity/microsurface/anisotropy) are no
    // longer per-slot samplers -- they live in the mask sampler2DArray, bound
    // once per program in the draw loop and selected per material by layer.
    uniform_set_int(u, "albedoTex", TEXUNIT_ALBEDO);
    uniform_set_int(u, "normalTex", TEXUNIT_NORMAL);
    uniform_set_int(u, "emissiveTex", TEXUNIT_EMISSIVE);
    uniform_set_int(u, "sceneColorTex", TEXUNIT_SCENE_COLOR); // refraction source
    uniform_set_int(u, "sheenTex", TEXUNIT_SHEEN);            // KHR sheen color (sRGB)
    uniform_set_int(u, "clearcoatNormalTex", TEXUNIT_CLEARCOAT_NORMAL);
    uniform_set_int(u, "heightTex", TEXUNIT_HEIGHT); // POM height map (§4.11)

    // Per-mask layer into the mask array (-1 = no texture -> scalar factor)
    uniform_set_int(u, "roughnessLayer", material->roughness_layer);
    uniform_set_int(u, "metallicLayer", material->metallic_layer);
    uniform_set_int(u, "aoLayer", material->ao_layer);
    uniform_set_int(u, "opacityLayer", material->opacity_layer);
    uniform_set_int(u, "microsurfaceLayer", material->microsurface_layer);
    uniform_set_int(u, "anisotropyLayer", material->anisotropy_layer);

    if (material->albedo_tex) {
        glActiveTexture(GL_TEXTURE0 + TEXUNIT_ALBEDO);
        glBindTexture(GL_TEXTURE_2D, material->albedo_tex->id);
    }

    if (material->normal_tex) {
        glActiveTexture(GL_TEXTURE0 + TEXUNIT_NORMAL);
        glBindTexture(GL_TEXTURE_2D, material->normal_tex->id);
    }

    if (material->emissive_tex) {
        glActiveTexture(GL_TEXTURE0 + TEXUNIT_EMISSIVE);
        glBindTexture(GL_TEXTURE_2D, material->emissive_tex->id);
    }

    // POM height map (§4.11): the parallax march samples it before every
    // material lookup. Guarded in-shader by parallaxEnabled/heightTexExists/
    // parallaxScale, so binding it is inert until a material opts in.
    if (material->height_tex) {
        glActiveTexture(GL_TEXTURE0 + TEXUNIT_HEIGHT);
        glBindTexture(GL_TEXTURE_2D, material->height_tex->id);
    }

    if (material->sheen_tex) {
        glActiveTexture(GL_TEXTURE0 + TEXUNIT_SHEEN);
        glBindTexture(GL_TEXTURE_2D, material->sheen_tex->id);
    }

    // material->reflectance_tex is loaded and owned but deliberately not bound:
    // no shader samples it (KHR specular color is deferred), and it has no
    // reserved unit -- unit 9 went to LTC in spec 9.2 and to the Charlie
    // sheen environment in 10.7.1. Binding it would cost a glActiveTexture +
    // glBindTexture per material switch feeding a unit nothing reads.

    if (material->clearcoat_normal_tex) {
        glActiveTexture(GL_TEXTURE0 + TEXUNIT_CLEARCOAT_NORMAL);
        glBindTexture(GL_TEXTURE_2D, material->clearcoat_normal_tex->id);
    }

    uniform_set_int(u, "albedoTexExists", material->albedo_tex ? 1 : 0);
    uniform_set_int(u, "normalTexExists", material->normal_tex ? 1 : 0);
    uniform_set_int(u, "emissiveTexExists", material->emissive_tex ? 1 : 0);
    uniform_set_int(u, "heightTexExists", material->height_tex ? 1 : 0);
    uniform_set_int(u, "sheenTexExists", material->sheen_tex ? 1 : 0);
    uniform_set_int(u, "clearcoatNormalExists", material->clearcoat_normal_tex ? 1 : 0);

    // Reset active texture unit
    glActiveTexture(GL_TEXTURE0);
}

static void _update_camera_uniforms(ShaderProgram* program, Camera* camera) {
    if (!program || !program->uniforms || !camera)
        return;

    UniformManager* u = program->uniforms;
    uniform_set_vec3(u, "camPos", (const float*)&camera->position);
    // nearClip/farClip are NOT uploaded: no shader in the corpus reads them.
    // The depth linearization they fed lives in pbr_frag.glsl.old, which is not
    // built. Post passes reconstruct view-Z from the projection matrix instead.
}

// Draws one item, carrying `instances` objects. Visibility is the caller's:
// once a run is formed the chunk holds exactly these objects, so nothing here
// may decline to draw.
static void _submit_item(const Engine* engine, Scene* scene, const DrawItem* item, Camera* camera,
                         mat4 view, mat4 projection, RenderMode render_mode, SubmitState* state,
                         OitSubpass oit_pass, size_t instances) {
    SceneNode* node = item->node;
    Mesh* mesh = item->mesh;
    // Only the opaque pass submits the opaque lane, so the lane says which pass
    // this is without the caller having to.
    bool alpha_pass = item->lane != DRAW_LANE_OPAQUE;

    if (!node->meshes || node->mesh_count == 0)
        return;

    // Alpha-to-coverage needs real MSAA samples to dither into; on a 1-sample
    // buffer glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE) is a no-op and the shader's
    // A2C path would keep every fragment down to alpha 0.02, so masked geometry
    // would render as solid quads. Fall back to the binary cutoff there.
    //
    // msaa_samples describes the SCENE framebuffer, which is not what is bound
    // during a capture -- capture targets are always single-sample, so reading
    // it alone would take the A2C path with no coverage hardware behind it and
    // bake solid quads into the capture.
    bool a2c_capable = engine->msaa_samples > 1 && !engine->capturing;
    SubmitStats* stats = profiler_submit(engine->profiler);

    {
        Material* mat = mesh->material;
        ShaderProgram* program = mat->shader_program;

        UniformManager* u = program->uniforms;

        // Only switch program if different from current
        if (submit_use_program(state, program->id)) {

            // Set view/projection/camera uniforms once per program switch.
            // projection is the jittered matrix (rasterization); the motion-vector
            // matrices are the engine's real un-jittered ones, computed once per
            // frame in render_current_scene.
            uniform_set_mat4(u, "view", (const float*)view);
            uniform_set_mat4(u, "projection", (const float*)projection);
            uniform_set_mat4(u, "uCurrViewProjNoJitter", (const float*)engine->view_proj);
            uniform_set_mat4(u, "uPrevViewProj", (const float*)engine->prev_view_proj);
            uniform_set_float(u, "time", (float)engine->render_time);
            // Global directional wind (NULL -> uWindStrength 0 -> shader early-out).
            // The shader evaluates the previous-frame position at
            // time - uDeltaTime, so this must be the advance of the SAME clock
            // `time` came from -- render_delta, not the wall-clock delta_time.
            // Under a game those differ: the sim advances in whole fixed steps
            // and stops entirely when paused, so a wall-clock delta would report
            // wind motion on geometry that never moved.
            uniform_set_float(u, "uDeltaTime", (float)engine->render_delta);
            wind_upload_to_program(scene ? scene->wind : NULL, u);
            uniform_set_int(u, "renderMode", render_mode);
            uniform_set_float(u, "specularAAStrength", engine->specular_aa_strength);
            uniform_set_int(u, "energyCompEnabled", engine->energy_comp_enabled ? 1 : 0);
            uniform_set_int(u, "clearcoatEnabled", engine->clearcoat_enabled ? 1 : 0);
            uniform_set_int(u, "specularEnabled", engine->specular_enabled ? 1 : 0);
            uniform_set_int(u, "sheenEnabled", engine->sheen_enabled ? 1 : 0);
            uniform_set_int(u, "parallaxEnabled", engine->parallax_enabled ? 1 : 0);
            uniform_set_int(u, "sssEnabled", engine->sss_enabled ? 1 : 0);
            // Pre-integrated skin is off under capture, and that is correctness
            // rather than thrift: it modifies FragColor, which lands in probe and
            // DDGI radiance, and its width depends on the CAPTURE camera's
            // projection and depth. Baking a view-dependent falloff into an
            // irradiance probe shows up later as GI that changes when you move.
            uniform_set_int(u, "skinPreintEnabled",
                            engine->skin_preint_enabled && !engine->capturing ? 1 : 0);
            _upload_skin_preint_uniforms(engine, u);
            uniform_set_int(u, "clusterDebug", engine->cluster_debug ? 1 : 0);
            uniform_set_int(u, "oitPass", oit_pass);
            uniform_set_int(u, "oitMomentWeighted", engine->moments_this_frame ? 1 : 0);
            // Split ambient specular only where attachment 7 is live: the
            // opaque pass of a split-mode PBR frame. The late/OIT passes and
            // captures draw into targets with no attachment 7, so their
            // specular must stay inline or it is silently discarded.
            uniform_set_int(u, "splitAmbientSpec",
                            engine->spec_this_frame && !alpha_pass && !engine->capturing ? 1 : 0);
            // Unit 6 has two disjoint tenants. The moment-weighted accumulate
            // binds the moment atlas there; every other pass binds the
            // refraction source, which is valid only in the late pass after the
            // mid-frame resolve ran (pass 2 forces a program re-switch by
            // resetting current_program, so this always re-uploads there).
            //
            // They cannot collide, and not by convention: the accumulate draws
            // only non-transmissive meshes, and that is the same routing that
            // makes sceneColorTex unreadable there.
            bool moments_bound = oit_pass == OIT_SUBPASS_ACCUMULATE &&
                                 engine->moments_this_frame && engine->moment_atlas_texture != 0;
            uniform_set_int(u, "sceneColorAvailable",
                            engine->scene_color_this_frame && !moments_bound ? 1 : 0);
            if (moments_bound || engine->scene_color_this_frame) {
                glActiveTexture(GL_TEXTURE0 + TEXUNIT_SCENE_COLOR);
                glBindTexture(GL_TEXTURE_2D, moments_bound ? engine->moment_atlas_texture
                                                           : engine->opaque_color_texture);
                glActiveTexture(GL_TEXTURE0);
            }
            // Both are read only inside an OIT sub-pass, so they upload only
            // there: the warp interval the moments are stated over, and (where
            // the atlas is actually bound) its reciprocal size, which is not the
            // frame's because the atlas is twice as tall.
            if (oit_pass != OIT_SUBPASS_NONE) {
                const float near_far[2] = {camera->near_clip, camera->far_clip};
                uniform_set_vec2(u, "oitNearFar", near_far);
            }
            if (moments_bound) {
                const float inv_size[2] = {1.0f / (float)engine->moment_w,
                                           1.0f / (float)(engine->moment_h * 2)};
                uniform_set_vec2(u, "oitMomentInvSize", inv_size);
            }
            _update_camera_uniforms(program, camera);

            // Light data arrives via the clustered UBOs (uploaded once per
            // frame in render_current_scene) -- no per-node upload.

            // Unconditional: every per-light-type gate lives inside, so this
            // file does not model which types can cast.
            if (scene && scene->shadow_system)
                bind_shadow_maps_to_program(scene->shadow_system, program);
            else
                uniform_set_int(u, "numShadowLights", 0);

            // Bind the material mask array (roughness/metallic/ao/opacity/
            // microsurface/anisotropy/subsurface packed into one layered
            // texture). Always bind to satisfy the sampler2DArray; each
            // material's per-mask layer indices select or skip a layer.
            mask_array_bind(scene ? scene->mask_array : NULL, TEXUNIT_MASKS);
            uniform_set_int(u, "maskArray", TEXUNIT_MASKS);

            bind_ltc_tables(engine->ltc, program);

            // Engine-owned BRDF tables: bound for every scene, environment
            // or not, so the shader's sheen E read is always valid
            glActiveTexture(GL_TEXTURE0 + IBL_BRDF_LUT_TEXTURE_UNIT);
            glBindTexture(GL_TEXTURE_2D, engine->brdf_lut);
            glActiveTexture(GL_TEXTURE0);
            uniform_set_int(u, "brdfLUT", IBL_BRDF_LUT_TEXTURE_UNIT);

            // Bind IBL textures if available
            if (scene && scene->ibl && scene->ibl->precomputed) {
                bind_ibl_textures(scene->ibl, program);
            } else {
                // Set IBL sampler uniforms to their designated texture units even when disabled
                // This prevents type mismatch when samplerCube defaults to unit 0 (which has 2D
                // textures)
                uniform_set_int(u, "irradianceMap", IBL_IRRADIANCE_TEXTURE_UNIT);
                uniform_set_int(u, "prefilteredMap", IBL_PREFILTER_TEXTURE_UNIT);
                uniform_set_int(u, "charliePrefilteredMap", IBL_CHARLIE_TEXTURE_UNIT);
                uniform_set_int(u, "iblEnabled", 0);
            }
            // Only the no-IBL branch reads it, but set it unconditionally: a
            // stale value on a program that later loses its IBL would be a
            // silent one.
            uniform_set_vec3(u, "ambientRadiance",
                             scene ? scene->ambient_radiance : (vec3){0.0f, 0.0f, 0.0f});

            // Local reflection probe (parallax-corrected specular), rebinding
            // the IBL prefilter unit to the probe capture. The probe joins
            // the scene only after its capture, so the capture pass itself
            // never consumes it.
            if (scene && reflection_probe_active(scene->probe)) {
                bind_reflection_probe(scene->probe, program);
            } else {
                uniform_set_int(u, "probeEnabled", 0);
            }

            // Indirect diffuse from the probe grid, replacing the flat
            // irradiance map. Self-gates to giEnabled = 0 while the volume is
            // absent or has never converged, so the call site stays one line.
            //
            // It does NOT gate on capture, so every sweep after the first reads
            // the atlas it is rewriting -- feedback, i.e. a bounce per sweep.
            // That is standard DDGI and probably what you want, but it arrived
            // as a side effect of first_pass doing double duty rather than as a
            // decision, and it means a volume converged at load (one bounce) and
            // one converged by moving the sun (many) do not match. Spec 9.7
            // records it as open.
            gi_volume_bind(scene ? scene->gi_volume : NULL, program);
        }

        // The three per-object transforms, for a draw that carries one object.
        // A batched draw reads them out of the instance block instead, and
        // writing them anyway is not merely wasted: `model` changing per draw
        // is what makes the driver's program uniform block dirty, which costs a
        // copy of the WHOLE block at submission. Batching removed most of the
        // draws that pay it; leaving these here would have kept the rest paying.
        //
        // Skipping the write leaves the value cache and GL agreeing -- the
        // cache records what it wrote -- so the next single-object draw of this
        // program still uploads correctly.
        if (instances == 1) {
            uniform_set_mat4(u, "model", (const float*)node->global_transform);
            uniform_set_mat4(u, "uPrevModel", (const float*)node->prev_global_transform);
            // Normal matrix = transpose(inverse(model)), the transform normals
            // need under non-uniform scale. The vertex shaders used to compute
            // it themselves, which meant a full mat4 inverse PER VERTEX for a
            // value that is constant across the draw -- hundreds of thousands
            // of times a frame on the grass mesh. It now rides on the node,
            // computed where the transform it derives from is. Location-guarded,
            // so programs without the uniform (particles) no-op.
            uniform_set_mat3(u, "uNormalMatrix", (const float*)node->normal_matrix);
        }
        uniform_set_float(u, "lineWidth", mesh->line_width);
        // Wind cloth-mask bounds: per-mesh geometry (local AABB Y). The shader
        // uses them only when this mesh's material opted in (uWindResponse > 0).
        uniform_set_float(u, "uWindMaskMinY", mesh->aabb.min[1]);
        uniform_set_float(u, "uWindMaskMaxY", mesh->aabb.max[1]);

        // Only update material uniforms if material changed
        if (submit_take_material(state, mat)) {
            _update_program_material_uniforms(program, mat, a2c_capable);
            if (stats)
                stats->material_switches++;
        }

        // Update skinning uniforms for skinned meshes
        render_update_skinning_uniforms(program, mesh);

        // Set mesh-specific uniforms for vertex colors and UV1
        uniform_set_int(u, "vertexColorExists", mesh->colors ? 1 : 0);
        uniform_set_int(u, "texCoords2Exists", mesh->tex_coords2 ? 1 : 0);

        // Alpha-masked materials (hair/foliage) render with alpha-to-coverage:
        // MSAA converts fractional alpha into sample coverage for soft,
        // order-independent edges, and uncovered samples stay open for the
        // skybox drawn later
        bool use_a2c = (item->flags & DRAW_ALPHA_MASKED) && a2c_capable;
        if (use_a2c) {
            glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
        }

        // Handle double-sided materials
        if (item->flags & DRAW_DOUBLE_SIDED) {
            glDisable(GL_CULL_FACE);
        }

        submit_bind_vao(state, mesh->vao);
        uniform_set_int(u, "uInstanced", instances > 1 ? 1 : 0);
        if (instances > 1)
            glDrawElementsInstanced(mesh->draw_mode, mesh->index_count, GL_UNSIGNED_INT, 0,
                                    (GLsizei)instances);
        else
            glDrawElements(mesh->draw_mode, mesh->index_count, GL_UNSIGNED_INT, 0);
        if (stats) {
            stats->draws++;
            stats->instances += instances;
        }

        if (item->flags & DRAW_DOUBLE_SIDED) {
            glEnable(GL_CULL_FACE);
        }
        if (use_a2c) {
            glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
        }
    }
}

static void _render_xyz(SceneNode* node, mat4 view, mat4 projection, SubmitState* state) {
    if (!node || !node->xyz_shader_program || !node->xyz_shader_program->uniforms)
        return;

    ShaderProgram* program = node->xyz_shader_program;
    UniformManager* u = program->uniforms;

    submit_use_program(state, program->id);

    uniform_set_mat4(u, "model", (const float*)node->global_transform);
    uniform_set_mat4(u, "view", (const float*)view);
    uniform_set_mat4(u, "projection", (const float*)projection);

    submit_bind_vao(state, node->xyz_vao);
    glDrawArrays(GL_LINES, 0, xyz_vertices_size / (6 * sizeof(float)));
}

// How many items from `first` one draw can carry: consecutive, same geometry,
// all visible, capped at a chunk.
//
// Contiguity is required in the ACCEPTED stream, not the raw list. An item this
// pass does not draw -- wrong lane, or culled -- ends the run, because the batch
// submits whatever the chunk holds and skipping one would draw the next in its
// place. Cutting the run short is always safe: the remainder starts a new one.
static size_t _visible_run(const DrawList* list, size_t first, unsigned lanes,
                           const Frustum* frustum) {
    const DrawItem* head = &list->items[first];
    size_t n = 1;
    while (first + n < list->count && n < UBO_INSTANCE_MAX) {
        const DrawItem* next = &list->items[first + n];
        if (!(lanes & (1u << next->lane)) || !draw_run_can_join(head, next, frustum))
            break;
        n++;
    }
    return n;
}

void instance_chunk_upload(Ubo* ubo, InstanceChunk* chunk, const DrawList* list, size_t first,
                           size_t run, bool shading) {
    for (size_t k = 0; k < run; ++k) {
        const SceneNode* node = list->items[first + k].node;
        glm_mat4_copy(node->global_transform, chunk->model[k]);
        if (!shading)
            continue;
        glm_mat4_copy(node->prev_global_transform, chunk->prev_model[k]);
        // The block declares the normal matrix as a mat4 because std140 pads a
        // mat3's columns to 16 bytes where C packs them tight; ins3 writes the
        // upper 3x3 of an identity at that stride.
        glm_mat4_identity(chunk->normal[k]);
        glm_mat4_ins3(node->normal_matrix, chunk->normal[k]);
    }

    // The whole block, however short the run: sending only the live prefix
    // measured slower, and ubo_upload records why. Everything past `run`, and
    // the two arrays a depth pass skips, therefore go up carrying whatever the
    // last batch left there -- unread either way, since the shader indexes
    // [0, run) and reads only what its stage declares.
    ubo_upload(ubo, chunk, sizeof(*chunk));
}

// One pass over the flattened list. `lanes` is a bitmask of DrawLane, so a
// pass names the meshes it draws instead of re-deriving them: the opaque pass
// takes OPAQUE and XYZ, the OIT sub-passes take BLEND, the late pass takes
// whichever of BLEND and TRANSMISSIVE the OIT routing left for it.
static void _submit_lanes(const Engine* engine, Scene* scene, const DrawList* list, Camera* camera,
                          mat4 view, mat4 projection, RenderMode render_mode, SubmitState* state,
                          const Frustum* frustum, unsigned lanes, OitSubpass oit_pass) {
    if (!scene || !list)
        return;

    SubmitStats* stats = profiler_submit(engine->profiler);
    InstanceChunk chunk;

    for (size_t i = 0; i < list->count; ++i) {
        const DrawItem* item = &list->items[i];
        if (!(lanes & (1u << item->lane)))
            continue;

        // Cull BEFORE the run is formed. A rejected item inside a chunk would
        // shift every instance after it, so visibility has to be settled while
        // the run can still be cut short.
        if (stats)
            stats->meshes_seen++;
        if (!draw_item_visible(item, frustum)) {
            if (stats)
                stats->meshes_culled++;
            continue;
        }

        // How many of the following items this draw can carry. One is the
        // ordinary path and issues exactly what it always did.
        //
        // Only a program that reads InstanceBlock may carry more than one: the
        // rest take their transform from a per-draw uniform, so a batch would
        // stack every instance on the first one's transform and the others
        // would simply not appear. pbr_skinned is in that class, and so is any
        // program an app assigns to a scene mesh.
        const Material* mat = item->mesh->material;
        bool batchable = mat && mat->shader_program && mat->shader_program->instanced;
        size_t run = 1;
        if (batchable && engine->instancing_enabled && engine->instance_ubo)
            run = _visible_run(list, i, lanes, frustum);
        // The run's own items were counted as seen above only for the first;
        // the rest are counted here so the identity still holds. A no-op at
        // run == 1.
        if (stats)
            stats->meshes_seen += run - 1;
        if (run > 1)
            instance_chunk_upload(engine->instance_ubo, &chunk, list, i, run, true);

        _submit_item(engine, scene, item, camera, view, projection, render_mode, state, oit_pass,
                     run);
        i += run - 1;
    }
}

// The axis gizmos, after the meshes of the pass that draws them. They used to be
// interleaved per node; nothing sorts against them (they are unlit lines drawn
// with depth test on) and no fixture enables them, so the move is recorded here
// rather than claimed as verified.
static void _submit_gizmos(const DrawList* list, mat4 view, mat4 projection, SubmitState* state) {
    if (!list)
        return;
    for (size_t i = 0; i < list->gizmo_count; ++i)
        _render_xyz(list->gizmos[i], view, projection, state);
}

// The three counts that gate the late passes, taken from the list rather than
// accumulated as a side effect of the opaque pass. They used to be, which meant
// changing or skipping that pass silently switched off the ones after it.
//
// The transmissive count is frustum-gated and the other two are not, which is
// deliberate: it triggers a full-frame resolve that off-screen glass must not
// pay for, where the late pass the other two gate is cheap to enter.
static void _count_late_meshes(Scene* scene, const DrawList* list, const Frustum* frustum) {
    scene->transparent_mesh_count = 0;
    scene->transmissive_mesh_count = 0;
    scene->oit_mesh_count = 0;
    if (!list)
        return;

    // Two of the three are lane populations, known at build.
    scene->oit_mesh_count = list->lane_count[DRAW_LANE_BLEND];
    scene->transparent_mesh_count =
        list->lane_count[DRAW_LANE_BLEND] + list->lane_count[DRAW_LANE_TRANSMISSIVE];

    // Only the third varies per view, so only it needs a scan -- and only over
    // the transmissive lane.
    if (list->lane_count[DRAW_LANE_TRANSMISSIVE] == 0)
        return;
    for (size_t i = 0; i < list->count; ++i) {
        const DrawItem* item = &list->items[i];
        if (item->lane != DRAW_LANE_TRANSMISSIVE)
            continue;
        if (!frustum ||
            frustum_test_aabb_transformed(frustum, item->mesh->aabb.min, item->mesh->aabb.max,
                                          item->node->global_transform))
            scene->transmissive_mesh_count++;
    }
}

// Halton low-discrepancy sequence element (1-based index), for sub-pixel jitter.
static float _halton(int index, int base) {
    float f = 1.0f;
    float r = 0.0f;
    for (int i = index; i > 0; i /= base) {
        f /= (float)base;
        r += f * (float)(i % base);
    }
    return r;
}

void render_current_scene(Engine* engine) {
    if (!engine) {
        log_error("error: render called with NULL engine");
        return;
    }

    Scene* scene = get_current_scene(engine);
    if (!scene) {
        log_error("error: render called with NULL scene");
        return;
    }

    SceneNode* root_node = scene->root_node;
    if (!root_node) {
        log_error("error: render called with NULL root node");
        return;
    }

    Camera* camera = engine->camera;
    if (!camera) {
        log_error("error: render called with NULL camera");
        return;
    }

    mat4* view = &engine->view_matrix;
    mat4* projection = &engine->projection_matrix; // un-jittered

    RenderMode render_mode = engine->current_render_mode;

    // Un-jittered view-projection, computed once per frame for frustum culling
    // and motion vectors (and stashed as next frame's prev at the end). Held on
    // the engine so _render_node uploads it without recomputing per program.
    glm_mat4_mul(*projection, *view, engine->view_proj);
    Frustum frustum;
    frustum_extract_from_vp(engine->view_proj, &frustum);

    // Flatten once. Cube captures re-enter here six times with their own
    // camera; the stamp makes those five reuses rather than five rebuilds,
    // which is right on both counts -- the graph did not change, and each face
    // still culls against its own frustum at submit.
    //
    // The frame index alone would NOT be enough: an app that rebuilds geometry
    // in its render callback (apps/tree on a slider) frees meshes the shadow
    // pass already flattened, and a list reused on frame index would then draw
    // freed memory. The graph epoch is what makes that a rebuild.
    //
    // A build failure falls through with an empty list rather than returning:
    // every consumer loops over count, and returning here would skip the
    // per-frame flag resets and the prev_view_proj stash below, poisoning the
    // NEXT frame's motion vectors as well as this one's composite.
    draw_list_build(&scene->draw_list, scene, engine->total_frames ^ (scene_graph_epoch() << 32));

    // Clustered forward (spec 9.1): rebuild the light grid + UBOs for THIS
    // invocation's camera and viewport -- probe-capture faces re-enter here
    // with their own view/projection, so each face gets a correct grid.
    if (engine->light_cluster) {
        profiler_scope_begin(engine->profiler, "cluster build");
        GLint cluster_viewport[4];
        glGetIntegerv(GL_VIEWPORT, cluster_viewport);
        light_cluster_build_and_upload(engine->light_cluster, scene, *view, *projection,
                                       cluster_viewport[2], cluster_viewport[3], camera->near_clip,
                                       camera->far_clip);
        profiler_scope_end(engine->profiler);
    }

    // ViewParams (spec 10.1): republished per invocation for the same reason the
    // cluster grid is -- a probe-capture face re-enters here with its own view.
    //
    // Carries the WHOLE exposure -- camera and adaptation both. Adaptation runs
    // a frame behind (exposure.h), which is what breaks the circularity: the
    // meter reads a pre-exposed buffer and divides the pre-exposure back out, so
    // it measures absolute radiance either way and cannot chase its own tail.
    // That only holds because nothing upstream of it is a fixed multiple of
    // white any more -- see spec 10.1 phase 5.
    if (engine->view_ubo) {
        float pre = exposure_multiplier(&engine->exposure);
        if (!(pre > 0.0f))
            pre = 1.0f; // a zero or NaN here would blank the frame
        // A capture bakes ABSOLUTE radiance, so it renders at unity. The cubemap
        // it produces substitutes for the IBL environment prefilter -- same
        // sampler unit, same shader path -- and that prefilter comes from an HDR
        // file or a sky bake, neither pre-exposed. Baking the capture in working
        // space instead would make every consumer convert a value that was
        // already converted: pbr_frag folds it into `ambient`, which the
        // composite multiplies, and the debug background goes through
        // skybox_frag, which multiplies too. Squared exposure, invisible at
        // unity and wrong everywhere else.
        if (engine->capturing)
            pre = 1.0f;
        const float view_params[4] = {pre, 1.0f / pre, exposure_ev100(&engine->exposure), 0.0f};
        ubo_upload(engine->view_ubo, view_params, sizeof(view_params));
    }

    // Draw projection: the un-jittered projection, sub-pixel-jittered when TAA
    // runs so the temporal resolve accumulates coverage. Recomputed here every
    // call so every render loop — including apps that call
    // render_current_scene from their own loop — gets a correct projection.
    // Off in headless (jitter would break deterministic screenshots).
    mat4 draw_projection;
    glm_mat4_copy(*projection, draw_projection);
    bool taa_jitter_live = render_mode == RENDER_MODE_PBR && postfx_taa_active(engine->postfx) &&
                           (!engine->headless || engine->headless_jitter);
    if (taa_jitter_live) {
        // 16 Halton phases when TAAU upscales: each display pixel is visited by
        // fewer render samples per cycle, so the sequence needs more phases to
        // cover the same subpixel area. 8 at full scale -- bit-for-bit the
        // sequence TAA has always run. Asking the same predicate the resolve
        // dispatches on is what stops the sequence and its consumer diverging.
        int phases = postfx_taau_active(engine->postfx) ? 16 : 8;
        int j = (int)(engine->total_frames % phases) + 1;
        int rw, rh;
        engine_render_size(engine, &rw, &rh);
        float jx = _halton(j, 2) - 0.5f;
        float jy = _halton(j, 3) - 0.5f;
        draw_projection[2][0] += jx * 2.0f / (float)rw;
        draw_projection[2][1] += jy * 2.0f / (float)rh;
        // Published in render pixels: a [2][0] offset of 2*jx/rw shifts the
        // raster exactly jx pixels, and the TAAU resolve un-applies that to
        // place this frame's samples on the display grid.
        engine->postfx->taau_jitter_px[0] = jx;
        engine->postfx->taau_jitter_px[1] = jy;
    } else if (engine->postfx) {
        engine->postfx->taau_jitter_px[0] = 0.0f;
        engine->postfx->taau_jitter_px[1] = 0.0f;
    }
    // Stochastic PCSS rides the same predicate: the kernel may only rotate
    // while the TAA accumulator is live to average the noise it trades the
    // tap banding for. The seed freezes when no accumulator will average it
    // -- stricter than SSR's gate, deliberately (shadow.h has the why) --
    // so a still frame is a still frame. Modulo keeps the shader's float
    // hash well-conditioned over long sessions.
    if (scene->shadow_system) {
        scene->shadow_system->pcss_stochastic = taa_jitter_live;
        scene->shadow_system->pcss_frame_index =
            taa_jitter_live ? (int)(engine->total_frames % 4096) : 0;
    }
    // Published for postfx: depth-buffer reconstruction must invert the
    // projection the depth was actually rasterized with
    glm_mat4_copy(draw_projection, engine->draw_projection);

    // Cloud shell march (spec 11.0), inside the scene render so it sees
    // this frame's actual camera (the sky_clouds_march contract). Never
    // during captures (the march texture belongs to the main camera) and
    // only when the PBR skybox slot below will consume it -- debug render
    // modes would march, advance the wind clock, and discard the result.
    // Unjittered projection, the aerial/froxel discipline.
    if (scene->sky && !engine->capturing && render_mode == RENDER_MODE_PBR) {
        // Clouds are off by default, and the callee self-gates on that, so an
        // unconditional scope files a phantom row on every --sky run.
        profiler_scope_begin_if(engine->profiler, scene->sky->clouds.enabled,
                                    "cloud march");
        sky_clouds_march(scene->sky, engine, *view, *projection);
        profiler_scope_end(engine->profiler);
    }

    // Bound state the draw loop skips re-setting; see render.h
    SubmitState submit_state = {0};

    _count_late_meshes(scene, &scene->draw_list, &frustum);

    // Pass 1: opaque and alpha-masked meshes. The only pass that publishes the
    // normals G-buffer; every later pass (skybox, translucents, catcher,
    // overlays) writes color only.
    engine_set_scene_draw_buffers(engine, true);
    // False until this frame's resolve runs, so pass 1 never uploads or
    // binds a stale refraction source
    engine->scene_color_this_frame = false;
    engine->oit_this_frame = false; // set true below if the OIT accumulate pass runs
    engine->moments_this_frame = false;
    profiler_scope_begin(engine->profiler, "opaque");
    _submit_lanes(engine, scene, &scene->draw_list, camera, *view, draw_projection, render_mode,
                  &submit_state, &frustum, 1u << DRAW_LANE_OPAQUE, OIT_SUBPASS_NONE);
    _submit_gizmos(&scene->draw_list, *view, draw_projection, &submit_state);
    profiler_scope_end(engine->profiler);
    engine_set_scene_draw_buffers(engine, false);

    // Skybox after opaques (depth-tested against them at the far plane).
    // Skipped in debug render modes: those frames bypass tone mapping, and
    // the skybox shader emits linear HDR that would display uncorrected.
    // With the probe debug view on, the probe content replaces the skybox
    // (environment-only probes have no capture; show their prefilter source).
    if (scene->ibl && scene->ibl->precomputed && render_mode == RENDER_MODE_PBR) {
        profiler_scope_begin(engine->profiler, "skybox");
        if (scene->probe && scene->probe->debug_background) {
            render_skybox_cubemap(scene->ibl,
                                  scene->probe->cubemap ? scene->probe->cubemap
                                                        : scene->ibl->environment_cubemap,
                                  *view, draw_projection);
        } else if (scene->sky && scene->sky->enabled) {
            // Procedural sky owns the background (sky-view LUT + sun disc)
            // rather than the cubemap skybox
            sky_render_background(scene->sky, scene->ibl, *view, draw_projection,
                                  !engine->capturing);
        } else if (scene->render_skybox) {
            render_skybox(scene->ibl, *view, draw_projection, scene->skybox_brightness,
                          scene->skybox_ground_projection, scene->skybox_gp_radius,
                          scene->skybox_gp_height);
        }
        profiler_scope_end(engine->profiler);
    }

    // Shadow catcher: darken the environment floor where the model blocks the
    // shadow-casting lights. Over the skybox, which it blends onto, and BEFORE
    // everything that has to sort against the floor (spec 11.18).
    //
    // The position is load-bearing. The unsorted late pass, the OIT accumulate
    // and the particle depth resolve all test against this one depth buffer, so
    // the plane has to be in it before any of them run or translucency behind
    // the floor composites over the shadow instead of being hidden by it.
    // Ahead of the refraction resolve too, so transmissive surfaces see the
    // shadowed floor rather than an unshadowed one.
    if (scene->shadow_catcher && scene->shadow_system && scene->shadow_system->enabled &&
        scene->shadow_system->directional_count > 0 && engine->shadow_catcher_program &&
        engine->catcher_vao) {
        profiler_scope_begin(engine->profiler, "shadow catcher");
        ShaderProgram* catcher = engine->shadow_catcher_program;
        ShadowSystem* ss = scene->shadow_system;

        glUseProgram(catcher->id);
        uniform_set_mat4(catcher->uniforms, "view", (const float*)*view);
        uniform_set_mat4(catcher->uniforms, "projection", (const float*)draw_projection);
        uniform_set_float(catcher->uniforms, "catcherStrength", scene->shadow_catcher_strength);
        uniform_set_float(catcher->uniforms, "planeRadius", scene->skybox_gp_radius);

        // With SSR active the floor publishes depth and the reflective
        // marker across the whole quad (surfaceMode 1 skips the unshadowed
        // discard) so the reflection march has a surface to start from
        bool ssr_floor =
            engine->postfx && postfx_ssr_active(engine->postfx, engine->normals_this_frame);
        uniform_set_int(catcher->uniforms, "surfaceMode", ssr_floor ? 1 : 0);
        uniform_set_int(catcher->uniforms, "numShadowLights", (int)ss->directional_count);
        uniform_set_float(catcher->uniforms, "shadowBias", ss->shadow_bias);

        // Weight each caster's shadow by its light's share of analytic light
        float weights[MAX_SHADOW_LIGHTS] = {0};
        float weight_total = 0.0f;
        for (size_t i = 0; i < scene->light_count; i++) {
            const Light* light = scene->lights[i];
            if (light && light->shadow_map_index >= 0 &&
                light->shadow_map_index < MAX_SHADOW_LIGHTS) {
                weights[light->shadow_map_index] = light->intensity;
                weight_total += light->intensity;
            }
        }
        shadow_upload_cascade_uniforms(ss, catcher->uniforms);

        for (size_t i = 0; i < ss->directional_count && i < MAX_SHADOW_LIGHTS; i++) {
            char name[64];
            snprintf(name, sizeof(name), "shadowLightWeight[%zu]", i);
            uniform_set_float(catcher->uniforms, name,
                              weight_total > 0.0f ? weights[i] / weight_total : 0.0f);
        }

        float texel = 1.0f / (float)ss->default_map_size;
        uniform_set_vec2(catcher->uniforms, "shadowTexelSize", (vec2){texel, texel});

        glActiveTexture(GL_TEXTURE0 + SHADOW_MAP_TEXTURE_UNIT);
        glBindTexture(GL_TEXTURE_2D_ARRAY, ss->shadow_map_array);
        uniform_set_int(catcher->uniforms, "shadowMaps", SHADOW_MAP_TEXTURE_UNIT);

        // Explicit state: blended, visible from both sides. Depth writes stay
        // ON: this plane is what the transparent and particle passes below sort
        // against, and what gives the model a contact shadow in the postfx SSAO
        // resolve. The backdrop it stands in for writes no depth of its own.
        GLboolean cull_was_enabled = glIsEnabled(GL_CULL_FACE);
        glDisable(GL_CULL_FACE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        // The quad sits at y=0, and a scene may ship its own ground plane at
        // exactly that height. Pushed a few depth ULPs behind everything so
        // real geometry always wins the depth test at equal depth -- without
        // this, interpolation rounding lets the quad win in jitter-dependent
        // patches and it stamps its own shadow term over the already-shaded
        // floor as flickering rectangles. The backdrop dome floor writes no
        // depth, so shadows land there exactly as before.
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(1.0f, 8.0f);
        // The floor writes the reflective marker only when SSR consumes it;
        // otherwise it draws color-only and leaves the normals buffer (and
        // SSAO's read of it) untouched. Must come after the blanket blend
        // enable above, which resets the indexed blend-off state this call
        // establishes for attachment 1.
        engine_set_scene_draw_buffers(engine, ssr_floor);

        glBindVertexArray(engine->catcher_vao);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);

        engine_set_scene_draw_buffers(engine, false);
        glDisable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(0.0f, 0.0f);
        if (cull_was_enabled)
            glEnable(GL_CULL_FACE);
        profiler_scope_end(engine->profiler);
    }

    // Refraction source: resolve the opaque scene (including the skybox
    // just drawn) into the mipped color texture transmissive surfaces
    // sample. Gated on this frame's count, the engine toggle, and PBR mode
    // (debug modes skip the skybox and never reach the shader branch).
    if (scene->transmissive_mesh_count > 0 && render_mode == RENDER_MODE_PBR &&
        engine->refraction_enabled) {
        profiler_scope_begin(engine->profiler, "refraction resolve");
        engine->scene_color_this_frame = engine_resolve_opaque_color(engine);
        profiler_scope_end(engine->profiler);
    }

    // Pass 2: blend-mode (translucent) and transmissive meshes, composited
    // over the real background. Depth writes off; not sorted back-to-front
    // (typical models have few translucent meshes, e.g. a visor). Skipped
    // entirely when pass 1 saw none.
    if (scene->transparent_mesh_count > 0) {
        submit_state_reset(&submit_state);
        glDepthMask(GL_FALSE);
        // OIT (PBR only, and only when there are alpha-blend meshes to
        // accumulate): the blend meshes accumulate into the OIT FBO, which postfx
        // resolves and composites. Sets oit_this_frame so the trailing late pass
        // below draws transmissive-only; if OIT is off/non-PBR/no blend
        // meshes/targets fail, oit_this_frame stays false and that pass draws all
        // late meshes (the classic unsorted path, --no-oit). _render_node routes
        // on oit_this_frame, so the trailing call is correct either way.
        // Not under capture: engine_end_oit_pass re-binds engine->framebuffer at
        // the main render size, same hijack as the particle path below.
        if (engine->oit_enabled && !engine->capturing && render_mode == RENDER_MODE_PBR &&
            scene->oit_mesh_count > 0) {
            // Moment generation (spec 11.17), over the same mesh set and before
            // the accumulate: it summarises WHERE the opacity is along each
            // pixel's depth axis, so the accumulate can weight each fragment by
            // measured transmittance instead of the depth curve. Absorbance adds
            // where transmittance multiplies, which is what lets one unsorted
            // pass build it. Failing to allocate leaves the weighted-blended
            // path exactly as it was.
            bool moments_ready = false;
            if (engine->oit_moments_enabled && engine_begin_moment_pass(engine)) {
                profiler_scope_begin(engine->profiler, "oit moments");
                _submit_lanes(engine, scene, &scene->draw_list, camera, *view, draw_projection,
                              render_mode, &submit_state, &frustum, 1u << DRAW_LANE_BLEND,
                              OIT_SUBPASS_MOMENTS);
                profiler_scope_end(engine->profiler);
                engine_end_moment_pass(engine);
                moments_ready = true;
                submit_state_reset(&submit_state);
            }
            if (engine_begin_oit_pass(engine)) {
                engine->oit_this_frame = true;
                // Published only here, where the accumulate that consumes them
                // provably runs: postfx reads this to pick the composite's blend,
                // so moments announced without an accumulate behind them would
                // mis-composite the whole frame.
                engine->moments_this_frame = moments_ready;
                profiler_scope_begin(engine->profiler, "oit accumulate");
                _submit_lanes(engine, scene, &scene->draw_list, camera, *view, draw_projection,
                              render_mode, &submit_state, &frustum, 1u << DRAW_LANE_BLEND,
                              OIT_SUBPASS_ACCUMULATE);
                profiler_scope_end(engine->profiler);
                engine_end_oit_pass(engine);
            }
            submit_state_reset(&submit_state);
        }
        // Whatever OIT did not take: transmissive alone when the accumulate ran
        // (blend went to the OIT FBO), both lanes when it did not.
        unsigned late_lanes = engine->oit_this_frame
                                  ? (1u << DRAW_LANE_TRANSMISSIVE)
                                  : ((1u << DRAW_LANE_BLEND) | (1u << DRAW_LANE_TRANSMISSIVE));
        profiler_scope_begin(engine->profiler, "transparent");
        _submit_lanes(engine, scene, &scene->draw_list, camera, *view, draw_projection, render_mode,
                      &submit_state, &frustum, late_lanes, OIT_SUBPASS_NONE);
        profiler_scope_end(engine->profiler);
        glDepthMask(GL_TRUE);
    }
    // The mesh passes no longer unbind after every draw, so the one they left
    // bound is released once here. Hygiene rather than a requirement: the
    // particle and post paths bind their own.
    submit_state_reset(&submit_state);
    glBindVertexArray(0);

    // Particle systems attached to scene nodes: transparent pass into the HDR
    // framebuffer (so bloom/tonemap apply), with their own depth/blend bracket.
    // A separate block because the transparent pass above is gated on mesh count
    // and particles must draw even in an all-opaque scene. engine_resolve_scene_depth
    // re-binds the scene FBO before returning. Skip the whole thing (incl. the
    // full-res depth blit) on frames where nothing is alive.
    // Skipped entirely under capture: engine_resolve_scene_depth blits at the
    // MAIN render size and re-binds engine->framebuffer on the way out, which
    // would redirect the rest of the capture into the scene FBO. View-facing
    // billboards are also meaningless in a probe's omnidirectional capture.
    size_t live_particles = 0;
    if (!engine->capturing) {
        for (size_t i = 0; i < scene->particle_system_count && live_particles == 0; i++)
            live_particles = particle_system_live_count(scene->particle_systems[i]);
    }
    if (live_particles > 0) {
        // One scope covering all systems, not one per system: the row is the
        // pass, not the emitter count.
        profiler_scope_begin(engine->profiler, "particles");
        GLuint particle_depth = engine_resolve_scene_depth(engine);
        glDepthMask(GL_FALSE);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); // premultiplied
        ParticleRenderContext pctx = {0};
        glm_mat4_copy(*view, pctx.view);
        glm_mat4_copy(draw_projection, pctx.proj);
        pctx.scene = scene;
        pctx.scene_depth_texture = particle_depth;
        for (size_t i = 0; i < scene->particle_system_count; i++)
            particle_system_render(scene->particle_systems[i], &pctx);
        glDepthMask(GL_TRUE);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); // restore engine baseline
        profiler_scope_end(engine->profiler);
    }

    // Light overlay last, so the X-ray lines sit on top of the finished scene
    if (engine->show_lights)
        render_light_overlay(engine, scene);

    // Reset program state at end of frame
    glUseProgram(0);

    // Remember this frame's un-jittered view-projection for next frame's motion
    // vectors. Done here (not in the engine loop) so every render path keeps it.
    glm_mat4_copy(engine->view_proj, engine->prev_view_proj);
}

// Supersample factor for the blit capture path. 2x because that capture has no
// MSAA and single-sample grazing-angle aliasing at its horizon bakes in as
// stripe moire that mirror reflections magnify into banded streaks.
#define CAPTURE_SS_FACTOR 2

void scene_capture_begin(Engine* engine, Scene* scene, SceneCaptureState* saved) {
    if (!engine || !scene || !saved)
        return;

    // Nothing inside a capture burst is timed, and this is the seam that owns
    // that -- not scene_capture_faces, which starts too late: the shadow
    // re-bake below opens the same cascade scopes the frame does, and a probe
    // captured before the loop starts would file its bake into the first
    // frame's cascade row. A caller cannot reach a capture without coming
    // through here, which is why the policy lives here rather than in each of
    // them.
    profiler_suspend(engine->profiler);

    // The mask array must be packed before the first face: a capture taken with
    // the scalar fallbacks still in place bakes them into the cubemap forever.
    mask_array_ensure_built(scene, engine);

    saved->cascade_count = scene->shadow_system ? scene->shadow_system->cascade_count : 1;
    saved->msm_enabled = scene->shadow_system ? scene->shadow_system->msm_enabled : false;
    saved->render_time = engine->render_time;
    saved->render_delta = engine->render_delta;
    engine_set_render_time(engine, 0.0, 0.0);

    // Guarded on `enabled`, which is the one place the two hand-written copies
    // disagreed. Baking a map nothing will sample is pure cost: the bind path
    // publishes numShadowLights 0 when shadows are off.
    if (scene->shadow_system && scene->shadow_system->enabled) {
        scene->shadow_system->cascade_count = 1;
        // A bake reads the depth cascades directly and never the moment ones
        // (spec 11.22). Resolving here would cost a full array pass per cube
        // face, and because the moment array is sized from the cascade count it
        // would thrash between the bake's one layer and the frame's three.
        scene->shadow_system->msm_enabled = false;
        render_shadow_depth_pass(engine, scene);
    }
}

void scene_capture_end(Engine* engine, Scene* scene, const SceneCaptureState* saved) {
    if (!engine || !scene || !saved)
        return;
    profiler_resume(engine->profiler);
    engine_set_render_time(engine, saved->render_time, saved->render_delta);
    if (scene->shadow_system) {
        scene->shadow_system->cascade_count = saved->cascade_count;
        scene->shadow_system->msm_enabled = saved->msm_enabled;
    }
}

void scene_capture_faces(Engine* engine, struct IBLResources* ibl, const vec3 position,
                         GLuint dst_cubemap, GLuint dst_depth_cubemap, int face_size,
                         float near_clip, float far_clip) {
    if (!engine || !engine->camera || !dst_cubemap || face_size <= 0)
        return;
    // Keeping the depth means rendering straight into the destination faces:
    // a blit would have to carry depth between two differently-sized targets.
    const bool keep_depth = dst_depth_cubemap != 0;
    // Supersample factor for the blit path. A parameter once, but only ever one
    // live value from one caller, and meaningless on the other path -- so it was
    // a signature argument whose meaning depended on another argument, plus a
    // clamp no caller could reach.
    const int ss_factor = keep_depth ? 1 : CAPTURE_SS_FACTOR;
    // `ibl` is only the scratch target the supersampled path renders into before
    // downsampling. The direct path needs none, which is what lets a GI probe
    // volume capture in a scene with no HDR environment at all.
    if (!keep_depth && !ibl)
        return;

    // Below every early return, because a suspend that leaks is a profiler that
    // silently times nothing for the rest of the run. Six re-entries into
    // render_current_scene follow, each opening the same scope names the frame
    // itself uses; timing them would file a 256-pixel cube face under the row
    // that means the main pass. This is the one place the renderer re-renders
    // the world, so it is the one place that has to say so.
    profiler_suspend(engine->profiler);

    // Save everything the capture substitutes
    mat4 saved_view, saved_projection, saved_view_proj, saved_prev_view_proj;
    glm_mat4_copy(engine->view_matrix, saved_view);
    glm_mat4_copy(engine->projection_matrix, saved_projection);
    glm_mat4_copy(engine->view_proj, saved_view_proj);
    glm_mat4_copy(engine->prev_view_proj, saved_prev_view_proj);

    Camera* camera = engine->camera;
    vec3 saved_cam_pos;
    glm_vec3_copy(camera->position, saved_cam_pos);
    float saved_near = camera->near_clip;
    float saved_far = camera->far_clip;

    bool saved_taa = engine->postfx ? engine->postfx->taa_enabled : false;
    // Each capture face re-enters render_current_scene with TAA off, which
    // republishes a zero jitter. Restoring it keeps a capture taken AFTER the
    // main raster (this API is public) from handing the TAAU resolve a zero
    // offset to un-apply against a raster that was jittered.
    float saved_jitter[2] = {0.0f, 0.0f};
    if (engine->postfx) {
        saved_jitter[0] = engine->postfx->taau_jitter_px[0];
        saved_jitter[1] = engine->postfx->taau_jitter_px[1];
    }
    bool saved_refraction = engine->refraction_enabled;
    bool saved_capturing = engine->capturing;

    GLint saved_viewport[4];
    GLint saved_fbo;
    glGetIntegerv(GL_VIEWPORT, saved_viewport);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &saved_fbo);

    // ONE flag says "a capture is running"; passes that must sit out ask it
    // rather than having their configuration falsified here. The G-buffer used
    // to be switched off by clearing three this_frame flags, which is a list
    // that fell behind the moment a fourth attachment appeared --
    // engine_set_scene_draw_buffers now consults `capturing` directly and covers
    // every attachment by construction.
    //
    // TAA and refraction still substitute because they are read through paths
    // that predate the flag; folding them in is the obvious next step and is not
    // done here only to keep this commit's blast radius honest.
    engine->refraction_enabled = false;
    engine->capturing = true;
    if (engine->postfx)
        engine->postfx->taa_enabled = false;

    // Scene GL state: capture may run before the render loop's per-frame
    // preamble has ever executed, so establish it explicitly
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Supersampled capture: render each face at ss_factor x into a temporary
    // target and box-downsample into the cube face (an exact 4-tap average at a
    // 2:1 blit). The capture has no MSAA, and single-sample grazing-angle
    // aliasing at its horizon bakes in as stripe moire that mirror reflections
    // then magnify into banded streaks.
    const int ss_size = ss_factor * face_size;
    GLuint ss_tex = 0, face_fbo = 0;
    glGenFramebuffers(1, &face_fbo);
    if (!keep_depth) {
        glGenTextures(1, &ss_tex);
        glBindTexture(GL_TEXTURE_2D, ss_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, ss_size, ss_size, 0, GL_RGB, GL_FLOAT, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        glBindFramebuffer(GL_FRAMEBUFFER, ibl->capture_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ss_tex, 0);
        glBindRenderbuffer(GL_RENDERBUFFER, ibl->capture_rbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, ss_size, ss_size);
    }

    mat4 views[6];
    ibl_capture_views((float*)position, views);

    // Per-face shading is evaluated from the capture point
    glm_vec3_copy((float*)position, camera->position);
    camera->near_clip = near_clip;
    camera->far_clip = far_clip;
    glm_perspective(glm_rad(90.0f), 1.0f, near_clip, far_clip, engine->projection_matrix);

    for (int i = 0; i < 6; ++i) {
        if (keep_depth) {
            glBindFramebuffer(GL_FRAMEBUFFER, face_fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, dst_cubemap, 0);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                   GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, dst_depth_cubemap, 0);
            // Checked on the first face only, and here rather than at creation:
            // the FBO carries no attachment until one is bound, so a driver that
            // rejects a depth-textured cube face would otherwise fail silently
            // and leave every probe's visibility moments reading the clear value.
            if (i == 0 && glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                log_error("Capture FBO incomplete with a depth cubemap; skipping capture");
                break;
            }
            glViewport(0, 0, face_size, face_size);
        } else {
            glBindFramebuffer(GL_FRAMEBUFFER, ibl->capture_fbo);
            glViewport(0, 0, ss_size, ss_size);
        }
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glm_mat4_copy(views[i], engine->view_matrix);
        render_current_scene(engine);

        if (keep_depth)
            continue; // already in the destination faces

        glBindFramebuffer(GL_READ_FRAMEBUFFER, ibl->capture_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, face_fbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, dst_cubemap, 0);
        glBlitFramebuffer(0, 0, ss_size, ss_size, 0, 0, face_size, face_size, GL_COLOR_BUFFER_BIT,
                          GL_LINEAR);
    }

    profiler_resume(engine->profiler);

    glDeleteFramebuffers(1, &face_fbo);
    if (ss_tex)
        glDeleteTextures(1, &ss_tex);

    // Restore
    glm_mat4_copy(saved_view, engine->view_matrix);
    glm_mat4_copy(saved_projection, engine->projection_matrix);
    glm_mat4_copy(saved_view_proj, engine->view_proj);
    glm_mat4_copy(saved_prev_view_proj, engine->prev_view_proj);
    glm_vec3_copy(saved_cam_pos, camera->position);
    camera->near_clip = saved_near;
    camera->far_clip = saved_far;
    engine->refraction_enabled = saved_refraction;
    engine->capturing = saved_capturing;
    if (engine->postfx) {
        engine->postfx->taa_enabled = saved_taa;
        engine->postfx->taau_jitter_px[0] = saved_jitter[0];
        engine->postfx->taau_jitter_px[1] = saved_jitter[1];
    }

    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)saved_fbo);
    glViewport(saved_viewport[0], saved_viewport[1], saved_viewport[2], saved_viewport[3]);
}

/*
 * Light overlay
 *
 * Draws every scene light as an X-ray line overlay: a cross at its position
 * tinted by the light's own color, plus a wireframe sphere at the cull radius
 * light_cluster.c derives for clustered assignment (spec 9.1). That radius is
 * the one quantity in the light pipeline with no other visible consequence --
 * it comes from an epsilon heuristic, and a wrong one shows up only indirectly
 * as popping or an index-pool overflow warning. Directional lights get a
 * direction ray instead of a sphere (they have no falloff); lights whose
 * radius rounds to nothing are drawn grey, which is how an authored-but-dead
 * light announces itself.
 */
#define LIGHT_OVERLAY_RING_SEGMENTS 24
#define LIGHT_OVERLAY_CROSS_PIXELS  14.0f
// cross (3 segs) + 3 rings + direction ray, 2 verts per seg, 6 floats per vert
#define LIGHT_OVERLAY_FLOATS_PER_LIGHT ((3 + 3 * LIGHT_OVERLAY_RING_SEGMENTS + 1) * 2 * 6)

static void _overlay_line(float* v, size_t* n, const vec3 a, const vec3 b, const vec3 color) {
    const float* ends[2] = {a, b};
    for (int e = 0; e < 2; e++) {
        v[(*n)++] = ends[e][0];
        v[(*n)++] = ends[e][1];
        v[(*n)++] = ends[e][2];
        v[(*n)++] = color[0];
        v[(*n)++] = color[1];
        v[(*n)++] = color[2];
    }
}

// One great circle of the radius sphere, in the plane spanned by axis_u/axis_v
static void _overlay_ring(float* v, size_t* n, const vec3 center, float radius, const vec3 axis_u,
                        const vec3 axis_v, const vec3 color) {
    vec3 prev;
    for (int i = 0; i <= LIGHT_OVERLAY_RING_SEGMENTS; i++) {
        float t = (float)i / (float)LIGHT_OVERLAY_RING_SEGMENTS * 2.0f * GLM_PIf;
        vec3 p, tmp;
        glm_vec3_scale((float*)axis_u, cosf(t) * radius, p);
        glm_vec3_scale((float*)axis_v, sinf(t) * radius, tmp);
        glm_vec3_add(p, tmp, p);
        glm_vec3_add(p, (float*)center, p);
        if (i > 0)
            _overlay_line(v, n, prev, p, color);
        glm_vec3_copy(p, prev);
    }
}

void render_light_overlay(Engine* engine, Scene* scene) {
    if (!engine || !engine->bone_program || !scene || scene->light_count == 0)
        return;

    float* vertices = malloc(scene->light_count * LIGHT_OVERLAY_FLOATS_PER_LIGHT * sizeof(float));
    if (!vertices)
        return;
    size_t vertex_floats = 0;

    // Screen-constant cross size: world units per pixel at unit depth, scaled
    // by each light's distance (same trick the bone overlay uses)
    int render_w, render_h;
    engine_render_size(engine, &render_w, &render_h);
    Camera* cam = engine->camera;
    float world_per_px = (render_h > 0 && cam)
                             ? (2.0f * tanf(cam->fov_radians * 0.5f) / (float)render_h)
                             : 0.002f;
    vec3 cam_pos;
    glm_vec3_copy(cam ? cam->position : (vec3){0.0f, 0.0f, 0.0f}, cam_pos);

    for (size_t i = 0; i < scene->light_count; i++) {
        Light* light = scene->lights[i];
        if (!light || light->type == LIGHT_UNKNOWN)
            continue;

        float radius = light_cull_radius(light);
        bool dead = (light->type != LIGHT_DIRECTIONAL) && radius == 0.0f;

        // Normalize the tint so a dim light still reads; grey marks a light
        // culled to nothing
        vec3 color;
        if (dead) {
            glm_vec3_copy((vec3){0.35f, 0.35f, 0.35f}, color);
        } else {
            float peak = fmaxf(light->color[0], fmaxf(light->color[1], light->color[2]));
            glm_vec3_scale((float*)light->color, peak > 1e-4f ? 1.0f / peak : 1.0f, color);
        }

        vec3 pos;
        glm_vec3_copy(light->global_position, pos);

        vec3 to_cam;
        glm_vec3_sub(cam_pos, pos, to_cam);
        float depth = fmaxf(glm_vec3_norm(to_cam), 1e-4f);
        float cross = 0.5f * LIGHT_OVERLAY_CROSS_PIXELS * world_per_px * depth;

        for (int axis = 0; axis < 3; axis++) {
            vec3 d = {0.0f, 0.0f, 0.0f}, a, b;
            d[axis] = cross;
            glm_vec3_sub(pos, d, a);
            glm_vec3_add(pos, d, b);
            _overlay_line(vertices, &vertex_floats, a, b, color);
        }

        if (light->type == LIGHT_DIRECTIONAL) {
            // No falloff to show: point a ray down the beam instead
            vec3 dir, tip;
            glm_vec3_copy(light->direction, dir);
            if (glm_vec3_norm2(dir) > 1e-8f) {
                glm_vec3_normalize(dir);
                glm_vec3_scale(dir, cross * 6.0f, dir);
                glm_vec3_add(pos, dir, tip);
                _overlay_line(vertices, &vertex_floats, pos, tip, color);
            }
        } else if (radius > 0.0f) {
            // Wireframe sphere at the clustered-lighting cull radius
            _overlay_ring(vertices, &vertex_floats, pos, radius, (vec3){1, 0, 0}, (vec3){0, 1, 0},
                        color);
            _overlay_ring(vertices, &vertex_floats, pos, radius, (vec3){1, 0, 0}, (vec3){0, 0, 1},
                        color);
            _overlay_ring(vertices, &vertex_floats, pos, radius, (vec3){0, 1, 0}, (vec3){0, 0, 1},
                        color);
        }
    }

    if (vertex_floats == 0) {
        free(vertices);
        return;
    }

    glBindVertexArray(engine->bone_line_vao);
    glBindBuffer(GL_ARRAY_BUFFER, engine->bone_line_vbo);
    glBufferData(GL_ARRAY_BUFFER, vertex_floats * sizeof(float), vertices, GL_DYNAMIC_DRAW);

    glUseProgram(engine->bone_program->id);
    uniform_set_mat4(engine->bone_program->uniforms, "view", (const float*)engine->view_matrix);
    uniform_set_mat4(engine->bone_program->uniforms, "projection",
                     (const float*)engine->projection_matrix);

    // X-ray: lights are usually inside or behind geometry
    glDisable(GL_DEPTH_TEST);
    glDrawArrays(GL_LINES, 0, (GLsizei)(vertex_floats / 6));
    glEnable(GL_DEPTH_TEST);
    glBindVertexArray(0);

    free(vertices);
}

// Bone-overlay line thickness, in PIXELS of the rendered image. Held in screen
// space so a bone reads the same whether the camera is close or far.
#define BONE_LINE_PIXELS 3.0f

// Emit one bone segment as two triangles: 6 vertices of (xyz, rgb).
//
// This is a quad rather than a GL_LINES draw with glLineWidth because core
// profile deprecated wide lines -- Apple reports GL_ALIASED_LINE_WIDTH_RANGE
// 1.0..1.0 and errors on anything else, so the old glLineWidth(3.0) raised
// GL_INVALID_VALUE every frame and silently drew 1px. (It also poisoned error
// reporting: the stale error surfaced at the next check_gl_error, in the postfx
// normals resolve, which had nothing to do with it.)
//
// The offset direction is cross(segment, toCamera), so the quad always faces
// the viewer. shape_geo.glsl solves the same problem with a world-space XY
// perpendicular, which is correct only for the flat, head-on layouts the pcb
// and shapes apps draw; a skeleton is fully 3D under an orbiting camera and
// would thin out or vanish at unlucky angles.
static void _emit_bone_quad(float* v, size_t* n, const vec3 a, const vec3 b, const vec3 cam_pos,
                            float world_per_pixel_at_unit_depth, float r, float g, float bl) {
    vec3 seg;
    glm_vec3_sub((float*)b, (float*)a, seg);
    if (glm_vec3_norm2(seg) < 1e-12f)
        return; // zero-length bone: nothing to face

    vec3 mid;
    glm_vec3_add((float*)a, (float*)b, mid);
    glm_vec3_scale(mid, 0.5f, mid);

    vec3 to_cam;
    glm_vec3_sub((float*)cam_pos, mid, to_cam);
    float depth = glm_vec3_norm(to_cam);
    if (depth < 1e-6f)
        return;

    vec3 side;
    glm_vec3_cross(seg, to_cam, side);
    if (glm_vec3_norm2(side) < 1e-12f)
        return; // segment points straight at the camera; no stable perpendicular
    glm_vec3_normalize(side);

    // Half-width that subtends BONE_LINE_PIXELS at this segment's depth.
    float half_w = 0.5f * BONE_LINE_PIXELS * world_per_pixel_at_unit_depth * depth;
    glm_vec3_scale(side, half_w, side);

    vec3 corner[4];
    glm_vec3_sub((float*)a, side, corner[0]);
    glm_vec3_add((float*)a, side, corner[1]);
    glm_vec3_add((float*)b, side, corner[2]);
    glm_vec3_sub((float*)b, side, corner[3]);

    const int tri[6] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 6; i++) {
        v[(*n)++] = corner[tri[i]][0];
        v[(*n)++] = corner[tri[i]][1];
        v[(*n)++] = corner[tri[i]][2];
        v[(*n)++] = r;
        v[(*n)++] = g;
        v[(*n)++] = bl;
    }
}

void render_skeleton_bones(Engine* engine, Skeleton* skeleton, AnimationState* anim_state) {
    if (!engine || !engine->bone_program) {
        return;
    }

    // Need at least skeleton or anim_state
    Skeleton* skel = skeleton ? skeleton : (anim_state ? anim_state->skeleton : NULL);
    if (!skel)
        return;

    // Both poses (bind green, animated red), each bone a quad:
    // 2 sets * 6 vertices * 6 floats per vertex.
    float* vertices = malloc(skel->bone_count * 2 * 6 * 6 * sizeof(float));
    if (!vertices)
        return;

    size_t vertex_count = 0;

    // World units per pixel at unit depth, from the vertical FOV and the render
    // height. Multiplying by a point's distance gives the world size of one
    // pixel there, which is what keeps the quads a constant screen thickness.
    // Render (not display) height: the scene target is supersampled, so a
    // "pixel" here is a render-target pixel, which is what the quads rasterize into.
    int render_w, render_h;
    engine_render_size(engine, &render_w, &render_h);
    Camera* bone_cam = engine->camera;
    float world_per_px = (render_h > 0 && bone_cam)
                             ? (2.0f * tanf(bone_cam->fov_radians * 0.5f) / (float)render_h)
                             : 0.002f;
    vec3 cam_pos;
    glm_vec3_copy(bone_cam ? bone_cam->position : (vec3){0.0f, 0.0f, 0.0f}, cam_pos);

    // First pass: Draw BIND POSE in GREEN (from skeleton's inverse_bind_pose)
    if (skeleton) {
        // Compute bind pose global transforms by inverting inverse_bind_pose
        mat4* bind_globals = malloc(skel->bone_count * sizeof(mat4));
        if (bind_globals) {
            for (size_t i = 0; i < skel->bone_count; i++) {
                glm_mat4_inv(skel->bones[i].inverse_bind_pose, bind_globals[i]);
            }

            for (size_t i = 0; i < skel->bone_count; i++) {
                const Bone* bone = &skel->bones[i];
                if (bone->parent_index < 0)
                    continue;

                float child_x = bind_globals[i][3][0];
                float child_y = bind_globals[i][3][1];
                float child_z = bind_globals[i][3][2];

                float parent_x = bind_globals[bone->parent_index][3][0];
                float parent_y = bind_globals[bone->parent_index][3][1];
                float parent_z = bind_globals[bone->parent_index][3][2];

                // GREEN for bind pose
                _emit_bone_quad(vertices, &vertex_count, (vec3){parent_x, parent_y, parent_z},
                                (vec3){child_x, child_y, child_z}, cam_pos, world_per_px, 0.0f,
                                1.0f, 0.0f); // GREEN: bind pose
            }
            free(bind_globals);
        }
    }

    // Second pass: Draw ANIMATED POSE in RED (when animation has been played)
    if (anim_state && anim_state->global_transforms && anim_state->current_time > 0.0f) {
        for (size_t i = 0; i < skel->bone_count; i++) {
            const Bone* bone = &skel->bones[i];
            if (bone->parent_index < 0)
                continue;

            float child_x = anim_state->global_transforms[i][3][0];
            float child_y = anim_state->global_transforms[i][3][1];
            float child_z = anim_state->global_transforms[i][3][2];

            float parent_x = anim_state->global_transforms[bone->parent_index][3][0];
            float parent_y = anim_state->global_transforms[bone->parent_index][3][1];
            float parent_z = anim_state->global_transforms[bone->parent_index][3][2];

            _emit_bone_quad(vertices, &vertex_count, (vec3){parent_x, parent_y, parent_z},
                            (vec3){child_x, child_y, child_z}, cam_pos, world_per_px, 1.0f, 0.0f,
                            0.0f); // RED: animated pose
        }
    }

    if (vertex_count == 0) {
        free(vertices);
        return;
    }

    // Upload to GPU and draw
    glBindVertexArray(engine->bone_line_vao);
    glBindBuffer(GL_ARRAY_BUFFER, engine->bone_line_vbo);
    glBufferData(GL_ARRAY_BUFFER, vertex_count * sizeof(float), vertices, GL_DYNAMIC_DRAW);

    // Use bone program
    glUseProgram(engine->bone_program->id);
    uniform_set_mat4(engine->bone_program->uniforms, "view", (const float*)engine->view_matrix);
    uniform_set_mat4(engine->bone_program->uniforms, "projection",
                     (const float*)engine->projection_matrix);

    // Disable depth test for X-ray effect (bones always visible). Quads are
    // camera-facing but built without regard to winding, so back-face culling
    // has to stand down or half the bones vanish depending on orientation.
    GLboolean cull_was_on = glIsEnabled(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(vertex_count / 6));
    glEnable(GL_DEPTH_TEST);
    if (cull_was_on)
        glEnable(GL_CULL_FACE);

    free(vertices);
}
