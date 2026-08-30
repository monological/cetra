#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <cglm/cglm.h>
#include <GL/glew.h>

#include "common.h"
#include "ext/log.h"
#include "layers_vt.h"
#include "material.h"
#include "program.h"

#define MP(field, ty, lo, hi)                                                                      \
    .offset = offsetof(Material, field), .type = ty, .min = lo, .max = hi

// Names, not indices: the vegetation modes also redefine what UV1 MEANS on a
// material, which is not a thing to discover by dragging an integer.
static const char* const WIND_MODE_NAMES[] = {"cloth", "vegetation branch", "vegetation leaf"};

// Order IS the stored value, and 0 must stay "light" so a calloc'd material opts in.
static const char* const EMISSIVE_LIGHT_NAMES[] = {"light", "off"};

// Order IS the stored value, and here 0 must stay "off" -- the opposite polarity
// from EMISSIVE_LIGHT_NAMES above, because the shadow-map exclusion is the
// default every existing material relies on. Reversed, every masked surface in
// the corpus would start casting.
static const char* const FOLIAGE_SHADOW_NAMES[] = {"off", "on"};

// Same polarity as FOLIAGE_SHADOW_NAMES and for the same reason: being an
// occlusion proxy is an opt-in claim about the surface, and a calloc'd
// material must not make it.
static const char* const OCCLUDER_NAMES[] = {"off", "on"};

