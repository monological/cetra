#include <math.h>
#include <stdio.h>
#include <string.h>

#include <cglm/cglm.h>

#include "cetra/cscene.h"
#include "cetra/engine.h"
#include "cetra/ext/cwalk.h"
#include "cetra/ext/log.h"
#include "cetra/light.h"
#include "cetra/noise.h"
#include "cetra/particle_emitter.h"
#include "cetra/particle_module.h"
#include "cetra/particle_renderer.h"
#include "cetra/particle_sim.h"
#include "cetra/particle_system.h"
#include "cetra/postfx.h"
#include "cetra/program.h"
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
    if (cscn->has_env_sun) {
        if (args->sun_elevation < -900.0f)
            args->sun_elevation = cscn->env_sun_elevation_deg;
        if (args->sun_azimuth < -900.0f)
            args->sun_azimuth = cscn->env_sun_azimuth_deg;
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
        // Say so. Setting an exposure also switches adaptation off downstream,
        // which surprises anyone who then finds the GUI's auto-exposure box
        // unticked in a scene whose file never mentions auto-exposure.
        if (!cscn->has_auto_exposure)
            log_info("cscene: exposure %.3f authored -- frame pinned, "
                     "auto-exposure off (set \"auto_exposure\": true to keep adapting)",
                     cscn->exposure);
    }
    // An explicit key breaks that coupling in both directions: a scene can keep
    // adapting from an authored starting exposure, or pin the frame without
    // naming a value. Guarded like every other field here, so the CLI still wins
    // -- the precedence this whole function implements is CLI > scene > default.
    if (cscn->has_camera_exposure && args->aperture <= 0.0f) {
        args->aperture = cscn->aperture;
        args->shutter_speed = cscn->shutter_speed;
        args->iso = cscn->iso;
    }
    if (cscn->has_auto_exposure && args->auto_exposure_override < 0) {
        args->auto_exposure_override = cscn->auto_exposure ? 1 : 0;
        log_info("cscene: auto-exposure %s (authored)", cscn->auto_exposure ? "on" : "off");
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
        set_light_original_position(light, (float*)sl->position);
        set_light_color(light, (float*)sl->color);
        set_light_intensity(light, sl->intensity);
        // Range bounds the punctual falloff; absent means keep create_light()'s
        // default. The old attenuation triple is parsed and warned about in
        // cscene.c but deliberately not applied here -- storing a value the
        // shaders no longer read is how a dead knob keeps looking live.
        if (sl->has_range)
            set_light_range(light, sl->range);
        // Type-independent: every light type now honours cast_shadows (spec 9.8
        // gave point and area lights maps), so it is set once rather than per
        // branch. The switch below only carries what genuinely varies by type.
        if (sl->cast_shadows)
            set_light_cast_shadows(light, true);

        switch (sl->type) {
        case CSCENE_LIGHT_AREA:
            set_light_type(light, LIGHT_AREA);
            set_light_direction(light, (float*)sl->direction);
            set_light_size(light, sl->size[0], sl->size[1]);
            if (sl->has_up)
                set_light_up(light, (float*)sl->up);
            printf("Scene file light '%s' (area %.2fx%.2f, radiance %.2f%s)\n", light->name,
                   sl->size[0], sl->size[1], sl->intensity, sl->cast_shadows ? ", shadows" : "");
            break;
        case CSCENE_LIGHT_DIRECTIONAL:
            set_light_type(light, LIGHT_DIRECTIONAL);
            set_light_direction(light, (float*)sl->direction);
            printf("Scene file light '%s' (directional, intensity %.2f%s)\n", light->name,
                   sl->intensity, sl->cast_shadows ? ", shadows" : "");
            break;
        case CSCENE_LIGHT_SPOT:
            set_light_type(light, LIGHT_SPOT);
            set_light_direction(light, (float*)sl->direction);
            // The engine stores cutoffs as cosines of the half-angles; authors
            // write degrees (see spec 6.2).
            set_light_cutoff(light, cosf(glm_rad(sl->cone[0])), cosf(glm_rad(sl->cone[1])));
            printf("Scene file light '%s' (spot, cone %.1f/%.1f deg%s)\n", light->name,
                   sl->cone[0], sl->cone[1], sl->cast_shadows ? ", shadows" : "");
            break;
        default: // CSCENE_LIGHT_POINT
            set_light_type(light, LIGHT_POINT);
            printf("Scene file light '%s' (point, intensity %.2f%s)\n", light->name, sl->intensity,
                   sl->cast_shadows ? ", shadows" : "");
            break;
        }
        add_light_to_scene(scene, light);

        SceneNode* light_node = create_node();
        set_node_light(light_node, light);
        set_node_name(light_node, light->name);
        add_child_node(scene->root_node, light_node);
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

void apply_cscene_dust(Engine* engine, Scene* scene, const CetraSceneDesc* cscn, vec3 center,
                       float radius) {
    if (!engine || !scene || !scene->root_node || !cscn || !cscn->dust.enabled)
        return;
    const CSceneDust* d = &cscn->dust;

    // Defaults reproduce the historical ambient-dust recipe; the .cscn overrides
    // only what it authors (like apply_cscene_wind over create_wind's defaults).
    float spawn_rate = 110.0f;
    float life_min = 15.0f, life_max = 30.0f;
    float size_min = 0.0015f, size_max = 0.006f;
    vec4 color = {0.4f, 0.4f, 0.4f, 0.35f};
    float color_jitter = 0.04f;
    float curl_scale = 0.3f, curl_strength = 0.06f, curl_timescale = 0.05f;
    vec3 drift = {0.0f, 0.004f, 0.0f};
    float damping = 0.99f;

    if (d->has_spawn_rate)
        spawn_rate = d->spawn_rate;
    if (d->has_lifetime) {
        life_min = d->lifetime[0];
        life_max = d->lifetime[1];
    }
    if (d->has_size) {
        size_min = d->size[0];
        size_max = d->size[1];
    }
    if (d->has_color)
        glm_vec4_copy((float*)d->color, color);
    if (d->has_color_jitter)
        color_jitter = d->color_jitter;
    if (d->has_curl) {
        curl_scale = d->curl[0];
        curl_strength = d->curl[1];
        curl_timescale = d->curl[2];
    }
    if (d->has_drift)
        glm_vec3_copy((float*)d->drift, drift);
    if (d->has_damping)
        damping = d->damping;

    noise_seed(20240720u);
    ShaderProgram* prog = create_particle_program();
    add_shader_program_to_engine(engine, prog);

    ParticleSystem* sys = create_particle_system("window_dust");
    particle_system_set_backend(sys, create_tf_particle_sim_backend());

    // Spawn box: the middle of the scene, a bit flatter than wide. Capacity must
    // clear spawn_rate * max_lifetime; +10% and a small floor leave margin.
    float ext = radius * 0.6f;
    vec3 lo = {center[0] - ext, center[1] - ext * 0.6f, center[2] - ext};
    vec3 hi = {center[0] + ext, center[1] + ext * 0.6f, center[2] + ext};
    size_t capacity = (size_t)(spawn_rate * life_max * 1.1f) + 64;

    ParticleEmitter* em = create_particle_emitter("dust", capacity);
    particle_emitter_set_renderer(em, create_billboard_particle_renderer(prog));
    particle_emitter_add_module(em, particle_module_spawn_rate(spawn_rate));
    particle_emitter_add_module(em, particle_module_init_box_location(lo, hi));
    particle_emitter_add_module(em, particle_module_init_lifetime(life_min, life_max));
    particle_emitter_add_module(em, particle_module_init_size(size_min, size_max));
    particle_emitter_add_module(em, particle_module_init_color(color, color_jitter));
    particle_emitter_add_module(
        em, particle_module_update_curl_noise(curl_scale, curl_strength, curl_timescale));
    particle_emitter_add_module(em, particle_module_update_drift(drift));
    particle_emitter_add_module(em, particle_module_update_integrate(damping));
    particle_system_add_emitter(sys, em);
    add_particle_system_to_scene(scene, sys); // scene owns it (freed in free_scene)

    SceneNode* node = create_node();
    set_node_name(node, "window_dust");
    set_node_particle_system(node, sys);
    add_child_node(scene->root_node, node);
    printf("Scene file: dust spawnRate=%.0f, curl.strength=%.3f (capacity %zu)\n", spawn_rate,
           curl_strength, capacity);
}
