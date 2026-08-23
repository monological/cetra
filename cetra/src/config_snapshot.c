#include "config_snapshot.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cglm/cglm.h>

#include "cJSON.h"
#include "camera.h"
#include "engine.h"
#include "exposure.h"
#include "gi_volume.h"
#include "ibl.h"
#include "light.h"
#include "light_cluster.h"
#include "material.h"
#include "postfx.h"
#include "probe.h"
#include "probe_set.h"
#include "scene.h"
#include "shadow.h"
#include "sky.h"
#include "util.h"
#include "water.h"

/*
 * Which live struct a row addresses. The resolver below turns one of these into
 * a base pointer, or NULL when the subsystem is absent -- an absent owner omits
 * its whole section rather than writing it empty, which is what lets one
 * snapshot format serve a scene with water and one without.
 */
typedef enum ConfigOwner {
    CFG_ENGINE = 0,
    CFG_POSTFX,
    CFG_EXPOSURE,
    CFG_SCENE,
    CFG_CAMERA,
    CFG_SHADOW,
    CFG_SKY,
    CFG_CLOUDS,
    CFG_IBL,
    CFG_GI,
    CFG_CLUSTER,
    CFG_WATER,
    CFG_WATER_WINDSEA,
    CFG_WATER_SWELL,
    // Owners below address ONE ELEMENT of a scene array rather than a
    // singleton, so the resolver cannot answer them -- the array walk supplies
    // each base in turn. Kept in the same table anyway: a probe's fields drift
    // from a probe's fields exactly as easily as anything else's.
    CFG_PROBE_ELEM,
    CFG_LIGHT_ELEM,
    // No static rows: a material's are synthesised from MATERIAL_PARAMS, which
    // is already the vocabulary. The owner exists so those rows can say what
    // they are.
    CFG_MATERIAL_ELEM,
} ConfigOwner;

// The first element owner. Rows at or above it are written by the array walk,
// and skipped by the singleton walk -- including its "this scene has no '%s'"
// warning, which is why testing `_owner_base() == NULL` alone will not do.
#define CFG_FIRST_ELEM_OWNER CFG_PROBE_ELEM
_Static_assert(CFG_FIRST_ELEM_OWNER > CFG_WATER_SWELL, "element owners must sort last");

typedef enum ConfigType {
    CFG_BOOL = 0,
    CFG_INT,
    CFG_FLOAT,
    CFG_VEC2,
    CFG_VEC3,
    // Stored as an int, written as one of `labels`. Named rather than numbered
    // because a snapshot is read by people as often as by the loader, and an
    // enum that renumbers would otherwise silently re-point every stored value.
    CFG_ENUM,
} ConfigType;

struct ConfigField;

// What a restore is allowed to touch beyond the field itself, and the two things
// it must defer. Both deferrals exist because the work is triggered by a MOVE
// rather than by a value: two sun angles are one sun move, and a camera pose is
// only coherent once eye, target and up have all arrived.
typedef struct ConfigApplyCtx {
    struct Engine* engine;
    struct Scene* scene;
    bool sun_moved;
    bool camera_moved;
} ConfigApplyCtx;

// Rows carrying one of these do NOT store; the function is responsible for the
// write. `v` holds the decoded value -- one element for a scalar or an enum,
// two or three for a vector.
typedef void (*ConfigApplyFn)(ConfigApplyCtx* ctx, void* base, const struct ConfigField* f,
                              const double* v, int n);

typedef struct ConfigField {
    unsigned char owner;
    unsigned char type;
    // Dotted path to the JSON object this key lives under. Nested objects are
    // created on demand, so the grouping is stated once per row and nowhere else.
    const char* section;
    const char* key;
    size_t offset;
    const char* const* labels; // CFG_ENUM only
    int label_count;
    ConfigApplyFn apply; // NULL = a plain store
} ConfigField;

#define CFG_ROW(owner_, type_, section_, key_, struct_, member_)                                   \
    {owner_, type_, section_, key_, offsetof(struct_, member_), NULL, 0, NULL}
#define CFG_ROW_ENUM(owner_, section_, key_, struct_, member_, labels_)                            \
    {owner_,                                                                                       \
     CFG_ENUM,                                                                                     \
     section_,                                                                                     \
     key_,                                                                                         \
     offsetof(struct_, member_),                                                                   \
     labels_,                                                                                      \
     (int)(sizeof(labels_) / sizeof((labels_)[0])),                                                \
     NULL}
#define CFG_ROW_FN(owner_, type_, section_, key_, struct_, member_, fn_)                           \
    {owner_, type_, section_, key_, offsetof(struct_, member_), NULL, 0, fn_}

// Enum vocabularies. Index IS the enum value, so a gap would misname every value
// after it -- the two that do not start at zero say so beside themselves.
static const char* const CFG_RENDER_MODES[] = {
    "pbr",     "normals", "world_pos",       "tex_coords",       "tangent_space",
    "flat",    "albedo",  "simple_lighting", "metallic_rough",   "velocity",
    "hdr_hotspots", "sss_hotspots", "extrapolation"};
static const char* const CFG_CAMERA_MODES[] = {"free", "orbit"};
static const char* const CFG_TONEMAPS[] = {"passthrough", "aces", "neutral", "agx"};
// 6 is a hole: the half-res fog buffer retired with the screen-space march,
// and the values are the shader's own debugView dispatch, so it cannot be closed.
static const char* const CFG_DEBUG_VIEWS[] = {"none", "ao",      "normals", "ssr",     "albedo",
                                              "ssgi", "unused_6", "spec_occ", "contact", "bent"};
static const char* const CFG_SPEC_OCC[] = {"off", "legacy", "bent", "split"};
static const char* const CFG_LUT_INTERP[] = {"trilinear", "tetrahedral"};
static const char* const CFG_METER_MODES[] = {"uniform", "centre", "spot"};
static const char* const CFG_WAVE_MODELS[] = {"gerstner", "fft"};
static const char* const CFG_LIGHT_UNITS[] = {"default", "candela", "lumens", "lux", "nits"};

/*
 * CFG_ENUM reads and writes through an int*, which is only the same object as
 * the enum member if the compiler sized it that way. It does here, and the
 * alternative -- a per-type read -- would put the type list in a second place.
 */
_Static_assert(sizeof(RenderMode) == sizeof(int), "CFG_ENUM addresses enums as int");
_Static_assert(sizeof(CameraMode) == sizeof(int), "CFG_ENUM addresses enums as int");
_Static_assert(sizeof(PostFXTonemapMode) == sizeof(int), "CFG_ENUM addresses enums as int");
_Static_assert(sizeof(PostFXDebugView) == sizeof(int), "CFG_ENUM addresses enums as int");
_Static_assert(sizeof(PostFXSpecOccMode) == sizeof(int), "CFG_ENUM addresses enums as int");
_Static_assert(sizeof(PostFXLutInterp) == sizeof(int), "CFG_ENUM addresses enums as int");
_Static_assert(sizeof(MeteringMode) == sizeof(int), "CFG_ENUM addresses enums as int");

/*
 * The row's field inside an owner. Returned as void* rather than cast at each
 * site: the offset arithmetic needs a byte pointer, and a char*-to-float* cast
 * is the one a portability checker reads as reinterpreting unaligned bytes.
 */
static void* _field_ptr(void* base, const ConfigField* f) {
    return (void*)((unsigned char*)base + f->offset);
}

static const void* _field_ptr_const(const void* base, const ConfigField* f) {
    return (const void*)((const unsigned char*)base + f->offset);
}

// Defined with the reader, and declared here so the apply hooks below can do
// their store through it rather than each spelling the vector rule again.
static void _store_value(const ConfigField* f, void* base, const double* v, int n);

/*
 * The rows that are not a store, and the rule for when a new one belongs here:
 * THE FIELD HAS AN ENTRY POINT THAT VALIDATES OR ALLOCATES, AND THE TABLE MUST
 * NOT GO ROUND IT.
 *
 * Stating it as "needs a target rebuild" would be wrong for two of the five
 * below and would mislead the next person adding a row. `render_scale` and
 * `ss_scale` do NOT need one -- `_engine_sync_render_targets` runs at every
 * frame top and rebuilds whenever the derived size disagrees, so a plain store
 * would be picked up anyway. What their setters buy is the CLAMP: a hand-edited
 * 0.2 would otherwise sail into a field no other entry point can reach, and the
 * frame-top sync would happily build it.
 */
static void _apply_render_scale(ConfigApplyCtx* ctx, void* base, const ConfigField* f,
                                const double* v, int n) {
    (void)base;
    (void)f;
    (void)n;
    set_engine_render_scale(ctx->engine, (float)v[0]);
}

static void _apply_msaa(ConfigApplyCtx* ctx, void* base, const ConfigField* f, const double* v,
                        int n) {
    (void)base;
    (void)f;
    (void)n;
    set_engine_msaa_samples(ctx->engine, (int)v[0]);
}

static void _apply_ss_scale(ConfigApplyCtx* ctx, void* base, const ConfigField* f, const double* v,
                            int n) {
    (void)base;
    (void)f;
    (void)n;
    set_engine_ss_scale(ctx->engine, (int)v[0]);
}