// Group order here is the order an editor shows them in, and it is deliberate:
// the handful of properties that describe every surface come first, and the
// ones that only matter to a material that opted into a feature follow. A flat
// list of all of them is technically complete and useless to tune against.
//
// Rows of a group must stay CONSECUTIVE -- an editor walks runs, so a row
// separated from its group would open a second section under the same name.
const MaterialParam MATERIAL_PARAMS[] = {
    {"albedo", "Base", MP(albedo, MATERIAL_PARAM_COLOR, 0.0f, 1.0f)},
    {"roughness", "Base", MP(roughness, MATERIAL_PARAM_FLOAT, 0.0f, 1.0f)},
    {"metallic", "Base", MP(metallic, MATERIAL_PARAM_FLOAT, 0.0f, 1.0f)},
    {"ao", "Base", MP(ao, MATERIAL_PARAM_FLOAT, 0.0f, 1.0f)},
    {"opacity", "Base", MP(opacity, MATERIAL_PARAM_FLOAT, 0.0f, 1.0f)},

    // Capped below 1: the anisotropic NDF divides by the cross-strand roughness,
    // which a strength of 1 drives to zero.
    {"anisotropy", "Anisotropy", MP(anisotropy, MATERIAL_PARAM_FLOAT, 0.0f, 0.95f)},
    {"anisotropyMap", "Anisotropy", .type = MATERIAL_PARAM_TEXTURE,
     .offset = offsetof(Material, anisotropy_tex), .set_tex = set_material_anisotropy_tex},

    {"emissive", "Emissive", MP(emissive, MATERIAL_PARAM_COLOR, 0.0f, 1.0f)},
    {"emissiveStrength", "Emissive", MP(emissive_strength, MATERIAL_PARAM_FLOAT, 0.0f, 20.0f)},
    {"emissiveLight", "Emissive", .offset = offsetof(Material, emissive_light),
     .type = MATERIAL_PARAM_INT, .enum_labels = EMISSIVE_LIGHT_NAMES,
     .enum_count = (int)(sizeof(EMISSIVE_LIGHT_NAMES) / sizeof(EMISSIVE_LIGHT_NAMES[0]))},

    {"clearcoat", "Coat and sheen", MP(clearcoat, MATERIAL_PARAM_FLOAT, 0.0f, 1.0f)},
    {"clearcoatRoughness", "Coat and sheen",
     MP(clearcoat_roughness, MATERIAL_PARAM_FLOAT, 0.0f, 1.0f)},
    {"sheenColor", "Coat and sheen", MP(sheen_color_factor, MATERIAL_PARAM_COLOR, 0.0f, 1.0f)},
    {"sheenRoughness", "Coat and sheen",
     MP(sheen_roughness_factor, MATERIAL_PARAM_FLOAT, 0.0f, 1.0f)},
    // -1 means the KHR extension was absent, which is NOT the same as 0 (an
    // explicit zero weight), so the range has to reach below zero to express it.
    {"specularFactor", "Coat and sheen", MP(specular_factor, MATERIAL_PARAM_FLOAT, -1.0f, 2.0f)},
    {"specularColor", "Coat and sheen",
     MP(specular_color_factor, MATERIAL_PARAM_COLOR, 0.0f, 1.0f)},

    // Subsurface was absent from this table for its whole life, which made the SSS path the
    // one shading feature the editor could not switch off -- so "every knob to zero, no
    // change" said nothing about it. subsurfaceColor tints the back-light transmission term
    // in pbr_frag; the SCATTER profile (colour and radius) is postfx state rather than
    // material state, so it is still not reachable from here.
    {"subsurface", "Subsurface", MP(subsurface, MATERIAL_PARAM_FLOAT, 0.0f, 1.0f)},
    {"subsurfaceColor", "Subsurface", MP(subsurface_color, MATERIAL_PARAM_COLOR, 0.0f, 1.0f)},

    {"ior", "Transmission", MP(ior, MATERIAL_PARAM_FLOAT, 1.0f, 3.0f)},
    {"transmission", "Transmission", MP(transmission, MATERIAL_PARAM_FLOAT, 0.0f, 1.0f)},
    {"thickness", "Transmission", MP(thickness, MATERIAL_PARAM_FLOAT, 0.0f, 5.0f)},
    {"attenuationColor", "Transmission",
     MP(attenuation_color, MATERIAL_PARAM_COLOR, 0.0f, 1.0f)},
    // Reaches 0 because that is how this field spells glTF's default of infinity;
    // an editor dragging to the bottom of the range turns absorption off rather
    // than driving the extinction to a divide by zero.
    {"attenuationDistance", "Transmission",
     MP(attenuation_distance, MATERIAL_PARAM_FLOAT, 0.0f, 20.0f)},
    {"filmThickness", "Transmission", MP(filmThickness, MATERIAL_PARAM_FLOAT, 0.0f, 1000.0f)},

    {"curvatureScale", "Skin", MP(curvature_scale, MATERIAL_PARAM_FLOAT, 0.0f, 2.0f)},

    {"normalScale", "Maps and wind", MP(normalScale, MATERIAL_PARAM_FLOAT, 0.0f, 2.0f)},
    {"aoStrength", "Maps and wind", MP(aoStrength, MATERIAL_PARAM_FLOAT, 0.0f, 1.0f)},
    {"parallaxScale", "Maps and wind", MP(parallax_scale, MATERIAL_PARAM_FLOAT, 0.0f, 0.1f)},
    {"windResponse", "Maps and wind", MP(wind_response, MATERIAL_PARAM_FLOAT, 0.0f, 1.0f)},
    {"shoreWetness", "Maps and wind", MP(shore_wetness, MATERIAL_PARAM_FLOAT, 0.0f, 1.0f)},
    {"windMode", "Maps and wind", .offset = offsetof(Material, wind_mode),
     .type = MATERIAL_PARAM_INT, .enum_labels = WIND_MODE_NAMES,
     .enum_count = (int)(sizeof(WIND_MODE_NAMES) / sizeof(WIND_MODE_NAMES[0]))},

    // The layer SET is an ordered array and cannot ride this table -- the scene
    // file parses it directly. What is here is the rest of a layered material:
    // the weights that select between the layers, and the two knobs that decide
    // how the blend and the projection behave.
    {"splat", "Layers", .type = MATERIAL_PARAM_TEXTURE,
     .offset = offsetof(Material, splat_tex), .set_tex = set_material_splat_tex},
    // 0 is a plain weighted average and reachable on purpose: it is what the
    // height blend has to be measured against.
    {"layerBlend", "Layers", MP(layer_blend_sharpness, MATERIAL_PARAM_FLOAT, 0.0f, 4.0f)},
    // Below 1 the projections smear into each other over most of the surface;
    // the useful range starts where one axis begins to dominate.
    {"layerTriplanar", "Layers",
     MP(layer_triplanar_sharpness, MATERIAL_PARAM_FLOAT, 1.0f, 16.0f)},

    // Its own group rather than "Base": this says nothing about how the surface
    // shades, only whether the shadow map may see it. Inert unless the material
    // is ALPHA_MASK with a positive alphaCutoff and an albedo texture.
    {"foliageShadows", "Shadows", .offset = offsetof(Material, foliage_shadows),
     .type = MATERIAL_PARAM_INT, .enum_labels = FOLIAGE_SHADOW_NAMES,
     .enum_count = (int)(sizeof(FOLIAGE_SHADOW_NAMES) / sizeof(FOLIAGE_SHADOW_NAMES[0]))},

    // Says the mesh's AABB may be rasterised as an occlusion proxy (spec
    // 11.98). Its own group for foliageShadows' reason: nothing about how the
    // surface shades. Guarded four ways at classification -- opaque lane, not
    // alpha-masked, not skinned, not displaced -- so every mechanically
    // checkable way the claim could be false is inert; what remains is the
    // author's interior contract, which the occlusion probe checks.
    {"occluder", "Occlusion", .offset = offsetof(Material, occluder),
     .type = MATERIAL_PARAM_INT, .enum_labels = OCCLUDER_NAMES,
     .enum_count = (int)(sizeof(OCCLUDER_NAMES) / sizeof(OCCLUDER_NAMES[0]))},
};

