#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h" // strcasecmp

#include "cscene.h"
#include "ext/cJSON.h"
#include "ext/log.h"
#include "util.h"

bool cscene_path_is_scene(const char* path) {
    const char* dot = path ? strrchr(path, '.') : NULL;
    return dot && strcasecmp(dot, ".cscn") == 0;
}

// Resolve a scene-file path against the file's directory in place
// (absolute paths pass through).
static void resolve_in_place(char* path, size_t cap, const char* dir) {
    if (!path[0] || path_is_absolute(path))
        return;
    char joined[CSCENE_MAX_PATH];
    snprintf(joined, sizeof(joined), "%s/%s", dir, path);
    snprintf(path, cap, "%s", joined);
}

static void copy_string(char* dst, size_t cap, const cJSON* item) {
    if (cJSON_IsString(item) && item->valuestring) {
        snprintf(dst, cap, "%s", item->valuestring);
    }
}

static bool get_floats(const cJSON* obj, const char* key, float* out, int n) {
    const cJSON* arr = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) != n)
        return false;
    for (int i = 0; i < n; i++) {
        const cJSON* v = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsNumber(v))
            return false;
        out[i] = (float)v->valuedouble;
    }
    return true;
}

static bool get_vec3(const cJSON* obj, const char* key, float out[3]) {
    return get_floats(obj, key, out, 3);
}

static bool get_float(const cJSON* obj, const char* key, float* out) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(v))
        return false;
    *out = (float)v->valuedouble;
    return true;
}

static bool get_bool(const cJSON* obj, const char* key, bool* out) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsBool(v))
        return false;
    *out = cJSON_IsTrue(v);
    return true;
}

/*
 * Report a key nothing read: a missing key and a MISSPELLED one are the same thing to
 * get_float, so without this a scene authoring "windspeed" gets the default and no
 * indication why.
 *
 * `known` must be CLOSED -- every key the caller reads directly -- which is what makes an
 * unknown one wrong, unlike a material's, whose key set belongs to the application. One
 * function rather than a loop per block: the leading-underscore escape is easy to forget in
 * a copy, and there are four blocks now.
 */
static void warn_unknown_keys(const cJSON* obj, const char* const* known, size_t count,
                              const char* what) {
    const cJSON* key = NULL;
    cJSON_ArrayForEach(key, obj) {
        if (!key->string || key->string[0] == '_') // _comment and friends
            continue;
        bool known_key = false;
        for (size_t i = 0; i < count && !known_key; i++)
            known_key = strcmp(key->string, known[i]) == 0;
        // "a recognised %s" rather than "a %s": the noun varies, and the extraction turned
        // parse_environment's "is not an environment parameter" into "is not a environment".
        if (!known_key)
            log_warn("cscene: %s key '%s' is not a recognised %s parameter; ignored", what,
                     key->string, what);
    }
}

static void parse_models(CetraSceneDesc* d, const cJSON* root, const char* path) {
    const cJSON* models = cJSON_GetObjectItemCaseSensitive(root, "models");
    if (!cJSON_IsArray(models))
        return;
    int count = cJSON_GetArraySize(models);
    if (count > 1) {
        log_warn("cscene '%s': v1 loads a single model; using the first of %d", path, count);
    }
    const cJSON* first = cJSON_GetArrayItem(models, 0);
    if (first) {
        copy_string(d->model_path, CSCENE_MAX_PATH,
                    cJSON_GetObjectItemCaseSensitive(first, "path"));
    }
}

static void parse_environment(CetraSceneDesc* d, const cJSON* root) {
    const cJSON* env = cJSON_GetObjectItemCaseSensitive(root, "environment");
    if (!cJSON_IsObject(env))
        return;
    const cJSON* mode = cJSON_GetObjectItemCaseSensitive(env, "mode");
    if (cJSON_IsString(mode)) {
        if (strcasecmp(mode->valuestring, "hdr") == 0)
            d->env_mode = CSCENE_ENV_HDR;
        else if (strcasecmp(mode->valuestring, "sky") == 0)
            d->env_mode = CSCENE_ENV_SKY;
    }
    copy_string(d->env_hdr, CSCENE_MAX_PATH, cJSON_GetObjectItemCaseSensitive(env, "hdr"));
    get_bool(env, "probe_scene", &d->env_probe_scene);
    d->has_env_intensity = get_float(env, "intensity", &d->env_intensity);
    // Uniform fill in cd/m^2, for scenes with no IBL. A real emitter, so a scale
    // test has to scale it alongside the lights or leave it at zero -- the same
    // rule the fog `ambient` follows.
    d->has_ambient = get_vec3(env, "ambient", d->ambient);
    // sky-mode sun angles (degrees): both required, else the sky's own default.
    const cJSON* sun = cJSON_GetObjectItemCaseSensitive(env, "sun");
    if (cJSON_IsObject(sun)) {
        bool ge = get_float(sun, "elevation", &d->env_sun_elevation_deg);
        bool ga = get_float(sun, "azimuth", &d->env_sun_azimuth_deg);
        d->has_env_sun = ge && ga;
    }

    /*
     * Report a key nothing above read. Same closed-block reasoning as parse_water, and
     * the same failure it is here for: water_fixture.cscn authored sun_elevation and
     * sun_azimuth FLAT for four specs, where the angles live one level down under "sun".
     * The scene rendered at the sky's own 35 degrees while the file said 26, every water
     * arm was calibrated against the frame rather than the authoring, and nothing could
     * say so -- a --sun-elevation 26 render moves 85% of that frame.
     */
    static const char* const known[] = {"mode", "hdr", "probe_scene", "intensity",
                                        "ambient", "sun"};
    warn_unknown_keys(env, known, sizeof(known) / sizeof(known[0]), "environment");
}

