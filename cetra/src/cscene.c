#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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
    if (!path[0] || path[0] == '/')
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

static bool get_vec3(const cJSON* obj, const char* key, float out[3]) {
    const cJSON* arr = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) != 3)
        return false;
    for (int i = 0; i < 3; i++) {
        const cJSON* v = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsNumber(v))
            return false;
        out[i] = (float)v->valuedouble;
    }
    return true;
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
        copy_string(out->name, CSCENE_MAX_NAME, cJSON_GetObjectItemCaseSensitive(l, "name"));
        // v1 renders point lights only; refuse rather than silently coerce.
        const cJSON* type = cJSON_GetObjectItemCaseSensitive(l, "type");
        if (cJSON_IsString(type) && strcasecmp(type->valuestring, "point") != 0) {
            log_warn("cscene: light '%s' type '%s' unsupported (v1 is point-only); skipped",
                     out->name, type->valuestring);
            continue;
        }
        if (!get_vec3(l, "position", out->position)) {
            log_warn("cscene: light '%s' missing position; skipped", out->name);
            continue;
        }
        if (!get_vec3(l, "color", out->color)) {
            out->color[0] = out->color[1] = out->color[2] = 1.0f;
        }
        if (!get_float(l, "intensity", &out->intensity))
            out->intensity = 1.0f;
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

    const cJSON* bloom = cJSON_GetObjectItemCaseSensitive(post, "bloom");
    if (cJSON_IsObject(bloom)) {
        d->has_bloom_enabled = get_bool(bloom, "enabled", &d->bloom_enabled);
        d->has_bloom_strength = get_float(bloom, "strength", &d->bloom_strength);
        d->has_bloom_threshold = get_float(bloom, "threshold", &d->bloom_threshold);
    }
    const cJSON* fog = cJSON_GetObjectItemCaseSensitive(post, "fog");
    if (cJSON_IsObject(fog)) {
        get_bool(fog, "enabled", &d->fog_enabled);
        d->has_fog_density = get_float(fog, "density", &d->fog_density);
        d->has_fog_anisotropy = get_float(fog, "anisotropy", &d->fog_anisotropy);
    }
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
    }
    out->has_drift = get_vec3(dust, "drift", out->drift);
    out->has_damping = get_float(dust, "damping", &out->damping);
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

        const cJSON* sss = cJSON_GetObjectItemCaseSensitive(m, "sss");
        out->has_sss = cJSON_IsObject(sss) && get_vec3(sss, "color", out->sss_color) &&
                       get_float(sss, "radius", &out->sss_radius);

        out->has_wind_response = get_float(m, "windResponse", &out->wind_response);

        if (!out->has_sss && !out->has_wind_response) {
            log_warn("cscene: material '%s' has no usable sss or windResponse; skipped",
                     out->material);
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
    return d;
}

void cscene_free(CetraSceneDesc* desc) {
    free(desc);
}