#undef MP

const size_t MATERIAL_PARAM_COUNT = sizeof(MATERIAL_PARAMS) / sizeof(MATERIAL_PARAMS[0]);

void material_emissive_factor(const Material* material, vec3 out) {
    glm_vec3_zero(out);
    if (!material)
        return;
    glm_vec3_scale((float*)material->emissive, material->emissive_strength, out);
    if (material->emissive_tex && glm_vec3_norm2((float*)material->emissive) < 1e-8f)
        glm_vec3_fill(out, material->emissive_strength);
}

const MaterialParam* material_param_find(const char* key) {
    if (!key)
        return NULL;
    for (size_t i = 0; i < MATERIAL_PARAM_COUNT; i++) {
        if (strcmp(MATERIAL_PARAMS[i].key, key) == 0)
            return &MATERIAL_PARAMS[i];
    }
    return NULL;
}

// The void* hop is not decoration: cppcheck's invalidPointerCast rejects a
// direct char* -> float* cast. offsetof already guarantees the alignment.
//
// Two helpers rather than one that casts constness away, because a single
// const-taking version makes cppcheck's constParameterPointer fire on the
// setter -- it cannot see the write through the void*, and that check is not
// suppressed here. memcpy rather than glm_vec3_copy for the same reason: cglm
// takes a non-const vec3, so copying through it would put the cast back.
static const void* material_param_field_const(const Material* material,
                                              const MaterialParam* param) {
    return (const void*)((const char*)material + param->offset);
}

static void* material_param_field(Material* material, const MaterialParam* param) {
    return (void*)((char*)material + param->offset);
}

void material_param_get(const Material* material, const MaterialParam* param, float* values) {
    if (!material || !param || !values || param->type == MATERIAL_PARAM_TEXTURE)
        return;
    const void* field = material_param_field_const(material, param);
    switch (param->type) {
    case MATERIAL_PARAM_COLOR:
        memcpy(values, field, sizeof(vec3));
        break;
    case MATERIAL_PARAM_FLOAT:
        values[0] = *(const float*)field;
        break;
    case MATERIAL_PARAM_INT:
        values[0] = (float)*(const int*)field;
        break;
    case MATERIAL_PARAM_TEXTURE:
        break; // unreachable; guarded above
    }
}

const Texture* material_param_texture(const Material* material, const MaterialParam* param) {
    if (!material || !param || param->type != MATERIAL_PARAM_TEXTURE)
        return NULL;
    const Texture* const* slot = material_param_field_const(material, param);
    return *slot;
}

