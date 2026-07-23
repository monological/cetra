#include <stdlib.h>
#include <string.h>

#include "wind.h"
#include "util.h" // safe_strdup

Wind* create_wind(const char* name) {
    Wind* wind = malloc(sizeof(Wind));
    if (!wind)
        return NULL;
    wind->name = NULL;
    set_wind_name(wind, name ? name : "wind");
    wind->type = WIND_DIRECTIONAL;
    glm_vec3_copy((vec3){0.0f, 0.0f, 1.0f}, wind->direction);
    wind->strength = 0.02f;
    wind->speed = 1.5f;
    wind->gust_frequency = 0.15f;
    wind->gust_amount = 0.8f;
    wind->turbulence = 0.2f;
    return wind;
}

void free_wind(Wind* wind) {
    if (!wind)
        return;
    free(wind->name);
    free(wind);
}

void set_wind_name(Wind* wind, const char* name) {
    if (!wind)
        return;
    free(wind->name);
    wind->name = safe_strdup(name);
}

void wind_upload_to_program(const Wind* wind, UniformManager* u) {
    if (!u)
        return;
    // No wind (or none on this scene): strength 0 makes every wind-aware shader
    // early-out, so the scene renders exactly as it did before the feature.
    if (!wind) {
        uniform_set_float(u, "uWindStrength", 0.0f);
        return;
    }
    uniform_set_vec3(u, "uWindDir", (const float*)wind->direction);
    uniform_set_float(u, "uWindStrength", wind->strength);
    uniform_set_float(u, "uWindSpeed", wind->speed);
    uniform_set_float(u, "uWindGustFreq", wind->gust_frequency);
    uniform_set_float(u, "uWindGustAmount", wind->gust_amount);
    uniform_set_float(u, "uWindTurbulence", wind->turbulence);
}