static void _apply_taa(ConfigApplyCtx* ctx, void* base, const ConfigField* f, const double* v,
                       int n) {
    (void)base;
    (void)f;
    (void)n;
    set_engine_taa_enabled(ctx->engine, v[0] != 0.0);
}

static void _apply_ssr_full_res(ConfigApplyCtx* ctx, void* base, const ConfigField* f,
                                const double* v, int n) {
    (void)base;
    (void)f;
    (void)n;
    postfx_set_ssr_full_res(ctx->engine->postfx, v[0] != 0.0);
}

// The store is the easy half. Clearing publish_fog_ambient is what makes it
// stick: the sky stamps its zenith radiance over this every time the sun moves,
// and the sun is about to move if the snapshot carried one.
static void _apply_fog_ambient(ConfigApplyCtx* ctx, void* base, const ConfigField* f,
                               const double* v, int n) {
    _store_value(f, base, v, n);
    if (ctx->scene && ctx->scene->sky)
        ctx->scene->sky->publish_fog_ambient = false;
}

/*
 * Two angles are ONE sun move, deferred so the re-bake runs once and never
 * against a half-restored sun.
 *
 * Flagged on a CHANGE, not on the row being present. Every snapshot of a scene
 * with a sky carries both angles, so flagging on presence re-bakes the
 * transmittance, multiscatter and sky-view LUTs, the environment cube and the
 * GGX prefilter on every restore -- including the common one, where the file is
 * restored onto the scene it was taken from and the sun has not moved at all.
 * That is 0.11 s a restore, and it drags sky_apply_sun_to_light along with it,
 * which overwrites the sun light's intensity the `lights` array just restored.
 */
static void _apply_sun_angle(ConfigApplyCtx* ctx, void* base, const ConfigField* f, const double* v,
                             int n) {
    if ((float)v[0] == *(const float*)_field_ptr_const(base, f))
        return;
    _store_value(f, base, v, n);
    ctx->sun_moved = true;
}

// Eye, target and up are one POSE: the view matrix built from any one of them is
// wrong until the other two have landed, hence the deferral rather than a
// rebuild per component.
static void _apply_camera_vec(ConfigApplyCtx* ctx, void* base, const ConfigField* f,
                              const double* v, int n) {
    _store_value(f, base, v, n);
    ctx->camera_moved = true;
}

/*
 * The table. Every row is one control; a control with no row is a control this
 * feature cannot carry, which is what `config-coverage` asserts against gui.c.
 */