void material_param_set(Material* material, const MaterialParam* param, const float* values) {
    if (!material || !param || !values || param->type == MATERIAL_PARAM_TEXTURE)
        return;
    void* field = material_param_field(material, param);
    switch (param->type) {
    case MATERIAL_PARAM_COLOR:
        memcpy(field, values, sizeof(vec3));
        break;
    case MATERIAL_PARAM_FLOAT:
        *(float*)field = values[0];
        break;
    case MATERIAL_PARAM_INT:
        *(int*)field = (int)values[0];
        break;
    case MATERIAL_PARAM_TEXTURE:
        break; // unreachable; guarded above
    }
}

// See Mesh.id: creation order, because a sort keyed on the pointer would order
// draws by allocator address and differ run to run.
static unsigned g_next_material_id = 1;

/*
 * Every field of a fresh Material that is not zero. `static const` + a whole-
 * struct copy, matching WATER_DEFAULT_WIND_SEA in water.c and for its stated
 * reasons -- a macro cannot carry a `//` on the field it explains, and a
 * compound literal has automatic storage where the name reads like a constant.
 *
 * THE POINT IS WHAT IS ABSENT. This function used to assign all seventy-one
 * fields by hand, and nothing enforced that the list was complete -- so adding a
 * field and forgetting a line gave a malloc'd material an indeterminate value.
 * That is not hypothetical: it is the bug 11.60 shipped. An uninitialised
 * layer_count armed the shader's layered path on random materials and
 * free_material released junk pointers at teardown, AFTER the screenshot was
 * written, so 21 goldens reported a render failure with a correct image on disk.
 *
 * With a template, a field added and not mentioned here is initialised by the
 * LANGUAGE -- C zero-fills every member a designated initialiser omits. The
 * remaining hazard is the copy itself: the allocation is still malloc, so
 * `*material = MATERIAL_DEFAULTS` is load-bearing. Assigning fields piecemeal
 * from this would leave the omitted ones indeterminate again.
 */
static const Material MATERIAL_DEFAULTS = {
    .albedo = {1.0f, 1.0f, 1.0f},
    .emissive_strength = 1.0f,
    .roughness = 1.0f,
    .ao = 1.0f,
    .opacity = 1.0f,
    .normalScale = 1.0f,
    .aoStrength = 1.0f,
    .ior = 1.5f,
    .attenuation_color = {1.0f, 1.0f, 1.0f},
    // -1 means KHR_materials_specular was ABSENT, which is not the same as an
    // explicit zero weight -- so the sentinel has to sit outside [0,1].
    .specular_factor = -1.0f,
    .specular_color_factor = {1.0f, 1.0f, 1.0f},
    .subsurface_color = {1.0f, 0.3f, 0.2f}, // skin-ish, until a material says otherwise
    .subsurface_profile = -1,               // no scatter profile until configured
    .uvScale = {1.0f, 1.0f},
    // No array layer until the material texture array is built from loaded
    // textures; -1 is what makes the shader fall back to the scalar factor.
    .roughness_layer = -1,
    .metallic_layer = -1,
    .ao_layer = -1,
    .opacity_layer = -1,
    .microsurface_layer = -1,
    .anisotropy_layer = -1,
    .splat_layer = -1,
    .splat_size = {1.0f, 1.0f},
    // layer_count 0 is what gates the whole layered path, so the four slots are
    // inert; the sentinels still have to be right, because lowering a count
    // later leaves the slots above it holding whatever was here.
    .layers = {{.uv_scale = 1.0f, .albedo_layer = -1, .surface_layer = -1},
               {.uv_scale = 1.0f, .albedo_layer = -1, .surface_layer = -1},
               {.uv_scale = 1.0f, .albedo_layer = -1, .surface_layer = -1},
               {.uv_scale = 1.0f, .albedo_layer = -1, .surface_layer = -1}},
    // Not zero, and deliberately: a sharpness of 0 is the LINEAR blend, so a
    // layered material nobody tuned should interlock -- that is the behaviour
    // the feature exists for.
    .layer_blend_sharpness = 0.5f,
    .layer_triplanar_sharpness = 4.0f,
};
_Static_assert(MATERIAL_MAX_LAYERS == 4,
               "MATERIAL_DEFAULTS spells out four layer slots; a fifth would be left zeroed, "
               "which makes its layer indices 0 rather than the -1 that means absent");

