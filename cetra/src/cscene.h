#ifndef CSCENE_H
#define CSCENE_H

#include <stdbool.h>
#include <stddef.h>

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

// v1 lights are point lights only (the exporter converts Blender area fills
// to points; spot/directional land here once the format carries their
// defining parameters -- cutoffs, direction).
typedef struct CSceneLight {
    char name[CSCENE_MAX_NAME];
    float position[3];
    float color[3];
    float intensity;
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

typedef struct CSceneMaterialOverride {
    char material[CSCENE_MAX_NAME];
    bool has_sss;
    float sss_color[3];
    float sss_radius;
    bool has_wind_response; // opts the material into the scene wind (WPO cloth)
    float wind_response;    // 0 = rigid; 1 = full sway
} CSceneMaterialOverride;

typedef struct CetraSceneDesc {
    // Paths are resolved against the scene file's directory at load time;
    // consumers receive directly usable paths.
    char model_path[CSCENE_MAX_PATH]; // first models[] entry
    char env_hdr[CSCENE_MAX_PATH];

    CSceneEnvMode env_mode;
    bool env_probe_scene;
    bool has_env_intensity;
    float env_intensity; // IBL/world ambient strength

    CSceneLight lights[CSCENE_MAX_LIGHTS];
    int light_count;

    CSceneLightOverride light_overrides[CSCENE_MAX_LIGHT_OVERRIDES];
    int light_override_count;

    // post -- each value carries its own presence flag so apps can apply
    // CLI > scene file > default precedence per field
    CSceneTonemap tonemap;
    bool has_exposure;
    float exposure; // linear multiplier (exporter converts Blender stops);
                    // an authored exposure pins the frame: auto-exposure off
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

    CSceneMaterialOverride materials[CSCENE_MAX_MATERIALS];
    int material_count;

    bool has_camera;
    float cam_eye[3];
    float cam_target[3];
    bool has_cam_fov;
    float cam_fov; // vertical, degrees
} CetraSceneDesc;

// True when path ends in .cscn (case-insensitive).
bool cscene_path_is_scene(const char* path);

// Parse a .cscn file. Returns NULL (with a log_warn) on unreadable or
// malformed input. Caller frees with cscene_free.
CetraSceneDesc* cscene_load(const char* path);

void cscene_free(CetraSceneDesc* desc);

#endif // CSCENE_H
