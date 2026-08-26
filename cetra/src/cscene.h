#ifndef CSCENE_H
#define CSCENE_H

#include <stdbool.h>
#include <stddef.h>

#include "light.h" // LightUnits: authored intensity units carry through to Light
#include "roads.h" // MaterialRoad: an authored road IS the runtime road, verbatim

/*
 * Cetra scene format (.cscn): a JSON scene description that owns the look
 * and references model assets -- the engine-native data glTF cannot carry
 * (environment, extra lights, per-light overrides, post settings, material
 * SSS tags, camera framing). This module only parses the file into a plain
 * struct; applying it to a Scene/PostFX is application policy.
 */

#define CSCENE_MAX_LIGHTS          16
#define CSCENE_MAX_FOG_VOLUMES     8
#define CSCENE_MAX_PROBES          8
#define CSCENE_MAX_DECALS          16
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
    //
    // has_intensity distinguishes "authored 1.0" from "said nothing", which an
    // IES profile needs: absent, the loader seeds the intensity from the file's
    // own peak candela, so a profiled light is physically correct without the
    // author transcribing a number the file already carries (spec 11.57).
    bool has_intensity;
    float intensity;
    LightUnits units;
    // Required for directional/spot/area, optional for a point -- which has no
    // cone to aim but does have an IES profile's measurement axis.
    bool has_direction;
    float direction[3]; // travel direction
    float size[2];      // area: width x height
    // The roll about `direction`: a panel's height axis, or an asymmetric
    // profile's azimuth zero. Optional for every type.
    bool has_up;
    float up[3];
    bool has_attenuation;
    float attenuation[3]; // point/spot: constant, linear, quadratic
    bool has_range;
    float range;       // point/spot cull radius (else derived from attenuation)
    float cone[2];     // spot: inner, outer half-angle in DEGREES
    bool cast_shadows; // directional/spot (point/area cannot cast)
    // IESNA LM-63 photometric profile, resolved against the scene file's own
    // directory like `models` and `environment.hdr`. Inline rather than a
    // pointer because cscene_free is a bare free(desc) and the struct must stay
    // POD. Empty = the light keeps its analytic cone.
    char ies_path[CSCENE_MAX_PATH];
} CSceneLight;

typedef struct CSceneLightOverride {
    // Matched against imported light names: the Blender object name, carried
    // through glTF and assimp into Light.name.
    char name[CSCENE_MAX_NAME];
    bool has_size_from_angle;
    float size_from_angle; // authored angular size in radians
    bool has_intensity;
    float intensity;
    bool has_cast_shadows;
    bool cast_shadows; // whether the named light casts; imported lights arrive false
} CSceneLightOverride;

// Sized to hold the whole material vocabulary with room to grow. It is not an
// arbitrary bound: exceeding it silently drops the extra keys, so a cap under
// the vocabulary's size turns "you used most of it" into "extras ignored".
#define CSCENE_MAX_MATERIAL_PARAMS 48
#define CSCENE_MAX_PARAM_KEY       24

// One authored key from a material override block. The parser deliberately does
// NOT know what any key means -- it records the name and the number(s) and
// nothing else, which is what keeps this header free of material.h and makes a
// new parameter one row in the vocabulary rather than an edit here.
typedef struct CSceneMaterialParam {
    char key[CSCENE_MAX_PARAM_KEY];
    float value[3]; // scalars occupy value[0]
    int components; // 1 or 3, as authored
} CSceneMaterialParam;

// Also holds enum LABELS, which are strings on a non-texture key and share this
// array because the parser routes by value type. Raised from 2 when the first
// label key landed, so a material can carry both its textures and a label.
#define CSCENE_MAX_MATERIAL_TEXTURES 4

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

// Most layers a layered material can author. Must equal MATERIAL_MAX_LAYERS in
// material.h; the app's resolver drops anything past it with a warning rather
// than clamping, so the two disagreeing is loud instead of silent.
#define CSCENE_MAX_MATERIAL_LAYERS 4

