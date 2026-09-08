
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <cglm/cglm.h>

#include "util.h"
#include "light.h"
#include "ext/log.h"

Light* create_light(const LightDesc* desc) {
    static const LightDesc none = {0};
    if (!desc)
        desc = &none;

    Light* light = calloc(1, sizeof(Light));
    if (!light) {
        log_error("Failed to allocate memory for light");
        return NULL;
    }

    light->name = safe_strdup(desc->name);
    light->type = desc->type;

    glm_vec3_copy((float*)desc->position, light->original_position);
    glm_vec3_copy((float*)desc->position, light->global_position);

    // The authored direction and up, and the world copies a node transform
    // rotates from them. With the default downward direction the default up
    // makes width = cross(up, dir) = +X: a ceiling panel spanning X by Z.
    vec3 direction = {0}, up = {0};
    vec3_or_default(desc->direction, (vec3){0.0f, -1.0f, 0.0f}, direction);
    vec3_or_default(desc->up, (vec3){0.0f, 0.0f, 1.0f}, up);
    light_set_direction(light, direction);
    light_set_up(light, up);

    vec3_or_default(desc->color, (vec3){1.0f, 1.0f, 1.0f}, light->color);
    glm_vec3_one(light->specular);
    glm_vec3_one(light->ambient);

    light_set_intensity_units(light, desc->intensity, desc->units);
    light->range = desc->range; // 0 = derive from attenuation (light_cull_radius)
    light->cutOff = cosf(desc->inner_cutoff > 0.0f ? desc->inner_cutoff : glm_rad(12.5f));
    light->outerCutOff = cosf(desc->outer_cutoff > 0.0f ? desc->outer_cutoff : glm_rad(15.0f));
    glm_vec2_copy((vec2){desc->size[0] > 0.0f ? desc->size[0] : 50.0f,
                         desc->size[1] > 0.0f ? desc->size[1] : 50.0f},
                  light->size);

    light->cast_shadows = desc->cast_shadows;
    light->shadow_map_index = -1;
    light->shadow_layer = -1;
    light->ies_profile = -1; // assigned by whoever loads a profile into the scene

    return light;
}

LightUnits light_canonical_units(LightType type) {
    switch (type) {
        case LIGHT_DIRECTIONAL:
            return LIGHT_UNITS_LUX;
        case LIGHT_AREA:
            return LIGHT_UNITS_NITS;
        default: // point, spot, and not-yet-typed
            return LIGHT_UNITS_CANDELA;
    }
}

bool light_units_valid_for_type(LightType type, LightUnits units) {
    LightUnits canonical = light_canonical_units(type);
    return units == LIGHT_UNITS_DEFAULT || units == canonical ||
           (units == LIGHT_UNITS_LUMENS && canonical == LIGHT_UNITS_CANDELA);
}

const char* light_units_name(LightUnits units) {
    switch (units) {
        case LIGHT_UNITS_LUMENS:
            return "lm";
        case LIGHT_UNITS_LUX:
            return "lx";
        case LIGHT_UNITS_NITS:
            return "nits";
        default:
            return "cd";
    }
}

// Phi/(4*pi), the isotropic conversion. Applied to spots too, on purpose:
// dividing by the cone's solid angle instead would make narrowing a beam
// brighten it, which is right for a bare emitter and wrong for how anyone
// expects a spot control to behave.
#define LUMENS_PER_CANDELA (4.0f * (float)M_PI)

// Deliberately does NOT consult light->type. Lumens converts by Phi/4pi and
// every other unit is already canonical, so the arithmetic is a function of the
// unit alone -- which is what lets create_light run it before anything else on
// the light is settled. Whether lumens makes SENSE for the light is an authoring
// question, checked where a type and a unit are read together (cscene.c), not a
// correctness one that a call order could silently get wrong.
void light_set_intensity_units(Light* light, float intensity, LightUnits units) {
    if (!light)
        return;
    light->units = units;
    light->intensity = units == LIGHT_UNITS_LUMENS ? intensity / LUMENS_PER_CANDELA : intensity;
}

LightUnits light_display_units(const Light* light) {
    if (!light)
        return LIGHT_UNITS_CANDELA;
    return light->units == LIGHT_UNITS_DEFAULT ? light_canonical_units(light->type) : light->units;
}

float light_intensity_in_units(const Light* light) {
    if (!light)
        return 0.0f;
    // A pure unit conversion of the stored canonical value, so it stays true no
    // matter which setter last touched the light.
    return light_display_units(light) == LIGHT_UNITS_LUMENS ? light->intensity * LUMENS_PER_CANDELA
                                                            : light->intensity;
}

