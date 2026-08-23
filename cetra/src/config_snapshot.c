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
#include "postfx.h"
#include "scene.h"

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
} ConfigOwner;

typedef enum ConfigType {
    CFG_BOOL = 0,
    CFG_INT,
    CFG_FLOAT,
    CFG_VEC3,
    // Stored as an int, written as one of `labels`. Named rather than numbered
    // because a snapshot is read by people as often as by the loader, and an
    // enum that renumbers would otherwise silently re-point every stored value.
    CFG_ENUM,
} ConfigType;

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
} ConfigField;

#define CFG_ROW(owner_, type_, section_, key_, struct_, member_)                                   \
    {owner_, type_, section_, key_, offsetof(struct_, member_), NULL, 0}
#define CFG_ROW_ENUM(owner_, section_, key_, struct_, member_, labels_)                            \
    {owner_,      CFG_ENUM, section_, key_, offsetof(struct_, member_), labels_,                   \
     (int)(sizeof(labels_) / sizeof((labels_)[0]))}

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
 * The table. Every row is one control; a control with no row is a control this
 * feature cannot carry, which is what `config-coverage` asserts against gui.c.
 */
static const ConfigField CFG_FIELDS[] = {
    // --- engine
    CFG_ROW_ENUM(CFG_ENGINE, "engine", "render_mode", Engine, current_render_mode,
                 CFG_RENDER_MODES),
    CFG_ROW_ENUM(CFG_ENGINE, "engine", "camera_mode", Engine, camera_mode, CFG_CAMERA_MODES),
    CFG_ROW(CFG_ENGINE, CFG_INT, "engine", "msaa_samples", Engine, msaa_samples),
    CFG_ROW(CFG_ENGINE, CFG_FLOAT, "engine", "render_scale", Engine, render_scale),
    CFG_ROW(CFG_ENGINE, CFG_INT, "engine", "ss_scale", Engine, ss_scale),

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
    CFG_ROW(CFG_POSTFX, CFG_BOOL, "postfx", "taa", PostFX, taa_enabled),
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
    CFG_ROW(CFG_POSTFX, CFG_BOOL, "postfx.ssr", "full_res", PostFX, ssr_full_res),

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
    CFG_ROW(CFG_POSTFX, CFG_VEC3, "postfx.fog", "ambient", PostFX, fog_ambient),
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
    CFG_ROW(CFG_CAMERA, CFG_VEC3, "camera", "eye", Camera, position),
    CFG_ROW(CFG_CAMERA, CFG_VEC3, "camera", "target", Camera, look_at),
    CFG_ROW(CFG_CAMERA, CFG_VEC3, "camera", "up", Camera, up_vector),
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
};

#define CFG_FIELD_COUNT ((int)(sizeof(CFG_FIELDS) / sizeof(CFG_FIELDS[0])))

// See the header: the GUI produces snapshots and cannot reach the app's
// arguments, so the app leaves them here.
static ConfigSnapshotSource _source;
static char _source_model[512];
static char _source_hdr[512];
static char _source_lut[512];
static char _source_textures[512];
static bool _source_set;

static void _copy_source_path(char* dst, size_t cap, const char* src) {
    if (!src || !src[0]) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, cap, "%s", src);
}

void config_snapshot_set_source(const ConfigSnapshotSource* src) {
    if (!src) {
        _source_set = false;
        return;
    }
    _copy_source_path(_source_model, sizeof(_source_model), src->model);
    _copy_source_path(_source_hdr, sizeof(_source_hdr), src->hdr);
    _copy_source_path(_source_lut, sizeof(_source_lut), src->lut);
    _copy_source_path(_source_textures, sizeof(_source_textures), src->textures);
    _source.model = _source_model;
    _source.hdr = _source_hdr;
    _source.lut = _source_lut;
    _source.textures = _source_textures;
    _source.sky = src->sky;
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
    }
    return NULL;
}

/*
 * Get-or-create the object a dotted section path names. Sections are created in
 * first-use order, so the file's layout is the table's order and a diff between
 * two snapshots stays line-for-line comparable -- which config-roundtrip reads.
 */
static cJSON* _section_object(cJSON* root, const char* path) {
    cJSON* node = root;
    const char* p = path;
    while (*p) {
        const char* dot = strchr(p, '.');
        size_t len = dot ? (size_t)(dot - p) : strlen(p);
        char name[64];
        if (len >= sizeof(name))
            len = sizeof(name) - 1;
        memcpy(name, p, len);
        name[len] = '\0';

        cJSON* child = cJSON_GetObjectItemCaseSensitive(node, name);
        if (!child) {
            child = cJSON_AddObjectToObject(node, name);
            if (!child)
                return NULL;
        }
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

static bool _write_field(cJSON* root, const ConfigField* f, const void* base) {
    cJSON* obj = _section_object(root, f->section);
    if (!obj)
        return false;
    // Through void*, not straight off the byte pointer: the offset arithmetic
    // needs a char*, but a char*-to-float* cast is the one a portability checker
    // reads as a reinterpretation of unaligned bytes.
    const void* p = (const void*)((const unsigned char*)base + f->offset);

    switch ((ConfigType)f->type) {
    case CFG_BOOL:
        return cJSON_AddBoolToObject(obj, f->key, *(const bool*)p) != NULL;
    case CFG_INT:
        return cJSON_AddNumberToObject(obj, f->key, *(const int*)p) != NULL;
    case CFG_FLOAT:
        return cJSON_AddNumberToObject(obj, f->key, *(const float*)p) != NULL;
    case CFG_VEC3: {
        const float* v = (const float*)p;
        double xyz[3] = {v[0], v[1], v[2]};
        cJSON* arr = cJSON_CreateDoubleArray(xyz, 3);
        return arr && cJSON_AddItemToObject(obj, f->key, arr);
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

char* config_snapshot_write(Engine* engine, Scene* scene) {
    cJSON* root = cJSON_CreateObject();
    if (!root)
        return NULL;
    cJSON_AddNumberToObject(root, "version", 1);
    _write_source(root, engine);

    for (int i = 0; i < CFG_FIELD_COUNT; i++) {
        const ConfigField* f = &CFG_FIELDS[i];
        const void* base = _owner_base((ConfigOwner)f->owner, engine, scene);
        // An absent subsystem omits its whole section rather than writing it
        // empty -- a snapshot from a waterless scene must not claim a water
        // block whose every value is uninitialised memory.
        if (!base)
            continue;
        if (!_write_field(root, f, base)) {
            cJSON_Delete(root);
            return NULL;
        }
    }

    char* text = cJSON_Print(root);
    cJSON_Delete(root);
    return text;
}

bool config_snapshot_save(Engine* engine, Scene* scene, const char* path) {
    if (!path || !path[0]) {
        fprintf(stderr, "Error: config snapshot: no output path\n");
        return false;
    }
    char* text = config_snapshot_write(engine, scene);
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
    printf("config snapshot written: %s (%d fields)\n", path, CFG_FIELD_COUNT);
    fflush(stdout);
    return true;
}