LightType cscene_light_type(CSceneLightType type) {
    switch (type) {
        case CSCENE_LIGHT_AREA:
            return LIGHT_AREA;
        case CSCENE_LIGHT_DIRECTIONAL:
            return LIGHT_DIRECTIONAL;
        case CSCENE_LIGHT_SPOT:
            return LIGHT_SPOT;
        default:
            return LIGHT_POINT;
    }
}

static void parse_lights(CetraSceneDesc* d, const cJSON* root) {
    const cJSON* lights = cJSON_GetObjectItemCaseSensitive(root, "lights");
    if (!cJSON_IsArray(lights))
        return;
    const cJSON* l = NULL;
    cJSON_ArrayForEach(l, lights) {
        if (d->light_count >= CSCENE_MAX_LIGHTS) {
            log_warn("cscene: more than %d lights; extras ignored", CSCENE_MAX_LIGHTS);
            break;
        }
        CSceneLight* out = &d->lights[d->light_count];
        memset(out, 0, sizeof(*out));
        copy_string(out->name, CSCENE_MAX_NAME, cJSON_GetObjectItemCaseSensitive(l, "name"));

        // point (default), area, directional, spot; anything else is refused
        // rather than silently coerced into the wrong shape.
        const cJSON* type = cJSON_GetObjectItemCaseSensitive(l, "type");
        out->type = CSCENE_LIGHT_POINT;
        if (cJSON_IsString(type)) {
            if (strcasecmp(type->valuestring, "area") == 0) {
                out->type = CSCENE_LIGHT_AREA;
            } else if (strcasecmp(type->valuestring, "directional") == 0) {
                out->type = CSCENE_LIGHT_DIRECTIONAL;
            } else if (strcasecmp(type->valuestring, "spot") == 0) {
                out->type = CSCENE_LIGHT_SPOT;
            } else if (strcasecmp(type->valuestring, "point") != 0) {
                log_warn("cscene: light '%s' type '%s' unsupported "
                         "(point/area/directional/spot); skipped",
                         out->name, type->valuestring);
                continue;
            }
        }

        // Position anchors point/spot/area; a directional is infinitely far, so
        // it has no meaningful position (read one if given, but do not require).
        bool have_pos = get_vec3(l, "position", out->position);
        if (out->type != CSCENE_LIGHT_DIRECTIONAL && !have_pos) {
            log_warn("cscene: light '%s' missing position; skipped", out->name);
            continue;
        }
        if (!get_vec3(l, "color", out->color)) {
            out->color[0] = out->color[1] = out->color[2] = 1.0f;
        }
        if (!get_float(l, "intensity", &out->intensity))
            out->intensity = 1.0f;
        // Authored unit only -- the conversion is Light's job, so this parser and
        // the glTF importer cannot drift apart on what a lumen is. Absent leaves
        // DEFAULT, which resolves against the type when something displays it.
        //
        // This is also the one place a type and a unit are read from the same
        // object, so it is where "lumens on a sun" gets caught. The setter
        // cannot check it without depending on assignment order.
        out->units = LIGHT_UNITS_DEFAULT;
        const cJSON* unit = cJSON_GetObjectItemCaseSensitive(l, "intensity_unit");
        if (cJSON_IsString(unit) && unit->valuestring) {
            if (strcasecmp(unit->valuestring, "lumens") == 0)
                out->units = LIGHT_UNITS_LUMENS;
            else if (strcasecmp(unit->valuestring, "candela") == 0)
                out->units = LIGHT_UNITS_CANDELA;
            else if (strcasecmp(unit->valuestring, "lux") == 0)
                out->units = LIGHT_UNITS_LUX;
            else if (strcasecmp(unit->valuestring, "nits") == 0)
                out->units = LIGHT_UNITS_NITS;
            else
                log_warn("cscene: light '%s' unknown intensity_unit '%s' "
                         "(candela|lumens|lux|nits)",
                         out->name, unit->valuestring);

            LightType lt = cscene_light_type(out->type);
            if (!light_units_valid_for_type(lt, out->units)) {
                log_warn("cscene: light '%s' is authored in %s, which a %s light is not "
                         "measured in; using %s instead",
                         out->name, light_units_name(out->units), light_type_name(lt),
                         light_units_name(light_canonical_units(lt)));
                out->units = LIGHT_UNITS_DEFAULT;
            }
        }

        // Direction (travel direction) defines directional/spot/area; point ignores it.
        if (out->type != CSCENE_LIGHT_POINT && !get_vec3(l, "direction", out->direction)) {
            log_warn("cscene: %s light '%s' missing direction; skipped", type->valuestring,
                     out->name);
            continue;
        }

        // Optional everywhere they apply: absent = keep the engine default.
        get_bool(l, "cast_shadows", &out->cast_shadows);
        out->has_attenuation = get_floats(l, "attenuation", out->attenuation, 3);
        // The constant/linear/quadratic triple is the fixed-function falloff and
        // no longer reaches the shader: punctual lights are inverse-square,
        // windowed by `range`. Left parsed so old scenes still load, but say so
        // rather than letting an authored value look like it still does anything.
        if (out->has_attenuation)
            log_warn("cscene: light '%s' authors 'attenuation' -- ignored; falloff is "
                     "inverse-square, use 'range' to bound it",
                     out->name);
        out->has_range = get_float(l, "range", &out->range);

        if (out->type == CSCENE_LIGHT_AREA) {
            // A panel with no extent has no defined shape, so size is required.
            if (!get_floats(l, "size", out->size, 2)) {
                log_warn("cscene: area light '%s' needs size [w, h]; skipped", out->name);
                continue;
            }
            if (out->size[0] <= 0.0f || out->size[1] <= 0.0f) {
                log_warn("cscene: area light '%s' has non-positive size; skipped", out->name);
                continue;
            }
            out->has_up = get_vec3(l, "up", out->up);
        } else if (out->type == CSCENE_LIGHT_SPOT) {
            // A cone with no angles has no shape; require [inner, outer] degrees.
            if (!get_floats(l, "cone", out->cone, 2)) {
                log_warn("cscene: spot light '%s' needs cone [inner, outer] degrees; skipped",
                         out->name);
                continue;
            }
            if (out->cone[0] <= 0.0f || out->cone[1] < out->cone[0] || out->cone[1] >= 90.0f) {
                log_warn("cscene: spot light '%s' cone must be 0 < inner <= outer < 90; skipped",
                         out->name);
                continue;
            }
        }
        d->light_count++;
    }
}

