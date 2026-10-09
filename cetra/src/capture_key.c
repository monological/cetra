#include "capture_key.h"

#include <GL/glew.h>

#include "build_digest.h" // generated: CETRA_BUILD_DIGEST
#include "config_snapshot.h"
#include "cook.h"
#include "draw_list.h"
#include "engine.h"
#include "ibl.h"
#include "ies.h"
#include "light.h"
#include "material.h"
#include "mesh.h"
#include "program.h"
#include "rain.h"
#include "scene.h"
#include "shader_hook.h"
#include "texture.h"
#include "wind.h"

// A texture slot by what it holds; the key refused when a texture cannot say what that is.
static void _fold_texture(CookKey* key, const Texture* tex) {
    if (tex && tex->content_key == 0)
        cook_key_refuse(key);
    cook_key_u64(key, tex ? tex->content_key : 0);
}

// A material as a capture draws it: every row of MATERIAL_PARAMS, the table that is its settings,
// every texture slot by content, its hook's source and parameters, its program, and the drive a
// fire gives its emission.
static void _fold_material(CookKey* key, const Material* mat) {
    for (size_t p = 0; p < MATERIAL_PARAM_COUNT; p++) {
        const MaterialParam* param = &MATERIAL_PARAMS[p];
        if (param->type == MATERIAL_PARAM_TEXTURE)
            continue;
        float values[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        material_param_get(mat, param, values);
        cook_key_f32s(key, values, material_param_components(param));
    }
    Texture* textures[MATERIAL_TEXTURE_SLOTS];
    material_textures(mat, textures);
    for (size_t t = 0; t < MATERIAL_TEXTURE_SLOTS; t++)
        _fold_texture(key, textures[t]);
    const ShaderHook* hook = shader_hook_live(mat->shader_hook);
    cook_key_str(key, hook && hook->surface ? hook->surface : "");
    cook_key_str(key, hook && hook->offset ? hook->offset : "");
    cook_key_i32(key, mat->shader_params.count);
    for (int i = 0; i < mat->shader_params.count; i++) {
        cook_key_str(key, mat->shader_params.list[i].name);
        cook_key_f32s(key, mat->shader_params.list[i].value, 4);
    }
    cook_key_str(key,
                 mat->shader_program && mat->shader_program->name ? mat->shader_program->name : "");
    cook_key_f32(key, mat->emissive_drive);
}

// A light as a capture sees it, where the rest hook holds it: where it stands and faces, what it
// gives off and the body that gives it, and its shadow. Not its name or the engine's slots for
// it, nor `units`, since `intensity` is always in the canonical one.
static void _fold_light(CookKey* key, const Light* light, const IesLibrary* ies) {
    cook_key_i32(key, (int32_t)light->type);
    cook_key_f32s(key, light->global_position, 3);
    cook_key_f32s(key, light->direction, 3);
    cook_key_f32s(key, light->up, 3);
    cook_key_f32s(key, light->color, 3);
    cook_key_f32s(key, light->ambient, 3);
    const float terms[] = {light->intensity,  light->specular,      light->range,
                           light->cutOff,     light->outerCutOff,   light->size[0],
                           light->size[1],    light->source_radius, light->source_length,
                           light->shadow_near};
    cook_key_f32s(key, terms, (int)(sizeof(terms) / sizeof(terms[0])));
    cook_key_bool(key, light->cast_shadows);
    cook_key_bool(key, light->shadow_cache);
    cook_key_bool(key, light->shadow_follow);
    // An IES profile by its file and what was read from it.
    const IesProfile* p =
        ies && light->ies_profile >= 0 ? ies_library_at(ies, light->ies_profile) : NULL;
    cook_key_str(key, p && p->path ? p->path : "");
    if (p) {
        cook_key_i32(key, p->v_taps);
        cook_key_i32(key, p->h_taps);
        const float shape[] = {p->span, p->v_lo, p->v_hi, p->peak_cd, p->support_deg};
        cook_key_f32s(key, shape, (int)(sizeof(shape) / sizeof(shape[0])));
    }
}

// What every capture of the scene reads wherever its box is: the engine's own source, the driver,
// the settings, the rain's state, the wind and the environment's source.
static void _fold_scene(CookKey* key, Engine* engine, Scene* scene) {
    cook_key_u64(key, CETRA_BUILD_DIGEST);
    // The driver does the arithmetic, so its answer is the driver's.
    const GLenum strings[] = {GL_VENDOR, GL_RENDERER, GL_VERSION};
    for (size_t s = 0; s < sizeof(strings) / sizeof(strings[0]); s++) {
        const char* text = (const char*)glGetString(strings[s]);
        cook_key_str(key, text ? text : "");
    }
    config_snapshot_fold(engine, scene, key);
    const Rain* rain = scene->rain;
    cook_key_bool(key, rain != NULL);
    if (rain) {
        cook_key_f32(key, rain->wetness);
        cook_key_f32(key, rain->puddle_level);
        cook_key_f32s(key, rain->travel, 3);
    }
    const Wind* wind = scene->wind;
    cook_key_bool(key, wind != NULL);
    if (wind) {
        cook_key_i32(key, (int32_t)wind->type);
        cook_key_f32s(key, wind->direction, 3);
        const float terms[] = {wind->strength,    wind->speed,      wind->gust_frequency,
                               wind->gust_amount, wind->turbulence, wind->phase_variation,
                               wind->air_speed};
        cook_key_f32s(key, terms, (int)(sizeof(terms) / sizeof(terms[0])));
    }
    const IBLResources* ibl = scene->ibl;
    cook_key_str(key, ibl && ibl->hdr_filepath ? ibl->hdr_filepath : "");
    cook_key_f32(key, ibl ? ibl->intensity : 0.0f);
    cook_key_bool(key, ibl && ibl->reflect_fog);
    cook_key_f32s(key, scene->world_origin, 3);
}

/*
 * _fold_scene's answer, folded once a frame and kept: every capture of a frame's bursts is taken
 * with the scene at rest and the same for each, and a sweep or a column is keyed twice -- as it
 * begins and once it is done -- so a frame of fetches would otherwise fold the whole settings table
 * a few hundred times. A key is folded again in the frame its capture finishes, so a change between
 * the two is still seen. One scene's at a time, which is the only one a frame captures.
 */
static struct {
    const Scene* scene;
    size_t frame;
    bool valid;
    uint64_t hash;
} _scene_memo;

static void _fold_scene_once(CookKey* key, Engine* engine, Scene* scene) {
    if (!_scene_memo.valid || _scene_memo.scene != scene ||
        _scene_memo.frame != engine->total_frames) {
        CookKey part = cook_key("capture-scene/1");
        _fold_scene(&part, engine, scene);
        _scene_memo.scene = scene;
        _scene_memo.frame = engine->total_frames;
        _scene_memo.valid = part.valid;
        _scene_memo.hash = part.hash;
    }
    if (!_scene_memo.valid)
        cook_key_refuse(key);
    cook_key_u64(key, _scene_memo.hash);
}

void scene_capture_fold(Engine* engine, Scene* scene, const AABB* box, CookKey* key) {
    if (!key->valid)
        return;
    _fold_scene_once(key, engine, scene);

    // Every light that gives off anything and reaches the box -- a directional reaches every box
    // -- in the list's order. One that emits nothing lights nothing wherever it stands, which is
    // what keeps a flashlight held off at rest from keying the capture by where the player is.
    for (size_t l = 0; l < scene->light_count; l++) {
        const Light* light = scene->lights[l];
        const float reach = light_cull_radius(light);
        if (light->intensity <= 0.0f || (light->type != LIGHT_DIRECTIONAL &&
                                         aabb_dist_sq(box, light->global_position) > reach * reach))
            continue;
        _fold_light(key, light, scene->ies_library);
    }

    // Every decal that meets the box: where it stands and how it projects, and its images.
    for (int d = 0; d < scene->decal_count; d++) {
        const Decal* decal = &scene->decals[d];
        const float reach = glm_vec3_norm((float*)decal->half_extent);
        if (aabb_dist_sq(box, decal->position) > reach * reach)
            continue;
        config_snapshot_fold_decal(decal, key);
        _fold_texture(key, decal->albedo_tex);
        _fold_texture(key, decal->surface_tex);
    }

    // Every item a capture of the box takes, in the list's order, which is the graph's.
    const DrawList* list = scene->draw_list;
    for (size_t i = 0; list && i < list->count && key->valid; ++i) {
        const DrawItem* item = &list->items[i];
        if (!draw_item_captured_in(item, scene->wind, box))
            continue;
        const Mesh* mesh = item->mesh;
        const uint64_t content = mesh_content_key(item->mesh);
        if (content == 0)
            cook_key_refuse(key);
        cook_key_u64(key, content);
        cook_key_f32s(key, (const float*)item->node->global_transform, 16);
        cook_key_u32(key, item->lane);
        cook_key_u32(key, item->flags);
        cook_key_bool(key, item->capture_always);
        cook_key_f32(key, mesh->lod_scale);
        cook_key_f32(key, mesh->wind_y0);
        cook_key_f32(key, mesh->wind_y1);
        _fold_material(key, mesh->material);
    }
}
