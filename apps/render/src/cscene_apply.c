#include <math.h>
#include <stdio.h>
#include <string.h>

#include <cglm/cglm.h>

#include "cetra/cscene.h"
#include "cetra/ext/cwalk.h"
#include "cetra/light.h"
#include "cetra/postfx.h"
#include "cetra/scene.h"
#include "cetra/util.h"
#include "cetra/wind.h"

#include "cscene_apply.h"

int cscene_setup(RenderArgs* args, CetraSceneDesc** out_cscn) {
    CetraSceneDesc* cscn = NULL;
    *out_cscn = NULL;
    if (args->model_path && cscene_path_is_scene(args->model_path)) {
        cscn = cscene_load(args->model_path);
        if (!cscn || !cscn->model_path[0]) {
            fprintf(stderr, "Error: scene file '%s' unreadable or lists no models\n",
                    args->model_path);
            cscene_free(cscn);
            return -1;
        }
        // The desc outlives args (freed at the end of main), so its resolved
        // path buffers back these pointers directly.
        args->model_path = cscn->model_path;
    } else if (args->model_path && !args->no_scene_file) {
        // Convenience: a bare model with a .cscn beside it picks it up.
        // cwalk swaps the basename's extension (and appends when there is
        // none) without tripping over dots in directory names.
        char probe_path[CSCENE_MAX_PATH];
        cwk_path_change_extension(args->model_path, "cscn", probe_path, sizeof(probe_path));
        if (path_exists(probe_path)) {
            cscn = cscene_load(probe_path);
        }
    }
    if (!cscn) {
        return 0;
    }
    if (args->no_scene_file) {
        // Explicit .cscn input with --no-scene-file: load the model it
        // references but skip every look field.
        printf("Scene file look skipped (--no-scene-file)\n");
        *out_cscn = cscn;
        return 0;
    }
    *out_cscn = cscn;

    // Scene files describe authored worlds: camera and light positions are in
    // the file's coordinates, so the viewer's auto-recenter would silently
    // shift the model out from under them.
    args->no_recenter = 1;

    if (!args->hdr_path && !args->sky) {
        if (cscn->env_mode == CSCENE_ENV_HDR && cscn->env_hdr[0]) {
            args->hdr_path = cscn->env_hdr;
        } else if (cscn->env_mode == CSCENE_ENV_SKY) {
            args->sky = 1;
        }
    }
    if (cscn->env_probe_scene && !args->probe) {
        args->probe = 1;
        args->probe_scene = 1;
    }
    if (args->ibl_intensity < 0.0f && cscn->has_env_intensity) {
        args->ibl_intensity = cscn->env_intensity;
    }
    if (args->tonemap_mode == 0) {
        switch (cscn->tonemap) {
            case CSCENE_TONEMAP_AGX:
                args->tonemap_mode = POSTFX_TONEMAP_AGX;
                break;
            case CSCENE_TONEMAP_ACES:
                args->tonemap_mode = POSTFX_TONEMAP_ACES;
                break;
            case CSCENE_TONEMAP_NEUTRAL:
                args->tonemap_mode = POSTFX_TONEMAP_NEUTRAL;
                break;
            case CSCENE_TONEMAP_NONE:
                break;
        }
    }
    if (args->exposure <= 0.0f && cscn->has_exposure) {
        args->exposure = cscn->exposure;
    }
    if (args->bloom_enable < 0 && cscn->has_bloom_enabled) {
        args->bloom_enable = cscn->bloom_enabled ? 1 : 0;
    }
    if (args->bloom_strength < 0.0f && cscn->has_bloom_strength) {
        args->bloom_strength = cscn->bloom_strength;
    }
    if (args->bloom_threshold < 0.0f && cscn->has_bloom_threshold) {
        args->bloom_threshold = cscn->bloom_threshold;
    }
    if (cscn->fog_enabled) {
        args->fog = 1;
    }
    if (args->fog_density <= 0.0f && cscn->has_fog_density) {
        args->fog_density = cscn->fog_density;
    }
    if (args->fog_anisotropy < -900.0f && cscn->has_fog_anisotropy) {
        args->fog_anisotropy = cscn->fog_anisotropy;
    }
    if (!args->cam_eye_set && !args->cam_target_set && cscn->has_camera) {
        glm_vec3_copy(cscn->cam_eye, args->cam_eye);
        glm_vec3_copy(cscn->cam_target, args->cam_target);
        args->cam_eye_set = 1;
        args->cam_target_set = 1;
    }
    if (args->fov_deg <= 0.0f && cscn->has_cam_fov) {
        args->fov_deg = cscn->cam_fov;
    }
    printf("Scene file: %d light(s), %d material override(s), env=%s, camera=%s\n",
           cscn->light_count, cscn->material_count,
           cscn->env_mode == CSCENE_ENV_HDR   ? "hdr"
           : cscn->env_mode == CSCENE_ENV_SKY ? "sky"
                                              : "none",
           cscn->has_camera ? "yes" : "no");
    return 0;
}