static void parse_light_overrides(CetraSceneDesc* d, const cJSON* root) {
    const cJSON* arr = cJSON_GetObjectItemCaseSensitive(root, "light_overrides");
    if (!cJSON_IsArray(arr))
        return;
    const cJSON* o = NULL;
    cJSON_ArrayForEach(o, arr) {
        if (d->light_override_count >= CSCENE_MAX_LIGHT_OVERRIDES)
            break;
        CSceneLightOverride* out = &d->light_overrides[d->light_override_count];
        copy_string(out->name, CSCENE_MAX_NAME, cJSON_GetObjectItemCaseSensitive(o, "name"));
        if (!out->name[0])
            continue;
        out->has_size_from_angle = get_float(o, "size_from_angle", &out->size_from_angle);
        out->has_intensity = get_float(o, "intensity", &out->intensity);
        out->has_cast_shadows = get_bool(o, "cast_shadows", &out->cast_shadows);
        d->light_override_count++;
    }
}

// A post.metering float, refused if it is outside the range the meter can act
// on. Absent leaves the engine default; out of range warns by name and is
// ignored, so the author sees the key they typed rather than a frame that is
// subtly wrong.
static bool _meter_range(const cJSON* obj, const char* key, float lo, float hi, float* out) {
    float v = 0.0f;
    if (!get_float(obj, key, &v))
        return false;
    if (!(v >= lo && v <= hi)) {
        log_warn("cscene: post.metering.%s %g is outside [%g, %g]; ignored", key, (double)v,
                 (double)lo, (double)hi);
        return false;
    }
    *out = v;
    return true;
}

