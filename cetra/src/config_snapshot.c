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
    CFG_DECAL_ELEM,
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
    // Anything the env cube, the sky-mirroring probes and the GI volume are
    // derived from -- the sun and the cloud layer both. One flag because they
    // feed ONE chain (scene_environment_changed), which is the whole point.
    bool env_changed;
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

/*
 * The struct each owner addresses, stated ONCE per owner rather than once per row.
 *
 * A row used to name both and nothing tied them together, so
 * CFG_ROW(CFG_SCENE, CFG_FLOAT, "postfx.ssr", "strength", ssr_strength)
 * compiled clean and then applied PostFX's offset to a Scene*, writing a float
 * past the end of the struct. That is the worst thing this file can do, arriving
 * by the likeliest edit in a 300-row table -- a copy-paste with one field
 * changed. Derived by token paste, the mismatch is a compile error instead.
 *
 * What survives is owner-vs-SECTION mismatch, which files a key under the wrong
 * JSON object and is cosmetic. Corruption to misfiling is the trade.
 */
#define CFG_STRUCT_CFG_ENGINE Engine
#define CFG_STRUCT_CFG_POSTFX PostFX
#define CFG_STRUCT_CFG_EXPOSURE Exposure
#define CFG_STRUCT_CFG_SCENE Scene
#define CFG_STRUCT_CFG_CAMERA Camera
#define CFG_STRUCT_CFG_SHADOW ShadowSystem
#define CFG_STRUCT_CFG_SKY SkyAtmosphere
#define CFG_STRUCT_CFG_CLOUDS CloudLayer
#define CFG_STRUCT_CFG_IBL IBLResources
#define CFG_STRUCT_CFG_GI GIVolume
#define CFG_STRUCT_CFG_CLUSTER LightClusterContext
#define CFG_STRUCT_CFG_WATER Water
#define CFG_STRUCT_CFG_WATER_WINDSEA WaterWaveTrain
#define CFG_STRUCT_CFG_WATER_SWELL WaterWaveTrain
#define CFG_STRUCT_CFG_PROBE_ELEM ReflectionProbe
#define CFG_STRUCT_CFG_DECAL_ELEM Decal
#define CFG_STRUCT_CFG_LIGHT_ELEM Light

