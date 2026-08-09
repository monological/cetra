#ifndef CSCENE_H
#define CSCENE_H

#include <stdbool.h>
#include <stddef.h>

#include "light.h" // LightUnits: authored intensity units carry through to Light

/*
 * Cetra scene format (.cscn): a JSON scene description that owns the look
 * and references model assets -- the engine-native data glTF cannot carry
 * (environment, extra lights, per-light overrides, post settings, material
 * SSS tags, camera framing). This module only parses the file into a plain
 * struct; applying it to a Scene/PostFX is application policy.
 */

#define CSCENE_MAX_LIGHTS          16
#define CSCENE_MAX_LIGHT_OVERRIDES 16
#define CSCENE_MAX_MATERIALS       8
#define CSCENE_MAX_NAME            128
#define CSCENE_MAX_PATH            1024

typedef enum {
    CSCENE_ENV_NONE = 0, // no environment block in the file
    CSCENE_ENV_HDR,
    CSCENE_ENV_SKY,
} CSceneEnvMode;

typedef enum {
    CSCENE_TONEMAP_NONE = 0, // no tonemap key in the file
    CSCENE_TONEMAP_AGX,
    CSCENE_TONEMAP_ACES,
    CSCENE_TONEMAP_NEUTRAL,
} CSceneTonemap;

typedef enum CSceneLightType {
    CSCENE_LIGHT_POINT = 0,
    CSCENE_LIGHT_AREA,        // rectangular LTC panel (spec 9.2)
    CSCENE_LIGHT_DIRECTIONAL, // infinite sun; the only type + first spot that cast shadows
    CSCENE_LIGHT_SPOT,        // cone light
} CSceneLightType;

// All four engine light types (spec 6.2). Shared keys: name, type, position
// (point/spot/area; ignored for directional), color, intensity. Per type:
//   directional -- `direction` (travel direction); shadow-capable via cast_shadows.
//   spot        -- `direction`, `cone` [inner, outer] half-angles in DEGREES,
//                  optional attenuation/range; shadow-capable (the first spot only).
//   area        -- `direction` (the normal; lights only the side it points at),
//                  `size`, optional `up` (orthonormalized against direction).
//   point       -- optional attenuation/range.
// Attenuation and range are optional everywhere they apply: absent = keep the
// engine default (so a 0-filled struct means "engine default", not "zero").
typedef struct CSceneLight {
    char name[CSCENE_MAX_NAME];
    CSceneLightType type;
    float position[3];
    float color[3];
    // As authored, in `units` -- NOT converted here. The conversion lives on
    // Light (set_light_intensity_units), so a .cscn and a glTF import cannot
    // disagree about what a lumen is.
    float intensity;
    LightUnits units;
    float direction[3]; // directional/spot/area (travel direction)
    float size[2];      // area: width x height
    bool has_up;
    float up[3]; // area, optional
    bool has_attenuation;
    float attenuation[3]; // point/spot: constant, linear, quadratic
    bool has_range;
    float range;       // point/spot cull radius (else derived from attenuation)
    float cone[2];     // spot: inner, outer half-angle in DEGREES
    bool cast_shadows; // directional/spot (point/area cannot cast)
} CSceneLight;

typedef struct CSceneLightOverride {
    // Matched against imported light names: the Blender object name, carried
    // through glTF and assimp into Light.name.
    char name[CSCENE_MAX_NAME];
    bool has_size_from_angle;
    float size_from_angle; // authored angular size in radians
    bool has_intensity;
    float intensity;
} CSceneLightOverride;

#define CSCENE_MAX_MATERIAL_PARAMS 24
#define CSCENE_MAX_PARAM_KEY       24

// One authored key from a material override block. The parser deliberately does
// NOT know what any key means -- it records the name and the number(s) and
// nothing else. Resolving a name to a Material field is application policy and
// lives in one table in cscene_apply.c, which is what keeps this header free of
// material.h and makes a new parameter one table row rather than an edit in
// three files.
typedef struct CSceneMaterialParam {
    char key[CSCENE_MAX_PARAM_KEY];
    float value[3]; // scalars occupy value[0]
    int components; // 1 or 3, as authored
} CSceneMaterialParam;

#define CSCENE_MAX_MATERIAL_TEXTURES 2

// One authored TEXTURE key from a material override block. Kept apart from the
// numeric params because a float array cannot hold a path, not because it is a
// different kind of authored value. The parser's blindness is preserved -- it
// records the key and the string and still learns nothing about what either
// means.
//
// Unlike model_path and env_hdr, this is NOT resolved against the scene file's
// directory: material textures go through the texture pool, which resolves
// against its own directory (the app's -t argument) like every other texture
// the engine loads. So the authored value is a plain filename living beside the
// model's other textures, with no path prefix to get wrong.
typedef struct CSceneMaterialTexture {
    char key[CSCENE_MAX_PARAM_KEY];
    char path[CSCENE_MAX_PATH]; // resolved by the texture pool, not by the scene file
} CSceneMaterialTexture;

// Per-material overrides, keyed on the AUTHORED material name. Absent keys keep
// whatever the model imported, so this is an override sheet rather than a
// material definition. Formats that cannot express a property at all (glTF has
// no subsurface) are the main reason it exists.
typedef struct CSceneMaterialOverride {
    char material[CSCENE_MAX_NAME];
    CSceneMaterialParam params[CSCENE_MAX_MATERIAL_PARAMS];
    int param_count;
    CSceneMaterialTexture textures[CSCENE_MAX_MATERIAL_TEXTURES];
    int texture_count;
    // sss stays explicit rather than joining the table above: it is compound
    // (colour and radius are one scatter profile and meaningless apart), and
    // its consumer is PostFX's profile table rather than any Material field, so
    // no offset could describe it.
    bool has_sss;
    float sss_color[3]; // per-channel scatter WIDTH weight, not a tint
    float sss_radius;   // world units
} CSceneMaterialOverride;