static void parse_post(CetraSceneDesc* d, const cJSON* root) {
    const cJSON* post = cJSON_GetObjectItemCaseSensitive(root, "post");
    if (!cJSON_IsObject(post))
        return;
    const cJSON* tonemap = cJSON_GetObjectItemCaseSensitive(post, "tonemap");
    if (cJSON_IsString(tonemap)) {
        // Vocabulary validation is schema knowledge: normalize here, like the
        // environment mode, so consumers only map enums.
        if (strcasecmp(tonemap->valuestring, "agx") == 0)
            d->tonemap = CSCENE_TONEMAP_AGX;
        else if (strcasecmp(tonemap->valuestring, "aces") == 0)
            d->tonemap = CSCENE_TONEMAP_ACES;
        else if (strcasecmp(tonemap->valuestring, "neutral") == 0)
            d->tonemap = CSCENE_TONEMAP_NEUTRAL;
        else
            log_warn("cscene: unknown tonemap '%s' (agx|aces|neutral)", tonemap->valuestring);
    }
    d->has_exposure = get_float(post, "exposure", &d->exposure);
    d->has_auto_exposure = get_bool(post, "auto_exposure", &d->auto_exposure);

    const cJSON* cam = cJSON_GetObjectItemCaseSensitive(post, "camera");
    if (cJSON_IsObject(cam)) {
        // All three or none: a partial camera would silently mix authored
        // settings with defaults, and the result reads as a wrong exposure with
        // nothing pointing at the missing key.
        bool a = get_float(cam, "aperture", &d->aperture);
        bool s = get_float(cam, "shutter", &d->shutter_speed);
        bool i = get_float(cam, "iso", &d->iso);
        if (a && s && i) {
            d->has_camera_exposure = true;
        } else {
            log_warn("cscene: post.camera needs aperture, shutter and iso together; ignored");
        }
        static const char* const cam_known[] = {"aperture", "shutter", "iso"};
        warn_unknown_keys(cam, cam_known, sizeof(cam_known) / sizeof(cam_known[0]),
                          "post.camera");
    }

    const cJSON* meter = cJSON_GetObjectItemCaseSensitive(post, "metering");
    if (cJSON_IsObject(meter)) {
        const cJSON* mode = cJSON_GetObjectItemCaseSensitive(meter, "mode");
        if (cJSON_IsString(mode)) {
            // strcasecmp, like the tonemap and environment-mode parses above:
            // vocabulary validation is schema knowledge, so "Spot" is the same
            // word as "spot" here rather than a key that warns and vanishes.
            const char* m = mode->valuestring;
            int v = strcasecmp(m, "uniform") == 0 ? CSCENE_METER_UNIFORM
                    : (strcasecmp(m, "centre") == 0 || strcasecmp(m, "center") == 0)
                        ? CSCENE_METER_CENTRE
                    : strcasecmp(m, "spot") == 0 ? CSCENE_METER_SPOT
                                                 : -1;
            if (v < 0)
                log_warn("cscene: unknown metering mode '%s' (uniform|centre|center|spot)", m);
            else {
                d->meter_mode = v;
                d->has_meter_mode = true;
            }
        }
        // Refused rather than clamped, which is this parser's stated rule a few
        // lines below for render_scale: silently moving an authored number into
        // range hides a typo behind a slightly wrong frame, and the author never
        // learns the value they wrote is not the value they got.
        // 0.02 lower bound: a smaller spot contains no texel of the 64x64 measure
        // target at all, and an empty histogram freezes the meter silently.
        d->has_meter_radius = _meter_range(meter, "radius", 0.02f, 1.0f, &d->meter_radius);
        d->has_meter_low = _meter_range(meter, "low", 0.0f, 1.0f, &d->meter_low);
        d->has_meter_high = _meter_range(meter, "high", 0.0f, 1.0f, &d->meter_high);
        d->has_adapt_up = _meter_range(meter, "adapt_up", 0.0f, 1.0f, &d->adapt_up);
        d->has_adapt_down = _meter_range(meter, "adapt_down", 0.0f, 1.0f, &d->adapt_down);
        static const char* const meter_known[] = {"mode", "radius",    "low",
                                                  "high", "adapt_up", "adapt_down"};
        warn_unknown_keys(meter, meter_known, sizeof(meter_known) / sizeof(meter_known[0]),
                          "post.metering");
    }

    // Refused rather than clamped: silently moving an authored number into
    // range hides a typo behind a slightly soft frame, and the author never
    // learns the value they wrote is not the value they got. The bound matches
    // postfx's own (see postfx_clamp_render_scale), which clamps instead
    // because it takes runtime input rather than an authored file.
    if (get_float(post, "render_scale", &d->render_scale)) {
        d->has_render_scale = d->render_scale >= 0.5f && d->render_scale < 1.0f;
        if (!d->has_render_scale)
            log_warn("cscene: post.render_scale %.3f out of [0.5, 1); ignored", d->render_scale);
    }

    d->has_flare = get_float(post, "flare", &d->flare);
    d->has_chromatic_aberration =
        get_float(post, "chromatic_aberration", &d->chromatic_aberration);

    const cJSON* bloom = cJSON_GetObjectItemCaseSensitive(post, "bloom");
    if (cJSON_IsObject(bloom)) {
        d->has_bloom_enabled = get_bool(bloom, "enabled", &d->bloom_enabled);
        d->has_bloom_strength = get_float(bloom, "strength", &d->bloom_strength);
        d->has_bloom_threshold = get_float(bloom, "threshold", &d->bloom_threshold);
        static const char* const bloom_known[] = {"enabled", "strength", "threshold"};
        warn_unknown_keys(bloom, bloom_known, sizeof(bloom_known) / sizeof(bloom_known[0]),
                          "post.bloom");
    }
    const cJSON* fog = cJSON_GetObjectItemCaseSensitive(post, "fog");
    if (cJSON_IsObject(fog)) {
        get_bool(fog, "enabled", &d->fog_enabled);
        d->has_fog_density = get_float(fog, "density", &d->fog_density);
        d->has_fog_anisotropy = get_float(fog, "anisotropy", &d->fog_anisotropy);
        d->has_fog_near = get_float(fog, "near", &d->fog_near);
        d->has_fog_far = get_float(fog, "far", &d->fog_far);
        d->has_fog_depth_dist = get_float(fog, "depthDistribution", &d->fog_depth_dist);
        // Isotropic in-scatter radiance (cd/m^2) -- skylight reaching the medium
        // from every direction. A real emitter, so it does not track the lamps;
        // a scale test has to scale it too, or leave it at zero.
        d->has_fog_ambient = get_vec3(fog, "ambient", d->fog_ambient);
        static const char* const fog_known[] = {
            "enabled", "density", "anisotropy", "near", "far", "depthDistribution", "ambient",
        };
        warn_unknown_keys(fog, fog_known, sizeof(fog_known) / sizeof(fog_known[0]), "post.fog");
    }

    static const char* const known[] = {
        "tonemap", "exposure", "auto_exposure",        "camera", "render_scale",
        "flare",   "bloom",    "chromatic_aberration", "fog",     "metering",
    };
    warn_unknown_keys(post, known, sizeof(known) / sizeof(known[0]), "post");
}