#define CFG_ROW(owner_, type_, section_, key_, member_)                                            \
    {owner_, type_, section_, key_, offsetof(CFG_STRUCT_##owner_, member_), NULL, 0, NULL}
#define CFG_ROW_ENUM(owner_, section_, key_, member_, labels_)                                     \
    {owner_,                                                                                       \
     CFG_ENUM,                                                                                     \
     section_,                                                                                     \
     key_,                                                                                         \
     offsetof(CFG_STRUCT_##owner_, member_),                                                       \
     labels_,                                                                                      \
     (int)(sizeof(labels_) / sizeof((labels_)[0])),                                                \
     NULL}
#define CFG_ROW_FN(owner_, type_, section_, key_, member_, fn_)                                    \
    {owner_, type_, section_, key_, offsetof(CFG_STRUCT_##owner_, member_), NULL, 0, fn_}

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
static const char* const CFG_SPEC_OCC[] = {"off", "legacy", "split"};
static const char* const CFG_LUT_INTERP[] = {"trilinear", "tetrahedral"};
static const char* const CFG_METER_MODES[] = {"uniform", "centre", "spot"};
static const char* const CFG_WAVE_MODELS[] = {"gerstner", "fft"};
static const char* const CFG_LIGHT_UNITS[] = {"default", "candela", "lumens", "lux", "nits"};

/*
 * Every vocabulary must have exactly as many labels as its enum has values.
 *
 * This is the one that rots WITHOUT a compile error otherwise: a value inserted
 * in the MIDDLE of an enum leaves the counts equal only if a label is added too,
 * and if it is not, every label after the insertion names the wrong value --
 * silently, in both directions at once, so a round trip stays green while every
 * previously-saved file re-points. Adding at the END is caught by these; adding
 * in the middle is caught by these only once the label count also moves, which
 * is why the label array is the thing to edit first.
 */
_Static_assert(sizeof(CFG_RENDER_MODES) / sizeof(*CFG_RENDER_MODES) ==
                   RENDER_MODE_EXTRAPOLATION + 1,
               "CFG_RENDER_MODES must name every RenderMode");
_Static_assert(sizeof(CFG_CAMERA_MODES) / sizeof(*CFG_CAMERA_MODES) == CAMERA_MODE_ORBIT + 1,
               "CFG_CAMERA_MODES must name every CameraMode");
_Static_assert(sizeof(CFG_TONEMAPS) / sizeof(*CFG_TONEMAPS) == POSTFX_TONEMAP_AGX + 1,
               "CFG_TONEMAPS must name every PostFXTonemapMode");
_Static_assert(sizeof(CFG_DEBUG_VIEWS) / sizeof(*CFG_DEBUG_VIEWS) == POSTFX_DEBUG_BENT + 1,
               "CFG_DEBUG_VIEWS must name every PostFXDebugView");
_Static_assert(sizeof(CFG_SPEC_OCC) / sizeof(*CFG_SPEC_OCC) == POSTFX_SPEC_OCC_SPLIT + 1,
               "CFG_SPEC_OCC must name every PostFXSpecOccMode");
_Static_assert(sizeof(CFG_LUT_INTERP) / sizeof(*CFG_LUT_INTERP) == POSTFX_LUT_TETRAHEDRAL + 1,
               "CFG_LUT_INTERP must name every PostFXLutInterp");
_Static_assert(sizeof(CFG_METER_MODES) / sizeof(*CFG_METER_MODES) == METERING_SPOT + 1,
               "CFG_METER_MODES must name every MeteringMode");
_Static_assert(sizeof(CFG_WAVE_MODELS) / sizeof(*CFG_WAVE_MODELS) == WATER_WAVES_FFT + 1,
               "CFG_WAVE_MODELS must name every WaterWaveModel");
_Static_assert(sizeof(CFG_LIGHT_UNITS) / sizeof(*CFG_LIGHT_UNITS) == LIGHT_UNITS_NITS + 1,
               "CFG_LIGHT_UNITS must name every LightUnits");

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
    ctx->env_changed = true;
}

/*
 * A cloud field, which the env cube, the sky-mirroring probes and the GI volume
 * are all derived from exactly as they are from the sun.
 *
 * These were plain stores, so restoring a cloud change re-derived nothing: the
 * screen march followed while the env, the probe and the GI kept the old deck.
 * Flagged on a CHANGE for _apply_sun_angle's reason -- the common restore is
 * onto the scene the file came from, and a bake plus a probe re-capture is not
 * a thing to pay for a value that did not move.
 */
static void _apply_cloud_field(ConfigApplyCtx* ctx, void* base, const ConfigField* f,
                               const double* v, int n) {
    const float before = *(const float*)_field_ptr_const(base, f);
    if ((float)v[0] == before)
        return;
    _store_value(f, base, v, n);
    ctx->env_changed = true;
}

/*
 * The cloud layer's master switch, which is the one cloud field that can be
 * asked for something this session cannot give.
 *
 * sky_bake_cloud_noise early-outs on !enabled and runs once at startup, so a
 * session started without clouds has no noise -- and storing `true` against it
 * yields a flag every consumer refuses, a clear sky, and a dump that writes
 * `true` straight back out. Refused BY NAME instead, in the IES idiom: the
 * snapshot's `source` block carries `clouds` so the app can arm the bake before
 * the engine exists, and this is what says so when it did not.
 */
static void _apply_cloud_enabled(ConfigApplyCtx* ctx, void* base, const ConfigField* f,
                                 const double* v, int n) {
    const bool want = v[0] != 0.0;
    const CloudLayer* clouds = (const CloudLayer*)base;
    if (want && !clouds->noise_baked) {
        fprintf(stderr, "Warning: config snapshot: this session baked no cloud noise; "
                        "sky.clouds.enabled ignored\n");
        return;
    }
    if (want == *(const bool*)_field_ptr_const(base, f))
        return;
    _store_value(f, base, v, n);
    ctx->env_changed = true;
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
 * The table.
 *
 * A control with no row is a setting this feature cannot carry, which is what
 * `config-coverage` asserts against gui.c. The converse is NOT true and the
 * comment here used to claim it was: about sixty rows have no control at all --
 * a CLI flag, a scene-file key or a field only code reaches. Rows exist to
 * describe the configuration, not to mirror the panel.
 */
static const ConfigField CFG_FIELDS[] = {
    // --- engine
    CFG_ROW_ENUM(CFG_ENGINE, "engine", "render_mode", current_render_mode, CFG_RENDER_MODES),
    CFG_ROW_ENUM(CFG_ENGINE, "engine", "camera_mode", camera_mode, CFG_CAMERA_MODES),
    CFG_ROW_FN(CFG_ENGINE, CFG_INT, "engine", "msaa_samples", msaa_samples, _apply_msaa),
    CFG_ROW_FN(CFG_ENGINE, CFG_FLOAT, "engine", "render_scale", render_scale, _apply_render_scale),
    CFG_ROW_FN(CFG_ENGINE, CFG_INT, "engine", "ss_scale", ss_scale, _apply_ss_scale),

    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.overlays", "gui", show_gui),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.overlays", "wireframe", show_wireframe),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.overlays", "xyz", show_xyz),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.overlays", "fps", show_fps),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.overlays", "camera_hud", show_camera_hud),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.overlays", "bones", show_bones),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.overlays", "lights", show_lights),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.overlays", "cluster_heatmap", cluster_debug),

    CFG_ROW(CFG_ENGINE, CFG_FLOAT, "engine.material", "specular_aa", specular_aa_strength),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.material", "energy_comp", energy_comp_enabled),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.material", "refraction", refraction_enabled),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.material", "clearcoat", clearcoat_enabled),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.material", "specular", specular_enabled),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.material", "sheen", sheen_enabled),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.material", "parallax", parallax_enabled),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.material", "sss", sss_enabled),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.material", "skin_preint", skin_preint_enabled),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.material", "oit", oit_enabled),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.material", "oit_moments", oit_moments_enabled),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.material", "emissive_lights", emissive_lights_enabled),

    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.draw", "instancing", instancing_enabled),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.draw", "lod", lod_enabled),
    CFG_ROW(CFG_ENGINE, CFG_FLOAT, "engine.draw", "lod_bias", lod_bias),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.draw", "frustum_cull", frustum_cull_enabled),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.draw", "morph", morph_enabled),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.draw", "opaque_sort", opaque_sort_enabled),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.draw", "depth_prepass", depth_prepass_enabled),

    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.layers_vt", "enabled", layers_vt_enabled),
    CFG_ROW(CFG_ENGINE, CFG_INT, "engine.layers_vt", "res", layers_vt_res),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.layers_vt", "pages", layers_vt_pages_enabled),
    CFG_ROW(CFG_ENGINE, CFG_BOOL, "engine.layers_vt", "feedback", layers_vt_feedback_enabled),
    CFG_ROW(CFG_ENGINE, CFG_INT, "engine.layers_vt", "page_slots", layers_vt_page_slots),
    CFG_ROW(CFG_ENGINE, CFG_INT, "engine.layers_vt", "page_budget", layers_vt_page_budget),

    // --- postfx
    CFG_ROW_ENUM(CFG_POSTFX, "postfx", "tonemap", tonemap_mode, CFG_TONEMAPS),
    CFG_ROW_ENUM(CFG_POSTFX, "postfx", "debug_view", debug_view, CFG_DEBUG_VIEWS),
    CFG_ROW_FN(CFG_POSTFX, CFG_BOOL, "postfx", "taa", taa_enabled, _apply_taa),
    CFG_ROW(CFG_POSTFX, CFG_BOOL, "postfx", "normals_gbuffer", normals_enabled),

    CFG_ROW(CFG_POSTFX, CFG_BOOL, "postfx.bloom", "enabled", bloom_enabled),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.bloom", "threshold", bloom_threshold),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.bloom", "knee", bloom_knee),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.bloom", "max_brightness", bloom_max_brightness),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.bloom", "strength", bloom_strength),

    CFG_ROW(CFG_POSTFX, CFG_BOOL, "postfx.flare", "enabled", flare_enabled),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.flare", "strength", flare_strength),
    CFG_ROW(CFG_POSTFX, CFG_INT, "postfx.flare", "ghosts", flare_ghosts),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.flare", "ghost_spacing", flare_ghost_spacing),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.flare", "halo_width", flare_halo_width),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.flare", "chroma", flare_chroma),
    CFG_ROW(CFG_POSTFX, CFG_INT, "postfx.flare", "source_lod", flare_source_lod),

    CFG_ROW(CFG_POSTFX, CFG_BOOL, "postfx.ao", "enabled", ssao_enabled),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.ao", "radius", ssao_radius),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.ao", "strength", ssao_strength),
    CFG_ROW_ENUM(CFG_POSTFX, "postfx.ao", "spec_occlusion", spec_occlusion_mode, CFG_SPEC_OCC),
    CFG_ROW(CFG_POSTFX, CFG_BOOL, "postfx.ao", "edge_filter", ao_edge_filter_enabled),

    CFG_ROW(CFG_POSTFX, CFG_BOOL, "postfx.contact_shadows", "enabled", contact_shadows_enabled),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.contact_shadows", "strength", cs_strength),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.contact_shadows", "distance", cs_distance),

    CFG_ROW(CFG_POSTFX, CFG_BOOL, "postfx.ssgi", "enabled", ssgi_enabled),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.ssgi", "intensity", ssgi_intensity),

    CFG_ROW(CFG_POSTFX, CFG_BOOL, "postfx.ssr", "enabled", ssr_enabled),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.ssr", "strength", ssr_strength),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.ssr", "max_distance", ssr_max_distance),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.ssr", "thickness_min", ssr_thickness_min),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.ssr", "max_roughness", ssr_max_roughness),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.ssr", "floor_roughness", ssr_floor_roughness),
    CFG_ROW(CFG_POSTFX, CFG_BOOL, "postfx.ssr", "temporal", ssr_temporal),
    CFG_ROW(CFG_POSTFX, CFG_BOOL, "postfx.ssr", "denoise", ssr_denoise),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.ssr", "jitter", ssr_jitter),
    CFG_ROW_FN(CFG_POSTFX, CFG_BOOL, "postfx.ssr", "full_res", ssr_full_res, _apply_ssr_full_res),

    CFG_ROW(CFG_POSTFX, CFG_BOOL, "postfx.fog", "enabled", fog_enabled),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.fog", "density", fog_density),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.fog", "height_falloff", fog_height_falloff),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.fog", "floor_y", fog_floor_y),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.fog", "near", fog_near),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.fog", "far", fog_far),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.fog", "depth_distribution", fog_depth_dist),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.fog", "temporal_blend", fog_temporal_blend),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.fog", "anisotropy", fog_anisotropy),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.fog", "sun_boost", fog_sun_boost),
    CFG_ROW_FN(CFG_POSTFX, CFG_VEC3, "postfx.fog", "ambient", fog_ambient, _apply_fog_ambient),
    CFG_ROW(CFG_POSTFX, CFG_BOOL, "postfx.fog", "esm", fog_esm_enabled),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.fog", "esm_sharpness", fog_esm_k),
    CFG_ROW(CFG_POSTFX, CFG_INT, "postfx.fog", "grid_x", froxel_grid_x),
    CFG_ROW(CFG_POSTFX, CFG_INT, "postfx.fog", "grid_y", froxel_grid_y),
    CFG_ROW(CFG_POSTFX, CFG_INT, "postfx.fog", "grid_z", froxel_grid_z),

    CFG_ROW(CFG_POSTFX, CFG_BOOL, "postfx.dof", "enabled", dof_enabled),
    CFG_ROW(CFG_POSTFX, CFG_BOOL, "postfx.dof", "autofocus", dof_autofocus),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.dof", "focus_distance", dof_focus_distance),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.dof", "focus_range", dof_focus_range),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.dof", "max_coc", dof_max_coc),
    CFG_ROW(CFG_POSTFX, CFG_INT, "postfx.dof", "blades", dof_blades),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.dof", "rotation", dof_rotation),

    CFG_ROW(CFG_POSTFX, CFG_BOOL, "postfx.motion_blur", "enabled", motion_blur_enabled),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.motion_blur", "scale", motion_blur_scale),

    CFG_ROW(CFG_POSTFX, CFG_BOOL, "postfx.sharpen", "enabled", sharpen_enabled),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.sharpen", "strength", sharpen_strength),

    CFG_ROW(CFG_POSTFX, CFG_BOOL, "postfx.grade", "enabled", grade_enabled),
    CFG_ROW(CFG_POSTFX, CFG_VEC3, "postfx.grade", "lift", grade_lift),
    CFG_ROW(CFG_POSTFX, CFG_VEC3, "postfx.grade", "gamma", grade_gamma),
    CFG_ROW(CFG_POSTFX, CFG_VEC3, "postfx.grade", "gain", grade_gain),

    CFG_ROW(CFG_POSTFX, CFG_BOOL, "postfx.vignette", "enabled", vignette_enabled),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.vignette", "strength", vignette_strength),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.vignette", "radius", vignette_radius),

    CFG_ROW(CFG_POSTFX, CFG_BOOL, "postfx.chromatic_aberration", "enabled", ca_enabled),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.chromatic_aberration", "strength", ca_strength),

    CFG_ROW(CFG_POSTFX, CFG_BOOL, "postfx.grain", "enabled", grain_enabled),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.grain", "strength", grain_strength),

    CFG_ROW(CFG_POSTFX, CFG_BOOL, "postfx.dither", "enabled", dither_enabled),
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.dither", "strength", dither_strength),

    // The .cube path rides `source`, not here: the table only carries values the
    // owner struct holds, and PostFX keeps the basename alone.
    CFG_ROW(CFG_POSTFX, CFG_FLOAT, "postfx.lut", "strength", lut_strength),
    CFG_ROW_ENUM(CFG_POSTFX, "postfx.lut", "interp", lut_interp, CFG_LUT_INTERP),

    // --- exposure
    CFG_ROW(CFG_EXPOSURE, CFG_BOOL, "exposure", "physical", physical),
    CFG_ROW(CFG_EXPOSURE, CFG_FLOAT, "exposure", "aperture", aperture),
    CFG_ROW(CFG_EXPOSURE, CFG_FLOAT, "exposure", "shutter_speed", shutter_speed),
    CFG_ROW(CFG_EXPOSURE, CFG_FLOAT, "exposure", "iso", iso),
    CFG_ROW(CFG_EXPOSURE, CFG_FLOAT, "exposure", "multiplier", multiplier),
    CFG_ROW(CFG_EXPOSURE, CFG_FLOAT, "exposure", "bias_stops", bias_stops),
    CFG_ROW(CFG_EXPOSURE, CFG_BOOL, "exposure", "automatic", automatic),
    CFG_ROW(CFG_EXPOSURE, CFG_FLOAT, "exposure", "key", key),
    CFG_ROW_ENUM(CFG_EXPOSURE, "exposure", "meter_mode", meter_mode, CFG_METER_MODES),
    CFG_ROW(CFG_EXPOSURE, CFG_FLOAT, "exposure", "meter_radius", meter_radius),
    CFG_ROW(CFG_EXPOSURE, CFG_FLOAT, "exposure", "meter_low", meter_low),
    CFG_ROW(CFG_EXPOSURE, CFG_FLOAT, "exposure", "meter_high", meter_high),
    CFG_ROW(CFG_EXPOSURE, CFG_FLOAT, "exposure", "meter_min_log2", meter_min_log2),
    CFG_ROW(CFG_EXPOSURE, CFG_FLOAT, "exposure", "meter_max_log2", meter_max_log2),
    CFG_ROW(CFG_EXPOSURE, CFG_FLOAT, "exposure", "adapt_rate_up", adapt_rate_up),
    CFG_ROW(CFG_EXPOSURE, CFG_FLOAT, "exposure", "adapt_rate_down", adapt_rate_down),
    /*
     * The adaptation state, and it is CONFIGURATION here even though it is
     * history everywhere else. Auto-exposure multiplies every pixel, and a
     * restored session that re-approaches from cold differs from the one it was
     * taken from in the whole frame -- AGENTS.md measures a provably-no-op change
     * at 99.77% of pixels for exactly this reason. Carrying the two makes the
     * first restored frame continue rather than converge.
     */
    CFG_ROW(CFG_EXPOSURE, CFG_FLOAT, "exposure", "adapted_luminance", adapted_luminance),
    CFG_ROW(CFG_EXPOSURE, CFG_BOOL, "exposure", "adapted_valid", adapted_valid),

    // --- scene
    CFG_ROW(CFG_SCENE, CFG_VEC3, "scene", "ambient_radiance", ambient_radiance),
    CFG_ROW(CFG_SCENE, CFG_BOOL, "scene", "render_skybox", render_skybox),
    CFG_ROW(CFG_SCENE, CFG_FLOAT, "scene", "skybox_brightness", skybox_brightness),
    CFG_ROW(CFG_SCENE, CFG_BOOL, "scene.ground_projection", "enabled", skybox_ground_projection),
    CFG_ROW(CFG_SCENE, CFG_FLOAT, "scene.ground_projection", "radius", skybox_gp_radius),
    CFG_ROW(CFG_SCENE, CFG_FLOAT, "scene.ground_projection", "height", skybox_gp_height),
    CFG_ROW(CFG_SCENE, CFG_BOOL, "scene.shadow_catcher", "enabled", shadow_catcher),
    CFG_ROW(CFG_SCENE, CFG_FLOAT, "scene.shadow_catcher", "strength", shadow_catcher_strength),

    // --- camera. Radians, not the degrees Print Camera emits: this file is read
    // back by the loader, and a unit conversion is a second place to disagree.
    CFG_ROW_FN(CFG_CAMERA, CFG_VEC3, "camera", "eye", position, _apply_camera_vec),
    CFG_ROW_FN(CFG_CAMERA, CFG_VEC3, "camera", "target", look_at, _apply_camera_vec),
    CFG_ROW_FN(CFG_CAMERA, CFG_VEC3, "camera", "up", up_vector, _apply_camera_vec),
    CFG_ROW(CFG_CAMERA, CFG_FLOAT, "camera", "fov_radians", fov_radians),
    CFG_ROW(CFG_CAMERA, CFG_FLOAT, "camera", "near_clip", near_clip),
    CFG_ROW(CFG_CAMERA, CFG_FLOAT, "camera", "far_clip", far_clip),
    CFG_ROW(CFG_CAMERA, CFG_FLOAT, "camera.orbit", "distance", distance),
    CFG_ROW(CFG_CAMERA, CFG_FLOAT, "camera.orbit", "height", height),
    CFG_ROW(CFG_CAMERA, CFG_FLOAT, "camera.orbit", "theta", theta),
    CFG_ROW(CFG_CAMERA, CFG_FLOAT, "camera.orbit", "phi", phi),
    CFG_ROW(CFG_CAMERA, CFG_FLOAT, "camera.orbit", "amplitude", amplitude),
    CFG_ROW(CFG_CAMERA, CFG_FLOAT, "camera.orbit", "zoom_speed", zoom_speed),
    CFG_ROW(CFG_CAMERA, CFG_FLOAT, "camera.orbit", "orbit_speed", orbit_speed),

    // --- shadows. The map SIZE is not here and cannot be: create_shadow_system
    // takes it once and no flag, key or slider reaches it either.
    CFG_ROW(CFG_SHADOW, CFG_BOOL, "shadow", "enabled", enabled),
    CFG_ROW(CFG_SHADOW, CFG_INT, "shadow", "cascades", cascade_count),
    CFG_ROW(CFG_SHADOW, CFG_BOOL, "shadow", "cascade_tint", csm_debug),
    CFG_ROW(CFG_SHADOW, CFG_BOOL, "shadow", "translucent", tsm_enabled),
    CFG_ROW(CFG_SHADOW, CFG_FLOAT, "shadow", "bias", shadow_bias),
    CFG_ROW(CFG_SHADOW, CFG_VEC3, "shadow", "scene_center", scene_center),
    CFG_ROW(CFG_SHADOW, CFG_BOOL, "shadow.moments", "enabled", msm_enabled),
    CFG_ROW(CFG_SHADOW, CFG_INT, "shadow.moments", "size", msm_size),
    CFG_ROW(CFG_SHADOW, CFG_FLOAT, "shadow.moments", "blur", msm_blur),
    CFG_ROW(CFG_SHADOW, CFG_FLOAT, "shadow.moments", "bleed", msm_bleed),
    CFG_ROW(CFG_SHADOW, CFG_BOOL, "shadow.pcss", "enabled", pcss_enabled),
    CFG_ROW(CFG_SHADOW, CFG_FLOAT, "shadow.pcss", "softness", pcss_softness),

    // --- sky. Moving the sun re-bakes the LUTs, the environment and the IBL, so
    // these rows have a setter on apply where the rest are stores.
    CFG_ROW_FN(CFG_SKY, CFG_FLOAT, "sky", "sun_elevation", sun_elevation_deg, _apply_sun_angle),
    CFG_ROW_FN(CFG_SKY, CFG_FLOAT, "sky", "sun_azimuth", sun_azimuth_deg, _apply_sun_angle),
    CFG_ROW(CFG_SKY, CFG_FLOAT, "sky", "sun_disc", sun_disc_deg),
    CFG_ROW(CFG_SKY, CFG_FLOAT, "sky", "world_units_per_km", world_units_per_km),
    CFG_ROW(CFG_SKY, CFG_BOOL, "sky", "aerial", aerial_enabled),
    // Stars are sampled live by the background pass like sun_disc: plain
    // stores, nothing re-bakes.
    CFG_ROW(CFG_SKY, CFG_BOOL, "sky", "stars", stars_enabled),
    CFG_ROW(CFG_SKY, CFG_FLOAT, "sky", "stars_brightness", stars_brightness),
    CFG_ROW(CFG_SKY, CFG_FLOAT, "sky", "latitude", latitude_deg),
    CFG_ROW(CFG_SKY, CFG_FLOAT, "sky", "star_hour", star_hour_deg),
    CFG_ROW_FN(CFG_CLOUDS, CFG_BOOL, "sky.clouds", "enabled", enabled, _apply_cloud_enabled),
    CFG_ROW_FN(CFG_CLOUDS, CFG_FLOAT, "sky.clouds", "coverage", coverage, _apply_cloud_field),
    CFG_ROW_FN(CFG_CLOUDS, CFG_FLOAT, "sky.clouds", "cloud_type", cloud_type, _apply_cloud_field),
    CFG_ROW_FN(CFG_CLOUDS, CFG_FLOAT, "sky.clouds", "density", density, _apply_cloud_field),
    // Wind re-bakes nothing: the env cube holds a still of the deck by design.
    CFG_ROW(CFG_CLOUDS, CFG_FLOAT, "sky.clouds", "wind_speed_kmh", wind_speed_kmh),
    CFG_ROW(CFG_CLOUDS, CFG_FLOAT, "sky.clouds", "wind_dir", wind_dir_deg),

    // --- environment
    CFG_ROW(CFG_IBL, CFG_FLOAT, "ibl", "intensity", intensity),

    // --- GI volume. The grid dimensions are an allocation, so they are absent
    // for the reason the shadow map size is.
    CFG_ROW(CFG_GI, CFG_BOOL, "gi", "enabled", enabled),
    CFG_ROW(CFG_GI, CFG_INT, "gi", "rate", rate),
    CFG_ROW(CFG_GI, CFG_BOOL, "gi", "debug_atlas", debug_atlas),

    CFG_ROW(CFG_CLUSTER, CFG_BOOL, "lighting", "area_lights", area_lights_enabled),

    // --- water. Editing any sea-state row re-seeds three 128^2 grids on the
    // next frame, which is a cost the restore pays once and says nothing about.
    CFG_ROW(CFG_WATER, CFG_BOOL, "water", "enabled", enabled),
    CFG_ROW(CFG_WATER, CFG_FLOAT, "water", "level", level),
    CFG_ROW(CFG_WATER, CFG_FLOAT, "water", "extent", extent),
    CFG_ROW(CFG_WATER, CFG_VEC3, "water", "absorption", absorption),
    CFG_ROW(CFG_WATER, CFG_VEC3, "water", "scatter", scatter),
    CFG_ROW(CFG_WATER, CFG_FLOAT, "water", "roughness", roughness),
    CFG_ROW(CFG_WATER, CFG_FLOAT, "water", "ior", ior),
    CFG_ROW(CFG_WATER, CFG_FLOAT, "water", "amplitude", amplitude),
    CFG_ROW(CFG_WATER, CFG_FLOAT, "water", "wavelength", wavelength),
    CFG_ROW(CFG_WATER, CFG_FLOAT, "water", "steepness", steepness),
    CFG_ROW(CFG_WATER, CFG_FLOAT, "water", "spread", spread),
    CFG_ROW(CFG_WATER, CFG_VEC2, "water", "wind_dir", wind_dir),
    CFG_ROW_ENUM(CFG_WATER, "water", "waves", wave_model, CFG_WAVE_MODELS),
    CFG_ROW(CFG_WATER, CFG_BOOL, "water", "caustics", caustics),
    CFG_ROW(CFG_WATER, CFG_BOOL, "water", "glitter", glitter),
    CFG_ROW(CFG_WATER, CFG_BOOL, "water", "foam_history", foam_history),
    CFG_ROW(CFG_WATER, CFG_FLOAT, "water", "foam_decay", foam_decay),
    CFG_ROW(CFG_WATER, CFG_FLOAT, "water", "foam_drift", foam_drift),
    CFG_ROW(CFG_WATER, CFG_BOOL, "water", "shore_coverage", shore_coverage),
    CFG_ROW(CFG_WATER, CFG_BOOL, "water", "surf", surf),
    CFG_ROW(CFG_WATER, CFG_BOOL, "water", "far_lod", far_lod),
    CFG_ROW(CFG_WATER, CFG_BOOL, "water", "wetness", wetness),
    CFG_ROW(CFG_WATER, CFG_BOOL, "water", "film", film),
    CFG_ROW(CFG_WATER, CFG_FLOAT, "water", "sea_depth", sea.sea_depth),

    CFG_ROW(CFG_WATER_WINDSEA, CFG_FLOAT, "water.wind_sea", "wind_speed", wind_speed),
    CFG_ROW(CFG_WATER_WINDSEA, CFG_FLOAT, "water.wind_sea", "fetch", fetch),
    CFG_ROW(CFG_WATER_WINDSEA, CFG_FLOAT, "water.wind_sea", "direction", direction),
    CFG_ROW(CFG_WATER_WINDSEA, CFG_FLOAT, "water.wind_sea", "scale", scale),
    CFG_ROW(CFG_WATER_WINDSEA, CFG_FLOAT, "water.wind_sea", "peak_enhancement", peak_enhancement),
    CFG_ROW(CFG_WATER_WINDSEA, CFG_FLOAT, "water.wind_sea", "focus", focus),
    CFG_ROW(CFG_WATER_WINDSEA, CFG_FLOAT, "water.wind_sea", "spread_gain", spread_gain),
    CFG_ROW(CFG_WATER_WINDSEA, CFG_FLOAT, "water.wind_sea", "spread_blend", spread_blend),

    CFG_ROW(CFG_WATER_SWELL, CFG_FLOAT, "water.swell", "wind_speed", wind_speed),
    CFG_ROW(CFG_WATER_SWELL, CFG_FLOAT, "water.swell", "fetch", fetch),
    CFG_ROW(CFG_WATER_SWELL, CFG_FLOAT, "water.swell", "direction", direction),
    CFG_ROW(CFG_WATER_SWELL, CFG_FLOAT, "water.swell", "scale", scale),
    CFG_ROW(CFG_WATER_SWELL, CFG_FLOAT, "water.swell", "peak_enhancement", peak_enhancement),
    CFG_ROW(CFG_WATER_SWELL, CFG_FLOAT, "water.swell", "focus", focus),
    CFG_ROW(CFG_WATER_SWELL, CFG_FLOAT, "water.swell", "spread_gain", spread_gain),
    CFG_ROW(CFG_WATER_SWELL, CFG_FLOAT, "water.swell", "spread_blend", spread_blend),

    /*
     * --- array elements. `section` is the ARRAY's name here, not a path: the
     * walk makes one object per element and writes these rows into it.
     *
     * A probe's position and box are absent because they are its CAPTURE, not
     * its tuning -- moving either without re-photographing the room describes a
     * mirror that is not where it says it is.
     */
    CFG_ROW(CFG_PROBE_ELEM, CFG_BOOL, "probes", "enabled", enabled),
    CFG_ROW(CFG_PROBE_ELEM, CFG_FLOAT, "probes", "intensity", intensity),
    CFG_ROW(CFG_PROBE_ELEM, CFG_FLOAT, "probes", "box_fade", box_fade),
    CFG_ROW(CFG_PROBE_ELEM, CFG_BOOL, "probes", "debug_background", debug_background),

    /*
     * Decals (spec 11.73). Unlike a probe, PLACEMENT is carried: a probe's
     * position and box are its CAPTURE, so moving one without re-photographing
     * describes a mirror that is not where it says it is -- where a decal has
     * nothing baked, and a restored position is simply next frame's descriptor.
     *
     * The image is not here for the opposite reason: it IS baked, into a layer
     * of the material texture array, and a snapshot cannot bind one. That half
     * belongs to the .cscn the snapshot names.
     */
    CFG_ROW(CFG_DECAL_ELEM, CFG_BOOL, "decals", "enabled", enabled),
    CFG_ROW(CFG_DECAL_ELEM, CFG_VEC3, "decals", "position", position),
    CFG_ROW(CFG_DECAL_ELEM, CFG_VEC3, "decals", "half_extent", half_extent),
    CFG_ROW(CFG_DECAL_ELEM, CFG_VEC3, "decals", "direction", direction),
    CFG_ROW(CFG_DECAL_ELEM, CFG_VEC3, "decals", "up", up),
    CFG_ROW(CFG_DECAL_ELEM, CFG_FLOAT, "decals", "opacity", opacity),
    CFG_ROW(CFG_DECAL_ELEM, CFG_FLOAT, "decals", "angle_fade", angle_fade),
    CFG_ROW(CFG_DECAL_ELEM, CFG_FLOAT, "decals", "feather", feather),
    CFG_ROW(CFG_DECAL_ELEM, CFG_FLOAT, "decals", "normal_strength", normal_strength),

    /*
     * The STORED canonical intensity beside the authored unit, NOT the value the
     * GUI displays.
     *
     * Deliberately not through set_light_intensity_units, and the reason is the
     * opposite of what it looks like: that setter CONVERTS, dividing by
     * LUMENS_PER_CANDELA when the unit is lumens. Handing it a value already in
     * canonical form would divide a lumens-authored lamp by 683 on every
     * restore. Writing both fields back is the same light, exactly.
     */
    CFG_ROW(CFG_LIGHT_ELEM, CFG_FLOAT, "lights", "intensity", intensity),
    CFG_ROW_ENUM(CFG_LIGHT_ELEM, "lights", "intensity_unit", units, CFG_LIGHT_UNITS),
    CFG_ROW(CFG_LIGHT_ELEM, CFG_FLOAT, "lights", "range", range),
};

