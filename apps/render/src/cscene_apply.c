#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <cglm/cglm.h>

#include "cetra/cscene.h"
#include "cetra/engine.h"
#include "cetra/ext/cwalk.h"
#include "cetra/ext/log.h"
#include "cetra/light.h"
#include "cetra/material.h"
#include "cetra/noise.h"
#include "cetra/particle_emitter.h"
#include "cetra/particle_module.h"
#include "cetra/particle_renderer.h"
#include "cetra/particle_sim.h"
#include "cetra/particle_system.h"
#include "cetra/postfx.h"
#include "cetra/program.h"
#include "cetra/scene.h"
#include "cetra/texture.h"
#include "cetra/util.h"
#include "cetra/water.h"
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
    if (args->render_scale == 0.0f && cscn->has_render_scale) {
        args->render_scale = cscn->render_scale;
        log_info("cscene: render scale %.2f authored (TAAU; headless also needs "
                 "--taa --headless-jitter)",
                 cscn->render_scale);
    }
    if (args->flare < 0.0f && cscn->has_flare) {
        args->flare = cscn->flare;
    }
    if (args->chromatic < 0.0f && cscn->has_chromatic_aberration) {
        args->chromatic = cscn->chromatic_aberration;
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
    if (args->fog_near < 0.0f && cscn->has_fog_near) {
        args->fog_near = cscn->fog_near;
    }
    if (args->fog_far < 0.0f && cscn->has_fog_far) {
        args->fog_far = cscn->fog_far;
    }
    if (args->fog_depth_dist < 0.0f && cscn->has_fog_depth_dist) {
        args->fog_depth_dist = cscn->fog_depth_dist;
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

void apply_cscene_ambient(Scene* scene, const CetraSceneDesc* cscn) {
    if (!scene || !cscn || !cscn->has_ambient)
        return;
    glm_vec3_copy((float*)cscn->ambient, scene->ambient_radiance);
    printf("Scene file: ambient %.3f %.3f %.3f cd/m2 (no-IBL fill)\n", cscn->ambient[0],
           cscn->ambient[1], cscn->ambient[2]);
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
        set_light_intensity_units(light, sl->intensity, sl->units);
        set_light_type(light, cscene_light_type(sl->type));
        switch (sl->type) {
        case CSCENE_LIGHT_AREA:
            set_light_direction(light, (float*)sl->direction);
            set_light_size(light, sl->size[0], sl->size[1]);
            if (sl->has_up)
                set_light_up(light, (float*)sl->up);
            printf("Scene file light '%s' (area %.2fx%.2f, radiance %.2f%s)\n", light->name,
                   sl->size[0], sl->size[1], sl->intensity, sl->cast_shadows ? ", shadows" : "");
            break;
        case CSCENE_LIGHT_DIRECTIONAL:
            set_light_direction(light, (float*)sl->direction);
            printf("Scene file light '%s' (directional, intensity %.2f%s)\n", light->name,
                   sl->intensity, sl->cast_shadows ? ", shadows" : "");
            break;
        case CSCENE_LIGHT_SPOT:
            set_light_direction(light, (float*)sl->direction);
            // The engine stores cutoffs as cosines of the half-angles; authors
            // write degrees (see spec 6.2).
            set_light_cutoff(light, cosf(glm_rad(sl->cone[0])), cosf(glm_rad(sl->cone[1])));
            printf("Scene file light '%s' (spot, cone %.1f/%.1f deg%s)\n", light->name,
                   sl->cone[0], sl->cone[1], sl->cast_shadows ? ", shadows" : "");
            break;
        default: // CSCENE_LIGHT_POINT
            printf("Scene file light '%s' (point, intensity %.2f%s)\n", light->name, sl->intensity,
                   sl->cast_shadows ? ", shadows" : "");
            break;
        }
        // Range bounds the punctual falloff; absent means keep create_light()'s
        // default. The old attenuation triple is parsed and warned about in
        // cscene.c but deliberately not applied here -- storing a value the
        // shaders no longer read is how a dead knob keeps looking live.
        if (sl->has_range)
            set_light_range(light, sl->range);
        // Type-independent: every light type now honours cast_shadows (spec 9.8
        // gave point and area lights maps), so it is set once rather than per
        // branch. The switch above only carries what genuinely varies by type.
        if (sl->cast_shadows)
            set_light_cast_shadows(light, true);

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
            // In whatever unit the light it overrides is shown in, so an
            // override on a lumens lamp reads as lumens too. An override block
            // carries no intensity_unit of its own; inheriting is the only
            // reading that does not silently change what the number means.
            set_light_intensity_units(light, ov->intensity, light_display_units(light));
        }
        if (ov->has_cast_shadows) {
            set_light_cast_shadows(light, ov->cast_shadows);
            printf("Scene file: light '%s' cast_shadows %s\n", ov->name,
                   ov->cast_shadows ? "on" : "off");
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

    // Per-material opt-in (windResponse, windMode) is not handled here: both
    // are plain Material fields, so they ride the shared parameter table in
    // apply_cscene_material_overrides like every other authored scalar. This
    // function owns only the wind FIELD, which is scene state and has no
    // material to hang off. The mask bounds that pin a cloth's top and free its
    // hem stay per-mesh, supplied at draw time from each mesh's AABB.
}

/*
 * The scene file's water surface. Attached here so `--water` and its family become
 * OVERRIDES of an authored surface rather than the only way to get one -- the CLI block
 * in render.c runs after this and writes over whatever it was given on the command line.
 *
 * Nothing is defaulted here: an absent field keeps create_water's own value, so a block
 * that authors a level alone still gets clear-water extinction and a Gerstner train,
 * and the two places that know those numbers stay one place.
 */
void apply_cscene_water(Scene* scene, const CetraSceneDesc* cscn) {
    if (!scene || !cscn || !cscn->water.enabled)
        return;
    const CSceneWater* w = &cscn->water;
    Water* water = create_water();
    if (!water)
        return;
    if (w->has_level)
        water->level = w->level;
    if (w->has_extent)
        water->extent = w->extent;
    if (w->has_waves)
        water->wave_model = w->waves_fft ? WATER_WAVES_FFT : WATER_WAVES_GERSTNER;
    if (w->has_wavelength)
        water->wavelength = w->wavelength;
    if (w->has_amplitude)
        water->amplitude = w->amplitude;
    if (w->has_steepness)
        water->steepness = w->steepness;
    if (w->has_spread)
        water->spread = w->spread;
    if (w->has_wind_dir)
        glm_vec2_copy((float*)w->wind_dir, water->wind_dir);
    if (w->has_roughness)
        water->roughness = w->roughness;
    if (w->has_ior)
        water->ior = w->ior;
    if (w->has_absorption)
        glm_vec3_copy((float*)w->absorption, water->absorption);
    if (w->has_scatter)
        glm_vec3_copy((float*)w->scatter, water->scatter);
    if (w->has_caustics)
        water->caustics = w->caustics;
    if (w->has_shore_coverage)
        water->shore_coverage = w->shore_coverage;
    if (w->has_far_lod)
        water->far_lod = w->far_lod;
    scene->water = water;
    printf("Scene file: water level %.2f, extent %.1f, %s waves\n", (double)water->level,
           (double)water->extent,
           water->wave_model == WATER_WAVES_FFT ? "spectral" : "gerstner");
}

/*
 * The scene file's local fog volumes (spec 11.39). No CLI counterpart: a box needs a
 * place and a size, which is more than a flag can carry, so authoring is the only way in.
 * Printed because a volume that lands somewhere the camera never enters is invisible and
 * indistinguishable from one that failed to parse.
 */
void apply_cscene_fog_volumes(Scene* scene, const CetraSceneDesc* cscn) {
    if (!scene || !cscn)
        return;
    for (int i = 0; i < cscn->fog_volume_count; i++) {
        const CSceneFogVolume* v = &cscn->fog_volumes[i];
        FogVolume out = {0};
        memcpy(out.center, v->center, sizeof(out.center));
        memcpy(out.half_extent, v->extent, sizeof(out.half_extent));
        out.density = v->density;
        out.feather = v->feather;
        memcpy(out.tint, v->tint, sizeof(out.tint));
        if (add_fog_volume_to_scene(scene, &out) < 0)
            break;
        printf("Scene file: fog volume at (%.2f %.2f %.2f) half-extent (%.2f %.2f %.2f) "
               "density %.3f feather %.2f\n",
               (double)out.center[0], (double)out.center[1], (double)out.center[2],
               (double)out.half_extent[0], (double)out.half_extent[1],
               (double)out.half_extent[2], (double)out.density, (double)out.feather);
    }
}

/*
 * The material vocabulary lives in material.c (MATERIAL_PARAMS), shared with
 * the GUI editor so the two cannot disagree about what a name means or which
 * properties are safe to set. The parser records keys generically and never
 * learns their meaning; this file only resolves them and applies the one thing
 * the engine cannot do for itself -- turning an authored path into a texture.
 */
void apply_cscene_material_overrides(Scene* scene, const CetraSceneDesc* cscn) {
    if (!scene || !cscn)
        return;
    for (int k = 0; k < cscn->material_count; k++) {
        const CSceneMaterialOverride* mo = &cscn->materials[k];
        if (mo->param_count == 0 && mo->texture_count == 0)
            continue; // sss-only entries belong to configure_sss_materials

        // Resolve and report the vocabulary once per override, not once per
        // matching material -- and before the match, so an unknown key is still
        // reported when the material name is also wrong.
        const MaterialParam* slots[CSCENE_MAX_MATERIAL_PARAMS];
        int usable = 0;
        for (int p = 0; p < mo->param_count; p++) {
            const CSceneMaterialParam* prm = &mo->params[p];
            const MaterialParam* slot = material_param_find(prm->key);
            if (!slot) {
                fprintf(stderr, "Warning: material '%s': unknown key '%s'\n", mo->material,
                        prm->key);
            } else if (slot->type == MATERIAL_PARAM_TEXTURE) {
                fprintf(stderr, "Warning: material '%s': key '%s' wants a path\n", mo->material,
                        prm->key);
                slot = NULL;
            } else if ((slot->type == MATERIAL_PARAM_COLOR) != (prm->components == 3)) {
                fprintf(stderr, "Warning: material '%s': key '%s' wants %s\n", mo->material,
                        prm->key,
                        slot->type == MATERIAL_PARAM_COLOR ? "3 numbers" : "one number");
                slot = NULL;
            }
            slots[p] = slot;
            if (slot)
                usable++;
        }

        // Textures resolve and LOAD here rather than per matching material: the
        // pool would dedup a repeat load anyway, and doing it once means a bad
        // path is reported once instead of once per material that shares it.
        //
        // LINEAR, never sRGB. Every texture reachable from here carries data
        // rather than colour, and an sRGB decode would silently bend the values.
        struct {
            const MaterialParam* slot;
            Texture* tex;
        } textures[CSCENE_MAX_MATERIAL_TEXTURES] = {0};
        for (int t = 0; t < mo->texture_count; t++) {
            const MaterialParam* slot = material_param_find(mo->textures[t].key);
            if (!slot || slot->type != MATERIAL_PARAM_TEXTURE) {
                fprintf(stderr, "Warning: material '%s': %s '%s'\n", mo->material,
                        slot ? "not a texture key:" : "unknown texture key",
                        mo->textures[t].key);
                continue;
            }
            Texture* tex = load_texture_path_into_pool(scene->tex_pool, mo->textures[t].path, false);
            if (!tex) {
                fprintf(stderr, "Warning: material '%s': cannot load texture '%s'\n", mo->material,
                        mo->textures[t].path);
                continue;
            }
            textures[t].slot = slot;
            textures[t].tex = tex;
            usable++;
        }

        if (usable == 0)
            continue;

        // Scene state, not per-material: the layer indices are assigned when the
        // array next rebuilds, and until then each material reads its fallback.
        if (mo->texture_count > 0)
            scene->mask_array_dirty = true;

        int tagged = 0;
        for (size_t i = 0; i < scene->material_count; i++) {
            Material* m = scene->materials[i];
            if (!m || !m->name || strcmp(m->name, mo->material) != 0)
                continue;
            for (int p = 0; p < mo->param_count; p++) {
                if (slots[p])
                    material_param_set(m, slots[p], mo->params[p].value);
            }
            for (int t = 0; t < mo->texture_count; t++) {
                if (textures[t].slot)
                    textures[t].slot->set_tex(m, textures[t].tex);
            }
            tagged++;
        }
        printf("Scene file: %d override(s) on material '%s' (%d material(s))\n", usable,
               mo->material, tagged);
        if (tagged == 0)
            fprintf(stderr, "Warning: material '%s' not found in scene\n", mo->material);
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