static void parse_wind(CetraSceneDesc* d, const cJSON* root) {
    const cJSON* wind = cJSON_GetObjectItemCaseSensitive(root, "wind");
    if (!cJSON_IsObject(wind))
        return;
    d->wind_enabled = true; // presence implies on unless "enabled": false
    get_bool(wind, "enabled", &d->wind_enabled);
    d->has_wind_direction = get_vec3(wind, "direction", d->wind_direction);
    d->has_wind_strength = get_float(wind, "strength", &d->wind_strength);
    d->has_wind_speed = get_float(wind, "speed", &d->wind_speed);
    d->has_wind_gust_frequency = get_float(wind, "gustFrequency", &d->wind_gust_frequency);
    d->has_wind_gust_amount = get_float(wind, "gustAmount", &d->wind_gust_amount);
    d->has_wind_turbulence = get_float(wind, "turbulence", &d->wind_turbulence);
    d->has_wind_phase_variation =
        get_float(wind, "phaseVariation", &d->wind_phase_variation);

    static const char* const known[] = {
        "enabled", "direction", "strength", "speed",      "gustFrequency",
        "gustAmount", "turbulence", "phaseVariation",
    };
    warn_unknown_keys(wind, known, sizeof(known) / sizeof(known[0]), "wind");
}

static void parse_dust(CetraSceneDesc* d, const cJSON* root) {
    const cJSON* dust = cJSON_GetObjectItemCaseSensitive(root, "dust");
    if (!cJSON_IsObject(dust))
        return;
    CSceneDust* out = &d->dust;
    out->enabled = true; // presence implies on unless "enabled": false
    get_bool(dust, "enabled", &out->enabled);
    out->has_spawn_rate = get_float(dust, "spawnRate", &out->spawn_rate);
    out->has_lifetime = get_floats(dust, "lifetime", out->lifetime, 2);
    out->has_size = get_floats(dust, "size", out->size, 2);
    out->has_color = get_floats(dust, "color", out->color, 4);
    out->has_color_jitter = get_float(dust, "colorJitter", &out->color_jitter);
    const cJSON* curl = cJSON_GetObjectItemCaseSensitive(dust, "curl");
    if (cJSON_IsObject(curl)) {
        // All three or none, so a partial curl block never half-overrides.
        out->has_curl = get_float(curl, "scale", &out->curl[0]) &&
                        get_float(curl, "strength", &out->curl[1]) &&
                        get_float(curl, "timescale", &out->curl[2]);
        static const char* const curl_known[] = {"scale", "strength", "timescale"};
        warn_unknown_keys(curl, curl_known, sizeof(curl_known) / sizeof(curl_known[0]),
                          "dust.curl");
    }
    out->has_drift = get_vec3(dust, "drift", out->drift);
    out->has_damping = get_float(dust, "damping", &out->damping);

    static const char* const known[] = {
        "enabled", "spawnRate", "lifetime", "size",  "color",
        "colorJitter", "curl",   "drift",    "damping",
    };
    warn_unknown_keys(dust, known, sizeof(known) / sizeof(known[0]), "dust");
}