static const ConfigField CFG_FIELDS[] = {
    // --- engine
    CFG_ROW_ENUM(CFG_ENGINE, "engine", "render_mode", Engine, current_render_mode,
                 CFG_RENDER_MODES),
    CFG_ROW_ENUM(CFG_ENGINE, "engine", "camera_mode", Engine, camera_mode, CFG_CAMERA_MODES),
    CFG_ROW_FN(CFG_ENGINE, CFG_INT, "engine", "msaa_samples", Engine, msaa_samples, _apply_msaa),
    CFG_ROW_FN(CFG_ENGINE, CFG_FLOAT, "engine", "render_scale", Engine, render_scale,
               _apply_render_scale),
    CFG_ROW_FN(CFG_ENGINE, CFG_INT, "engine", "ss_scale", Engine, ss_scale, _apply_ss_scale),

    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.overlays", "gui", Engine, show_gui),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.overlays", "wireframe", Engine, show_wireframe),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.overlays", "xyz", Engine, show_xyz),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.overlays", "fps", Engine, show_fps),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.overlays", "camera_hud", Engine, show_camera_hud),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.overlays", "bones", Engine, show_bones),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.overlays", "lights", Engine, show_lights),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.overlays", "cluster_heatmap", Engine, cluster_debug),

    CFG_ROW(CFG_ENGINE, CFG_FLOAT, "engine.material", "specular_aa", Engine,
            specular_aa_strength),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.material", "energy_comp", Engine, energy_comp_enabled),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.material", "refraction", Engine, refraction_enabled),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.material", "clearcoat", Engine, clearcoat_enabled),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.material", "specular", Engine, specular_enabled),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.material", "sheen", Engine, sheen_enabled),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.material", "parallax", Engine, parallax_enabled),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.material", "sss", Engine, sss_enabled),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.material", "skin_preint", Engine, skin_preint_enabled),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.material", "oit", Engine, oit_enabled),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.material", "oit_moments", Engine, oit_moments_enabled),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.material", "emissive_lights", Engine,
            emissive_lights_enabled),

    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.draw", "instancing", Engine, instancing_enabled),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.draw", "lod", Engine, lod_enabled),
    CFG_ROW(CFG_ENGINE, CFG_FLOAT, "engine.draw", "lod_bias", Engine, lod_bias),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.draw", "frustum_cull", Engine, frustum_cull_enabled),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.draw", "morph", Engine, morph_enabled),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.draw", "opaque_sort", Engine, opaque_sort_enabled),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.draw", "depth_prepass", Engine, depth_prepass_enabled),

    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.layers_vt", "enabled", Engine, layers_vt_enabled),
    CFG_ROW(CFG_ENGINE, CFG_INT, "engine.layers_vt", "res", Engine, layers_vt_res),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.layers_vt", "pages", Engine, layers_vt_pages_enabled),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.layers_vt", "feedback", Engine,
            layers_vt_feedback_enabled),
    CFG_ROW(CFG_ENGINE, CFG_INT, "engine.layers_vt", "page_slots", Engine, layers_vt_page_slots),
    CFG_ROW(CFG_ENGINE, CFG_INT, "engine.layers_vt", "page_budget", Engine, layers_vt_page_budget),

    // --- postfx
    CFG_ROW_ENUM(CFG_POSTFX, "postfx", "tonemap", PostFX, tonemap_mode, CFG_TONEMAPS),
    CFG_ROW_ENUM(CFG_POSTFX, "postfx", "debug_view", PostFX, debug_view, CFG_DEBUG_VIEWS),
    CFG_ROW_FN(CFG_POSTFX, CFG_BOOL, "postfx", "taa", PostFX, taa_enabled, _apply_taa),
    CFG_ROW(CFG_POSTFX, CFG_BOOL, "postfx", "normals_gbuffer", PostFX, normals_enabled),

    CFG_ROW(CFG_POSTFX, CFG_BOOL, "postfx.bloom", "enabled", PostFX, bloom_enabled),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.bloom", "threshold", PostFX, bloom_threshold),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.bloom", "knee", PostFX, bloom_knee),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.bloom", "max_brightness", PostFX, bloom_max_brightness),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.bloom", "strength", PostFX, bloom_strength),

    CFG_ROW(CFG_POSTFX, CFG_BOOL, "postfx.flare", "enabled", PostFX, flare_enabled),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.flare", "strength", PostFX, flare_strength),
    CFG_ROW(CFG_POSTFX, CFG_INT, "postfx.flare", "ghosts", PostFX, flare_ghosts),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.flare", "ghost_spacing", PostFX, flare_ghost_spacing),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.flare", "halo_width", PostFX, flare_halo_width),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.flare", "chroma", PostFX, flare_chroma),
    CFG_ROW(CFG_POSTFX, CFG_INT, "postfx.flare", "source_lod", PostFX, flare_source_lod),

    CFG_ROW(CFG_POSTFX, CFG_BOOL, "postfx.ao", "enabled", PostFX, ssao_enabled),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.ao", "radius", PostFX, ssao_radius),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.ao", "strength", PostFX, ssao_strength),
    CFG_ROW_ENUM(CFG_POSTFX, "postfx.ao", "spec_occlusion", PostFX, spec_occlusion_mode,
                 CFG_SPEC_OCC),
    CFG_ROW(CFG_POSTFX, CFG_BOOL, "postfx.ao", "edge_filter", PostFX, ao_edge_filter_enabled),

    CFG_ROW(CFG_POSTFX, CFG_BOOL, "postfx.contact_shadows", "enabled", PostFX,
            contact_shadows_enabled),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.contact_shadows", "strength", PostFX, cs_strength),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.contact_shadows", "distance", PostFX, cs_distance),

    CFG_ROW(CFG_POSTFX, CFG_BOOL, "postfx.ssgi", "enabled", PostFX, ssgi_enabled),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.ssgi", "intensity", PostFX, ssgi_intensity),

    CFG_ROW(CFG_POSTFX, CFG_BOOL, "postfx.ssr", "enabled", PostFX, ssr_enabled),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.ssr", "strength", PostFX, ssr_strength),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.ssr", "max_distance", PostFX, ssr_max_distance),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.ssr", "thickness_min", PostFX, ssr_thickness_min),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.ssr", "max_roughness", PostFX, ssr_max_roughness),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.ssr", "floor_roughness", PostFX, ssr_floor_roughness),
    CFG_ROW(CFG_POSTFX, CFG_BOOL, "postfx.ssr", "temporal", PostFX, ssr_temporal),
    CFG_ROW(CFG_POSTFX, CFG_BOOL, "postfx.ssr", "denoise", PostFX, ssr_denoise),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.ssr", "jitter", PostFX, ssr_jitter),
    CFG_ROW_FN(CFG_POSTFX, CFG_BOOL, "postfx.ssr", "full_res", PostFX, ssr_full_res,
               _apply_ssr_full_res),

    CFG_ROW(CFG_POSTFX, CFG_BOOL, "postfx.fog", "enabled", PostFX, fog_enabled),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.fog", "density", PostFX, fog_density),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.fog", "height_falloff", PostFX, fog_height_falloff),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.fog", "floor_y", PostFX, fog_floor_y),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.fog", "near", PostFX, fog_near),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.fog", "far", PostFX, fog_far),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.fog", "depth_distribution", PostFX, fog_depth_dist),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.fog", "temporal_blend", PostFX, fog_temporal_blend),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.fog", "anisotropy", PostFX, fog_anisotropy),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.fog", "sun_boost", PostFX, fog_sun_boost),
    CFG_ROW_FN(CFG_POSTFX, CFG_VEC3, "postfx.fog", "ambient", PostFX, fog_ambient,
               _apply_fog_ambient),
    CFG_ROW(CFG_POSTFX, CFG_BOOL, "postfx.fog", "esm", PostFX, fog_esm_enabled),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.fog", "esm_sharpness", PostFX, fog_esm_k),
    CFG_ROW(CFG_POSTFX, CFG_INT, "postfx.fog", "grid_x", PostFX, froxel_grid_x),
    CFG_ROW(CFG_POSTFX, CFG_INT, "postfx.fog", "grid_y", PostFX, froxel_grid_y),
    CFG_ROW(CFG_POSTFX, CFG_INT, "postfx.fog", "grid_z", PostFX, froxel_grid_z),

    CFG_ROW(CFG_POSTFX, CFG_BOOL, "postfx.dof", "enabled", PostFX, dof_enabled),
    CFG_ROW(CFG_POSTFX, CFG_BOOL, "postfx.dof", "autofocus", PostFX, dof_autofocus),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.dof", "focus_distance", PostFX, dof_focus_distance),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.dof", "focus_range", PostFX, dof_focus_range),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.dof", "max_coc", PostFX, dof_max_coc),
    CFG_ROW(CFG_POSTFX, CFG_INT, "postfx.dof", "blades", PostFX, dof_blades),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.dof", "rotation", PostFX, dof_rotation),

    CFG_ROW(CFG_POSTFX, CFG_BOOL, "postfx.motion_blur", "enabled", PostFX, motion_blur_enabled),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.motion_blur", "scale", PostFX, motion_blur_scale),

    CFG_ROW(CFG_POSTFX, CFG_BOOL, "postfx.sharpen", "enabled", PostFX, sharpen_enabled),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.sharpen", "strength", PostFX, sharpen_strength),

    CFG_ROW(CFG_POSTFX, CFG_BOOL, "postfx.grade", "enabled", PostFX, grade_enabled),
    CFG_ROW(CFG_POSTFX, CFG_VEC3, "postfx.grade", "lift", PostFX, grade_lift),
    CFG_ROW(CFG_POSTFX, CFG_VEC3, "postfx.grade", "gamma", PostFX, grade_gamma),
    CFG_ROW(CFG_POSTFX, CFG_VEC3, "postfx.grade", "gain", PostFX, grade_gain),

    CFG_ROW(CFG_POSTFX, CFG_BOOL, "postfx.vignette", "enabled", PostFX, vignette_enabled),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.vignette", "strength", PostFX, vignette_strength),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.vignette", "radius", PostFX, vignette_radius),

    CFG_ROW(CFG_POSTFX, CFG_BOOL, "postfx.chromatic_aberration", "enabled", PostFX, ca_enabled),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.chromatic_aberration", "strength", PostFX, ca_strength),

    CFG_ROW(CFG_POSTFX, CFG_BOOL, "postfx.grain", "enabled", PostFX, grain_enabled),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.grain", "strength", PostFX, grain_strength),

    CFG_ROW(CFG_POSTFX, CFG_BOOL, "postfx.dither", "enabled", PostFX, dither_enabled),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.dither", "strength", PostFX, dither_strength),

    // The .cube path rides `source`, not here: the table only carries values the
    // owner struct holds, and PostFX keeps the basename alone.
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.lut", "strength", PostFX, lut_strength),
    CFG_ROW_ENUM(CFG_POSTFX, "postfx.lut", "interp", PostFX, lut_interp, CFG_LUT_INTERP),

    // --- exposure
    CFG_ROW(CFG_EXPOSURE, CFG_BOOL, "exposure", "physical", Exposure, physical),
    CFG_ROW(CFG_EXPOSURE, CFG_FLOAT, "exposure", "aperture", Exposure, aperture),
    CFG_ROW(CFG_EXPOSURE, CFG_FLOAT, "exposure", "shutter_speed", Exposure, shutter_speed),
    CFG_ROW(CFG_EXPOSURE, CFG_FLOAT, "exposure", "iso", Exposure, iso),
    CFG_ROW(CFG_EXPOSURE, CFG_FLOAT, "exposure", "multiplier", Exposure, multiplier),
    CFG_ROW(CFG_EXPOSURE, CFG_FLOAT, "exposure", "bias_stops", Exposure, bias_stops),
    CFG_ROW(CFG_EXPOSURE, CFG_BOOL, "exposure", "automatic", Exposure, automatic),
    CFG_ROW(CFG_EXPOSURE, CFG_FLOAT, "exposure", "key", Exposure, key),
    CFG_ROW_ENUM(CFG_EXPOSURE, "exposure", "meter_mode", Exposure, meter_mode, CFG_METER_MODES),
    CFG_ROW(CFG_EXPOSURE, CFG_FLOAT, "exposure", "meter_radius", Exposure, meter_radius),
    CFG_ROW(CFG_EXPOSURE, CFG_FLOAT, "exposure", "meter_low", Exposure, meter_low),
    CFG_ROW(CFG_EXPOSURE, CFG_FLOAT, "exposure", "meter_high", Exposure, meter_high),
    CFG_ROW(CFG_EXPOSURE, CFG_FLOAT, "exposure", "meter_min_log2", Exposure, meter_min_log2),
    CFG_ROW(CFG_EXPOSURE, CFG_FLOAT, "exposure", "meter_max_log2", Exposure, meter_max_log2),
    CFG_ROW(CFG_EXPOSURE, CFG_FLOAT, "exposure", "adapt_rate_up", Exposure, adapt_rate_up),
    CFG_ROW(CFG_EXPOSURE, CFG_FLOAT, "exposure", "adapt_rate_down", Exposure, adapt_rate_down),
    /*
     * The adaptation state, and it is CONFIGURATION here even though it is
     * history everywhere else. Auto-exposure multiplies every pixel, and a
     * restored session that re-approaches from cold differs from the one it was
     * taken from in the whole frame -- AGENTS.md measures a provably-no-op change
     * at 99.77% of pixels for exactly this reason. Carrying the two makes the
     * first restored frame continue rather than converge.
     */
    CFG_ROW(CFG_EXPOSURE, CFG_FLOAT, "exposure", "adapted_luminance", Exposure, adapted_luminance),
    CFG_ROW(CFG_EXPOSURE, CFG_BOOL, "exposure", "adapted_valid", Exposure, adapted_valid),

    // --- scene
    CFG_ROW(CFG_SCENE, CFG_VEC3, "scene", "ambient_radiance", Scene, ambient_radiance),
    CFG_ROW(CFG_SCENE, CFG_BOOL, "scene", "render_skybox", Scene, render_skybox),
    CFG_ROW(CFG_SCENE, CFG_FLOAT, "scene", "skybox_brightness", Scene, skybox_brightness),
    CFG_ROW(CFG_SCENE, CFG_BOOL, "scene.ground_projection", "enabled", Scene,
            skybox_ground_projection),
    CFG_ROW(CFG_SCENE, CFG_FLOAT, "scene.ground_projection", "radius", Scene, skybox_gp_radius),
    CFG_ROW(CFG_SCENE, CFG_FLOAT, "scene.ground_projection", "height", Scene, skybox_gp_height),
    CFG_ROW(CFG_SCENE, CFG_BOOL, "scene.shadow_catcher", "enabled", Scene, shadow_catcher),
    CFG_ROW(CFG_SCENE, CFG_FLOAT, "scene.shadow_catcher", "strength", Scene,
            shadow_catcher_strength),

    // --- camera. Radians, not the degrees Print Camera emits: this file is read
    // back by the loader, and a unit conversion is a second place to disagree.
    CFG_ROW_FN(CFG_CAMERA, CFG_VEC3, "camera", "eye", Camera, position, _apply_camera_vec),
    CFG_ROW_FN(CFG_CAMERA, CFG_VEC3, "camera", "target", Camera, look_at, _apply_camera_vec),
    CFG_ROW_FN(CFG_CAMERA, CFG_VEC3, "camera", "up", Camera, up_vector, _apply_camera_vec),
    CFG_ROW(CFG_CAMERA, CFG_FLOAT, "camera", "fov_radians", Camera, fov_radians),
    CFG_ROW(CFG_CAMERA, CFG_FLOAT, "camera", "near_clip", Camera, near_clip),
    CFG_ROW(CFG_CAMERA, CFG_FLOAT, "camera", "far_clip", Camera, far_clip),
    CFG_ROW(CFG_CAMERA, CFG_FLOAT, "camera.orbit", "distance", Camera, distance),
    CFG_ROW(CFG_CAMERA, CFG_FLOAT, "camera.orbit", "height", Camera, height),
    CFG_ROW(CFG_CAMERA, CFG_FLOAT, "camera.orbit", "theta", Camera, theta),
    CFG_ROW(CFG_CAMERA, CFG_FLOAT, "camera.orbit", "phi", Camera, phi),
    CFG_ROW(CFG_CAMERA, CFG_FLOAT, "camera.orbit", "amplitude", Camera, amplitude),
    CFG_ROW(CFG_CAMERA, CFG_FLOAT, "camera.orbit", "zoom_speed", Camera, zoom_speed),
    CFG_ROW(CFG_CAMERA, CFG_FLOAT, "camera.orbit", "orbit_speed", Camera, orbit_speed),

    // --- shadows. The map SIZE is not here and cannot be: create_shadow_system
    // takes it once and no flag, key or slider reaches it either.
    CFG_ROW(CFG_SHADOW, CFG_BOOL, "shadow", "enabled", ShadowSystem, enabled),
    CFG_ROW(CFG_SHADOW, CFG_INT, "shadow", "cascades", ShadowSystem, cascade_count),
    CFG_ROW(CFG_SHADOW, CFG_BOOL, "shadow", "cascade_tint", ShadowSystem, csm_debug),
    CFG_ROW(CFG_SHADOW, CFG_BOOL, "shadow", "translucent", ShadowSystem, tsm_enabled),
    CFG_ROW(CFG_SHADOW, CFG_FLOAT, "shadow", "bias", ShadowSystem, shadow_bias),
    CFG_ROW(CFG_SHADOW, CFG_VEC3, "shadow", "scene_center", ShadowSystem, scene_center),
    CFG_ROW(CFG_SHADOW, CFG_BOOL, "shadow.moments", "enabled", ShadowSystem, msm_enabled),
    CFG_ROW(CFG_SHADOW, CFG_INT, "shadow.moments", "size", ShadowSystem, msm_size),
    CFG_ROW(CFG_SHADOW, CFG_FLOAT, "shadow.moments", "blur", ShadowSystem, msm_blur),
    CFG_ROW(CFG_SHADOW, CFG_FLOAT, "shadow.moments", "bleed", ShadowSystem, msm_bleed),
    CFG_ROW(CFG_SHADOW, CFG_BOOL, "shadow.pcss", "enabled", ShadowSystem, pcss_enabled),
    CFG_ROW(CFG_SHADOW, CFG_FLOAT, "shadow.pcss", "softness", ShadowSystem, pcss_softness),

    // --- sky. Moving the sun re-bakes the LUTs, the environment and the IBL, so
    // these rows have a setter on apply where the rest are stores.
    CFG_ROW_FN(CFG_SKY, CFG_FLOAT, "sky", "sun_elevation", SkyAtmosphere, sun_elevation_deg,
               _apply_sun_angle),
    CFG_ROW_FN(CFG_SKY, CFG_FLOAT, "sky", "sun_azimuth", SkyAtmosphere, sun_azimuth_deg,
               _apply_sun_angle),
    CFG_ROW(CFG_SKY, CFG_FLOAT, "sky", "sun_disc", SkyAtmosphere, sun_disc_deg),
    CFG_ROW(CFG_SKY, CFG_FLOAT, "sky", "world_units_per_km", SkyAtmosphere, world_units_per_km),
    CFG_ROW(CFG_SKY, CFG_BOOL, "sky", "aerial", SkyAtmosphere, aerial_enabled),
    CFG_ROW(CFG_CLOUDS, CFG_BOOL, "sky.clouds", "enabled", CloudLayer, enabled),
    CFG_ROW(CFG_CLOUDS, CFG_FLOAT, "sky.clouds", "coverage", CloudLayer, coverage),
    CFG_ROW(CFG_CLOUDS, CFG_FLOAT, "sky.clouds", "cloud_type", CloudLayer, cloud_type),
    CFG_ROW(CFG_CLOUDS, CFG_FLOAT, "sky.clouds", "density", CloudLayer, density),
    CFG_ROW(CFG_CLOUDS, CFG_FLOAT, "sky.clouds", "wind_speed_kmh", CloudLayer, wind_speed_kmh),
    CFG_ROW(CFG_CLOUDS, CFG_FLOAT, "sky.clouds", "wind_dir", CloudLayer, wind_dir_deg),

    // --- environment
    CFG_ROW(CFG_IBL, CFG_FLOAT, "ibl", "intensity", IBLResources, intensity),

    // --- GI volume. The grid dimensions are an allocation, so they are absent
    // for the reason the shadow map size is.
    CFG_ROW(CFG_GI, CFG_BOOL, "gi", "enabled", GIVolume, enabled),
    CFG_ROW(CFG_GI, CFG_INT, "gi", "rate", GIVolume, rate),
    CFG_ROW(CFG_GI, CFG_BOOL, "gi", "debug_atlas", GIVolume, debug_atlas),

    CFG_ROW(CFG_CLUSTER, CFG_BOOL, "lighting", "area_lights", LightClusterContext,
            area_lights_enabled),

    // --- water. Editing any sea-state row re-seeds three 128^2 grids on the
    // next frame, which is a cost the restore pays once and says nothing about.
    CFG_ROW(CFG_WATER, CFG_BOOL, "water", "enabled", Water, enabled),
    CFG_ROW(CFG_WATER, CFG_FLOAT, "water", "level", Water, level),
    CFG_ROW(CFG_WATER, CFG_FLOAT, "water", "extent", Water, extent),
    CFG_ROW(CFG_WATER, CFG_VEC3, "water", "absorption", Water, absorption),
    CFG_ROW(CFG_WATER, CFG_VEC3, "water", "scatter", Water, scatter),
    CFG_ROW(CFG_WATER, CFG_FLOAT, "water", "roughness", Water, roughness),
    CFG_ROW(CFG_WATER, CFG_FLOAT, "water", "ior", Water, ior),
    CFG_ROW(CFG_WATER, CFG_FLOAT, "water", "amplitude", Water, amplitude),
    CFG_ROW(CFG_WATER, CFG_FLOAT, "water", "wavelength", Water, wavelength),
    CFG_ROW(CFG_WATER, CFG_FLOAT, "water", "steepness", Water, steepness),
    CFG_ROW(CFG_WATER, CFG_FLOAT, "water", "spread", Water, spread),
    CFG_ROW(CFG_WATER, CFG_VEC2, "water", "wind_dir", Water, wind_dir),
    CFG_ROW_ENUM(CFG_WATER, "water", "waves", Water, wave_model, CFG_WAVE_MODELS),
    CFG_ROW(CFG_WATER, CFG_BOOL, "water", "caustics", Water, caustics),
    CFG_ROW(CFG_WATER, CFG_BOOL, "water", "glitter", Water, glitter),
    CFG_ROW(CFG_WATER, CFG_BOOL, "water", "foam_history", Water, foam_history),
    CFG_ROW(CFG_WATER, CFG_FLOAT, "water", "foam_decay", Water, foam_decay),
    CFG_ROW(CFG_WATER, CFG_FLOAT, "water", "foam_drift", Water, foam_drift),
    CFG_ROW(CFG_WATER, CFG_BOOL, "water", "shore_coverage", Water, shore_coverage),
    CFG_ROW(CFG_WATER, CFG_BOOL, "water", "surf", Water, surf),
    CFG_ROW(CFG_WATER, CFG_BOOL, "water", "far_lod", Water, far_lod),
    CFG_ROW(CFG_WATER, CFG_BOOL, "water", "wetness", Water, wetness),
    CFG_ROW(CFG_WATER, CFG_BOOL, "water", "film", Water, film),
    CFG_ROW(CFG_WATER, CFG_FLOAT, "water", "sea_depth", Water, sea.sea_depth),

    CFG_ROW(CFG_WATER_WINDSEA, CFG_FLOAT, "water.wind_sea", "wind_speed", WaterWaveTrain,
            wind_speed),
    CFG_ROW(CFG_WATER_WINDSEA, CFG_FLOAT, "water.wind_sea", "fetch", WaterWaveTrain, fetch),
    CFG_ROW(CFG_WATER_WINDSEA, CFG_FLOAT, "water.wind_sea", "direction", WaterWaveTrain, direction),
    CFG_ROW(CFG_WATER_WINDSEA, CFG_FLOAT, "water.wind_sea", "scale", WaterWaveTrain, scale),
    CFG_ROW(CFG_WATER_WINDSEA, CFG_FLOAT, "water.wind_sea", "peak_enhancement", WaterWaveTrain,
            peak_enhancement),
    CFG_ROW(CFG_WATER_WINDSEA, CFG_FLOAT, "water.wind_sea", "focus", WaterWaveTrain, focus),
    CFG_ROW(CFG_WATER_WINDSEA, CFG_FLOAT, "water.wind_sea", "spread_gain", WaterWaveTrain,
            spread_gain),
    CFG_ROW(CFG_WATER_WINDSEA, CFG_FLOAT, "water.wind_sea", "spread_blend", WaterWaveTrain,
            spread_blend),

    CFG_ROW(CFG_WATER_SWELL, CFG_FLOAT, "water.swell", "wind_speed", WaterWaveTrain, wind_speed),
    CFG_ROW(CFG_WATER_SWELL, CFG_FLOAT, "water.swell", "fetch", WaterWaveTrain, fetch),
    CFG_ROW(CFG_WATER_SWELL, CFG_FLOAT, "water.swell", "direction", WaterWaveTrain, direction),
    CFG_ROW(CFG_WATER_SWELL, CFG_FLOAT, "water.swell", "scale", WaterWaveTrain, scale),
    CFG_ROW(CFG_WATER_SWELL, CFG_FLOAT, "water.swell", "peak_enhancement", WaterWaveTrain,
            peak_enhancement),
    CFG_ROW(CFG_WATER_SWELL, CFG_FLOAT, "water.swell", "focus", WaterWaveTrain, focus),
    CFG_ROW(CFG_WATER_SWELL, CFG_FLOAT, "water.swell", "spread_gain", WaterWaveTrain, spread_gain),
    CFG_ROW(CFG_WATER_SWELL, CFG_FLOAT, "water.swell", "spread_blend", WaterWaveTrain,
            spread_blend),

    /*
     * --- array elements. `section` is the ARRAY's name here, not a path: the
     * walk makes one object per element and writes these rows into it.
     *
     * A probe's position and box are absent because they are its CAPTURE, not
     * its tuning -- moving either without re-photographing the room describes a
     * mirror that is not where it says it is.
     */
    CFG_ROW(CFG_PROBE_ELEM, CFG_BOOL, "probes", "enabled", ReflectionProbe, enabled),
    CFG_ROW(CFG_PROBE_ELEM, CFG_FLOAT, "probes", "intensity", ReflectionProbe, intensity),
    CFG_ROW(CFG_PROBE_ELEM, CFG_FLOAT, "probes", "box_fade", ReflectionProbe, box_fade),
    CFG_ROW(CFG_PROBE_ELEM, CFG_BOOL, "probes", "debug_background", ReflectionProbe,
            debug_background),

    // Intensity is the STORED canonical value beside the authored unit, not the
    // GUI's display conversion: set_light_intensity_units is those two stores,
    // so writing both back is the same light and one conversion fewer to drift.
    CFG_ROW(CFG_LIGHT_ELEM, CFG_FLOAT, "lights", "intensity", Light, intensity),
    CFG_ROW_ENUM(CFG_LIGHT_ELEM, "lights", "intensity_unit", Light, units, CFG_LIGHT_UNITS),
    CFG_ROW(CFG_LIGHT_ELEM, CFG_FLOAT, "lights", "range", Light, range),
};