void add_cscene_lights(Scene* scene, const CetraSceneDesc* cscn) {
    if (!scene || !cscn)
        return;
    for (int i = 0; i < cscn->light_count; i++) {
        const CSceneLight* sl = &cscn->lights[i];
        Light* light = create_light();
        if (!light)
            continue;
        set_light_name(light, sl->name[0] ? sl->name : "cscn_light");
        set_light_type(light, LIGHT_POINT);
        set_light_original_position(light, (float*)sl->position);
        set_light_color(light, (float*)sl->color);
        set_light_intensity(light, sl->intensity);
        add_light_to_scene(scene, light);

        SceneNode* light_node = create_node();
        set_node_light(light_node, light);
        set_node_name(light_node, light->name);
        add_child_node(scene->root_node, light_node);
        printf("Scene file light '%s' (point, intensity %.2f)\n", light->name, sl->intensity);
    }
}

void apply_cscene_light_overrides(Scene* scene, const CetraSceneDesc* cscn, float scene_radius) {
    if (!scene || !cscn)
        return;
    // Names are unique in authored scenes, so first match is the match.
    for (int k = 0; k < cscn->light_override_count; k++) {
        const CSceneLightOverride* ov = &cscn->light_overrides[k];
        Light* light = find_light_by_name(scene, ov->name);
        if (!light) {
            fprintf(stderr, "Warning: scene-file light override '%s' matches no light\n", ov->name);
            continue;
        }
        if (ov->has_size_from_angle) {
            float s = tanf(ov->size_from_angle) * scene_radius;
            set_light_size(light, s, s);
            printf("Scene file: light '%s' penumbra size %.3f (angle %.3f rad)\n", ov->name, s,
                   ov->size_from_angle);
        }
        if (ov->has_intensity) {
            set_light_intensity(light, ov->intensity);
        }
    }
}

void apply_cscene_wind(Scene* scene, const CetraSceneDesc* cscn) {
    if (!scene || !cscn)
        return;

    // 1. The scene wind field -- a first-class scene object (like the sky).
    //    Absent fields keep create_wind's gentle-draft defaults.
    if (cscn->wind_enabled) {
        Wind* wind = create_wind("SceneWind");
        if (wind) {
            if (cscn->has_wind_direction)
                glm_vec3_copy((float*)cscn->wind_direction, wind->direction);
            if (cscn->has_wind_strength)
                wind->strength = cscn->wind_strength;
            if (cscn->has_wind_speed)
                wind->speed = cscn->wind_speed;
            if (cscn->has_wind_gust_frequency)
                wind->gust_frequency = cscn->wind_gust_frequency;
            if (cscn->has_wind_gust_amount)
                wind->gust_amount = cscn->wind_gust_amount;
            if (cscn->has_wind_turbulence)
                wind->turbulence = cscn->wind_turbulence;
            set_scene_wind(scene, wind);
            printf("Scene file: wind dir=(%.2f, %.2f, %.2f), strength %.3f\n", wind->direction[0],
                   wind->direction[1], wind->direction[2], wind->strength);
        }
    }

    // 2. Per-material opt-in: mark responsive materials by name (keyed against
    //    the scene's flat material registry, like configure_sss_materials). The
    //    mask bounds are supplied per-mesh at draw time from each mesh's AABB.
    for (int k = 0; k < cscn->material_count; k++) {
        const CSceneMaterialOverride* mo = &cscn->materials[k];
        if (!mo->has_wind_response)
            continue;
        int tagged = 0;
        for (size_t i = 0; i < scene->material_count; i++) {
            Material* m = scene->materials[i];
            if (m && m->name && strcmp(m->name, mo->material) == 0) {
                m->wind_response = mo->wind_response;
                tagged++;
            }
        }
        printf("Scene file: wind response %.2f on material '%s' (%d material(s))\n",
               mo->wind_response, mo->material, tagged);
        if (tagged == 0)
            fprintf(stderr, "Warning: wind material '%s' not found in scene\n", mo->material);
    }
}