Material* create_material() {
    Material* material = (Material*)malloc(sizeof(Material));
    if (!material) {
        log_error("Failed to allocate memory for material");
        return NULL;
    }

    // Load-bearing: this writes the WHOLE object, which is what lets
    // MATERIAL_DEFAULTS omit every zero field after a malloc.
    *material = MATERIAL_DEFAULTS;

    // Per-instance, not a default.
    material->id = g_next_material_id++;

    // The one DERIVED table, and the reason it is not in the template: C has no
    // initialiser-list arithmetic, so the alternative is 192 spelled literals.
    //
    // The identity inverse-CDF. stochastic_scale is 0, so nothing reads this
    // until a material both asks and supplies a transformed map -- but it is the
    // transform the shader would apply if it ran anyway, so a half-wired material
    // renders its texture rather than a black one.
    for (int i = 0; i < STOCHASTIC_LUT_SIZE; i++) {
        const float v = ((float)i + 0.5f) / (float)STOCHASTIC_LUT_SIZE;
        material->stochastic_lut[i * 3 + 0] = v;
        material->stochastic_lut[i * 3 + 1] = v;
        material->stochastic_lut[i * 3 + 2] = v;
    }

    return material;
}

void free_material(Material* material) {
    if (material) {
        if (material->name)
            free(material->name);
        // Release all texture references
        if (material->albedo_tex)
            texture_release(material->albedo_tex);
        if (material->normal_tex)
            texture_release(material->normal_tex);
        if (material->roughness_tex)
            texture_release(material->roughness_tex);
        if (material->metalness_tex)
            texture_release(material->metalness_tex);
        if (material->ambient_occlusion_tex)
            texture_release(material->ambient_occlusion_tex);
        if (material->emissive_tex)
            texture_release(material->emissive_tex);
        if (material->height_tex)
            texture_release(material->height_tex);
        if (material->opacity_tex)
            texture_release(material->opacity_tex);
        if (material->microsurface_tex)
            texture_release(material->microsurface_tex);
        if (material->anisotropy_tex)
            texture_release(material->anisotropy_tex);
        if (material->sheen_tex)
            texture_release(material->sheen_tex);
        if (material->reflectance_tex)
            texture_release(material->reflectance_tex);
        if (material->clearcoat_normal_tex)
            texture_release(material->clearcoat_normal_tex);
        if (material->splat_tex)
            texture_release(material->splat_tex);
        // Every slot, not layer_count of them: lowering the count leaves the
        // textures above it retained, and releasing only the live prefix would
        // leak exactly the layers somebody edited away.
        for (int i = 0; i < MATERIAL_MAX_LAYERS; i++) {
            if (material->layers[i].albedo_tex)
                texture_release(material->layers[i].albedo_tex);
            if (material->layers[i].surface_tex)
                texture_release(material->layers[i].surface_tex);
        }
        free_material_layers_vt(material->layers_vt);

        // Shader program managed by engine. Do not free here.
        free(material);
    }
}

void set_material_shader_program(Material* material, ShaderProgram* shader_program) {
    if (!material) {
        log_error("Cannot set shader program for NULL material");
        return;
    }

    if (!shader_program) {
        log_error("Cannot set NULL shader program for material");
        return;
    }
    material->shader_program = shader_program;
}

void set_material_albedo_tex(Material* material, Texture* texture) {
    if (!material)
        return;
    if (material->albedo_tex)
        texture_release(material->albedo_tex);
    material->albedo_tex = texture_retain(texture);
}

void set_material_normal_tex(Material* material, Texture* texture) {
    if (!material)
        return;
    if (material->normal_tex)
        texture_release(material->normal_tex);
    material->normal_tex = texture_retain(texture);
}