void light_set_direction(Light* light, vec3 direction) {
    if (!light)
        return;
    // Authored direction: both the immutable local copy and the world-space
    // one the renderer reads. Nodes with a non-identity global transform
    // re-derive `direction` from `original_direction` during scene update.
    glm_vec3_copy(direction, light->original_direction);
    glm_vec3_copy(direction, light->direction);
}

void light_set_up(Light* light, vec3 up) {
    if (!light)
        return;
    // Same authored/world split as light_set_direction. Only area lights read
    // it, and pack time orthonormalizes against direction, so callers may pass
    // any non-parallel vector.
    glm_vec3_copy(up, light->original_up);
    glm_vec3_copy(up, light->up);
}

// Radiance below this reads as black at the project-standard -E 1.0 (one LDR
// LSB); the derived cull radius is where attenuation crosses it.
#define LIGHT_CULL_EPSILON (1.0f / 256.0f)

float light_effective_intensity(const struct Light* light) {
    const float peak = fmaxf(light->color[0], fmaxf(light->color[1], light->color[2]));
    return light->intensity * peak;
}

float light_cull_radius(const struct Light* light) {
    if (light->range > 0.0f)
        return light->range;

    float i_eff = light_effective_intensity(light);
    if (i_eff <= 0.0f)
        return 0.0f;

    // Area panels ignore the attenuation coefficients entirely -- the LTC
    // form factor carries the falloff, and `intensity` is emitted radiance.
    // Bound the reach by the head-on far-field irradiance I*A/(pi*d^2),
    // solved against the same 1/256 visibility floor the point path uses.
    // Head-on is the directional maximum (real response is that times NdotL),
    // so this is conservative; the half-diagonal covers the panel's own extent.
    if (light->type == LIGHT_AREA) {
        float area = light->size[0] * light->size[1];
        if (area <= 0.0f)
            return 0.0f;
        float half_diagonal =
            0.5f * sqrtf(light->size[0] * light->size[0] + light->size[1] * light->size[1]);
        return sqrtf(i_eff * area / (LIGHT_CULL_EPSILON * (float)M_PI)) + half_diagonal;
    }

    // No authored range: fall back to where bare inverse-square drops under the
    // visibility floor, i_eff/d^2 = epsilon. An authored one returned above --
    // the window makes a light exactly zero past its range, so the range IS the
    // cull radius and there is nothing to solve.
    return sqrtf(i_eff / LIGHT_CULL_EPSILON);
}

void orientation_frame(const float dir[3], const float ref[3], vec3 axis, vec3 up) {
    glm_vec3_copy((float*)dir, axis);
    if (glm_vec3_norm(axis) < 1e-6f)
        glm_vec3_copy((vec3){0.0f, -1.0f, 0.0f}, axis);
    glm_vec3_normalize(axis);

    // Gram-Schmidt the authored `ref` against the axis rather than trusting it.
    // Parallel (or degenerate) leaves no roll to recover -- it is genuinely
    // undefined there -- so take the canonical perpendicular and stay buildable.
    vec3 proj;
    glm_vec3_proj((float*)ref, axis, proj);
    glm_vec3_sub((float*)ref, proj, up);
    if (glm_vec3_norm(up) < 1e-4f)
        glm_vec3_ortho(axis, up);
    glm_vec3_normalize(up);
}

void light_emission_frame(const struct Light* light, vec3 axis, vec3 up) {
    orientation_frame(light->direction, light->up, axis, up);
}

void free_light(Light* light) {
    if (!light)
        return;

    if (light->name) {
        free(light->name);
    }
    free(light);
}

const char* light_type_name(LightType type) {
    switch (type) {
        case LIGHT_DIRECTIONAL:
            return "Directional";
        case LIGHT_POINT:
            return "Point";
        case LIGHT_SPOT:
            return "Spot";
        case LIGHT_AREA:
            return "Area";
        default:
            return "Unknown";
    }
}

void light_print(const Light* light) {
    if (!light) {
        printf("<Invalid light pointer>\n");
        return;
    }

    printf("<Light name='%s', type='%s', original_position=(%f, %f, %f) global_position=(%f, %f, "
           "%f), direction=(%f, %f, %f), "
           "color=(%f, %f, %f), specular=(%f, %f, %f), ambient=(%f, %f, %f), "
           "intensity=%f %s, range=%f, cutOff=%f, outerCutOff=%f>\n",
           light->name, light_type_name(light->type), light->original_position[0],
           light->original_position[1], light->original_position[2], light->global_position[0],
           light->global_position[1], light->global_position[2], light->direction[0],
           light->direction[1], light->direction[2], light->color[0], light->color[1],
           light->color[2], light->specular[0], light->specular[1], light->specular[2],
           light->ambient[0], light->ambient[1], light->ambient[2], light_intensity_in_units(light),
           light_units_name(light_display_units(light)), light->range, light->cutOff,
           light->outerCutOff);
}