#define CFG_FIELD_COUNT ((int)(sizeof(CFG_FIELDS) / sizeof(CFG_FIELDS[0])))

/*
 * The element arrays: the JSON key, the owner whose rows describe an entry,
 * whether entries have a NAME to match on, and how to reach element i.
 *
 * ONE list, and it drives the writer, the reader, the unknown-key sweep and that
 * sweep's skip list. It briefly drove only the last of those while a comment
 * here claimed all of them -- which is the failure this whole file exists to
 * prevent, and worse than not having the list at all: the next reader trusts the
 * comment and edits one site.
 *
 * Probes are `by_name` false because they have no name: their order IS their
 * identity, probes[0] being the one the .cscn authored first. Lights and
 * materials prefer their name and fall back to the index for the unnamed, which
 * every CLI-spawned light and procedural material is.
 */
typedef struct ConfigArray {
    const char* key;  // the JSON array's name
    const char* noun; // one element of it, for warnings that read as English
    ConfigOwner owner;
    bool by_name;
    // Element i of `scene`, or NULL past the end. *name is its identity, or NULL
    // when it has none.
    void* (*at)(Scene* scene, int i, const char** name);
} ConfigArray;

static void* _probe_at(Scene* scene, int i, const char** name) {
    *name = NULL;
    ReflectionProbeSet* set = scene ? scene->probe_set : NULL;
    return (set && i >= 0 && i < set->count) ? set->probes[i] : NULL;
}

