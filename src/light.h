
#ifndef _LIGHT_H_
#define _LIGHT_H_

#include <cglm/cglm.h>

typedef enum {
    LIGHT_DIRECTIONAL,
    LIGHT_POINT,
    LIGHT_SPOT,
    LIGHT_AREA,
    LIGHT_UNKNOWN
} LightType;

typedef struct Light {
    char* name;

    LightType type;

    vec3 position;
    vec3 direction;
    vec3 color;
    vec3 specular;
    vec3 ambient;
    float intensity;

    // Attenuation factors (for point and spot lights)
    float constant;
    float linear;
    float quadratic;

    // Spot light specific properties
    float cutOff;      // Cut-off angle
    float outerCutOff; // Outer cut-off angle

    // Area
    vec2 size;
} Light;

Light* create_light();
void set_light_name(Light* light, const char* name);
void set_light_type(Light* light, LightType type);
void set_light_specular(Light* light, vec3 specular);
void set_light_ambient(Light* light, vec3 ambient);
void set_light_position(Light* light, vec3 position);
void set_light_direction(Light* light, vec3 direction);
void set_light_color(Light* light, vec3 color);
void set_light_intensity(Light* light, float intensity);
void set_light_attenuation(Light* light, float constant, float linear, float quadratic);
void set_light_cutoff(Light* light, float cutOff, float outerCutOff);
void free_light(Light* light);
void print_light(const Light* light);

#endif // _LIGHT_H_