#define CFG_FIELD_COUNT ((int)(sizeof(CFG_FIELDS) / sizeof(CFG_FIELDS[0])))

/*
 * The element arrays: the JSON key, the owner whose rows describe an entry, and
 * whether entries have a NAME to match on.
 *
 * One list, because these three facts were stated in three places -- the writer,
 * the reader and the unknown-key sweep's skip list -- and a fourth array meant
 * editing all three, where forgetting the skip list warns about an array the
 * loader handled correctly.
 *
 * Probes are `false` because they have no name: their order IS their identity,
 * probes[0] being the one the .cscn authored first.
 */
static const struct {
    const char* key;
    ConfigOwner owner;
    bool by_name;
} CFG_ARRAYS[] = {
    {"probes", CFG_PROBE_ELEM, false},
    {"lights", CFG_LIGHT_ELEM, true},
    {"materials", CFG_MATERIAL_ELEM, true},
};

#define CFG_ARRAY_COUNT (sizeof(CFG_ARRAYS) / sizeof(CFG_ARRAYS[0]))

/*
 * What THIS session loaded, for the writer. See the header: the GUI produces
 * snapshots and cannot reach the app's arguments, so the app leaves them here.
 */
static ConfigSnapshotSource _source;
static char _source_model[512];
static char _source_hdr[512];
static char _source_lut[512];
static char _source_textures[512];
static bool _source_set;