/*
 * water -- a scene subsystem, so a TOP-LEVEL block beside wind and dust rather than one
 * inside `post`. The plan for this put it next to `fog`, which is wrong about where it
 * belongs: fog is a field on PostFX and water is owned by the Scene, so authoring it
 * under `post` would put a scene-graph citizen in the post chain's namespace.
 */
/*
 * fogVolumes[] -- boxes of denser air, a top-level block for the same reason water is one.
 *
 * center and extent are REQUIRED; a volume without a place and a size is not a volume, and
 * defaulting either would silently put a box at the origin. Everything else has a default:
 * a white tint leaves the surrounding air's colour alone, and a zero feather is a hard
 * edge, which is at least visibly wrong rather than quietly so.
 */
static void parse_fog_volumes(CetraSceneDesc* d, const cJSON* root) {
    const cJSON* volumes = cJSON_GetObjectItemCaseSensitive(root, "fogVolumes");
    if (!cJSON_IsArray(volumes))
        return;
    const cJSON* v = NULL;
    cJSON_ArrayForEach(v, volumes) {
        if (d->fog_volume_count >= CSCENE_MAX_FOG_VOLUMES) {
            log_warn("cscene: more than %d fog volumes; extras ignored",
                     CSCENE_MAX_FOG_VOLUMES);
            break;
        }
        CSceneFogVolume* out = &d->fog_volumes[d->fog_volume_count];
        memset(out, 0, sizeof(*out));
        if (!get_floats(v, "center", out->center, 3) ||
            !get_floats(v, "extent", out->extent, 3)) {
            log_warn("cscene: fog volume needs both center and extent; skipped");
            continue;
        }
        // The memset above supplies every zero default; only the non-zero one is spelled.
        get_float(v, "density", &out->density);
        get_float(v, "feather", &out->feather);
        out->tint[0] = out->tint[1] = out->tint[2] = 1.0f;
        get_floats(v, "tint", out->tint, 3);
        d->fog_volume_count++;
    }
}

/*
 * One nested wave train, `water.windSea` or `water.swell` (spec 11.48).
 *
 * Nested rather than flat-prefixed (`swellWindSpeed`, `swellSpreadGain`, ...) because a flat
 * set leaves the wind sea's own fields unprefixed, so they read as belonging to the water
 * rather than to a train -- which is exactly the confusion that let a hardcoded swell sit
 * beside an authorable wind sea for six specs without anyone seeing it.
 */
static void parse_wave_train(const cJSON* water, const char* name, CSceneWaveTrain* out) {
    const cJSON* train = cJSON_GetObjectItemCaseSensitive(water, name);
    if (!cJSON_IsObject(train))
        return;
    out->has_wind_speed = get_float(train, "windSpeed", &out->wind_speed);
    out->has_fetch = get_float(train, "fetch", &out->fetch);
    out->has_direction = get_float(train, "direction", &out->direction);
    out->has_scale = get_float(train, "scale", &out->scale);
    out->has_peak_enhancement = get_float(train, "peakEnhancement", &out->peak_enhancement);
    out->has_focus = get_float(train, "focus", &out->focus);
    out->has_spread_gain = get_float(train, "spreadGain", &out->spread_gain);
    out->has_spread_blend = get_float(train, "spreadBlend", &out->spread_blend);

    static const char* const known[] = {
        "windSpeed", "fetch", "direction",  "scale",
        "peakEnhancement", "focus", "spreadGain", "spreadBlend",
    };
    // Named with the block so a warning says WHICH train, since the two accept the same keys.
    char what[32];
    snprintf(what, sizeof(what), "water.%s", name);
    warn_unknown_keys(train, known, sizeof(known) / sizeof(known[0]), what);
}