// One authored layer of a layered surface (spec 11.60). Compound like `sss`
// above and for the same reason: the two maps and the scale describe a single
// material, and a flat key per layer would have to encode the index in its name.
//
// Both paths go through the texture pool exactly as CSceneMaterialTexture does,
// and both load LINEAR -- the albedo is stored in un-decoded codes on purpose,
// because the shader decodes after the blend.
typedef struct CSceneMaterialLayer {
    char albedo[CSCENE_MAX_PATH];  // rgb albedo in stored codes, a = height
    char surface[CSCENE_MAX_PATH]; // rg = normal xy, b = roughness, a = AO
    float uv_scale;                // world units per texture tile
    bool has_uv_scale;
} CSceneMaterialLayer;

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
    // Layered surface. Compound for the same reason sss is, and an ARRAY on top
    // of that, so it cannot ride the key/value table at all.
    CSceneMaterialLayer layers[CSCENE_MAX_MATERIAL_LAYERS];
    int layer_count; // 0 = an ordinary material
    // Splat coordinate space (spec 11.66). Present = the splat map is addressed
    // in world XZ over this rectangle; absent = UV1, the default. One compound
    // key rather than a space enum beside a rectangle, because the two are
    // meaningless apart: world addressing without a domain has no mapping, and
    // UV1 addressing has no use for one.
    bool has_splat_domain;
    float splat_domain[4]; // origin x, origin z, size x, size z, world units
    // Roads over the layer set (spec 11.68). Compound and an array for the same
    // reasons layers are, and the course is an array inside that -- the only
    // key in the format whose value is an array of arrays.
    //
    // The RUNTIME type, not a mirror of it. Unlike CSceneMaterialLayer, which
    // holds paths where MaterialLayer holds Texture*, an authored road and a
    // runtime road are the same five fields in the same units -- so a mirror
    // would be a second declaration, a second pair of caps and a hand-written
    // copy, with nothing but a comment holding the two ceilings together.
    MaterialRoad roads[MATERIAL_MAX_ROADS];
    int road_count; // 0 = no roads
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

/*
 * One authored wave train (spec 11.48), mirroring WaterWaveTrain field for field.
 *
 * A `has_` flag PER FIELD rather than one for the object: a scene naming only the swell's
 * wind speed keeps the library's defaults for the other seven, so authoring a calmer swell
 * cannot silently reset its shape to whatever a zeroed struct means.
 */
typedef struct CSceneWaveTrain {
    bool has_wind_speed;
    float wind_speed; // m/s at the 10 m reference height
    bool has_fetch;
    float fetch; // metres of open water the wind has blown across
    bool has_direction;
    float direction; // radians, relative to windDirection
    bool has_scale;
    float scale; // spectral density weight; 0 removes the train
    bool has_peak_enhancement;
    float peak_enhancement; // JONSWAP gamma
    bool has_focus;
    float focus; // 0..1, how narrowly the energy sits about the train's heading
    bool has_spread_gain;
    float spread_gain; // outer factor on the directional spread power
    bool has_spread_blend;
    float spread_blend; // 0 = a broad cos^2 lobe, 1 = the focused cos^2s one
} CSceneWaveTrain;

// Water surface (water.h). Presence-flagged like dust and wind, so a block that
// authors only a level leaves every other property at the subsystem's own default
// rather than at whatever zero happens to mean for it.
//
// `waves` is spelled rather than numbered -- "gerstner" or "fft" -- because the two
// are different simulations with wildly different costs, and a scene file that said
// `1` would tell a reader nothing about which one it asked for.
typedef struct CSceneWater {
    bool enabled;
    bool has_level;
    float level; // still-water plane, world Y
    bool has_extent;
    // Half-size of the SHOALING BED's domain, not of the drawn surface: since spec 11.35 the
    // grid is projected from the frustum and reaches the horizon, and outside this the bed
    // field reads its nearest edge texel, which is open water.
    float extent;
    bool has_waves;
    bool waves_fft; // false = Gerstner octaves
    bool has_wavelength;
    float wavelength; // the LONGEST octave; Gerstner only
    bool has_amplitude;
    float amplitude; // half crest-to-trough of that octave; Gerstner only
    bool has_steepness;
    float steepness; // 0..1, where 1 is the steepest still-injective crest
    bool has_spread;
    float spread; // per-octave direction fan, radians
    bool has_wind_dir;
    // The direction the waves travel, XZ; normalised on upload. Reaches BOTH models since
    // spec 11.42 -- it fans the Gerstner octaves and centres the spectral spread.
    float wind_dir[2];
    // Spectral sea state (WaterSeaState), all of it inert on the Gerstner path. Physical
    // quantities in metres and m/s: a calmer spectral ocean is a lower wind speed, not a
    // smaller amplitude, which is why the spectral path has no amplitude at all.
    bool has_sea_depth;
    float sea_depth; // metres; drives the TMA shallow-water correction
    CSceneWaveTrain wind_sea;
    CSceneWaveTrain swell;
    bool has_roughness;
    float roughness; // interface roughness; picks the environment lobe's mip
    bool has_ior;
    float ior; // 1.333 for water; F0 falls out of it rather than being authored
    bool has_absorption;
    float absorption[3]; // extinction per world unit, per channel
    bool has_scatter;
    float scatter[3]; // colour the absorbed energy returns as
    bool has_caustics;
    bool caustics;
    bool has_shore_coverage;
    bool shore_coverage; // false = hard shoreline cutoff, no derivative coverage
    bool has_far_lod;
    bool far_lod; // false = full wave detail whatever world a cell covers
} CSceneWater;