/*
 * What the last READ found, which is a different fact and now has different
 * storage. These shared one cell until a review pointed out what that meant: a
 * function named `read` wrote the process's record of what the live session
 * loaded, so a `--config-dump` beside a `--config` named the restored file's
 * source instead of its own -- repaired only by the two calls happening 1,380
 * lines apart in main(), in an order neither call site mentions.
 */
static ConfigSnapshotSource _read_back;
static char _read_model[512];
static char _read_hdr[512];
static char _read_lut[512];
static char _read_textures[512];

// The app hands back pointers it got from a previous read, which are these very
// buffers -- so source and destination can be the same object, and snprintf with
// overlapping arguments is undefined.
static void _copy_source_path(char* dst, size_t cap, const char* src) {
    if (!src || !src[0]) {
        dst[0] = '\0';
        return;
    }
    if (dst != src)
        snprintf(dst, cap, "%s", src);
}

void config_snapshot_set_source(const ConfigSnapshotSource* src) {
    if (!src)
        return;
    _copy_source_path(_source_model, sizeof(_source_model), src->model);
    _copy_source_path(_source_hdr, sizeof(_source_hdr), src->hdr);
    _copy_source_path(_source_lut, sizeof(_source_lut), src->lut);
    _copy_source_path(_source_textures, sizeof(_source_textures), src->textures);
    _source.model = _source_model;
    _source.hdr = _source_hdr;
    _source.lut = _source_lut;
    _source.textures = _source_textures;
    _source.sky = src->sky;
    // No width/height: the writer takes those from the live engine, which is the
    // only place that knows what the window ended up as.
    _source_set = true;
}

static void* _owner_base(ConfigOwner owner, Engine* engine, Scene* scene) {
    switch (owner) {
    case CFG_ENGINE:
        return engine;
    case CFG_POSTFX:
        return engine ? engine->postfx : NULL;
    case CFG_EXPOSURE:
        return engine ? &engine->exposure : NULL;
    case CFG_SCENE:
        return scene;
    case CFG_CAMERA:
        return engine ? engine->camera : NULL;
    case CFG_SHADOW:
        return scene ? scene->shadow_system : NULL;
    case CFG_SKY:
        return scene ? scene->sky : NULL;
    case CFG_CLOUDS:
        return scene && scene->sky ? &scene->sky->clouds : NULL;
    case CFG_IBL:
        return scene ? scene->ibl : NULL;
    case CFG_GI:
        return scene ? scene->gi_volume : NULL;
    case CFG_CLUSTER:
        return engine ? engine->light_cluster : NULL;
    case CFG_WATER:
        return scene ? scene->water : NULL;
    case CFG_WATER_WINDSEA:
        return scene && scene->water ? &scene->water->sea.wind_sea : NULL;
    case CFG_WATER_SWELL:
        return scene && scene->water ? &scene->water->sea.swell : NULL;
    case CFG_PROBE_ELEM:
    case CFG_LIGHT_ELEM:
    case CFG_MATERIAL_ELEM:
        // Element owners have no singleton to resolve; the array walk supplies
        // each base. Answering NULL here is what keeps them out of the scalar
        // pass rather than needing a second test at every call.
        return NULL;
    }
    return NULL;
}

/*
 * The object a dotted section path names, created on the way if `create`.
 *
 * One walker for both directions: the writer creates as it goes, so sections
 * appear in the table's order and two snapshots diff line for line; the reader
 * must never create, because a missing section means the file did not carry it.
 * That is the whole difference, and splitting it in two would put the name
 * buffer's truncation rule in two places -- where the create side and the find
 * side must agree exactly or a long path is written under one name and looked up
 * under another.
 */
static cJSON* _section(cJSON* root, const char* path, bool create) {
    cJSON* node = root;
    const char* p = path;
    while (*p && node) {
        const char* dot = strchr(p, '.');
        size_t len = dot ? (size_t)(dot - p) : strlen(p);
        char name[64];
        if (len >= sizeof(name))
            len = sizeof(name) - 1;
        memcpy(name, p, len);
        name[len] = '\0';

        cJSON* child = cJSON_GetObjectItemCaseSensitive(node, name);
        if (!child && create)
            child = cJSON_AddObjectToObject(node, name);
        node = child;
        p = dot ? dot + 1 : p + len;
    }
    return node;
}

static const char* _enum_label(const ConfigField* f, int value) {
    if (value < 0 || value >= f->label_count)
        return NULL;
    return f->labels[value];
}

