
#ifndef _LIGHT_H_
#define _LIGHT_H_

#include <cglm/cglm.h>
#include <stdbool.h>

typedef enum { LIGHT_DIRECTIONAL, LIGHT_POINT, LIGHT_SPOT, LIGHT_AREA, LIGHT_UNKNOWN } LightType;

// The unit an intensity was AUTHORED in. Every light type has exactly one unit
// the shading path can use -- candela for point and spot, lux for directional,
// nits for an area panel -- and `Light.intensity` always holds that one, so no
// conversion ever reaches a shader.
//
// This enum exists for the one unit that is neither: lumens. It is what a bulb's
// box prints and what an artist reaches for, and it is NOT what inverse-square
// falloff integrates. Storing which one was written lets the value be handed
// back in the same unit it arrived in -- a lamp authored as 30 lm reads 30 lm,
// not the 2.39 cd it is shaded as.
//
// DEFAULT is zero so a calloc'd Light starts there, and it resolves against the
// type at READ time rather than being baked in by a setter. That is what keeps
// the setters order-free: nothing here depends on type being assigned first, so
// no call sequence can silently produce the wrong intensity.
typedef enum {
    LIGHT_UNITS_DEFAULT, // whatever the light's type is shaded in; resolved lazily
    LIGHT_UNITS_CANDELA, // cd; point + spot, and what they are shaded in
    LIGHT_UNITS_LUMENS,  // lm; point + spot, authored only -- Phi/4pi to candela
    LIGHT_UNITS_LUX,     // lx; directional
    LIGHT_UNITS_NITS,    // cd/m^2; area panels
} LightUnits;

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

    // Always in the canonical unit for `type` (candela / lux / nits), whatever
    // `units` says was authored -- shading reads this directly.
    float intensity;
    LightUnits units;

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

    // Index into the scene's IesLibrary, or -1 for none (spec 11.57). An IES
    // profile is the measured angular distribution of a real luminaire and
    // REPLACES the analytic cone, so a spot carrying one ignores its cutOff
    // pair -- and so a point light finally has a use for `direction`, which it
    // has always carried and nothing has ever read.
    //
    // An index and not an IesProfile*, for emissive_source_id's reason one field
    // down: the library is scene-owned and a pointer would outlive it in exactly
    // the teardown order that is easiest to get wrong.
    int ies_profile;

    // The Mesh whose emissive surface this panel was derived from (spec 11.49),
    // by that mesh's stable `id`. 0 means AUTHORED -- a light somebody made --
    // and the emissive reconcile will not touch one, so the two populations
    // share this array without either being able to delete the other.
    //
    // The id and not a Mesh*: the reconcile learns a mesh is gone from the graph
    // epoch, by which time the pointer is already dangling, where mesh.h
    // guarantees an id is "assigned once and never reused".
    unsigned emissive_source_id;
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

// Cull radius for a light: the authored range if set, else the distance where
// the light falls under ~1/256 (LDR LSB at the project-standard -E 1.0).
// Punctual lights solve that against their attenuation coefficients; area
// panels ignore those entirely (the LTC form factor carries the falloff) and
// instead invert the head-on far-field irradiance, plus half the panel
// diagonal to cover its own extent. Returns 0 for a light that never reaches
// the epsilon (drop it) and a negative value for an uncullable light
// (constant-only attenuation: assign everywhere).
//
// Lives here rather than with the cluster grid because it is a pure function of
// one light and part of what `range` above MEANS -- the culler is its largest
// consumer, not its owner, and two of its callers have nothing to do with
// clustering.
float light_cull_radius(const struct Light* light);
void set_light_cutoff(Light* light, float cutOff, float outerCutOff);
void set_light_cast_shadows(Light* light, bool cast_shadows);
void set_light_size(Light* light, float width, float height);
void free_light(Light* light);
void print_light(const Light* light);

// Set an intensity authored in `units`, converting to the canonical unit. The
// conversion reads ONLY the unit -- lumens is Phi/4pi whatever the light is --
// so this may be called before or after set_light_type with the same result.
void set_light_intensity_units(Light* light, float intensity, LightUnits units);

// `light->intensity` expressed back in the light's display unit -- the inverse
// of the conversion above, for showing an author the number they wrote.
float light_intensity_in_units(const Light* light);

// `light->units` with DEFAULT resolved against the current type. Everything that
// displays or formats an intensity goes through this rather than reading the
// field, so a light that was never given an explicit unit still reports the one
// it is actually shaded in.
LightUnits light_display_units(const Light* light);

// The only unit a type can be shaded in.
LightUnits light_canonical_units(LightType type);

// Whether an authored unit means anything for this type: its own, or lumens on
// something measured in candela. A pure predicate with no ordering dependence,
// so any asset path can call it -- which is the point. It lives here rather
// than in a parser because the second parser to want it would otherwise
// re-derive the type-to-unit table, and the two would drift.
bool light_units_valid_for_type(LightType type, LightUnits units);

// Display names, for logs and GUI labels. Never NULL.
const char* light_type_name(LightType type);
const char* light_units_name(LightUnits units);

#endif // _LIGHT_H_
