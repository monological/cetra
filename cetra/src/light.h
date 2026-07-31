
#ifndef _LIGHT_H_
#define _LIGHT_H_

#include <cglm/cglm.h>
#include <stdbool.h>

typedef enum { LIGHT_DIRECTIONAL, LIGHT_POINT, LIGHT_SPOT, LIGHT_AREA, LIGHT_UNKNOWN } LightType;

typedef struct Light {
    char* name;

    LightType type;

    vec3 original_position;
    vec3 global_position;

    // Authored light-local direction; the scene graph rotates it by the
    // owning node's global transform into `direction` each update (same
    // split as original_position/global_position).
    vec3 original_direction;
    vec3 direction;

    // Area-light panel orientation: `up` spans the height axis, `direction`
    // the panel normal, and width follows from cross(up, direction). Same
    // authored/rotated split as direction. Orthonormalized against direction
    // at pack time (light_cluster.c), so a sloppy authored up is fine.
    // Ignored by every other light type.
    vec3 original_up;
    vec3 up;
    vec3 color;
    vec3 specular;
    vec3 ambient;
    float intensity;

    // Where the inverse-square falloff is windowed to zero, and the cull radius
    // (spec 9.9). 0 = unbounded, which is also KHR_lights_punctual's default;
    // light_cull_radius then falls back to where the falloff drops under the
    // visibility floor.
    float range;

    // Spot light specific properties
    float cutOff;      // Cut-off angle
    float outerCutOff; // Outer cut-off angle

    // Area
    vec2 size;

    // Shadow mapping. Two indices because the two map sets are addressed
    // differently and a light can only be in one of them: shadow_map_index is
    // a DIRECTIONAL caster slot in the cascade array (layers stride by the
    // runtime cascade count), shadow_layer is a base layer in the punctual
    // array. One int meaning either would have to be read against the light's
    // type at every use. Both are -1 for "no map", reassigned every frame by
    // the depth pass.
    bool cast_shadows;
    int shadow_map_index;
    int shadow_layer;
} Light;

Light* create_light();
void set_light_name(Light* light, const char* name);
void set_light_type(Light* light, LightType type);
void set_light_specular(Light* light, vec3 specular);
void set_light_ambient(Light* light, vec3 ambient);
void set_light_original_position(Light* light, vec3 original_position);
void set_light_global_position(Light* light, vec3 global_position);
void set_light_direction(Light* light, vec3 direction);
void set_light_up(Light* light, vec3 up);
void set_light_color(Light* light, vec3 color);
void set_light_intensity(Light* light, float intensity);
void set_light_range(Light* light, float range);
void set_light_cutoff(Light* light, float cutOff, float outerCutOff);
void set_light_cast_shadows(Light* light, bool cast_shadows);
void set_light_size(Light* light, float width, float height);
void free_light(Light* light);
void print_light(const Light* light);

// Display name for a light type, for logs and GUI labels. Never NULL.
const char* light_type_name(LightType type);

#endif // _LIGHT_H_
