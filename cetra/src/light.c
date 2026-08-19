
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <cglm/cglm.h>

#include "util.h"
#include "light.h"
#include "ext/log.h"

Light* create_light() {
    Light* light = malloc(sizeof(Light));

    if (!light) {
        log_error("Failed to allocate memory for light");
        return NULL;
    }
    memset(light, 0, sizeof(Light));

    light->name = NULL;
    light->type = LIGHT_UNKNOWN;

    glm_vec3_copy((vec3){0.0f, 0.0f, 0.0f}, light->original_position);
    glm_vec3_copy((vec3){0.0f, 0.0f, 0.0f}, light->global_position);

    glm_vec3_copy((vec3){0.0f, -1.0f, 0.0f}, light->original_direction);
    glm_vec3_copy((vec3){0.0f, -1.0f, 0.0f}, light->direction);
    // Area-panel height axis. With the default downward direction this makes
    // width = cross(up, dir) = +X: a ceiling panel spanning X by Z.
    glm_vec3_copy((vec3){0.0f, 0.0f, 1.0f}, light->original_up);
    glm_vec3_copy((vec3){0.0f, 0.0f, 1.0f}, light->up);
    glm_vec3_copy((vec3){1.0f, 1.0f, 1.0f}, light->color);
    glm_vec3_copy((vec3){1.0f, 1.0f, 1.0f}, light->specular);
    glm_vec3_copy((vec3){1.0f, 1.0f, 1.0f}, light->ambient);
    light->intensity = 1.0f;
    light->range = 0.0f; // 0 = derive from attenuation (light_cull_radius)
    light->cutOff = cosf(glm_rad(12.5f));
    light->outerCutOff = cosf(glm_rad(15.0f));

    glm_vec2_copy((vec2){50.0f, 50.0f}, light->size);

    light->cast_shadows = false;
    light->shadow_map_index = -1;
    light->shadow_layer = -1;
    light->ies_profile = -1;

    return light;
}

void set_light_name(Light* light, const char* name) {
    if (!light || !name)
        return;
    if (light->name != NULL) {
        free(light->name);
    }
    light->name = safe_strdup(name);
}

void set_light_type(Light* light, LightType type) {
    if (!light)
        return;
    light->type = type;
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
// unit alone -- which is what lets this be called in any order relative to
// set_light_type. Whether lumens makes SENSE for the light is an authoring
// question, checked where a type and a unit are read together (cscene.c), not a
// correctness one that a call order could silently get wrong.
void set_light_intensity_units(Light* light, float intensity, LightUnits units) {
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

void set_light_specular(Light* light, vec3 specular) {
    if (!light)
        return;
    glm_vec3_copy(specular, light->specular);
}

void set_light_ambient(Light* light, vec3 ambient) {
    if (!light)
        return;
    glm_vec3_copy(ambient, light->ambient);
}

void set_light_original_position(Light* light, vec3 original_position) {
    if (!light)
        return;
    glm_vec3_copy(original_position, light->original_position);
}

void set_light_global_position(Light* light, vec3 global_position) {
    if (!light)
        return;
    glm_vec3_copy(global_position, light->global_position);
}

void set_light_direction(Light* light, vec3 direction) {
    if (!light)
        return;
    // Authored direction: both the immutable local copy and the world-space
    // one the renderer reads. Nodes with a non-identity global transform
    // re-derive `direction` from `original_direction` during scene update.
    glm_vec3_copy(direction, light->original_direction);
    glm_vec3_copy(direction, light->direction);
}

void set_light_up(Light* light, vec3 up) {
    if (!light)
        return;
    // Same authored/world split as set_light_direction. Only area lights read
    // it, and pack time orthonormalizes against direction, so callers may pass
    // any non-parallel vector.
    glm_vec3_copy(up, light->original_up);
    glm_vec3_copy(up, light->up);
}

void set_light_color(Light* light, vec3 color) {
    if (!light)
        return;
    glm_vec3_copy(color, light->color);
}

// Set intensity in the light type's own unit: candela for point and spot, lux
// for a directional, nits for an area panel. To author in lumens, which is the
// only other unit that converts, call set_light_intensity_units.
//
// Leaves `units` alone on purpose. It is a DISPLAY unit over a canonical value,
// so it stays correct across a canonical write -- 2.39 cd and 30 lm are the same
// light, and a lamp being shown in lumens should keep being shown in lumens.
void set_light_intensity(Light* light, float intensity) {
    if (!light)
        return;
    light->intensity = intensity;
}

void set_light_range(Light* light, float range) {
    if (!light)
        return;
    light->range = range;
}

// Radiance below this reads as black at the project-standard -E 1.0 (one LDR
// LSB); the derived cull radius is where attenuation crosses it.
#define LIGHT_CULL_EPSILON (1.0f / 256.0f)

float light_cull_radius(const struct Light* light) {
    if (light->range > 0.0f)
        return light->range;

    float peak = fmaxf(light->color[0], fmaxf(light->color[1], light->color[2]));
    float i_eff = light->intensity * peak;
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

/**
 * Sets the cutoff angles for a spotlight.
 *
 * @param light A pointer to the Light structure.
 * @param cutOff The inner cutoff angle, inside of which the light is at full
 *               intensity. This is typically expressed in radians or as a
 *               cosine of the angle for efficiency.
 * @param outerCutOff The outer cutoff angle, beyond which the light intensity
 *                    falls off to zero. Also typically in radians or as a cosine.
 */
void set_light_cutoff(Light* light, float cutOff, float outerCutOff) {
    if (!light)
        return;
    light->cutOff = cutOff;
    light->outerCutOff = outerCutOff;
}

void set_light_cast_shadows(Light* light, bool cast_shadows) {
    if (!light)
        return;
    light->cast_shadows = cast_shadows;
}

void set_light_size(Light* light, float width, float height) {
    if (!light)
        return;
    glm_vec2_copy((vec2){width, height}, light->size);
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

void print_light(const Light* light) {
    if (!light) {
        printf("<Invalid light pointer>\n");
        return;
    }

    printf("<Light name='%s', type='%s', original_position=(%f, %f, %f) global_position=(%f, %f, "
           "%f), direction=(%f, %f, %f), "
           "color=(%f, %f, %f), specular=(%f, %f, %f), ambient=(%f, %f, %f), "
           "intensity=%f %s, range=%f, cutOff=%f, outerCutOff=%f>\n",
           light->name, light_type_name(light->type), light->original_position[0],
           light->original_position[1],
           light->original_position[2], light->global_position[0], light->global_position[1],
           light->global_position[2], light->direction[0], light->direction[1], light->direction[2],
           light->color[0], light->color[1], light->color[2], light->specular[0],
           light->specular[1], light->specular[2], light->ambient[0], light->ambient[1],
           light->ambient[2], light_intensity_in_units(light),
           light_units_name(light_display_units(light)), light->range, light->cutOff,
           light->outerCutOff);
}
