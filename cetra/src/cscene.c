#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h" // strcasecmp

#include "cscene.h"
#include "ext/cJSON.h"
#include "ext/log.h"
#include "sky.h"
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

// Exactly `n` numbers into `out`, or false with `out` untouched: an array with one bad element
// leaves the default whole rather than half replaced.
static bool get_floats(const cJSON* obj, const char* key, float* out, int n) {
    const cJSON* arr = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) != n)
        return false;
    for (int i = 0; i < n; i++)
        if (!cJSON_IsNumber(cJSON_GetArrayItem(arr, i)))
            return false;
    for (int i = 0; i < n; i++)
        out[i] = (float)cJSON_GetArrayItem(arr, i)->valuedouble;
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

// A float refused if it is outside the range the consumer can act on. Absent
// leaves the engine default; out of range warns by its FULL key and is
// ignored, so the author sees the key they typed rather than a frame that is
// subtly wrong.
//
// `block` is passed rather than baked in: this started as a post.metering
// helper with that name in its format string, and the second caller would
// otherwise have reported a bad post.lut value as a metering one.
static bool _ranged_float(const cJSON* obj, const char* block, const char* key, float lo, float hi,
                          float* out) {
    float v = 0.0f;
    if (!get_float(obj, key, &v))
        return false;
    if (!(v >= lo && v <= hi)) {
        log_warn("cscene: %s.%s %g is outside [%g, %g]; ignored", block, key, (double)v, (double)lo,
                 (double)hi);
        return false;
    }
    *out = v;
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

    // sky-mode star field (spec 11.79). Every key independent, each with its
    // own presence flag (this file's stated convention; the sun block's
    // both-or-nothing is the outlier) -- so a block authoring only
    // "brightness" stores it and arms nothing, which is deliberate: a file
    // describes values, a flag takes actions.
    const cJSON* stars = cJSON_GetObjectItemCaseSensitive(env, "stars");
    if (cJSON_IsObject(stars)) {
        d->has_env_stars = get_bool(stars, "enabled", &d->env_stars_enabled);
        d->has_env_stars_brightness = get_float(stars, "brightness", &d->env_stars_brightness);
        d->has_env_stars_latitude = get_float(stars, "latitude", &d->env_stars_latitude_deg);
        d->has_env_stars_hour = get_float(stars, "hour_angle", &d->env_stars_hour_deg);
        // The nested `sun` object above has no such call and that is how
        // water_fixture ran at the wrong sun for four specs; a new nested
        // block starts closed.
        static const char* const stars_known[] = {"enabled", "brightness", "latitude",
                                                  "hour_angle"};
        warn_unknown_keys(stars, stars_known, sizeof(stars_known) / sizeof(stars_known[0]),
                          "environment.stars");
    }

    // sky-mode night floor (spec 11.80), the stars block's shape: every key
    // independent with its own presence flag.
    const cJSON* nfloor = cJSON_GetObjectItemCaseSensitive(env, "night_floor");
    if (cJSON_IsObject(nfloor)) {
        d->has_env_night_floor = get_bool(nfloor, "enabled", &d->env_night_floor_enabled);
        d->has_env_night_floor_brightness =
            get_float(nfloor, "brightness", &d->env_night_floor_brightness);
        static const char* const nfloor_known[] = {"enabled", "brightness"};
        warn_unknown_keys(nfloor, nfloor_known, sizeof(nfloor_known) / sizeof(nfloor_known[0]),
                          "environment.night_floor");
    }

    // sky-mode overcast (spec 13.7): one value, how much of the sky is under
    // cloud, so a key rather than a block -- there is nothing to enable apart
    // from the amount, and 0 is the clear sky.
    d->has_env_overcast =
        _ranged_float(env, "environment", "overcast", 0.0f, 1.0f, &d->env_overcast);

    // sky-mode radiance scale (spec 13.7): a number, or "photometric" for the
    // one value a file will usually want, by name rather than as 42500 copied
    // from a header.
    const cJSON* sky_scale = cJSON_GetObjectItemCaseSensitive(env, "sky_scale");
    if (cJSON_IsString(sky_scale)) {
        d->has_env_sky_scale = strcasecmp(sky_scale->valuestring, "photometric") == 0;
        if (d->has_env_sky_scale)
            d->env_sky_scale = SKY_PHOTOMETRIC_SCALE;
        else
            log_warn("cscene: environment.sky_scale '%s' is not \"photometric\" or a number; "
                     "ignored",
                     sky_scale->valuestring);
    } else {
        d->has_env_sky_scale =
            _ranged_float(env, "environment", "sky_scale", SKY_RADIANCE_SCALE_MIN,
                          SKY_RADIANCE_SCALE_MAX, &d->env_sky_scale);
    }

    // sky-mode day/night cycle (spec 11.81). `enabled` ARMS it, exactly as in
    // the stars and night_floor blocks above -- a file describes values, a
    // flag takes actions, and letting `day_seconds` arm would have made this
    // block the one exception to a convention stated 40 lines up. So
    // `hour` alone places the sun and animates nothing, and `day_seconds`
    // alone sets a rate for a cycle somebody still has to turn on.
    const cJSON* cycle = cJSON_GetObjectItemCaseSensitive(env, "cycle");
    if (cJSON_IsObject(cycle)) {
        d->has_env_cycle = get_bool(cycle, "enabled", &d->env_cycle_enabled);
        d->has_env_cycle_day_seconds = get_float(cycle, "day_seconds", &d->env_cycle_day_seconds);
        d->has_env_cycle_hour = get_float(cycle, "hour", &d->env_cycle_hour);
        static const char* const cycle_known[] = {"enabled", "day_seconds", "hour"};
        warn_unknown_keys(cycle, cycle_known, sizeof(cycle_known) / sizeof(cycle_known[0]),
                          "environment.cycle");
    }

    // sky-mode moon (spec 11.82), the same shape again. No `phase` key and
    // that is the design rather than an omission: the phase is the angle
    // between the two bodies, so authoring it beside a position would let a
    // file describe a full moon sitting next to the sun.
    const cJSON* moon = cJSON_GetObjectItemCaseSensitive(env, "moon");
    if (cJSON_IsObject(moon)) {
        d->has_env_moon = get_bool(moon, "enabled", &d->env_moon_enabled);
        d->has_env_moon_brightness = get_float(moon, "brightness", &d->env_moon_brightness);
        d->has_env_moon_size = get_float(moon, "size", &d->env_moon_size);
        d->has_env_moon_elevation = get_float(moon, "elevation", &d->env_moon_elevation_deg);
        d->has_env_moon_azimuth = get_float(moon, "azimuth", &d->env_moon_azimuth_deg);
        static const char* const moon_known[] = {"enabled", "brightness", "size", "elevation",
                                                 "azimuth"};
        warn_unknown_keys(moon, moon_known, sizeof(moon_known) / sizeof(moon_known[0]),
                          "environment.moon");
    }

    /*
     * Report a key nothing above read. Same closed-block reasoning as parse_water, and
     * the same failure it is here for: water_fixture.cscn authored sun_elevation and
     * sun_azimuth FLAT for four specs, where the angles live one level down under "sun".
     * The scene rendered at the sky's own 35 degrees while the file said 26, every water
     * arm was calibrated against the frame rather than the authoring, and nothing could
     * say so -- a --sun-elevation 26 render moves 85% of that frame.
     */
    static const char* const known[] = {"mode",    "hdr",  "probe_scene", "intensity",
                                        "ambient", "sun",  "stars",       "night_floor",
                                        "cycle",   "moon", "overcast",    "sky_scale"};
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
        out->has_intensity = get_float(l, "intensity", &out->intensity);
        if (!out->has_intensity)
            out->intensity = 1.0f;
        copy_string(out->ies_path, CSCENE_MAX_PATH, cJSON_GetObjectItemCaseSensitive(l, "profile"));
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

        // Direction (travel direction) defines directional/spot/area, and is
        // REQUIRED for those. Read for a point light too, and optional there: a
        // point has no analytic cone to aim, but an IES profile is measured about
        // an axis, so the key is meaningful on one. Reading it only for the types
        // that require it accepted the key and dropped the value, which is the
        // silent-ignore failure warn_unknown_keys below exists to end.
        out->has_direction = get_vec3(l, "direction", out->direction);
        if (out->type != CSCENE_LIGHT_POINT && !out->has_direction) {
            log_warn("cscene: %s light '%s' missing direction; skipped", type->valuestring,
                     out->name);
            continue;
        }

        // The roll about `direction`: a panel's height axis, or an asymmetric
        // profile's azimuth zero. Read for every type for the same reason
        // `direction` is -- a wall-washer aimed down still has to say which wall,
        // and it is a point or spot that carries the profile.
        out->has_up = get_vec3(l, "up", out->up);

        // Optional everywhere they apply: absent = keep the engine default.
        get_bool(l, "cast_shadows", &out->cast_shadows);
        get_bool(l, "shadow_cache", &out->shadow_cache);
        get_float(l, "emitter_size", &out->emitter_size);
        get_float(l, "shadow_near", &out->shadow_near);
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

        // The lights block was the ONLY parse block without this, against twelve
        // that had it -- so a misspelled key on a light was silently ignored and
        // the light rendered at its default. That is the failure water_fixture
        // shipped for four specs with a flat `sun_elevation` nothing read.
        //
        // Checked last, after every `continue` above: a light that was refused
        // has already been reported by name, and warning twice about the same
        // entry reads as two problems.
        static const char* const known[] = {
            "name",           "type",       "position",     "color",       "intensity",
            "intensity_unit", "direction",  "cast_shadows", "attenuation", "range",
            "size",           "up",         "cone",         "profile",     "shadow_cache",
            "emitter_size",   "shadow_near"};
        warn_unknown_keys(l, known, sizeof(known) / sizeof(known[0]), "light");
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
        warn_unknown_keys(cam, cam_known, sizeof(cam_known) / sizeof(cam_known[0]), "post.camera");
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
        static const char* const blk = "post.metering";
        d->has_meter_radius = _ranged_float(meter, blk, "radius", 0.02f, 1.0f, &d->meter_radius);
        d->has_meter_low = _ranged_float(meter, blk, "low", 0.0f, 1.0f, &d->meter_low);
        d->has_meter_high = _ranged_float(meter, blk, "high", 0.0f, 1.0f, &d->meter_high);
        d->has_adapt_up = _ranged_float(meter, blk, "adapt_up", 0.0f, 1.0f, &d->adapt_up);
        d->has_adapt_down = _ranged_float(meter, blk, "adapt_down", 0.0f, 1.0f, &d->adapt_down);
        static const char* const meter_known[] = {"mode", "radius",   "low",
                                                  "high", "adapt_up", "adapt_down"};
        warn_unknown_keys(meter, meter_known, sizeof(meter_known) / sizeof(meter_known[0]), blk);
    }

    // post.lut: a .cube colour-grading table. The only way to attach one to a
    // scene; --lut overrides the path and --no-lut is the only way to stop it
    // loading at all.
    const cJSON* lut = cJSON_GetObjectItemCaseSensitive(post, "lut");
    if (cJSON_IsObject(lut)) {
        copy_string(d->lut_path, sizeof(d->lut_path),
                    cJSON_GetObjectItemCaseSensitive(lut, "path"));
        d->has_lut_strength =
            _ranged_float(lut, "post.lut", "strength", 0.0f, 1.0f, &d->lut_strength);
        const cJSON* interp = cJSON_GetObjectItemCaseSensitive(lut, "interp");
        if (cJSON_IsString(interp)) {
            const char* m = interp->valuestring;
            int v = strcasecmp(m, "trilinear") == 0     ? CSCENE_LUT_TRILINEAR
                    : strcasecmp(m, "tetrahedral") == 0 ? CSCENE_LUT_TETRAHEDRAL
                                                        : -1;
            if (v < 0)
                log_warn("cscene: unknown lut interp '%s' (trilinear|tetrahedral)", m);
            else {
                d->lut_interp = v;
                d->has_lut_interp = true;
            }
        }
        static const char* const lut_known[] = {"path", "strength", "interp"};
        warn_unknown_keys(lut, lut_known, sizeof(lut_known) / sizeof(lut_known[0]), "post.lut");
    }

    // post.purkinje: the scotopic shift. `enabled` ARMS it and the values alone
    // do not, the stars/night_floor/cycle convention -- a file describes values,
    // a flag takes actions.
    const cJSON* pk = cJSON_GetObjectItemCaseSensitive(post, "purkinje");
    if (cJSON_IsObject(pk)) {
        d->has_purkinje = get_bool(pk, "enabled", &d->purkinje_enabled);
        d->has_purkinje_strength =
            _ranged_float(pk, "post.purkinje", "strength", 0.0f, 1.0f, &d->purkinje_strength);
        d->has_purkinje_bias_ev =
            _ranged_float(pk, "post.purkinje", "bias_ev", -30.0f, 30.0f, &d->purkinje_bias_ev);
        d->has_purkinje_acuity =
            _ranged_float(pk, "post.purkinje", "acuity", 0.0f, 4.0f, &d->purkinje_acuity);
        d->has_purkinje_noise =
            _ranged_float(pk, "post.purkinje", "noise", 0.0f, 4.0f, &d->purkinje_noise);
        // Its own closed list. A nested block without one is how water_fixture
        // ran at the wrong sun for four specs.
        static const char* const pk_known[] = {"enabled", "strength", "bias_ev", "acuity", "noise"};
        warn_unknown_keys(pk, pk_known, sizeof(pk_known) / sizeof(pk_known[0]), "post.purkinje");
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
    d->has_chromatic_aberration = get_float(post, "chromatic_aberration", &d->chromatic_aberration);

    const cJSON* bloom = cJSON_GetObjectItemCaseSensitive(post, "bloom");
    if (cJSON_IsObject(bloom)) {
        d->has_bloom_enabled = get_bool(bloom, "enabled", &d->bloom_enabled);
        d->has_bloom_strength = get_float(bloom, "strength", &d->bloom_strength);
        d->has_bloom_threshold = get_float(bloom, "threshold", &d->bloom_threshold);
        static const char* const bloom_known[] = {"enabled", "strength", "threshold"};
        warn_unknown_keys(bloom, bloom_known, sizeof(bloom_known) / sizeof(bloom_known[0]),
                          "post.bloom");
    }
    const cJSON* glare = cJSON_GetObjectItemCaseSensitive(post, "glare");
    if (cJSON_IsObject(glare)) {
        d->has_glare_enabled = get_bool(glare, "enabled", &d->glare_enabled);
        d->has_glare_strength = get_float(glare, "strength", &d->glare_strength);
        d->has_glare_threshold = get_float(glare, "threshold", &d->glare_threshold);
        static const char* const glare_known[] = {"enabled", "strength", "threshold"};
        warn_unknown_keys(glare, glare_known, sizeof(glare_known) / sizeof(glare_known[0]),
                          "post.glare");
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
        "tonemap",      "exposure", "auto_exposure", "camera",
        "render_scale", "flare",    "bloom",         "chromatic_aberration",
        "fog",          "metering", "lut",           "purkinje",
        "glare",
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
    d->has_wind_phase_variation = get_float(wind, "phaseVariation", &d->wind_phase_variation);
    d->has_wind_air_speed =
        _ranged_float(wind, "wind", "airSpeed", 0.0f, 100.0f, &d->wind_air_speed);

    static const char* const known[] = {
        "enabled",    "direction",  "strength",       "speed",    "gustFrequency",
        "gustAmount", "turbulence", "phaseVariation", "airSpeed",
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
        "enabled",     "spawnRate", "lifetime", "size",    "color",
        "colorJitter", "curl",      "drift",    "damping",
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
            log_warn("cscene: more than %d fog volumes; extras ignored", CSCENE_MAX_FOG_VOLUMES);
            break;
        }
        CSceneFogVolume* out = &d->fog_volumes[d->fog_volume_count];
        memset(out, 0, sizeof(*out));
        if (!get_floats(v, "center", out->center, 3) || !get_floats(v, "extent", out->extent, 3)) {
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
 * probes[] -- local reflection probes (spec 11.70).
 *
 * position, boxMin and boxMax are all REQUIRED, for the reason center and extent are
 * above: a probe missing any of them is not a probe, and the default would be a capture
 * at the origin box-projected against a degenerate box, which renders as a plausible
 * wrong reflection rather than as an error.
 *
 * Closed key list, unlike fogVolumes: this block arrived after warn_unknown_keys existed
 * and a typo'd `boxmin` is exactly the silent failure the required-key check cannot catch.
 */
static void parse_probes(CetraSceneDesc* d, const cJSON* root) {
    static const char* known[] = {"position",  "boxMin",  "boxMax",
                                  "intensity", "boxFade", "envOnly"};

    const cJSON* probes = cJSON_GetObjectItemCaseSensitive(root, "probes");
    if (!cJSON_IsArray(probes))
        return;
    const cJSON* p = NULL;
    cJSON_ArrayForEach(p, probes) {
        if (d->probe_count >= CSCENE_MAX_PROBES) {
            log_warn("cscene: more than %d reflection probes; extras ignored", CSCENE_MAX_PROBES);
            break;
        }
        if (!cJSON_IsObject(p)) {
            log_warn("cscene: reflection probe that is not an object; skipped");
            continue;
        }
        warn_unknown_keys(p, known, sizeof(known) / sizeof(known[0]), "probe");

        CSceneProbe* out = &d->probes[d->probe_count];
        memset(out, 0, sizeof(*out));
        if (!get_floats(p, "position", out->position, 3) ||
            !get_floats(p, "boxMin", out->box_min, 3) ||
            !get_floats(p, "boxMax", out->box_max, 3)) {
            log_warn("cscene: reflection probe needs position, boxMin and boxMax; skipped");
            continue;
        }
        // create_reflection_probe's own defaults, restated because the memset
        // above cleared them and a probe at intensity 0 is an invisible probe.
        out->intensity = 1.0f;
        out->box_fade = 0.2f;
        get_float(p, "intensity", &out->intensity);
        get_float(p, "boxFade", &out->box_fade);
        get_bool(p, "envOnly", &out->env_only);
        d->probe_count++;
    }
}

/*
 * occluders[] -- boxes the occlusion cull treats as solid (spec 11.98).
 *
 * Both keys REQUIRED, and the required-key rule bites harder here than on the
 * neighbours: a defaulted probe renders visibly wrong, where a defaulted
 * occluder covers nothing and reads as culling that quietly is not happening.
 * Inversion (min >= max) is checked at APPLY, not here -- this module parses
 * shape, not meaning, and the Scene's add function owns the refusal.
 */
static void parse_occluders(CetraSceneDesc* d, const cJSON* root) {
    static const char* known[] = {"boxMin", "boxMax"};

    const cJSON* occluders = cJSON_GetObjectItemCaseSensitive(root, "occluders");
    if (!cJSON_IsArray(occluders))
        return;
    const cJSON* o = NULL;
    cJSON_ArrayForEach(o, occluders) {
        if (d->occluder_count >= CSCENE_MAX_OCCLUDERS) {
            log_warn("cscene: more than %d occluders; extras ignored", CSCENE_MAX_OCCLUDERS);
            break;
        }
        if (!cJSON_IsObject(o)) {
            log_warn("cscene: occluder that is not an object; skipped");
            continue;
        }
        warn_unknown_keys(o, known, sizeof(known) / sizeof(known[0]), "occluder");

        CSceneOccluder* out = &d->occluders[d->occluder_count];
        memset(out, 0, sizeof(*out));
        if (!get_floats(o, "boxMin", out->box_min, 3) ||
            !get_floats(o, "boxMax", out->box_max, 3)) {
            log_warn("cscene: occluder needs both boxMin and boxMax; skipped");
            continue;
        }
        d->occluder_count++;
    }
}

/*
 * decals[] -- a mark projected onto whatever is inside its box (spec 11.73).
 *
 * Four required keys, the probe rule: a decal is a picture, a box and a facing, and a
 * default for any of them is a poster on a wall nobody chose. `image` is required for the
 * same reason and refused by name -- a decal whose image failed to load is invisible, and
 * silently dropping it looks exactly like the feature not working.
 */
static void parse_decals(CetraSceneDesc* d, const cJSON* root) {
    static const char* known[] = {"position", "size",    "direction", "up",      "image",
                                  "surface",  "opacity", "angleFade", "feather", "normalStrength"};

    const cJSON* decals = cJSON_GetObjectItemCaseSensitive(root, "decals");
    if (!cJSON_IsArray(decals))
        return;
    const cJSON* p = NULL;
    cJSON_ArrayForEach(p, decals) {
        if (d->decal_count >= CSCENE_MAX_DECALS) {
            log_warn("cscene: more than %d decals; extras ignored", CSCENE_MAX_DECALS);
            break;
        }
        if (!cJSON_IsObject(p)) {
            log_warn("cscene: decal that is not an object; skipped");
            continue;
        }
        warn_unknown_keys(p, known, sizeof(known) / sizeof(known[0]), "decal");

        CSceneDecal* out = &d->decals[d->decal_count];
        memset(out, 0, sizeof(*out));
        if (!get_floats(p, "position", out->position, 3) || !get_floats(p, "size", out->size, 3) ||
            !get_floats(p, "direction", out->direction, 3)) {
            log_warn("cscene: decal needs position, size and direction; skipped");
            continue;
        }
        copy_string(out->image, CSCENE_MAX_PATH, cJSON_GetObjectItemCaseSensitive(p, "image"));
        if (out->image[0] == '\0') {
            log_warn("cscene: decal needs an image; skipped");
            continue;
        }
        // A zero half-extent is a box with no inside, which projects nothing at the price
        // of a per-fragment test. Refused rather than clamped: the authored number is
        // meaningless, where roads refuse a negative feather on the same argument.
        if (out->size[0] <= 0.0f || out->size[1] <= 0.0f || out->size[2] <= 0.0f) {
            log_warn("cscene: decal size must be positive in every axis "
                     "(got %.3f, %.3f, %.3f); skipped",
                     (double)out->size[0], (double)out->size[1], (double)out->size[2]);
            continue;
        }

        // The library defaults, and this is their ONLY statement -- there is no
        // create_decal to inherit them from, so a Decal reaching the scene from
        // anywhere but here gets zeros: an invisible mark with a hard edge.
        // Restated after the memset because a
        // decal at opacity 0 is an invisible decal -- the probe-intensity lesson.
        out->opacity = 1.0f;
        out->angle_fade = 60.0f;
        out->feather = 0.1f;
        out->normal_strength = 1.0f;
        out->has_up = get_floats(p, "up", out->up, 3);
        get_float(p, "opacity", &out->opacity);
        get_float(p, "angleFade", &out->angle_fade);
        get_float(p, "feather", &out->feather);
        get_float(p, "normalStrength", &out->normal_strength);
        copy_string(out->surface, CSCENE_MAX_PATH, cJSON_GetObjectItemCaseSensitive(p, "surface"));
        d->decal_count++;
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
        "windSpeed",       "fetch", "direction",  "scale",
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
    out->has_scatter_albedo = get_vec3(water, "scatterAlbedo", out->scatter_albedo);
    out->has_scatter_glow = get_vec3(water, "scatterGlow", out->scatter_glow);
    out->has_scatter_g = get_float(water, "scatterG", &out->scatter_g);
    /*
     * The old `scatter` is refused BY NAME rather than accepted (spec 11.84), which is
     * the 11.48 precedent and the only detectable option here. Its UNITS changed: it was
     * an absolute radiance and the field that replaces it is a fraction of the incident
     * light, so an old value still parses, still renders, and means something several
     * times too large -- on water_fixture, 5.3x in red through 3.7x in blue, since the
     * factor IS that scene's own incident and the incident is not white. Silence would hand the
     * author a sea that is merely wrong rather than a message saying what to do about it.
     */
    if (cJSON_GetObjectItemCaseSensitive(water, "scatter"))
        log_warn("cscene: water.scatter is gone (spec 11.84). It was an absolute "
                 "radiance; use scatterAlbedo for the FRACTION of incident light the "
                 "body returns (divide the old value by the light falling on it, which "
                 "--water-probe reports as `incident`), and scatterGlow for a sea that "
                 "lights itself. Ignored.");
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
     * Spec 11.84 did the same to `scatter`, which splits into scatterAlbedo and
     * scatterGlow. It is absent below for that reason, and a scene still carrying it gets
     * TWO messages: the specific one above, which is the useful one because its UNITS
     * changed rather than its name, and the generic one from here. Listing it to silence
     * the second was tried and is wrong -- this list means "what parse_water reads", a
     * refused key is not read, and water-fixture-roundtrip asserts exactly that.
     *
     * water-fixture-roundtrip asserts this list, what parse_water reads and what the fixture
     * authors all agree, rather than trusting anyone to keep them so.
     */
    static const char* const known[] = {
        "enabled",       "level",         "extent",     "wavelength",    "amplitude",   "steepness",
        "spread",        "windDirection", "waves",      "seaDepth",      "windSea",     "swell",
        "roughness",     "ior",           "absorption", "scatterAlbedo", "scatterGlow", "caustics",
        "shoreCoverage", "farLod",        "scatterG",
    };
    warn_unknown_keys(water, known, sizeof(known) / sizeof(known[0]), "water");
}

/*
 * A block's keys as one table: every key it accepts is a row, so the table is the closed list
 * warn_unknown_keys needs and a key cannot be read without being known or known without being
 * read. A scalar row names the field it fills, by offset into the block's struct, and for a
 * number the range outside which the value means nothing to the consumer; an OWN row is a key
 * the block's own code reads. `only` limits a row to some of a block's variants -- a fire's
 * kinds -- as a bit set over them, 0 being every variant: a key the variant does not read is
 * then unknown and warned about, rather than read into a field nothing uses.
 */
typedef enum CSceneKeyType {
    CSCENE_KEY_FLOAT,
    CSCENE_KEY_INT, // an int field, read as a JSON number like the rest
    CSCENE_KEY_BOOL,
    CSCENE_KEY_OWN,
} CSceneKeyType;

typedef struct CSceneKey {
    const char* key;
    size_t offset;
    CSceneKeyType type;
    float lo, hi;
    unsigned only;
} CSceneKey;

// The most rows a block's table holds, which sizes the known-key list built from it.
#define CSCENE_MAX_BLOCK_KEYS 64

static bool _key_reads(const CSceneKey* k, unsigned variant) {
    return !k->only || (k->only & variant);
}

// The scalar rows of `keys` that `variant` reads and `obj` carries, into the struct at `base`.
static void apply_keys(const cJSON* obj, const char* what, const CSceneKey* keys, size_t count,
                       unsigned variant, void* base) {
    for (size_t i = 0; i < count; i++) {
        const CSceneKey* k = &keys[i];
        if (k->type == CSCENE_KEY_OWN || !_key_reads(k, variant))
            continue;
        // Through void*, as config_snapshot's _field_ptr does: a char* cast straight to a
        // float* is what a portability checker reads as reinterpreting bytes.
        void* field = (unsigned char*)base + k->offset;
        if (k->type == CSCENE_KEY_BOOL) {
            get_bool(obj, k->key, (bool*)field);
            continue;
        }
        float v = 0.0f;
        if (!_ranged_float(obj, what, k->key, k->lo, k->hi, &v))
            continue;
        if (k->type == CSCENE_KEY_INT)
            *(int*)field = (int)v;
        else
            *(float*)field = v;
    }
}

// Every key of `obj` that no row `variant` reads, warned about.
static void warn_unknown_table_keys(const cJSON* obj, const char* what, const CSceneKey* keys,
                                    size_t count, unsigned variant) {
    const char* known[CSCENE_MAX_BLOCK_KEYS] = {NULL};
    size_t n = 0;
    for (size_t i = 0; i < count; i++)
        if (_key_reads(&keys[i], variant))
            known[n++] = keys[i].key;
    warn_unknown_keys(obj, known, n, what);
}

// `word`'s index in `names`, ignoring case as the file's other enum words do; -1 for none.
static int word_index(const char* word, const char* const* names, int count) {
    for (int i = 0; i < count; i++)
        if (names[i] && strcasecmp(word, names[i]) == 0)
            return i;
    return -1;
}

/*
 * Each object of `obj`'s list `key` handed to `take`, which appends what it accepts to `into`
 * and counts it in `*count`, until `cap` are taken: each checked against `known`, one that is
 * not an object warned and skipped, and any past the cap warned once and dropped. Only what
 * `take` accepted counts against the cap, so an item it refuses costs a later one nothing. A
 * `key` that is there and is not a list is warned too.
 */
typedef void (*ListTake)(const cJSON* item, void* into);

static void take_list(const cJSON* obj, const char* key, const char* what, const char* const* known,
                      size_t known_count, const int* count, int cap, ListTake take, void* into) {
    const cJSON* list = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!list)
        return;
    if (!cJSON_IsArray(list)) {
        log_warn("cscene: %s's %s is not a list; ignored", what, key);
        return;
    }
    const cJSON* item = NULL;
    cJSON_ArrayForEach(item, list) {
        if (!cJSON_IsObject(item)) {
            log_warn("cscene: a %s that is not an object; skipped", what);
            continue;
        }
        if (*count >= cap) {
            log_warn("cscene: more than %d of %s; the rest are ignored", cap, what);
            break;
        }
        warn_unknown_keys(item, known, known_count, what);
        take(item, into);
    }
}

/*
 * rain -- a scene subsystem, top-level for water's reason. Every scalar is one row, with the
 * range outside which the value means nothing to the consumer -- a negative rate, a
 * non-positive time constant, a coverage past full. The keys are the config snapshot's rain
 * keys, so a dumped block pastes into a scene file.
 */
#define RAIN_FLOAT_KEY(k, field, lo, hi) {k, offsetof(Rain, field), CSCENE_KEY_FLOAT, lo, hi, 0}
#define RAIN_INT_KEY(k, field, lo, hi)   {k, offsetof(Rain, field), CSCENE_KEY_INT, lo, hi, 0}
#define RAIN_OWN_KEY(k)                  {k, 0, CSCENE_KEY_OWN, 0.0f, 0.0f, 0}
static const CSceneKey RAIN_KEYS[] = {
    RAIN_OWN_KEY("enabled"),
    RAIN_OWN_KEY("wind"),
    RAIN_OWN_KEY("settled"),
    RAIN_OWN_KEY("dryFor"),
    RAIN_OWN_KEY("drips"),
    {"followSceneWind", offsetof(Rain, follow_scene_wind), CSCENE_KEY_BOOL, 0.0f, 0.0f, 0},
    RAIN_FLOAT_KEY("rate", rate_mmh, 0.0f, 500.0f),
    RAIN_FLOAT_KEY("fallScale", fall_scale, 0.0f, 10.0f),
    RAIN_FLOAT_KEY("wetTime", wet_time, 1e-3f, 1e6f),
    RAIN_FLOAT_KEY("dryTime", dry_time, 1e-3f, 1e6f),
    RAIN_FLOAT_KEY("puddleFillTime", puddle_fill_time, 1e-3f, 1e6f),
    RAIN_FLOAT_KEY("puddleDrainTime", puddle_drain_time, 1e-3f, 1e6f),
    RAIN_FLOAT_KEY("puddleCoverage", puddle_coverage, 0.0f, 1.0f),
    RAIN_FLOAT_KEY("puddleScale", puddle_scale, 0.05f, 1000.0f),
    RAIN_FLOAT_KEY("puddleRelief", puddle_relief, 0.0f, 1.0f),
    RAIN_FLOAT_KEY("rippleStrength", ripple_strength, 0.0f, 10.0f),
    RAIN_FLOAT_KEY("rippleSize", ripple_size, 0.02f, 10.0f),
    RAIN_FLOAT_KEY("wetDarkening", wet_darkening, 0.0f, 2.0f),
    RAIN_FLOAT_KEY("occlusionExtent", occlusion_extent, 1.0f, 4096.0f),
    RAIN_FLOAT_KEY("occlusionSoftness", occlusion_softness, 0.0f, 100.0f),
    RAIN_INT_KEY("streakCount", streak_count, 0.0f, 262144.0f),
    RAIN_FLOAT_KEY("streakRadius", streak_radius, 0.1f, 1000.0f),
    RAIN_FLOAT_KEY("shutter", shutter_s, 1e-4f, 1.0f),
    RAIN_FLOAT_KEY("streakWidth", streak_width, 0.0f, 100.0f),
    RAIN_FLOAT_KEY("streakBrightness", streak_brightness, 0.0f, 1000.0f),
    RAIN_FLOAT_KEY("streakForwardG", streak_forward_g, -0.99f, 0.99f),
    RAIN_FLOAT_KEY("streakGlint", streak_glint, 0.0f, 1.0f),
    RAIN_FLOAT_KEY("streakSheen", streak_sheen, 0.0f, 100.0f),
    RAIN_INT_KEY("splashCount", splash_count, 0.0f, 65536.0f),
    RAIN_FLOAT_KEY("splashRadius", splash_radius, 0.1f, 1000.0f),
    RAIN_FLOAT_KEY("splashAmount", splash_amount, 0.0f, 100.0f),
    RAIN_FLOAT_KEY("splashSize", splash_size, 0.0f, 100.0f),
    RAIN_FLOAT_KEY("mist", mist, 0.0f, 1000.0f),
    RAIN_FLOAT_KEY("mistForwardG", mist_forward_g, -0.99f, 0.99f),
    RAIN_FLOAT_KEY("glassLens", glass_lens, 0.0f, 10.0f),
    RAIN_FLOAT_KEY("glassDropSize", glass_drop_size, 0.1f, 20.0f),
    RAIN_INT_KEY("dripCount", drip_count, 0.0f, 65536.0f),
    RAIN_FLOAT_KEY("dripBrightness", drip_brightness, 0.0f, 1000.0f),
};
#undef RAIN_FLOAT_KEY
#undef RAIN_INT_KEY
#undef RAIN_OWN_KEY
#define RAIN_KEY_COUNT (sizeof(RAIN_KEYS) / sizeof(RAIN_KEYS[0]))
_Static_assert(RAIN_KEY_COUNT <= CSCENE_MAX_BLOCK_KEYS, "rain's keys overflow the known list");

/*
 * rain.drips[] -- the lines water drips from (spec 13.12): {from, to, rate, ground}. `from` is
 * REQUIRED, since a line nobody placed drips from the world origin; `to` defaults to it, which
 * is a single source; `rate` to one drop a second and `ground` to y = 0.
 */
static void parse_drips(Rain* out, const cJSON* rain) {
    static const char* known[] = {"from", "to", "rate", "ground"};
    const cJSON* drips = cJSON_GetObjectItemCaseSensitive(rain, "drips");
    if (!cJSON_IsArray(drips) || cJSON_GetArraySize(drips) == 0)
        return;
    // Every line the file names, so the rain can say how many it has no room for.
    RainDripLine* lines = malloc((size_t)cJSON_GetArraySize(drips) * sizeof(RainDripLine));
    if (!lines) {
        log_error("cscene: out of memory for the rain's drip lines");
        return;
    }
    int count = 0;
    const cJSON* l = NULL;
    cJSON_ArrayForEach(l, drips) {
        if (!cJSON_IsObject(l)) {
            log_warn("cscene: rain drip line that is not an object; skipped");
            continue;
        }
        warn_unknown_keys(l, known, sizeof(known) / sizeof(known[0]), "rain drip line");
        RainDripLine line = {.rate = 1.0f};
        if (!get_vec3(l, "from", line.from)) {
            log_warn("cscene: rain drip line needs from; skipped");
            continue;
        }
        if (!get_vec3(l, "to", line.to))
            glm_vec3_copy(line.from, line.to);
        _ranged_float(l, "rain drip line", "rate", 0.0f, 1e4f, &line.rate);
        get_float(l, "ground", &line.ground);
        lines[count++] = line;
    }
    rain_set_drip_lines(out, lines, count);
    free(lines);
}

static void parse_rain(CetraSceneDesc* d, const cJSON* root) {
    const cJSON* rain = cJSON_GetObjectItemCaseSensitive(root, "rain");
    if (!cJSON_IsObject(rain))
        return;
    CSceneRain* out = &d->rain;
    out->enabled = true; // presence implies on unless "enabled": false
    get_bool(rain, "enabled", &out->enabled);
    rain_init_defaults(&out->rain);
    vec3 wind = GLM_VEC3_ZERO_INIT;
    if (get_vec3(rain, "wind", wind))
        glm_vec3_copy(wind, out->rain.wind);
    apply_keys(rain, "rain", RAIN_KEYS, RAIN_KEY_COUNT, 0, &out->rain);
    out->has_settled = get_bool(rain, "settled", &out->settled);
    _ranged_float(rain, "rain", "dryFor", 0.0f, 1e6f, &out->dry_for);
    parse_drips(&out->rain, rain);
    warn_unknown_table_keys(rain, "rain", RAIN_KEYS, RAIN_KEY_COUNT, 0);
}

/*
 * fire -- the scene's fires (spec 13.14): the simulation's settings, and `fires`, each a GRID
 * fire, a candle's FLAME or a FLIPBOOK over its kind's defaults. Each kind reads its own keys --
 * the table's `only` -- so a key that does nothing on the kind it is written on is warned about
 * as unknown rather than taken silently.
 */
#define FIRE_SYSTEM_KEY(k, field, type, lo, hi) {k, offsetof(FireSystem, field), type, lo, hi, 0}
static const CSceneKey FIRE_SYSTEM_KEYS[] = {
    {"enabled", 0, CSCENE_KEY_OWN, 0.0f, 0.0f, 0},
    {"fires", 0, CSCENE_KEY_OWN, 0.0f, 0.0f, 0},
    FIRE_SYSTEM_KEY("simHz", sim_hz, CSCENE_KEY_FLOAT, 1.0f, 1000.0f),
    FIRE_SYSTEM_KEY("warmup", warmup, CSCENE_KEY_FLOAT, 0.0f, 60.0f),
    FIRE_SYSTEM_KEY("jacobi", jacobi_iterations, CSCENE_KEY_INT, 1.0f, 500.0f),
    FIRE_SYSTEM_KEY("maccormack", maccormack, CSCENE_KEY_BOOL, 0.0f, 0.0f),
};
#undef FIRE_SYSTEM_KEY
#define FIRE_SYSTEM_KEY_COUNT (sizeof(FIRE_SYSTEM_KEYS) / sizeof(FIRE_SYSTEM_KEYS[0]))

#define ON_GRID                                 (1u << FIRE_GRID)
#define ON_FLAME                                (1u << FIRE_FLAME)
#define ON_FLIPBOOK                             (1u << FIRE_FLIPBOOK)
#define FIRE_KEY(k, field, type, lo, hi, kinds) {k, offsetof(Fire, field), type, lo, hi, kinds}
#define FIRE_PARAM(k, field, lo, hi, kinds) \
    FIRE_KEY(k, params.field, CSCENE_KEY_FLOAT, lo, hi, kinds)
#define FIRE_OWN(k, kinds) {k, 0, CSCENE_KEY_OWN, 0.0f, 0.0f, kinds}
static const CSceneKey FIRE_KEYS[] = {
    FIRE_OWN("name", 0),
    FIRE_OWN("kind", 0),
    FIRE_OWN("light", 0),
    FIRE_OWN("lightOffset", 0),
    FIRE_KEY("lightScale", light_scale, CSCENE_KEY_FLOAT, 0.0f, 1000.0f, 0),
    FIRE_OWN("embers", 0),
    FIRE_KEY("enabled", enabled, CSCENE_KEY_BOOL, 0.0f, 0.0f, 0),
    FIRE_OWN("center", ON_GRID | ON_FLAME),
    FIRE_OWN("size", ON_GRID | ON_FLAME),
    FIRE_KEY("cell", grid.cell, CSCENE_KEY_FLOAT, 0.002f, 1.0f, ON_GRID),
    FIRE_KEY("floor", grid.floor, CSCENE_KEY_BOOL, 0.0f, 0.0f, ON_GRID),
    FIRE_OWN("sources", ON_GRID),
    FIRE_OWN("obstacles", ON_GRID),
    FIRE_OWN("draft", ON_GRID),
    FIRE_OWN("flipbook", ON_FLIPBOOK),
    FIRE_OWN("cards", ON_FLIPBOOK),
    FIRE_PARAM("ambient", ambient, 150.0f, 400.0f, ON_GRID | ON_FLAME),
    FIRE_PARAM("temperature", temperature, 600.0f, 4000.0f, ON_GRID | ON_FLAME),
    FIRE_PARAM("reactionRate", reaction_rate, 0.01f, 100.0f, ON_GRID),
    FIRE_PARAM("cooling", cooling, 0.0f, 1e6f, ON_GRID),
    FIRE_PARAM("entrainment", entrainment, 0.0f, 1000.0f, ON_GRID),
    FIRE_PARAM("core", core, 0.0f, 10.0f, ON_GRID),
    FIRE_PARAM("expansion", expansion, 0.0f, 1000.0f, ON_GRID),
    FIRE_PARAM("sootYield", soot_yield, 0.0f, 1000.0f, ON_GRID),
    FIRE_PARAM("sootBurnout", soot_burnout, 0.0f, 1000.0f, ON_GRID),
    FIRE_PARAM("sootBurnoutAt", soot_burnout_at, 300.0f, 4000.0f, ON_GRID),
    FIRE_PARAM("smokeFade", smoke_fade, 0.0f, 1000.0f, ON_GRID),
    FIRE_PARAM("buoyancy", buoyancy, 0.0f, 100.0f, ON_GRID),
    FIRE_PARAM("vorticity", vorticity, 0.0f, 100.0f, ON_GRID),
    FIRE_PARAM("windResponse", wind_response, 0.0f, 10.0f, ON_GRID | ON_FLAME),
    FIRE_PARAM("sootAbsorption", soot_absorption, 0.0f, 1000.0f, ON_GRID | ON_FLAME),
    FIRE_PARAM("smokeAlbedo", smoke_albedo, 0.0f, 0.99f, ON_GRID | ON_FLAME),
    FIRE_PARAM("blueCore", blue_core, 0.0f, 1e7f, ON_GRID | ON_FLAME),
    FIRE_PARAM("adaptation", adaptation, 0.0f, 1.0f, ON_GRID | ON_FLAME),
    FIRE_PARAM("brightness", brightness, 0.0f, 1000.0f, 0),
    FIRE_PARAM("flameSoot", flame_soot, 0.0f, 1000.0f, ON_FLAME),
    FIRE_PARAM("flicker", flicker, 0.0f, 10.0f, ON_FLAME),
};
#undef FIRE_KEY
#undef FIRE_PARAM
#undef FIRE_OWN
#undef ON_GRID
#undef ON_FLAME
#undef ON_FLIPBOOK
#define FIRE_KEY_COUNT (sizeof(FIRE_KEYS) / sizeof(FIRE_KEYS[0]))
_Static_assert(FIRE_KEY_COUNT <= CSCENE_MAX_BLOCK_KEYS, "fire's keys overflow the known list");

static const char* const FIRE_KIND_NAMES[FIRE_KIND_COUNT] = {
    [FIRE_GRID] = "grid", [FIRE_FLAME] = "flame", [FIRE_FLIPBOOK] = "flipbook"};
static const char* const FIRE_SHAPE_NAMES[] = {
    [FIRE_SHAPE_BOX] = "box", [FIRE_SHAPE_SPHERE] = "sphere", [FIRE_SHAPE_CAPSULE] = "capsule"};

// A box authored as {min, max}, both or neither: a fire's obstacles and its chimney's draft.
static bool get_min_max(const cJSON* obj, vec3 min, vec3 max) {
    vec3 lo = {0.0f, 0.0f, 0.0f}, hi = {0.0f, 0.0f, 0.0f};
    if (!get_vec3(obj, "min", lo) || !get_vec3(obj, "max", hi))
        return false;
    glm_vec3_copy(lo, min);
    glm_vec3_copy(hi, max);
    return true;
}

// sources[] on a GRID fire: {shape, center, halfSize, from, to, radius, coverage, lift}. A box
// takes center and halfSize, a sphere center and radius, a capsule from, to and radius.
static void take_fire_source(const cJSON* s, void* into) {
    FireGrid* grid = into;
    FireSource src = fire_source_default();
    char shape[16] = "box";
    copy_string(shape, sizeof(shape), cJSON_GetObjectItemCaseSensitive(s, "shape"));
    const int shape_index =
        word_index(shape, FIRE_SHAPE_NAMES, (int)(sizeof(FIRE_SHAPE_NAMES) / sizeof(char*)));
    if (shape_index < 0) {
        log_warn("cscene: fire source shape '%s' is not box, sphere or capsule; skipped", shape);
        return;
    }
    src.shape = (FireShape)shape_index;
    if (src.shape == FIRE_SHAPE_CAPSULE) {
        if (!get_vec3(s, "from", src.a) || !get_vec3(s, "to", src.b)) {
            log_warn("cscene: a capsule fire source needs from and to; skipped");
            return;
        }
    } else if (!get_vec3(s, "center", src.a)) {
        log_warn("cscene: a fire source needs a center; skipped");
        return;
    }
    if (src.shape == FIRE_SHAPE_BOX)
        get_vec3(s, "halfSize", src.b);
    _ranged_float(s, "fire source", "radius", 0.0f, 100.0f, &src.radius);
    _ranged_float(s, "fire source", "coverage", 0.0f, 1.0f, &src.coverage);
    _ranged_float(s, "fire source", "lift", -100.0f, 100.0f, &src.lift);
    grid->sources[grid->source_count++] = src;
}

// cards[] on a FLIPBOOK fire: {base, size, phase}, each a quad standing on its bottom centre;
// no size is the size the sheet's frames were made at.
static void take_fire_card(const cJSON* c, void* into) {
    FireCards* cards = into;
    FireCard card = {.base = {0.0f, 0.0f, 0.0f}, .size = {0.0f, 0.0f}, .phase = 0.0f};
    if (!get_vec3(c, "base", card.base)) {
        log_warn("cscene: a fire card needs a base; skipped");
        return;
    }
    get_floats(c, "size", card.size, 2);
    _ranged_float(c, "fire card", "phase", 0.0f, 1.0f, &card.phase);
    cards->list[cards->count++] = card;
}

// obstacles[] on a GRID fire: {min, max}, the solids no flow passes.
static void take_fire_obstacle(const cJSON* o, void* into) {
    FireGrid* grid = into;
    FireBox* b = &grid->obstacles[grid->obstacle_count];
    if (get_min_max(o, b->min, b->max))
        grid->obstacle_count++;
    else
        log_warn("cscene: a fire obstacle needs min and max; skipped");
}

// A GRID fire's box (center, size), its sources, its obstacles, and its chimney ({min, max,
// speed}, the flue and how fast it draws).
static void parse_fire_grid(Fire* fire, const cJSON* f) {
    static const char* source_known[] = {"shape", "center", "halfSize", "from",
                                         "to",    "radius", "coverage", "lift"};
    static const char* box_known[] = {"min", "max"};
    FireGrid* grid = &fire->grid;
    get_vec3(f, "center", grid->center);
    get_vec3(f, "size", grid->size);
    take_list(f, "sources", "fire source", source_known,
              sizeof(source_known) / sizeof(source_known[0]), &grid->source_count, FIRE_MAX_SOURCES,
              take_fire_source, grid);
    take_list(f, "obstacles", "fire obstacle", box_known, 2, &grid->obstacle_count,
              FIRE_MAX_OBSTACLES, take_fire_obstacle, grid);
    const cJSON* draft = cJSON_GetObjectItemCaseSensitive(f, "draft");
    if (cJSON_IsObject(draft)) {
        static const char* draft_known[] = {"min", "max", "speed"};
        warn_unknown_keys(draft, draft_known, 3, "fire draft");
        if (get_min_max(draft, grid->draft.min, grid->draft.max))
            _ranged_float(draft, "fire draft", "speed", 0.0f, 100.0f, &grid->draft_speed);
        else
            log_warn("cscene: fire '%s' draft needs min and max; ignored", fire->name);
    }
}

// A FLAME's wick tip (center), and its width and height (size: two numbers, or three as a GRID
// fire's box is written, the third unread).
static void parse_fire_flame(FireFlame* flame, const cJSON* f) {
    get_vec3(f, "center", flame->wick);
    vec3 size = {flame->width, flame->height, 0.0f};
    if (get_floats(f, "size", size, 2) || get_vec3(f, "size", size)) {
        flame->width = size[0];
        flame->height = size[1];
    } else if (cJSON_GetObjectItemCaseSensitive(f, "size")) {
        log_warn("cscene: a flame's size is two or three numbers; ignored");
    }
}

static void parse_fire(CetraSceneDesc* d, const cJSON* root) {
    static const char* card_known[] = {"base", "size", "phase"};
    const cJSON* block = cJSON_GetObjectItemCaseSensitive(root, "fire");
    if (!cJSON_IsObject(block))
        return;
    CSceneFire* out = &d->fire;
    out->enabled = true;
    get_bool(block, "enabled", &out->enabled);
    FireSystem* fs = &out->system;
    fire_system_init(fs);
    apply_keys(block, "fire", FIRE_SYSTEM_KEYS, FIRE_SYSTEM_KEY_COUNT, 0, fs);
    warn_unknown_table_keys(block, "fire", FIRE_SYSTEM_KEYS, FIRE_SYSTEM_KEY_COUNT, 0);

    const cJSON* fires = cJSON_GetObjectItemCaseSensitive(block, "fires");
    const cJSON* f = NULL;
    cJSON_ArrayForEach(f, fires) {
        if (!cJSON_IsObject(f)) {
            log_warn("cscene: a fire that is not an object; skipped");
            continue;
        }
        char kind[16] = "grid";
        copy_string(kind, sizeof(kind), cJSON_GetObjectItemCaseSensitive(f, "kind"));
        const int kind_index = word_index(kind, FIRE_KIND_NAMES, FIRE_KIND_COUNT);
        if (kind_index < 0) {
            log_warn("cscene: fire kind '%s' is not grid, flame or flipbook; skipped", kind);
            continue;
        }
        char name[32] = "fire";
        copy_string(name, sizeof(name), cJSON_GetObjectItemCaseSensitive(f, "name"));
        const int index = fs->count;
        Fire* fire = fire_system_add(fs, (FireKind)kind_index, name);
        if (!fire)
            break;
        const unsigned variant = 1u << fire->kind;
        warn_unknown_table_keys(f, "fire", FIRE_KEYS, FIRE_KEY_COUNT, variant);
        apply_keys(f, "fire", FIRE_KEYS, FIRE_KEY_COUNT, variant, fire);
        get_vec3(f, "lightOffset", fire->light_offset);
        copy_string(out->light[index], CSCENE_MAX_NAME,
                    cJSON_GetObjectItemCaseSensitive(f, "light"));
        copy_string(out->embers[index], CSCENE_MAX_NAME,
                    cJSON_GetObjectItemCaseSensitive(f, "embers"));
        switch (fire->kind) {
            case FIRE_GRID:
                parse_fire_grid(fire, f);
                break;
            case FIRE_FLAME:
                parse_fire_flame(&fire->flame, f);
                break;
            case FIRE_FLIPBOOK:
                if (!cJSON_IsString(cJSON_GetObjectItemCaseSensitive(f, "flipbook")))
                    log_warn("cscene: flipbook fire '%s' names no flipbook; it draws nothing",
                             fire->name);
                copy_string(out->flipbook[index], CSCENE_MAX_PATH,
                            cJSON_GetObjectItemCaseSensitive(f, "flipbook"));
                take_list(f, "cards", "fire card", card_known,
                          sizeof(card_known) / sizeof(card_known[0]), &fire->cards.count,
                          FIRE_MAX_CARDS, take_fire_card, &fire->cards);
                break;
            case FIRE_KIND_COUNT:
                break;
        }
    }
}

/*
 * layers[] on a material -- an ordered set of surfaces the splat map blends between.
 *
 * ORDERED, which is why it is an array and not an object: layer 0 is the one that takes
 * whatever weight the splat map does not spend, so the position in the list is data. An
 * object keyed by name would leave that to whatever order the JSON happened to be written
 * in.
 *
 * `albedo` is REQUIRED and `surface` is not. A layer with no albedo has nothing to
 * contribute and defaulting it would blend in a grey nobody asked for, where a layer with
 * no surface map is an ordinary matte one -- the shader falls back to the geometric normal
 * and the scalar roughness, which is a real material rather than a placeholder.
 */
static void parse_material_layers(CSceneMaterialOverride* out, const cJSON* m) {
    static const char* const known[] = {"albedo", "surface", "uvScale"};
    const cJSON* layers = cJSON_GetObjectItemCaseSensitive(m, "layers");
    if (!layers)
        return;
    if (!cJSON_IsArray(layers)) {
        log_warn("cscene: material '%s' key 'layers' is not an array; ignored", out->material);
        return;
    }
    const cJSON* l = NULL;
    cJSON_ArrayForEach(l, layers) {
        if (out->layer_count >= CSCENE_MAX_MATERIAL_LAYERS) {
            log_warn("cscene: material '%s' has more than %d layers; extras ignored", out->material,
                     CSCENE_MAX_MATERIAL_LAYERS);
            break;
        }
        if (!cJSON_IsObject(l)) {
            log_warn("cscene: material '%s' has a layer that is not an object; skipped",
                     out->material);
            continue;
        }
        warn_unknown_keys(l, known, sizeof(known) / sizeof(known[0]), "material layer");

        CSceneMaterialLayer* out_layer = &out->layers[out->layer_count];
        memset(out_layer, 0, sizeof(*out_layer));
        // An albedo-less layer KEEPS ITS SLOT. The splat channel is the slot
        // index, so skipping one here renumbers every layer after it and the
        // scene renders the wrong ground in every channel -- silently, since the
        // result is still a plausible terrain.
        const cJSON* albedo = cJSON_GetObjectItemCaseSensitive(l, "albedo");
        if (!cJSON_IsString(albedo) || !albedo->valuestring || !albedo->valuestring[0])
            log_warn("cscene: material '%s' layer %d has no albedo; it keeps its slot and "
                     "renders its fallback",
                     out->material, out->layer_count);
        else
            copy_string(out_layer->albedo, CSCENE_MAX_PATH, albedo);
        const cJSON* surface = cJSON_GetObjectItemCaseSensitive(l, "surface");
        if (cJSON_IsString(surface) && surface->valuestring && surface->valuestring[0])
            copy_string(out_layer->surface, CSCENE_MAX_PATH, surface);
        out_layer->has_uv_scale = get_float(l, "uvScale", &out_layer->uv_scale);
        out->layer_count++;
    }
}

/*
 * Roads over a layered material (spec 11.68).
 *
 * `points`, `width` and `layer` are REQUIRED and `feather` is not: a road with
 * no course, no width, or no surface to be made of is not a road, where one
 * with no shoulder is a hard-edged road -- visibly wrong rather than quietly
 * so, which is the fog volume's argument for the same split.
 *
 * The course is the only array-of-arrays in the format, so it is walked by hand
 * rather than through get_floats. A malformed vertex refuses the WHOLE road: a
 * road missing a point in the middle is a different road, not a shorter one,
 * and it would run somewhere nobody authored.
 */
static void parse_material_roads(CSceneMaterialOverride* out, const cJSON* m) {
    static const char* const known[] = {"points", "width", "feather", "layer"};
    const cJSON* roads = cJSON_GetObjectItemCaseSensitive(m, "roads");
    if (!roads)
        return;
    if (!cJSON_IsArray(roads)) {
        log_warn("cscene: material '%s' key 'roads' is not an array; ignored", out->material);
        return;
    }
    const cJSON* r = NULL;
    // The AUTHORED index, which is what a reader can find in the file --
    // road_count is the SLOT being filled and does not advance on a refusal,
    // so using it names every rejected road 'road 0'.
    int idx = -1;
    cJSON_ArrayForEach(r, roads) {
        idx++;
        if (out->road_count >= MATERIAL_MAX_ROADS) {
            log_warn("cscene: material '%s' has more than %d roads; extras ignored", out->material,
                     MATERIAL_MAX_ROADS);
            break;
        }
        if (!cJSON_IsObject(r)) {
            log_warn("cscene: material '%s' has a road that is not an object; skipped",
                     out->material);
            continue;
        }
        const cJSON* road = r;
        warn_unknown_keys(road, known, sizeof(known) / sizeof(known[0]), "material road");

        MaterialRoad* out_road = &out->roads[out->road_count];
        memset(out_road, 0, sizeof(*out_road));

        const cJSON* points = cJSON_GetObjectItemCaseSensitive(road, "points");
        if (!cJSON_IsArray(points)) {
            log_warn("cscene: material '%s' road %d has no points array; skipped", out->material,
                     idx);
            continue;
        }
        bool malformed = false;
        const cJSON* pt = NULL;
        cJSON_ArrayForEach(pt, points) {
            if (out_road->point_count >= MATERIAL_MAX_ROAD_POINTS) {
                log_warn("cscene: material '%s' road %d has more than %d points; skipped",
                         out->material, idx, MATERIAL_MAX_ROAD_POINTS);
                malformed = true;
                break;
            }
            const cJSON* px = cJSON_GetArrayItem(pt, 0);
            const cJSON* pz = cJSON_GetArrayItem(pt, 1);
            if (!cJSON_IsArray(pt) || cJSON_GetArraySize(pt) != 2 || !cJSON_IsNumber(px) ||
                !cJSON_IsNumber(pz)) {
                log_warn("cscene: material '%s' road %d has a point that is not a 2-array of "
                         "numbers; skipped",
                         out->material, idx);
                malformed = true;
                break;
            }
            out_road->points[out_road->point_count][0] = (float)px->valuedouble;
            out_road->points[out_road->point_count][1] = (float)pz->valuedouble;
            out_road->point_count++;
        }
        if (malformed)
            continue;
        if (out_road->point_count < 2) {
            log_warn("cscene: material '%s' road %d needs at least two points; skipped",
                     out->material, idx);
            continue;
        }
        if (!get_float(road, "width", &out_road->width) || out_road->width <= 0.0f) {
            log_warn("cscene: material '%s' road %d needs a positive width; skipped", out->material,
                     idx);
            continue;
        }
        // The memset above supplies the feather default. A negative one is
        // refused rather than clamped, for the reason a non-positive width is:
        // both describe a road nobody can have meant, and silently repairing
        // one while refusing the other is an asymmetry with no argument behind
        // it.
        if (get_float(road, "feather", &out_road->feather) && out_road->feather < 0.0f) {
            log_warn("cscene: material '%s' road %d has a negative feather; skipped", out->material,
                     idx);
            continue;
        }
        // Read as a float because the format has no integer reader, and a
        // fractional index is refused here -- the apply-side warning only sees
        // the truncated value, so "layer": 2.7 would otherwise become 2 in
        // silence.
        float layer = -1.0f;
        if (!get_float(road, "layer", &layer) || layer < 0.0f || layer != floorf(layer)) {
            log_warn("cscene: material '%s' road %d needs a whole layer index; skipped",
                     out->material, idx);
            continue;
        }
        out_road->layer = (int)layer;
        out->road_count++;
    }
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

        out->layer_count = 0;
        parse_material_layers(out, m);
        out->road_count = 0;
        parse_material_roads(out, m);

        // Compound like sss: four numbers describing one rectangle. Skipped by
        // the generic walk below, which would otherwise warn on the 4-array as
        // "neither a number nor a 3-array" -- so a malformed one is warned here
        // or it would be dropped silently.
        out->has_splat_domain = get_floats(m, "splatDomain", out->splat_domain, 4);
        if (!out->has_splat_domain && cJSON_GetObjectItemCaseSensitive(m, "splatDomain"))
            log_warn("cscene: material '%s' key 'splatDomain' is not a 4-array of numbers; "
                     "ignored",
                     out->material);

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
            if (strcmp(p->string, "sss") == 0 || strcmp(p->string, "layers") == 0 ||
                strcmp(p->string, "splatDomain") == 0 || strcmp(p->string, "roads") == 0)
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

        if (!out->has_sss && out->layer_count == 0 && out->road_count == 0 &&
            out->param_count == 0 && out->texture_count == 0) {
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
    parse_rain(d, root);
    parse_fire(d, root);
    parse_fog_volumes(d, root);
    parse_probes(d, root);
    parse_occluders(d, root);
    parse_decals(d, root);
    parse_materials(d, root);
    parse_camera(d, root);
    cJSON_Delete(root);

    log_info("cscene '%s': model '%s', %d light(s), %d override(s), %d material(s)", path,
             d->model_path[0] ? d->model_path : "-", d->light_count, d->light_override_count,
             d->material_count);

    // Resolve stored paths against the scene file's directory (dirname idiom
    // as in import.c effective_texture_dir) so consumers get usable paths.
    char dir[CSCENE_MAX_PATH];
    const char* slash = path_last_sep(path);
    if (slash) {
        snprintf(dir, sizeof(dir), "%.*s", (int)(slash - path), path);
    } else {
        snprintf(dir, sizeof(dir), ".");
    }
    resolve_in_place(d->model_path, CSCENE_MAX_PATH, dir);
    resolve_in_place(d->env_hdr, CSCENE_MAX_PATH, dir);
    // An IES profile resolves like the HDR above and NOT like a material
    // texture: it is not a texture, never goes through the pool, and so has no
    // second resolver that would win. Empty paths pass through untouched.
    for (int i = 0; i < d->light_count; i++)
        resolve_in_place(d->lights[i].ies_path, CSCENE_MAX_PATH, dir);
    // A .cube is the same kind of thing as the two above and for the same
    // reason: not a texture, never through the pool, no second resolver.
    resolve_in_place(d->lut_path, CSCENE_MAX_PATH, dir);
    // A flipbook's sidecar is the same again; the sheet it names resolves beside the sidecar.
    for (int i = 0; i < d->fire.system.count; i++)
        resolve_in_place(d->fire.flipbook[i], CSCENE_MAX_PATH, dir);
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