static void parse_water(CetraSceneDesc* d, const cJSON* root) {
    const cJSON* water = cJSON_GetObjectItemCaseSensitive(root, "water");
    if (!cJSON_IsObject(water))
        return;
    CSceneWater* out = &d->water;
    out->enabled = true; // presence implies on unless "enabled": false
    get_bool(water, "enabled", &out->enabled);
    out->has_level = get_float(water, "level", &out->level);
    out->has_extent = get_float(water, "extent", &out->extent);
    out->has_wavelength = get_float(water, "wavelength", &out->wavelength);
    out->has_amplitude = get_float(water, "amplitude", &out->amplitude);
    out->has_steepness = get_float(water, "steepness", &out->steepness);
    out->has_spread = get_float(water, "spread", &out->spread);
    out->has_wind_dir = get_floats(water, "windDirection", out->wind_dir, 2);
    out->has_sea_depth = get_float(water, "seaDepth", &out->sea_depth);
    parse_wave_train(water, "windSea", &out->wind_sea);
    parse_wave_train(water, "swell", &out->swell);
    out->has_roughness = get_float(water, "roughness", &out->roughness);
    out->has_ior = get_float(water, "ior", &out->ior);
    out->has_absorption = get_vec3(water, "absorption", out->absorption);
    out->has_scatter = get_vec3(water, "scatter", out->scatter);
    out->has_caustics = get_bool(water, "caustics", &out->caustics);
    out->has_shore_coverage = get_bool(water, "shoreCoverage", &out->shore_coverage);
    out->has_far_lod = get_bool(water, "farLod", &out->far_lod);

    // Refused rather than defaulted, on the same reasoning post.render_scale is: a
    // misspelled model name should not quietly select the cheap simulation and leave
    // the author wondering where their ocean went.
    const cJSON* waves = cJSON_GetObjectItemCaseSensitive(water, "waves");
    if (cJSON_IsString(waves) && waves->valuestring) {
        if (strcmp(waves->valuestring, "fft") == 0) {
            out->has_waves = true;
            out->waves_fft = true;
        } else if (strcmp(waves->valuestring, "gerstner") == 0) {
            out->has_waves = true;
            out->waves_fft = false;
        } else {
            log_warn("cscene: water.waves '%s' is not gerstner or fft; ignored",
                     waves->valuestring);
        }
    }

    /*
     * Report a key nothing above read. Water is where this bites hardest: the sea-state
     * keys have no flag, so a scene file is the only way to set them at all.
     *
     * Spec 11.48 moved four of them -- windSpeed, fetch, peakEnhancement and swell -- one
     * level down into windSea{} and swell{}, so a scene written against the old flat block
     * now gets a warning per key rather than a silently different sea. That is the loud
     * migration it wanted, and the reason those four are absent below rather than tolerated.
     *
     * water-fixture-roundtrip asserts this list, what parse_water reads and what the fixture
     * authors all agree, rather than trusting anyone to keep them so.
     */
    static const char* const known[] = {
        "enabled",   "level",         "extent",  "wavelength", "amplitude",  "steepness",
        "spread",    "windDirection", "waves",   "seaDepth",   "windSea",    "swell",
        "roughness", "ior",           "absorption", "scatter",  "caustics",  "shoreCoverage",
        "farLod",
    };
    warn_unknown_keys(water, known, sizeof(known) / sizeof(known[0]), "water");
}

static void parse_materials(CetraSceneDesc* d, const cJSON* root) {
    const cJSON* mats = cJSON_GetObjectItemCaseSensitive(root, "materials");
    if (!cJSON_IsObject(mats))
        return;
    const cJSON* m = NULL;
    cJSON_ArrayForEach(m, mats) { // iterates object members; m->string is the key
        if (d->material_count >= CSCENE_MAX_MATERIALS) {
            log_warn("cscene: more than %d material overrides; extras ignored",
                     CSCENE_MAX_MATERIALS);
            break;
        }
        if (!m->string || !cJSON_IsObject(m))
            continue;
        CSceneMaterialOverride* out = &d->materials[d->material_count];
        snprintf(out->material, CSCENE_MAX_NAME, "%s", m->string);

        // sss is the one compound key: colour and radius describe a single
        // scatter profile and are meaningless apart, so both are required.
        const cJSON* sss = cJSON_GetObjectItemCaseSensitive(m, "sss");
        out->has_sss = cJSON_IsObject(sss) && get_vec3(sss, "color", out->sss_color) &&
                       get_float(sss, "radius", &out->sss_radius);

        // Everything else is recorded by NAME and SHAPE only. Whether a key
        // exists is the application's business (cscene_apply.c owns the table),
        // so a parameter added there needs no change here -- and an unknown key
        // reaches the app to be reported rather than being swallowed silently.
        out->param_count = 0;
        out->texture_count = 0;
        const cJSON* p = NULL;
        cJSON_ArrayForEach(p, m) {
            if (!p->string || p->string[0] == '_') // _comment and friends
                continue;
            if (strcmp(p->string, "sss") == 0)
                continue;
            // A string value is a texture path. Recorded apart from the numeric
            // params only because a float array cannot hold one; the key still
            // means nothing here.
            if (cJSON_IsString(p)) {
                if (!p->valuestring || !p->valuestring[0]) {
                    log_warn("cscene: material '%s' key '%s' is an empty path; ignored",
                             out->material, p->string);
                    continue;
                }
                if (out->texture_count >= CSCENE_MAX_MATERIAL_TEXTURES) {
                    log_warn("cscene: material '%s' has more than %d textures; extras ignored",
                             out->material, CSCENE_MAX_MATERIAL_TEXTURES);
                    continue;
                }
                CSceneMaterialTexture* tex = &out->textures[out->texture_count];
                snprintf(tex->key, CSCENE_MAX_PARAM_KEY, "%s", p->string);
                copy_string(tex->path, CSCENE_MAX_PATH, p);
                out->texture_count++;
                continue;
            }
            if (out->param_count >= CSCENE_MAX_MATERIAL_PARAMS) {
                log_warn("cscene: material '%s' has more than %d parameters; extras ignored",
                         out->material, CSCENE_MAX_MATERIAL_PARAMS);
                break;
            }
            CSceneMaterialParam* prm = &out->params[out->param_count];
            if (cJSON_IsNumber(p)) {
                prm->value[0] = (float)p->valuedouble;
                prm->components = 1;
            } else if (cJSON_IsArray(p) && cJSON_GetArraySize(p) == 3) {
                for (int c = 0; c < 3; c++) {
                    const cJSON* e = cJSON_GetArrayItem(p, c);
                    if (!cJSON_IsNumber(e)) {
                        prm->components = 0;
                        break;
                    }
                    prm->value[c] = (float)e->valuedouble;
                    prm->components = 3;
                }
            } else {
                prm->components = 0;
            }
            if (prm->components == 0) {
                log_warn("cscene: material '%s' key '%s' is neither a number nor a 3-array; "
                         "ignored",
                         out->material, p->string);
                continue;
            }
            snprintf(prm->key, CSCENE_MAX_PARAM_KEY, "%s", p->string);
            out->param_count++;
        }

        if (!out->has_sss && out->param_count == 0 && out->texture_count == 0) {
            log_warn("cscene: material '%s' has no usable keys; skipped", out->material);
            continue;
        }
        d->material_count++;
    }
}