// Ambient dust: a scene-level particle effect (like fog). Each field carries a
// presence flag; absent fields keep the recipe defaults in apply_cscene_dust.
typedef struct CSceneDust {
    bool enabled;
    bool has_spawn_rate;
    float spawn_rate;
    bool has_lifetime;
    float lifetime[2]; // min, max seconds
    bool has_size;
    float size[2]; // min, max
    bool has_color;
    float color[4]; // rgba
    bool has_color_jitter;
    float color_jitter;
    bool has_curl;
    float curl[3]; // scale, strength, timescale
    bool has_drift;
    float drift[3];
    bool has_damping;
    float damping;
} CSceneDust;

typedef struct CetraSceneDesc {
    // Paths are resolved against the scene file's directory at load time;
    // consumers receive directly usable paths.
    char model_path[CSCENE_MAX_PATH]; // first models[] entry
    char env_hdr[CSCENE_MAX_PATH];

    CSceneEnvMode env_mode;
    bool env_probe_scene;
    bool has_env_intensity;
    float env_intensity; // IBL/world ambient strength
    bool has_ambient;
    float ambient[3]; // env.ambient: uniform fill radiance (cd/m^2), no-IBL only
    bool has_env_sun; // sky-mode sun angles authored (else the sky default)
    float env_sun_elevation_deg;
    float env_sun_azimuth_deg;

    CSceneLight lights[CSCENE_MAX_LIGHTS];
    int light_count;

    CSceneLightOverride light_overrides[CSCENE_MAX_LIGHT_OVERRIDES];
    int light_override_count;

    // post -- each value carries its own presence flag so apps can apply
    // CLI > scene file > default precedence per field
    CSceneTonemap tonemap;
    bool has_exposure;
    float exposure; // linear multiplier (exporter converts Blender stops)
    // Adaptation, decoupled from the exposure value above. Absent, an authored
    // exposure still pins the frame, which is what every existing scene relies
    // on -- but a scene can now author a starting exposure AND keep adapting,
    // which the implicit coupling made unexpressible.
    bool has_auto_exposure;
    bool auto_exposure;
    // post.camera: aperture/shutter/iso. Present switches exposure from a linear
    // multiplier to EV100, and `exposure` above becomes a bias in stops. Absent
    // leaves both alone, which is what every scene written before this expects.
    bool has_camera_exposure;
    float aperture;      // f-number
    float shutter_speed; // seconds
    float iso;
    // TAAU render-resolution scale in [0.5, 1). Everything before the TAA seam
    // renders at this fraction and the upscale resolve brings it to post res.
    // It rides TAA, which windowed sessions turn on and headless ones do not --
    // so a headless run of a scene that authors this still needs --taa
    // --headless-jitter or the app refuses it and renders full res.
    bool has_render_scale;
    float render_scale;
    bool has_bloom_enabled;
    bool bloom_enabled;
    bool has_bloom_strength;
    float bloom_strength;
    bool has_bloom_threshold;
    float bloom_threshold;
    bool fog_enabled;
    bool has_fog_density;
    float fog_density;
    bool has_fog_anisotropy;
    float fog_anisotropy;
    // The volume's own depth range and how its slices bunch inside it. Distinct
    // from the camera's clip planes, which have no business sizing a medium.
    bool has_fog_near;
    float fog_near; // fog.near: 0 derives from far
    bool has_fog_far;
    float fog_far;
    bool has_fog_depth_dist;
    float fog_depth_dist; // fog.depthDistribution: 1 = pure exponential
    bool has_fog_ambient;
    float fog_ambient[3]; // fog.ambient: isotropic in-scatter radiance (cd/m^2)

    // wind -- a first-class directional scene wind (wind.h). Each value carries
    // its own presence flag; absent fields keep the Wind's built-in defaults.
    bool wind_enabled;
    bool has_wind_direction;
    float wind_direction[3];
    bool has_wind_strength;
    float wind_strength;
    bool has_wind_speed;
    float wind_speed;
    bool has_wind_gust_frequency;
    float wind_gust_frequency;
    bool has_wind_gust_amount;
    float wind_gust_amount;
    bool has_wind_turbulence;
    float wind_turbulence;

    CSceneDust dust;

    CSceneMaterialOverride materials[CSCENE_MAX_MATERIALS];
    int material_count;

    bool has_camera;
    float cam_eye[3];
    float cam_target[3];
    bool has_cam_fov;
    float cam_fov; // vertical, degrees
} CetraSceneDesc;

// The engine LightType a parsed CSceneLightType denotes. Exported because the
// mapping was previously written out at each site that needed it -- the parser,
// the applier, and the unit validation -- and adding a light type left whichever
// one you forgot silently stale.
LightType cscene_light_type(CSceneLightType type);

// True when path ends in .cscn (case-insensitive).
bool cscene_path_is_scene(const char* path);

// Parse a .cscn file. Returns NULL (with a log_warn) on unreadable or
// malformed input. Caller frees with cscene_free.
CetraSceneDesc* cscene_load(const char* path);

void cscene_free(CetraSceneDesc* desc);

#endif // CSCENE_H