void set_material_roughness_tex(Material* material, Texture* texture) {
    if (!material)
        return;
    if (material->roughness_tex)
        texture_release(material->roughness_tex);
    material->roughness_tex = texture_retain(texture);
}

void set_material_metalness_tex(Material* material, Texture* texture) {
    if (!material)
        return;
    if (material->metalness_tex)
        texture_release(material->metalness_tex);
    material->metalness_tex = texture_retain(texture);
}

void set_material_ambient_occlusion_tex(Material* material, Texture* texture) {
    if (!material)
        return;
    if (material->ambient_occlusion_tex)
        texture_release(material->ambient_occlusion_tex);
    material->ambient_occlusion_tex = texture_retain(texture);
}

void set_material_emissive_tex(Material* material, Texture* texture) {
    if (!material)
        return;
    if (material->emissive_tex)
        texture_release(material->emissive_tex);
    material->emissive_tex = texture_retain(texture);
}

void set_material_height_tex(Material* material, Texture* texture) {
    if (!material)
        return;
    if (material->height_tex)
        texture_release(material->height_tex);
    material->height_tex = texture_retain(texture);
}

void set_material_opacity_tex(Material* material, Texture* texture) {
    if (!material)
        return;
    if (material->opacity_tex)
        texture_release(material->opacity_tex);
    material->opacity_tex = texture_retain(texture);
}

void set_material_sheen_tex(Material* material, Texture* texture) {
    if (!material)
        return;
    if (material->sheen_tex)
        texture_release(material->sheen_tex);
    material->sheen_tex = texture_retain(texture);
}

void set_material_reflectance_tex(Material* material, Texture* texture) {
    if (!material)
        return;
    if (material->reflectance_tex)
        texture_release(material->reflectance_tex);
    material->reflectance_tex = texture_retain(texture);
}

void set_material_clearcoat_normal_tex(Material* material, Texture* texture) {
    if (!material)
        return;
    if (material->clearcoat_normal_tex)
        texture_release(material->clearcoat_normal_tex);
    material->clearcoat_normal_tex = texture_retain(texture);
}

void set_material_microsurface_tex(Material* material, Texture* texture) {
    if (!material)
        return;
    if (material->microsurface_tex)
        texture_release(material->microsurface_tex);
    material->microsurface_tex = texture_retain(texture);
}

void set_material_anisotropy_tex(Material* material, Texture* texture) {
    if (!material)
        return;
    if (material->anisotropy_tex)
        texture_release(material->anisotropy_tex);
    material->anisotropy_tex = texture_retain(texture);
}

void set_material_splat_tex(Material* material, Texture* texture) {
    if (!material)
        return;
    if (material->splat_tex)
        texture_release(material->splat_tex);
    material->splat_tex = texture_retain(texture);
}

// Bounds-checked because the index reaches these from a scene file and from an
// app's own loop, neither of which the material can see. Out of range is dropped
// with a warning rather than clamped: clamping would silently overwrite layer 3
// with what layer 7 was meant to be, which renders as a plausible surface.
static bool layer_slot_ok(const Material* material, int index, const char* what) {
    if (!material)
        return false;
    if (index < 0 || index >= MATERIAL_MAX_LAYERS) {
        log_warn("material: layer %d is outside the %d slots a material has, ignoring its %s",
                 index, MATERIAL_MAX_LAYERS, what);
        return false;
    }
    return true;
}

void set_material_layer_albedo_tex(Material* material, int index, Texture* texture) {
    if (!layer_slot_ok(material, index, "albedo"))
        return;
    if (material->layers[index].albedo_tex)
        texture_release(material->layers[index].albedo_tex);
    material->layers[index].albedo_tex = texture_retain(texture);
}

void set_material_layer_surface_tex(Material* material, int index, Texture* texture) {
    if (!layer_slot_ok(material, index, "surface map"))
        return;
    if (material->layers[index].surface_tex)
        texture_release(material->layers[index].surface_tex);
    material->layers[index].surface_tex = texture_retain(texture);
}