static void parse_camera(CetraSceneDesc* d, const cJSON* root) {
    const cJSON* cam = cJSON_GetObjectItemCaseSensitive(root, "camera");
    if (!cJSON_IsObject(cam))
        return;
    bool ok_eye = get_vec3(cam, "eye", d->cam_eye);
    bool ok_target = get_vec3(cam, "target", d->cam_target);
    d->has_camera = ok_eye && ok_target;
    if (!d->has_camera && (ok_eye || ok_target)) {
        log_warn("cscene: camera needs both eye and target; ignored");
    }
    d->has_cam_fov = get_float(cam, "fov", &d->cam_fov);

    static const char* const known[] = {"eye", "target", "fov"};
    warn_unknown_keys(cam, known, sizeof(known) / sizeof(known[0]), "camera");
}

CetraSceneDesc* cscene_load(const char* path) {
    char* text = read_entire_file(path, NULL);
    if (!text) {
        log_warn("cscene: cannot read '%s'", path);
        return NULL;
    }

    cJSON* root = cJSON_Parse(text);
    free(text);
    if (!root) {
        const char* err = cJSON_GetErrorPtr();
        log_warn("cscene: malformed JSON in '%s' near '%.24s'", path, err ? err : "?");
        return NULL;
    }

    CetraSceneDesc* d = calloc(1, sizeof(CetraSceneDesc));
    if (!d) {
        cJSON_Delete(root);
        return NULL;
    }

    const cJSON* version = cJSON_GetObjectItemCaseSensitive(root, "version");
    if (cJSON_IsNumber(version) && version->valueint != 1) {
        log_warn("cscene '%s': format version %d (this build reads v1)", path, version->valueint);
    }

    parse_models(d, root, path);
    parse_environment(d, root);
    parse_lights(d, root);
    parse_light_overrides(d, root);
    parse_post(d, root);
    parse_wind(d, root);
    parse_dust(d, root);
    parse_water(d, root);
    parse_fog_volumes(d, root);
    parse_materials(d, root);
    parse_camera(d, root);
    cJSON_Delete(root);

    log_info("cscene '%s': model '%s', %d light(s), %d override(s), %d material(s)", path,
             d->model_path[0] ? d->model_path : "-", d->light_count, d->light_override_count,
             d->material_count);

    // Resolve stored paths against the scene file's directory (dirname idiom
    // as in import.c effective_texture_dir) so consumers get usable paths.
    char dir[CSCENE_MAX_PATH];
    const char* slash = strrchr(path, '/');
    if (slash) {
        snprintf(dir, sizeof(dir), "%.*s", (int)(slash - path), path);
    } else {
        snprintf(dir, sizeof(dir), ".");
    }
    resolve_in_place(d->model_path, CSCENE_MAX_PATH, dir);
    resolve_in_place(d->env_hdr, CSCENE_MAX_PATH, dir);
    // Material texture paths are deliberately NOT resolved here. Every texture
    // the engine loads resolves against the texture pool's directory (the -t
    // argument) through find_existing_subpath, and a material's textures are
    // not a different kind of thing just because a scene file named them.
    // Resolving them here instead would put the same path through two
    // resolvers, and the pool's would win.
    return d;
}

void cscene_free(CetraSceneDesc* desc) {
    free(desc);
}