/*
 * fogVolumes[] -- a box of denser air (spec 11.39). A TOP-LEVEL block beside water and
 * wind rather than one under `post`, by the same rule water follows: fog's global
 * parameters are fields on PostFX, but a volume is a thing placed in the world, so it
 * belongs in the scene's namespace and not the post chain's.
 *
 * Mirrors FogVolume in scene.h. The cap is this parser's own; a static assert where the two
 * meet keeps it from exceeding the scene's, and extras are dropped with a warning.
 */
typedef struct CSceneFogVolume {
    float center[3];
    float extent[3]; // HALF-extent, matching the engine struct
    float density;
    float feather;
    float tint[3];
} CSceneFogVolume;

/*
 * probes[] -- local reflection probes (spec 11.70), a top-level block for the same reason
 * fogVolumes is one: a probe is a thing placed in the world.
 *
 * position, boxMin and boxMax are all REQUIRED. A probe is a capture point plus the box
 * that box-projects it, and defaulting either would put a probe at the origin reflecting a
 * room it is not in -- which renders plausibly and is wrong everywhere. The array replaces
 * `environment.probe_scene` when both are present: that flag asks for ONE auto-placed
 * probe, and a file that authored its own has already answered the question the flag asks.
 *
 * Mirrors ReflectionProbe in probe.h. The cap is this parser's own; a static assert where
 * the two meet keeps it from exceeding PROBE_SET_MAX.
 */
typedef struct CSceneProbe {
    float position[3];
    float box_min[3];
    float box_max[3];
    float intensity;
    float box_fade;
    bool env_only; // prefilter the global environment instead of capturing the scene
} CSceneProbe;

/*
 * decals[] -- marks projected onto the surfaces inside them (spec 11.73). A top-level
 * block for the reason the two above are: a decal is a thing placed in the world.
 *
 * position, size, direction and image are all REQUIRED, the probe rule again. A decal is
 * a picture, a box to project it through and the way it faces; defaulting any of the four
 * gives a mark somewhere nobody asked for, facing somewhere nobody asked, which renders as
 * a plausible frame with the poster on the wrong wall. `up` is optional because a roll of
 * zero is a real answer and orientation_frame supplies the canonical one -- the same split
 * a light's `up` takes.
 *
 * A MIRROR rather than the runtime type, unlike MaterialRoad: this holds image PATHS where
 * Decal holds Texture*, which is CSceneMaterialLayer's relationship to MaterialLayer and
 * not a road's to itself.
 */
typedef struct CSceneDecal {
    float position[3];
    float size[3]; // HALF-extent, matching the engine struct
    float direction[3];
    float up[3];
    bool has_up;
    float opacity;
    float angle_fade; // DEGREES here; the pack converts to the cosine the shader wants
    float feather;
    float normal_strength;
    // Resolved through the texture pool against the app's -t directory, like every
    // material texture -- not against the scene file's own directory.
    char image[CSCENE_MAX_PATH];
    char surface[CSCENE_MAX_PATH]; // optional; empty = none
} CSceneDecal;

// Mirrors MeteringMode in cetra/src/exposure.h. Kept as its own enum for the
// reason CSceneTonemap is: this header carries no engine headers, and the values
// must agree numerically -- which is unwritten anywhere if both sides use bare
// integer literals instead.
enum {
    CSCENE_METER_UNIFORM = 0,
    CSCENE_METER_CENTRE = 1,
    CSCENE_METER_SPOT = 2,
};