static void* _decal_at(Scene* scene, int i, const char** name) {
    // No name: a decal has none to carry, so order is identity -- the probes'
    // arrangement, and for the probes' reason.
    *name = NULL;
    return (scene && i >= 0 && i < scene->decal_count) ? &scene->decals[i] : NULL;
}

static void* _light_at(Scene* scene, int i, const char** name) {
    *name = NULL;
    if (!scene || i < 0 || (size_t)i >= scene->light_count)
        return NULL;
    Light* light = scene->lights[i];
    if (light)
        *name = light->name;
    return light;
}

static void* _material_at(Scene* scene, int i, const char** name) {
    *name = NULL;
    if (!scene || i < 0 || (size_t)i >= scene->material_count)
        return NULL;
    Material* material = scene->materials[i];
    if (material)
        *name = material->name;
    return material;
}

static const ConfigArray CFG_ARRAYS[] = {
    {"probes", "probe", CFG_PROBE_ELEM, false, _probe_at},
    {"decals", "decal", CFG_DECAL_ELEM, false, _decal_at},
    {"lights", "light", CFG_LIGHT_ELEM, true, _light_at},
    {"materials", "material", CFG_MATERIAL_ELEM, true, _material_at},
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
    _source.clouds = src->clouds;
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
    case CFG_DECAL_ELEM:
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
        cJSON_AddBoolToObject(src, "clouds", _source.clouds);
    }
    if (engine) {
        cJSON_AddNumberToObject(src, "width", engine->win_width);
        cJSON_AddNumberToObject(src, "height", engine->win_height);
    }
}