// Write one row into an already-resolved object. Split from the section lookup
// so an array element can supply its own, which is the only difference between
// a singleton row and an element row.
static bool _write_field(cJSON* obj, const ConfigField* f, const void* base) {
    if (!obj)
        return false;
    const void* p = _field_ptr_const(base, f);

    switch ((ConfigType)f->type) {
    case CFG_BOOL:
        return cJSON_AddBoolToObject(obj, f->key, *(const bool*)p) != NULL;
    case CFG_INT:
        return cJSON_AddNumberToObject(obj, f->key, *(const int*)p) != NULL;
    case CFG_FLOAT:
        return cJSON_AddNumberToObject(obj, f->key, *(const float*)p) != NULL;
    case CFG_VEC2:
    case CFG_VEC3: {
        const float* v = (const float*)p;
        const int n = f->type == CFG_VEC2 ? 2 : 3;
        double xyz[3] = {v[0], v[1], n > 2 ? v[2] : 0.0};
        cJSON* arr = cJSON_CreateDoubleArray(xyz, n);
        if (!arr)
            return false;
        if (!cJSON_AddItemToObject(obj, f->key, arr)) {
            cJSON_Delete(arr);
            return false;
        }
        return true;
    }
    case CFG_ENUM: {
        int value = *(const int*)p;
        const char* label = _enum_label(f, value);
        // An out-of-range value is a bug upstream, not here: name it rather than
        // writing a label that would read back as something else.
        if (!label) {
            fprintf(stderr, "Warning: config snapshot: %s.%s holds unknown value %d\n", f->section,
                    f->key, value);
            return cJSON_AddNumberToObject(obj, f->key, value) != NULL;
        }
        return cJSON_AddStringToObject(obj, f->key, label) != NULL;
    }
    }
    return false;
}

static void _write_source(cJSON* root, Engine* engine) {
    cJSON* src = cJSON_AddObjectToObject(root, "source");
    if (!src)
        return;
    if (_source_set) {
        if (_source.model[0])
            cJSON_AddStringToObject(src, "model", _source.model);
        if (_source.hdr[0])
            cJSON_AddStringToObject(src, "hdr", _source.hdr);
        if (_source.lut[0])
            cJSON_AddStringToObject(src, "lut", _source.lut);
        if (_source.textures[0])
            cJSON_AddStringToObject(src, "textures", _source.textures);
        cJSON_AddBoolToObject(src, "sky", _source.sky);
    }
    if (engine) {
        cJSON_AddNumberToObject(src, "width", engine->win_width);
        cJSON_AddNumberToObject(src, "height", engine->win_height);
    }
}

/*
 * Every row of one element owner, into one object, under a key the reader can
 * match it back by.
 *
 * NAME where there is one, `index` where there is not, and both are written
 * rather than one or the other: `Light.name` and `Material.name` start NULL and
 * only an importer or a scene file fills them, so every CLI-spawned light and
 * every procedural material has none. A name is the stable key -- an index moves
 * when the content does -- but an unnamed element has no other, and writing the
 * index under the "name" key (which is what this did) produces an entry the
 * reader silently drops.
 */
static bool _write_element(cJSON* array, ConfigOwner owner, const void* base, const char* id_name,
                           int id_index, int* count) {
    cJSON* obj = cJSON_CreateObject();
    if (!obj || !cJSON_AddItemToArray(array, obj)) {
        cJSON_Delete(obj);
        return false;
    }
    if (id_name)
        cJSON_AddStringToObject(obj, "name", id_name);
    cJSON_AddNumberToObject(obj, "index", id_index);

    for (int i = 0; i < CFG_FIELD_COUNT; i++) {
        const ConfigField* f = &CFG_FIELDS[i];
        if ((ConfigOwner)f->owner != owner)
            continue;
        if (!_write_field(obj, f, base))
            return false;
        (*count)++;
    }
    return true;
}

/*
 * One MATERIAL_PARAMS entry, as a row this file's codec understands.
 *
 * MATERIAL_PARAMS already IS the material vocabulary -- the scene parser
 * resolves authored keys through it and the GUI builds its controls from it --
 * so the key, the offset and the enum labels all still come from there, and a
 * property added there is carried here for free. What the adapter buys is the
 * CODEC: shape checking, enum labels by name, and "refused by name rather than
 * coerced" all come from the same two functions every other row uses, instead of
 * a second encoder and a second decoder with quietly different rules.
 *
 * Returns false for TEXTURE rows: rebinding an image is authoring, which is the
 * .cscn's half of the boundary this whole file sits on.
 */
static bool _material_row(const MaterialParam* p, ConfigField* out) {
    if (!p || p->type == MATERIAL_PARAM_TEXTURE)
        return false;
    ConfigType type = CFG_FLOAT;
    if (p->type == MATERIAL_PARAM_COLOR)
        type = CFG_VEC3;
    else if (p->type == MATERIAL_PARAM_INT)
        type = p->enum_labels ? CFG_ENUM : CFG_INT;
    *out = (ConfigField){.owner = CFG_MATERIAL_ELEM,
                         .type = (unsigned char)type,
                         .section = "materials",
                         .key = p->key,
                         .offset = p->offset,
                         .labels = p->enum_labels,
                         .label_count = p->enum_count,
                         .apply = NULL};
    return true;
}
static bool _write_materials(cJSON* root, Scene* scene, int* count) {
    if (!scene || scene->material_count == 0)
        return true;
    cJSON* array = cJSON_AddArrayToObject(root, "materials");
    if (!array)
        return false;

    for (size_t m = 0; m < scene->material_count; m++) {
        const Material* material = scene->materials[m];
        if (!material)
            continue;
        cJSON* obj = cJSON_CreateObject();
        if (!obj || !cJSON_AddItemToArray(array, obj)) {
            cJSON_Delete(obj);
            return false;
        }
        // Name and index both, for the reason _write_element states.
        if (material->name)
            cJSON_AddStringToObject(obj, "name", material->name);
        cJSON_AddNumberToObject(obj, "index", (double)m);

        for (size_t p = 0; p < MATERIAL_PARAM_COUNT; p++) {
            ConfigField row;
            if (!_material_row(&MATERIAL_PARAMS[p], &row))
                continue;
            if (!_write_field(obj, &row, material))
                return false;
            (*count)++;
        }
    }
    return true;
}

char* config_snapshot_write(Engine* engine, Scene* scene, int* out_fields) {
    int written = 0;
    if (out_fields)
        *out_fields = 0;
    cJSON* root = cJSON_CreateObject();
    if (!root)
        return NULL;
    cJSON_AddNumberToObject(root, "version", 1);
    _write_source(root, engine);

    for (int i = 0; i < CFG_FIELD_COUNT; i++) {
        const ConfigField* f = &CFG_FIELDS[i];
        if ((ConfigOwner)f->owner >= CFG_FIRST_ELEM_OWNER)
            continue;
        const void* base = _owner_base((ConfigOwner)f->owner, engine, scene);
        // An absent subsystem omits its whole section rather than writing it
        // empty -- a snapshot from a waterless scene must not claim a water
        // block whose every value is uninitialised memory.
        if (!base)
            continue;
        if (!_write_field(_section(root, f->section, true), f, base)) {
            cJSON_Delete(root);
            return NULL;
        }
        written++;
    }

    bool ok = true;
    const ReflectionProbeSet* probes = scene ? scene->probe_set : NULL;
    if (probes && probes->count > 0) {
        cJSON* array = cJSON_AddArrayToObject(root, "probes");
        ok = array != NULL;
        for (int i = 0; ok && i < probes->count; i++) {
            if (probes->probes[i])
                ok = _write_element(array, CFG_PROBE_ELEM, probes->probes[i], NULL, i, &written);
        }
    }
    if (ok && scene && scene->light_count > 0) {
        cJSON* array = cJSON_AddArrayToObject(root, "lights");
        ok = array != NULL;
        for (size_t i = 0; ok && i < scene->light_count; i++) {
            const Light* light = scene->lights[i];
            if (light)
                ok = _write_element(array, CFG_LIGHT_ELEM, light, light->name, (int)i, &written);
        }
    }
    if (ok)
        ok = _write_materials(root, scene, &written);

    if (!ok) {
        cJSON_Delete(root);
        return NULL;
    }

    char* text = cJSON_Print(root);
    cJSON_Delete(root);
    if (text && out_fields)
        *out_fields = written;
    return text;
}

bool config_snapshot_save(Engine* engine, Scene* scene, const char* path) {
    if (!path || !path[0]) {
        fprintf(stderr, "Error: config snapshot: no output path\n");
        return false;
    }
    int fields = 0;
    char* text = config_snapshot_write(engine, scene, &fields);
    if (!text) {
        fprintf(stderr, "Error: config snapshot: could not serialise\n");
        return false;
    }

    FILE* f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "Error: config snapshot: cannot write '%s'\n", path);
        free(text);
        return false;
    }
    size_t len = strlen(text);
    bool ok = fwrite(text, 1, len, f) == len && fputc('\n', f) != EOF;
    fclose(f);
    free(text);

    if (!ok) {
        fprintf(stderr, "Error: config snapshot: short write to '%s'\n", path);
        return false;
    }
    printf("config snapshot written: %s (%d fields)\n", path, fields);
    fflush(stdout);
    return true;
}

// ---------------------------------------------------------------------------
// Reading one back.

static int _enum_value(const ConfigField* f, const char* label) {
    for (int i = 0; i < f->label_count; i++) {
        if (f->labels[i] && strcmp(f->labels[i], label) == 0)
            return i;
    }
    return -1;
}

