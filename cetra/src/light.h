
#ifndef _LIGHT_H_
#define _LIGHT_H_

/*
 * A light: directional, point, spot or area panel, in photometric units
 * (candela, lux, nits), with an authored frame the owning node's transform
 * carries into the world each frame (specs 9.9, 11.57, 11.106).
 *
 * Created from a LightDesc; NULL means every default, which is a white
 * directional pointing down that emits NOTHING -- intensity is the one desc
 * field whose zero is a value, because a scene file declines the default rig
 * that way, so a lit light says how bright. A light reaches a fragment
 * through the scene's registry (scene_add_light) and a node
 * (node_set_light); local lights go through the clustered grid, directionals
 * reach everything.
 */

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
    // ENGINE-OWNED: derived state. Read freely, never write.
    char* name; // Copied at creation; freed with the light
    // The authored frame carried into world space by the owning node's global
    // transform, each transform walk: position moved, direction and up rotated.
    vec3 global_position;
    vec3 direction;
    vec3 up;
    // Shadow mapping. Two indices because the two map sets are addressed
    // differently and a light can only be in one of them: shadow_map_index is
    // a DIRECTIONAL caster slot in the cascade array (layers stride by the
    // runtime cascade count), shadow_layer is a base layer in the punctual
    // array. One int meaning either would have to be read against the light's
    // type at every use. Both are -1 for "no map", reassigned every frame by
    // the depth pass.
    int shadow_map_index;
    int shadow_layer;
    // The Mesh whose emissive surface this panel was derived from (spec 11.49),
    // by that mesh's stable `id`. 0 means AUTHORED -- a light somebody made --
    // and the emissive reconcile will not touch one, so the two populations
    // share this array without either being able to delete the other.
    //
    // The id and not a Mesh*: the reconcile learns a mesh is gone from the graph
    // epoch, by which time the pointer is already dangling, where mesh.h
    // guarantees an id is "assigned once and never reused".
    unsigned emissive_source_id;

    // BY FUNCTION: each through the function named. The frame's three
    // authored copies: the walk carries them into the world copies above for
    // a light on a node, and the setters write both for a light on none.
    vec3 original_position;  // light_set_position
    vec3 original_direction; // light_set_direction
    // The luminaire's roll about `direction`. On a panel it spans the height
    // axis, with the normal along `direction` and width from cross(up,
    // direction); on a point or spot it is an asymmetric IES profile's azimuth
    // zero, which is the only thing that says which way such a lamp is turned.
    // Orthonormalized against the direction (light_emission_frame), so a sloppy
    // authored up is fine. Read only by those two -- a symmetric profile and a
    // bare cone never ask.
    vec3 original_up; // light_set_up
    // Always in the canonical unit for `type` (candela / lux / nits), whatever
    // `units` says was authored -- shading reads this directly, and
    // light_set_intensity_units is what converts.
    float intensity;
    LightUnits units;

    // SETTINGS: plain stores. Write them directly, at any time.
    LightType type;
    vec3 color;
    vec3 specular;
    vec3 ambient;

    // Where the inverse-square falloff is windowed to zero, and the cull radius
    // (spec 9.9). 0 = unbounded, which is also KHR_lights_punctual's default;
    // light_cull_radius then falls back to where the falloff drops under the
    // visibility floor.
    float range;

    // Spot cone, as COSINES of the half-angles -- not radians, and not degrees.
    // Stating the unit here because the ambiguity has cost once already: the
    // glTF importer assigned assimp's half-angles in radians straight into these
    // for several specs, which read cos-1(0.524) = 58 degrees for an authored 30
    // and built a 117 degree shadow frustum, and looked like a plausible spot
    // the whole time.
    float cutOff;
    float outerCutOff;

    // A panel's extent, or a directional's PCSS emitter size
    vec2 size;

    bool cast_shadows;

    // Index into the scene's IesLibrary, or -1 for none (spec 11.57). An IES
    // profile is the measured angular distribution of a real luminaire and
    // REPLACES the analytic cone, so a spot carrying one ignores its cutOff
    // pair -- and so a point light finally has a use for `direction`, which it
    // has always carried and nothing has ever read.
    //
    // An index and not an IesProfile*, for emissive_source_id's reason above:
    // the library is scene-owned and a pointer would outlive it in exactly the
    // teardown order that is easiest to get wrong.
    int ies_profile;
} Light;

