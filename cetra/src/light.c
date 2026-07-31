
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
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

/**
 * Sets the intensity of the light.
 *
 * @param light A pointer to the Light structure.
 * @param intensity The intensity value to be set. This is a scalar value
 *                  that affects the overall brightness of the light.
 *                  Typical values range from 0 (no light) upwards, where
 *                  1 represents the light's natural intensity.
 */
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

void print_light(const Light* light) {
    if (!light) {
        printf("<Invalid light pointer>\n");
        return;
    }

    const char* typeString;
    switch (light->type) {
        case LIGHT_DIRECTIONAL:
            typeString = "Directional";
            break;
        case LIGHT_POINT:
            typeString = "Point";
            break;
        case LIGHT_SPOT:
            typeString = "Spot";
            break;
        case LIGHT_AREA:
            typeString = "AREA";
            break;
        default:
            typeString = "Unknown";
            break;
    }

    printf("<Light name='%s', type='%s', original_position=(%f, %f, %f) global_position=(%f, %f, "
           "%f), direction=(%f, %f, %f), "
           "color=(%f, %f, %f), specular=(%f, %f, %f), ambient=(%f, %f, %f), "
           "intensity=%f, range=%f, cutOff=%f, outerCutOff=%f>\n",
           light->name, typeString, light->original_position[0], light->original_position[1],
           light->original_position[2], light->global_position[0], light->global_position[1],
           light->global_position[2], light->direction[0], light->direction[1], light->direction[2],
           light->color[0], light->color[1], light->color[2], light->specular[0],
           light->specular[1], light->specular[2], light->ambient[0], light->ambient[1],
           light->ambient[2], light->intensity, light->range, light->cutOff, light->outerCutOff);
}