/*
 * Decode one JSON item into up to three doubles, in the shape the row wants.
 * Returns the count, or 0 when the item cannot be that shape -- which is refused
 * BY NAME rather than coerced, because a silently-zeroed field renders as a
 * setting somebody chose.
 */
static int _decode_value(const ConfigField* f, const cJSON* item, double* out) {
    switch ((ConfigType)f->type) {
    case CFG_BOOL:
        if (!cJSON_IsBool(item))
            return 0;
        out[0] = cJSON_IsTrue(item) ? 1.0 : 0.0;
        return 1;
    case CFG_INT:
    case CFG_FLOAT:
        if (!cJSON_IsNumber(item))
            return 0;
        out[0] = item->valuedouble;
        return 1;
    case CFG_VEC2:
    case CFG_VEC3: {
        const int want = f->type == CFG_VEC2 ? 2 : 3;
        if (!cJSON_IsArray(item) || cJSON_GetArraySize(item) != want)
            return 0;
        for (int i = 0; i < want; i++) {
            const cJSON* e = cJSON_GetArrayItem(item, i);
            if (!cJSON_IsNumber(e))
                return 0;
            out[i] = e->valuedouble;
        }
        return want;
    }
    case CFG_ENUM: {
        // A number is accepted as well as a label, so a hand-edited file that
        // wrote the raw value still loads; the writer only ever emits labels.
        if (cJSON_IsNumber(item)) {
            out[0] = item->valuedouble;
            return 1;
        }
        if (!cJSON_IsString(item) || !item->valuestring)
            return 0;
        const int value = _enum_value(f, item->valuestring);
        if (value < 0)
            return 0;
        out[0] = value;
        return 1;
    }
    }
    return 0;
}

// Store a decoded value into the row's field. The mirror of _write_field's
// switch, and the reason both live in this file: they are one contract.
static void _store_value(const ConfigField* f, void* base, const double* v, int n) {
    void* p = _field_ptr(base, f);
    switch ((ConfigType)f->type) {
    case CFG_BOOL:
        *(bool*)p = v[0] != 0.0;
        break;
    case CFG_INT:
    case CFG_ENUM:
        *(int*)p = (int)v[0];
        break;
    case CFG_FLOAT:
        *(float*)p = (float)v[0];
        break;
    case CFG_VEC2:
    case CFG_VEC3: {
        float* dst = (float*)p;
        for (int i = 0; i < n; i++)
            dst[i] = (float)v[i];
        break;
    }
    }
}

// One row against one already-located object. Absent is silence -- a snapshot
// may legitimately predate a field -- but present-and-wrong is named.
static bool _apply_field(ConfigApplyCtx* ctx, const cJSON* obj, const ConfigField* f, void* base) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, f->key);
    if (!item)
        return false;
    double v[3] = {0.0, 0.0, 0.0};
    const int n = _decode_value(f, item, v);
    if (n == 0) {
        fprintf(stderr, "Warning: config snapshot: %s.%s is not a %s; ignored\n", f->section,
                f->key, f->type == CFG_ENUM ? "known value" : "value of the right shape");
        return false;
    }
    if (f->apply)
        f->apply(ctx, base, f, v, n);
    else
        _store_value(f, base, v, n);
    return true;
}

/*
 * The element arrays. Probes match on their index because their order IS their
 * identity -- probes[0] is the probe the .cscn authored first. Lights and
 * materials match on NAME, because theirs is the importer's order and a mesh
 * added to a model would silently re-point every entry after it.
 */
static int _apply_elements(ConfigApplyCtx* ctx, const cJSON* root, const char* array_key,
                           ConfigOwner owner) {
    const cJSON* array = cJSON_GetObjectItemCaseSensitive(root, array_key);
    if (!array)
        return 0;
    if (!cJSON_IsArray(array)) {
        fprintf(stderr, "Warning: config snapshot: '%s' is not an array; ignored\n", array_key);
        return 0;
    }

    int written = 0;
    const cJSON* entry = NULL;
    cJSON_ArrayForEach(entry, array) {
        void* base = NULL;
        if (owner == CFG_PROBE_ELEM) {
            const cJSON* idx = cJSON_GetObjectItemCaseSensitive(entry, "index");
            const ReflectionProbeSet* set = ctx->scene ? ctx->scene->probe_set : NULL;
            const int i = cJSON_IsNumber(idx) ? (int)idx->valuedouble : -1;
            if (set && i >= 0 && i < set->count)
                base = set->probes[i];
            if (!base) {
                fprintf(stderr, "Warning: config snapshot: no probe %d in this scene; ignored\n",
                        i);
                continue;
            }
        } else {
            // Name first, index second. Not interchangeable: a name survives the
            // light order changing and an index does not, so falling back is for
            // the lights that HAVE no name -- every one the CLI spawned.
            const cJSON* name = cJSON_GetObjectItemCaseSensitive(entry, "name");
            const char* want = cJSON_IsString(name) ? name->valuestring : NULL;
            for (size_t i = 0; want && ctx->scene && i < ctx->scene->light_count; i++) {
                Light* light = ctx->scene->lights[i];
                if (light && light->name && strcmp(light->name, want) == 0) {
                    base = light;
                    break;
                }
            }
            const cJSON* idx = cJSON_GetObjectItemCaseSensitive(entry, "index");
            const int i = cJSON_IsNumber(idx) ? (int)idx->valuedouble : -1;
            if (!base && !want && ctx->scene && i >= 0 && (size_t)i < ctx->scene->light_count)
                base = ctx->scene->lights[i];
            if (!base) {
                if (want)
                    fprintf(stderr,
                            "Warning: config snapshot: no light '%s' in this scene; ignored\n",
                            want);
                else
                    fprintf(stderr,
                            "Warning: config snapshot: no light %d in this scene; ignored\n", i);
                continue;
            }
        }

        for (int i = 0; i < CFG_FIELD_COUNT; i++) {
            const ConfigField* f = &CFG_FIELDS[i];
            if ((ConfigOwner)f->owner == owner && _apply_field(ctx, entry, f, base))
                written++;
        }
    }
    return written;
}

// Materials, through MATERIAL_PARAMS for the reason _write_materials states.
static int _apply_materials(ConfigApplyCtx* ctx, const cJSON* root) {
    const cJSON* array = cJSON_GetObjectItemCaseSensitive(root, "materials");
    if (!array || !cJSON_IsArray(array))
        return 0;

    int written = 0;
    const cJSON* entry = NULL;
    cJSON_ArrayForEach(entry, array) {
        // Name first, index for the unnamed -- _apply_elements' reasoning.
        const cJSON* name = cJSON_GetObjectItemCaseSensitive(entry, "name");
        const char* want = cJSON_IsString(name) ? name->valuestring : NULL;
        Material* material = NULL;
        for (size_t i = 0; want && ctx->scene && i < ctx->scene->material_count; i++) {
            Material* m = ctx->scene->materials[i];
            if (m && m->name && strcmp(m->name, want) == 0) {
                material = m;
                break;
            }
        }
        const cJSON* idx = cJSON_GetObjectItemCaseSensitive(entry, "index");
        const int at = cJSON_IsNumber(idx) ? (int)idx->valuedouble : -1;
        if (!material && !want && ctx->scene && at >= 0 &&
            (size_t)at < ctx->scene->material_count)
            material = ctx->scene->materials[at];
        if (!material) {
            if (want)
                fprintf(stderr,
                        "Warning: config snapshot: no material '%s' in this scene; ignored\n",
                        want);
            else
                fprintf(stderr, "Warning: config snapshot: no material %d in this scene; "
                                "ignored\n",
                        at);
            continue;
        }

        for (size_t p = 0; p < MATERIAL_PARAM_COUNT; p++) {
            const MaterialParam* param = &MATERIAL_PARAMS[p];
            ConfigField row;
            if (!_material_row(param, &row))
                continue;
            const cJSON* item = cJSON_GetObjectItemCaseSensitive(entry, param->key);
            if (!item)
                continue;
            double v[3] = {0.0, 0.0, 0.0};
            const int n = _decode_value(&row, item, v);
            if (n == 0) {
                fprintf(stderr, "Warning: config snapshot: materials.%s is not a %s; ignored\n",
                        param->key,
                        row.type == CFG_ENUM ? "known value" : "value of the right shape");
                continue;
            }
            // Stored through the material's own setter rather than through
            // _store_value: it is the same offset write today, and keeping the
            // canonical accessor is what carries a future change there into this
            // path instead of silently around it.
            const float fv[3] = {(float)v[0], (float)v[1], (float)v[2]};
            material_param_set(material, param, fv);
            written++;
        }
    }
    return written;
}

/*
 * Warn about anything in the file the table does not know.
 *
 * Derived from the table rather than from a hand-written key list per section
 * (which is how cscene.c does it, because its parser has no table to derive
 * from). Silence here is the failure that matters: these files get hand-edited,
 * and a mistyped key that quietly does nothing looks exactly like a setting that
 * does not work.
 */
static bool _table_has(const char* section, const char* key) {
    for (int i = 0; i < CFG_FIELD_COUNT; i++) {
        if (strcmp(CFG_FIELDS[i].section, section) == 0 && strcmp(CFG_FIELDS[i].key, key) == 0)
            return true;
    }
    return false;
}