// What a light is created from. Fill the fields you mean with designated
// initialisers and leave the rest zero: zero is the default, named beside each
// field. A zero vec3 is the default too, so a colour left out is white, not
// black, and a direction left out points down. Intensity is the one field
// whose zero is a value and not a default -- a light that emits nothing is
// how a scene declines a light it cannot delete -- so a lit light says how
// bright.
//
// intensity is read in `units`, so a lumen figure converts to candela at
// creation the way light_set_intensity_units does; LIGHT_UNITS_DEFAULT (zero)
// takes the number as the type's own unit.
typedef struct LightDesc {
    const char* name;   // copied; NULL = unnamed
    LightType type;     // zero is LIGHT_DIRECTIONAL
    vec3 position;      // the authored position; the origin
    vec3 direction;     // the authored direction; 0 = straight down
    vec3 up;            // an area panel's height axis; 0 = +Z
    vec3 color;         // 0 = white
    float intensity;    // in `units`; 0 emits nothing
    LightUnits units;   // zero = the type's own unit
    float range;        // where the falloff is windowed to zero; 0 = derived
    float inner_cutoff; // spot cone half-angles, RADIANS; 0 = 12.5 and 15 degrees
    float outer_cutoff;
    // An area panel's extent, or a directional's emitter size for the PCSS
    // penumbra; 0 = 50 by 50
    vec2 size;
    bool cast_shadows;
} LightDesc;

// NULL means every default: a white directional pointing down, emitting nothing.
Light* create_light(const LightDesc* desc);

// The frame: each writes the authored copy and the world copy together, so a
// light on no node (which the walk never re-derives) is placed too.
void light_set_position(Light* light, vec3 position);
void light_set_direction(Light* light, vec3 direction);
void light_set_up(Light* light, vec3 up);

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

// How strong a light is as ONE number: intensity scaled by its brightest channel. The
// codebase's answer to that question, so anything comparing or thresholding lights uses
// this rather than re-deriving it -- the reduction was written out twice before, in
// light_cull_radius and in water's key-light pick, and a third would have made it a rule
// with no home.
//
// Peak rather than luminance because it answers "does this light still do anything",
// where a saturated blue must not be discounted for having no green in it.
float light_effective_intensity(const struct Light* light);

// An orthonormal frame from an authored direction and an authored roll
// reference. Always buildable -- a degenerate `dir` falls back to -Y and a
// degenerate or parallel `ref` to the canonical perpendicular -- which every
// consumer assumes rather than testing for.
//
// Takes vectors rather than a Light because the second consumer is not one: a
// decal is oriented by exactly this pair (decal.h), and an authored frame that
// resolves differently in the two would aim the same numbers two ways while
// both halves still rendered something plausible.
void orientation_frame(const float dir[3], const float ref[3], vec3 axis, vec3 up);

// The luminaire's orthonormal emission frame: `axis` the unit direction it emits
// along, `up` the roll reference orthogonal to it -- orientation_frame over the
// light's own two fields. Consumers assume it is buildable, from the LTC corner
// construction to an asymmetric profile's azimuth zero.
void light_emission_frame(const struct Light* light, vec3 axis, vec3 up);

void free_light(Light* light);
void light_print(const Light* light);

// Set an intensity authored in `units`, converting to the canonical unit. The
// conversion reads ONLY the unit -- lumens is Phi/4pi whatever the light is --
// so it does not depend on the light's type being settled first.
void light_set_intensity_units(Light* light, float intensity, LightUnits units);

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