/*
 * A material's tunable, stored through the material's own setter.
 *
 * The offset write would be identical today -- material_param_set is that same
 * store -- but going through it is what carries a future change there into this
 * path instead of silently around it. The param is re-found by key rather than
 * carried on the row, which is a 30-entry scan once per field at restore and
 * saves putting a field on ConfigField for one owner's benefit.
 */
static void _apply_material_param(ConfigApplyCtx* ctx, void* base, const ConfigField* f,
                                  const double* v, int n) {
    (void)ctx;
    (void)n;
    const MaterialParam* p = material_param_find(f->key);
    if (!p)
        return;
    const float fv[3] = {(float)v[0], (float)v[1], (float)v[2]};
    material_param_set((Material*)base, p, fv);
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
    if (!p)
        return false;
    // A switch with no default, like every other type dispatch in this file and
    // in material.c: -Wall implies -Wswitch, so a new MaterialParamType breaks
    // the build here. The if/else chain this replaced defaulted to CFG_FLOAT,
    // which would have read four bytes off a one-byte bool.
    ConfigType type;
    switch (p->type) {
    case MATERIAL_PARAM_FLOAT:
        type = CFG_FLOAT;
        break;
    case MATERIAL_PARAM_COLOR:
        type = CFG_VEC3;
        break;
    case MATERIAL_PARAM_INT:
        type = p->enum_labels ? CFG_ENUM : CFG_INT;
        break;
    case MATERIAL_PARAM_TEXTURE:
        return false; // rebinding an image is authoring, not tuning
    }
    *out = (ConfigField){.owner = CFG_MATERIAL_ELEM,
                         .type = (unsigned char)type,
                         .section = "materials",
                         .key = p->key,
                         .offset = p->offset,
                         .labels = p->enum_labels,
                         .label_count = p->enum_count,
                         .apply = _apply_material_param};
    return true;
}