// Is this path a section the table uses -- either one rows sit in directly, or
// one they sit BELOW? Both count: "postfx" holds rows and contains "postfx.ssr",
// while "engine.overlays" only holds rows, and testing for the second alone
// warned about every real subsection in the file.
static bool _table_has_section(const char* path) {
    const size_t len = strlen(path);
    for (int i = 0; i < CFG_FIELD_COUNT; i++) {
        const char* s = CFG_FIELDS[i].section;
        if (strncmp(s, path, len) == 0 && (s[len] == '\0' || s[len] == '.'))
            return true;
    }
    return false;
}

static void _warn_unknown_keys(const cJSON* obj, const char* path) {
    const cJSON* item = NULL;
    cJSON_ArrayForEach(item, obj) {
        if (!item->string || item->string[0] == '_') // _comment and friends
            continue;
        char child[128];
        snprintf(child, sizeof(child), "%s%s%s", path, path[0] ? "." : "", item->string);
        if (cJSON_IsObject(item) && _table_has_section(child)) {
            _warn_unknown_keys(item, child);
            continue;
        }
        if (!_table_has(path, item->string))
            fprintf(stderr, "Warning: config snapshot: '%s' is not a known setting; ignored\n",
                    child);
    }
}

// Does this owner have a row under this key? Materials answer from
// MATERIAL_PARAMS, which is where their rows come from.
static bool _element_has_key(ConfigOwner owner, const char* key) {
    if (owner == CFG_MATERIAL_ELEM) {
        ConfigField row;
        return _material_row(material_param_find(key), &row);
    }
    for (int i = 0; i < CFG_FIELD_COUNT; i++) {
        if ((ConfigOwner)CFG_FIELDS[i].owner == owner && strcmp(CFG_FIELDS[i].key, key) == 0)
            return true;
    }
    return false;
}

// Element objects carry both id keys beside the rows, so neither is a stray. One
// function for all three arrays: the skip list is the load-bearing part, and it
// was written twice -- so a third id key meant remembering both, and forgetting
// one warns on every element of one array, which is the noise that trains a
// reader to stop looking at these.
static void _warn_unknown_element_keys(const cJSON* array, ConfigOwner owner,
                                       const char* array_key) {
    const cJSON* entry = NULL;
    cJSON_ArrayForEach(entry, array) {
        const cJSON* item = NULL;
        cJSON_ArrayForEach(item, entry) {
            if (!item->string || item->string[0] == '_' || strcmp(item->string, "name") == 0 ||
                strcmp(item->string, "index") == 0)
                continue;
            if (!_element_has_key(owner, item->string))
                fprintf(stderr, "Warning: config snapshot: '%s.%s' is not a known setting; "
                                "ignored\n",
                        array_key, item->string);
        }
    }
}

// Top-level names the table does not own: the file's own version, and the source
// block's separate vocabulary. The arrays are not listed here -- CFG_ARRAYS is
// what says they exist, so adding a fourth cannot leave this list behind.
static void _warn_unknown_root(const cJSON* root) {
    static const char* const SKIP[] = {"version", "source"};
    const cJSON* item = NULL;
    cJSON_ArrayForEach(item, root) {
        if (!item->string || item->string[0] == '_')
            continue;
        bool skip = false;
        for (size_t i = 0; i < sizeof(SKIP) / sizeof(SKIP[0]) && !skip; i++)
            skip = strcmp(item->string, SKIP[i]) == 0;
        for (size_t i = 0; i < CFG_ARRAY_COUNT && !skip; i++)
            skip = strcmp(item->string, CFG_ARRAYS[i].key) == 0;
        if (skip)
            continue;
        if (cJSON_IsObject(item) && _table_has_section(item->string))
            _warn_unknown_keys(item, item->string);
        else
            fprintf(stderr, "Warning: config snapshot: '%s' is not a known section; ignored\n",
                    item->string);
    }

    for (size_t i = 0; i < CFG_ARRAY_COUNT; i++) {
        const cJSON* array = cJSON_GetObjectItemCaseSensitive(root, CFG_ARRAYS[i].key);
        if (cJSON_IsArray(array))
            _warn_unknown_element_keys(array, CFG_ARRAYS[i].owner, CFG_ARRAYS[i].key);
    }
}

int config_snapshot_apply_file(Engine* engine, Scene* scene, const char* path) {
    if (!engine || !path || !path[0])
        return -1;
    char* text = read_entire_file(path, NULL);
    if (!text) {
        fprintf(stderr, "Error: config snapshot: cannot read '%s'\n", path);
        return -1;
    }
    cJSON* root = cJSON_Parse(text);
    free(text);
    if (!root) {
        fprintf(stderr, "Error: config snapshot: '%s' is not valid JSON\n", path);
        return -1;
    }
    const cJSON* version = cJSON_GetObjectItemCaseSensitive(root, "version");
    if (cJSON_IsNumber(version) && (int)version->valuedouble != 1) {
        fprintf(stderr, "Warning: config snapshot version %d; expected 1\n",
                (int)version->valuedouble);
    }

    // Before applying, so a file with a typo says so even if the typo is in the
    // only section this scene could have used.
    _warn_unknown_root(root);

    ConfigApplyCtx ctx = {.engine = engine, .scene = scene};
    int written = 0;
    const char* absent = NULL;
    for (int i = 0; i < CFG_FIELD_COUNT; i++) {
        const ConfigField* f = &CFG_FIELDS[i];
        if ((ConfigOwner)f->owner >= CFG_FIRST_ELEM_OWNER)
            continue;
        const cJSON* obj = _section((cJSON*)root, f->section, false);
        if (!obj || !cJSON_IsObject(obj))
            continue;
        void* base = _owner_base((ConfigOwner)f->owner, engine, scene);
        if (!base) {
            // The file describes a subsystem this scene does not have -- a water
            // block on dry land. Said once per section rather than per row: rows
            // of one section are contiguous, so comparing against the last is
            // enough to collapse them.
            if (absent != f->section) {
                absent = f->section;
                fprintf(stderr, "Warning: config snapshot: this scene has no '%s'; ignored\n",
                        f->section);
            }
            continue;
        }
        if (_apply_field(&ctx, obj, f, base))
            written++;
    }

    written += _apply_elements(&ctx, root, "probes", CFG_PROBE_ELEM);
    written += _apply_elements(&ctx, root, "lights", CFG_LIGHT_ELEM);
    written += _apply_materials(&ctx, root);
    cJSON_Delete(root);

    // The two deferrals, in the order their costs demand: the sun re-bake feeds
    // the environment every later read uses, and the pose is last because the
    // orbit bookkeeping is derived from it.
    if (ctx.sun_moved && scene && scene->sky)
        sky_update_sun(scene->sky, scene->ibl, engine);
    if (ctx.camera_moved && engine->camera) {
        camera_sync_spherical_from_position(engine->camera);
        update_engine_camera_lookat(engine);
    }

    printf("config snapshot applied: %s (%d fields)\n", path, written);
    fflush(stdout);
    return written;
}

// The two shapes the source block is made of. cscene.c has the same pair as
// file statics of its own; they are not shared because a `static` in one .c is
// not reachable from another, which is a seam worth widening the day a third
// cJSON reader appears rather than on the second.
static const char* _string_or(const cJSON* obj, const char* key) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static int _int_or(const cJSON* obj, const char* key, int fallback) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(item) ? (int)item->valuedouble : fallback;
}

bool config_snapshot_read_source(const char* path, const ConfigSnapshotSource** out) {
    if (out)
        *out = NULL;
    if (!path || !path[0])
        return false;
    char* text = read_entire_file(path, NULL);
    if (!text) {
        fprintf(stderr, "Error: config snapshot: cannot read '%s'\n", path);
        return false;
    }
    cJSON* root = cJSON_Parse(text);
    free(text);
    if (!root) {
        fprintf(stderr, "Error: config snapshot: '%s' is not valid JSON\n", path);
        return false;
    }

    const cJSON* src = cJSON_GetObjectItemCaseSensitive(root, "source");
    if (!cJSON_IsObject(src)) {
        fprintf(stderr, "Error: config snapshot: '%s' has no source block\n", path);
        cJSON_Delete(root);
        return false;
    }

    // Into the READ buffers, so the answer survives the parse tree without
    // touching what the writer will emit.
    _copy_source_path(_read_model, sizeof(_read_model), _string_or(src, "model"));
    _copy_source_path(_read_hdr, sizeof(_read_hdr), _string_or(src, "hdr"));
    _copy_source_path(_read_lut, sizeof(_read_lut), _string_or(src, "lut"));
    _copy_source_path(_read_textures, sizeof(_read_textures), _string_or(src, "textures"));
    _read_back.model = _read_model;
    _read_back.hdr = _read_hdr;
    _read_back.lut = _read_lut;
    _read_back.textures = _read_textures;
    _read_back.sky = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(src, "sky"));
    _read_back.width = _int_or(src, "width", 0);
    _read_back.height = _int_or(src, "height", 0);

    cJSON_Delete(root);
    if (out)
        *out = &_read_back;
    return true;
}