// Mirrors PostFXLutInterp in cetra/src/postfx.h, for the same reason.
enum {
    CSCENE_LUT_TRILINEAR = 0,
    CSCENE_LUT_TETRAHEDRAL = 1,
};

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
    bool has_env_stars; // the "enabled" key specifically was authored (spec 11.79)
    bool env_stars_enabled;
    bool has_env_stars_brightness;
    float env_stars_brightness;
    bool has_env_stars_latitude;
    float env_stars_latitude_deg;
    bool has_env_stars_hour;
    float env_stars_hour_deg;
    bool has_env_night_floor; // the "enabled" key specifically was authored (spec 11.80)
    bool env_night_floor_enabled;
    bool has_env_night_floor_brightness;
    float env_night_floor_brightness;
    bool has_env_cycle; // the "enabled" key specifically was authored (spec 11.81)
    bool env_cycle_enabled;
    bool has_env_cycle_day_seconds;
    float env_cycle_day_seconds;
    bool has_env_cycle_hour;
    float env_cycle_hour;
    bool has_env_moon; // the "enabled" key specifically was authored (spec 11.82)
    bool env_moon_enabled;
    bool has_env_moon_brightness;
    float env_moon_brightness;
    bool has_env_moon_size;
    float env_moon_size;
    bool has_env_moon_elevation;
    float env_moon_elevation_deg;
    bool has_env_moon_azimuth;
    float env_moon_azimuth_deg;

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
    // post.metering: how the frame is weighted and which tails are discarded
    // before the mean. Each carries its own presence flag rather than the block
    // carrying one, so a scene can author a mode without also pinning the
    // percentiles a later default change would otherwise stop reaching it.
    bool has_meter_mode;
    // CSCENE_METER_* below; int here since cscene.h carries no engine headers, the
    // same reason CSceneEnvMode and CSceneTonemap exist.
    int meter_mode;
    bool has_meter_radius;
    float meter_radius;
    bool has_meter_low;
    float meter_low;
    bool has_meter_high;
    float meter_high;
    bool has_adapt_up;
    float adapt_up;
    bool has_adapt_down;
    float adapt_down;
    // post.lut: a .cube colour-grading table, applied after the gamma encode.
    // Path is inline and fixed-size like model_path and env_hdr above, never a
    // pointer -- cscene_free is a bare free(desc). Resolved against the scene
    // file's directory, since a .cube is not a pooled texture and so has no
    // second resolver that would win.
    char lut_path[CSCENE_MAX_PATH];
    bool has_lut_strength;
    float lut_strength;
    bool has_lut_interp;
    // CSCENE_LUT_* below; int for the reason meter_mode is one.
    int lut_interp;
    // post.purkinje: the scotopic shift (spec 11.83). Every key independent with
    // its own presence flag, this file's stated convention -- so a block naming
    // only `strength` stores it and arms nothing.
    bool has_purkinje;
    bool purkinje_enabled;
    bool has_purkinje_strength;
    float purkinje_strength;
    bool has_purkinje_bias_ev;
    float purkinje_bias_ev;
    bool has_purkinje_acuity;
    float purkinje_acuity;
    bool has_purkinje_noise;
    float purkinje_noise;
    // TAAU render-resolution scale in [0.5, 1). Everything before the TAA seam
    // renders at this fraction and the upscale resolve brings it to post res.
    // It rides TAA, which windowed sessions turn on and headless ones do not --
    // so a headless run of a scene that authors this still needs --taa
    // --headless-jitter or the app refuses it and renders full res.
    bool has_render_scale;
    float render_scale;
    bool has_flare;
    float flare; // Lens ghost strength
    bool has_chromatic_aberration;
    float chromatic_aberration; // Channel separation at the corner, in pixels
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
    bool has_wind_phase_variation;
    float wind_phase_variation;

    CSceneDust dust;
    CSceneWater water;

    CSceneFogVolume fog_volumes[CSCENE_MAX_FOG_VOLUMES];
    int fog_volume_count;

    CSceneProbe probes[CSCENE_MAX_PROBES];
    int probe_count;

    CSceneDecal decals[CSCENE_MAX_DECALS];
    int decal_count;

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