/*
 * The rows describing one element of `owner`, one per call, `*cursor` opaque.
 *
 * This is what lets ONE writer and ONE reader serve all three arrays: materials
 * come from MATERIAL_PARAMS and everything else from CFG_FIELDS, and neither
 * walker has to know which.
 */
static bool _element_row(ConfigOwner owner, int* cursor, ConfigField* out) {
    if (owner == CFG_MATERIAL_ELEM) {
        while (*cursor < (int)MATERIAL_PARAM_COUNT) {
            if (_material_row(&MATERIAL_PARAMS[(*cursor)++], out))
                return true;
        }
        return false;
    }
    while (*cursor < CFG_FIELD_COUNT) {
        const ConfigField* f = &CFG_FIELDS[(*cursor)++];
        if ((ConfigOwner)f->owner == owner) {
            *out = *f;
            return true;
        }
    }
    return false;
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

    int cursor = 0;
    ConfigField row;
    while (_element_row(owner, &cursor, &row)) {
        if (!_write_field(obj, &row, base))
            return false;
        (*count)++;
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

    // One loop for all three arrays, off CFG_ARRAYS. An array is omitted rather
    // than written empty, for the same reason an absent subsystem's section is.
    bool ok = true;
    for (size_t a = 0; ok && a < CFG_ARRAY_COUNT; a++) {
        const ConfigArray* arr = &CFG_ARRAYS[a];
        const char* name = NULL;
        if (!arr->at(scene, 0, &name))
            continue;
        cJSON* array = cJSON_AddArrayToObject(root, arr->key);
        ok = array != NULL;
        for (int i = 0; ok; i++) {
            const void* base = arr->at(scene, i, &name);
            if (!base)
                break;
            ok = _write_element(array, arr->owner, base, arr->by_name ? name : NULL, i, &written);
        }
    }

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
 * The element arrays, all three through one walk off CFG_ARRAYS.
 *
 * Matching: by NAME where the array has one, falling back to `index` for the
 * unnamed -- every CLI-spawned light and every procedural material. Probes have
 * no name at all, so they match by index alone: their order IS their identity.
 * A name is the stable key; an index moves when the content does.
 *
 * This took an `owner` parameter and then hardcoded scene->lights in its else
 * branch, so passing CFG_MATERIAL_ELEM searched the light array -- while the
 * comment above it invited exactly that call.
 */
static int _apply_array(ConfigApplyCtx* ctx, const cJSON* root, const ConfigArray* arr) {
    const cJSON* array = cJSON_GetObjectItemCaseSensitive(root, arr->key);
    if (!array)
        return 0;
    if (!cJSON_IsArray(array)) {
        fprintf(stderr, "Warning: config snapshot: '%s' is not an array; ignored\n", arr->key);
        return 0;
    }

    int written = 0;
    const cJSON* entry = NULL;
    cJSON_ArrayForEach(entry, array) {
        const cJSON* name_item = cJSON_GetObjectItemCaseSensitive(entry, "name");
        const char* want = (arr->by_name && cJSON_IsString(name_item)) ? name_item->valuestring
                                                                      : NULL;
        const cJSON* idx_item = cJSON_GetObjectItemCaseSensitive(entry, "index");
        const int want_index = cJSON_IsNumber(idx_item) ? (int)idx_item->valuedouble : -1;

        void* base = NULL;
        const char* have = NULL;
        if (want) {
            for (int i = 0; !base; i++) {
                void* el = arr->at(ctx->scene, i, &have);
                if (!el)
                    break;
                if (have && strcmp(have, want) == 0)
                    base = el;
            }
        } else if (want_index >= 0) {
            base = arr->at(ctx->scene, want_index, &have);
        }
        if (!base) {
            if (want)
                fprintf(stderr, "Warning: config snapshot: no %s '%s' in this scene; ignored\n",
                        arr->noun, want);
            else
                fprintf(stderr, "Warning: config snapshot: no %s %d in this scene; ignored\n",
                        arr->noun, want_index);
            continue;
        }

        int cursor = 0;
        ConfigField row;
        while (_element_row(arr->owner, &cursor, &row)) {
            if (_apply_field(ctx, entry, &row, base))
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
    // One bit per singleton owner, so an absent subsystem is named once.
    unsigned absent_warned = 0;
    _Static_assert(CFG_FIRST_ELEM_OWNER <= 32, "absent_warned needs a bit per singleton owner");
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
            // block on dry land. Once per OWNER rather than per row: an owner is
            // an integer, where the section is a string this used to dedupe by
            // POINTER, which C leaves free to give two identical literals
            // different addresses and which additionally assumed rows of one
            // section never interleave. Two unenforced invariants for a warning
            // count.
            if (!(absent_warned & (1u << f->owner))) {
                absent_warned |= 1u << f->owner;
                fprintf(stderr, "Warning: config snapshot: this scene has no '%s'; ignored\n",
                        f->section);
            }
            continue;
        }
        if (_apply_field(&ctx, obj, f, base))
            written++;
    }

    /*
     * The sun BEFORE the arrays, which is not where it reads most naturally.
     * sky_update_sun ends in sky_apply_sun_to_light, which rewrites the coupled
     * sun light's intensity, colour, direction and cast_shadows from the sky's
     * own base intensity -- so running it after the lights array silently threw
     * away whatever that array had just restored for that light. Measured on a
     * moved sun: asked 2.5, got 10.
     */
    if (ctx.sun_moved && scene && scene->sky)
        sky_update_sun(scene->sky, scene->ibl, engine);
    /*
     * ...then the chain everything else derived from the environment goes
     * through, which is the same one gui.c's sliders call on release.
     *
     * sky_update_sun alone was what this used to do, and it is a strict SUBSET:
     * it re-bakes the env with sky_bake, which is sky_bake_ex(..., false), so a
     * restored sun did not merely skip the cloud deck -- it stripped the deck
     * out of an env cube that had one, refreshed no probe and re-armed no GI.
     */
    if (ctx.env_changed && scene)
        scene_environment_changed(scene, engine);

    for (size_t a = 0; a < CFG_ARRAY_COUNT; a++)
        written += _apply_array(&ctx, root, &CFG_ARRAYS[a]);
    cJSON_Delete(root);

    /*
     * The pose last, once eye, target and up have all landed -- a view matrix
     * built from any one of them alone is wrong.
     *
     * NOT paired with camera_sync_spherical_from_position, which derives
     * distance/theta/phi FROM the pose and so overwrote the three camera.orbit
     * rows this file had just restored. The snapshot carries them itself,
     * written in the same breath as the pose and therefore already agreeing with
     * it; re-deriving them discarded a hand-edited orbit block and made three
     * rows unreachable from outside the process.
     */
    if (ctx.camera_moved && engine->camera)
        update_engine_camera_lookat(engine);

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
    _read_back.clouds = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(src, "clouds"));
    _read_back.width = _int_or(src, "width", 0);
    _read_back.height = _int_or(src, "height", 0);

    cJSON_Delete(root);
    if (out)
        *out = &_read_back;
    return true;
}
