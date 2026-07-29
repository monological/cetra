// Punctual shadow map interface: the uniforms that shadow.c's
// bind_shadow_maps_to_program fills for the perspective light types, and the
// one lookup that reads them.
//
// Same contract as csm.glsl: these names are shared with ONE C function and
// the binder is location-guarded, so renaming a uniform here silently no-ops
// the upload in whichever shader you forgot, with no error anywhere.
//
// One 2D array serves every perspective type. A point light's six 90-degree
// frusta are six ordinary layers the caller picks between, so no
// samplerCubeArray (GLSL 400 against a shader set that is uniformly 330) and
// no second texture unit -- and there is no second unit to be had, since
// pbr_frag samples all 16.
//
// MAX_PUNCTUAL_SHADOW_LAYERS is mirrored in C at shadow.h; uniform.c runs the
// same drift check on this array as on the cascade ones.

#define MAX_PUNCTUAL_SHADOW_LAYERS 8
// Mirrors PUNCTUAL_SHADOW_MAP_SIZE (shadow.h). A compile-time constant, unlike
// the cascade array's runtime `default_map_size` -- so the PCF texel step is a
// literal here rather than a per-fragment textureSize() query.
#define PUNCTUAL_SHADOW_MAP_SIZE 2048.0

uniform sampler2DArray punctualShadowMaps;
uniform mat4 punctualShadowMatrix[MAX_PUNCTUAL_SHADOW_LAYERS];
// Layers rendered this frame. 0 disables every punctual shadow at once, which
// is what the shadow system's master switch and an absent depth pass both
// reduce to; a light carries its own base layer in its UBO entry, so this is
// the only global the lookup needs.
uniform int punctualShadowCount;

// Which of a point light's six layers covers a direction, by dominant axis.
// The order is +X -X +Y -Y +Z -Z, the GL cubemap face order, and it is the ONE
// thing this file and shadow.c must agree on -- everything else about a face
// travels in its matrix. shadow.c renders the faces in this order.
//
// A boundary is exactly a 45-degree plane, so a PCF tap taken near one lands
// past the face's edge and reads the array's border -- "lit", since a 2D array
// has no neighbouring face to sample. That is a real mechanism and it measures
// below the noise floor at this map size; shadow.c records the measurement
// beside the 90-degree fov it decided on.
int punctualCubeFace(vec3 toFrag) {
    vec3 a = abs(toFrag);
    if (a.x >= a.y && a.x >= a.z)
        return toFrag.x > 0.0 ? 0 : 1;
    if (a.y >= a.z)
        return toFrag.y > 0.0 ? 2 : 3;
    return toFrag.z > 0.0 ? 4 : 5;
}

// Occlusion for one perspective map: 1 = lit, 0 = fully occluded. A layer
// outside the live range, a fragment behind the light, and a fragment off the
// map all read as lit -- the last via the array's white border.
float punctualShadow(int layer, vec3 worldPos, float NdotL) {
    if (layer < 0 || layer >= punctualShadowCount)
        return 1.0;

    vec4 ls = punctualShadowMatrix[layer] * vec4(worldPos, 1.0);
    if (ls.w <= 0.0)
        return 1.0;

    vec3 pc = ls.xyz / ls.w * 0.5 + 0.5;
    if (pc.z > 1.0 || pc.x < 0.0 || pc.x > 1.0 || pc.y < 0.0 || pc.y > 1.0)
        return 1.0;

    // Slope-scaled by tan(angle of incidence), not by (1 - NdotL). The two agree
    // while a surface faces the light and diverge exactly where it matters: a
    // face seen edge-on spans many depth units across one texel, and (1 - NdotL)
    // saturates at 1 there while the tangent goes where the geometry does. That
    // shows up the moment a light sits directly above a room, which puts every
    // wall near edge-on -- an area panel's usual position.
    // tan(acos(x)) == sqrt(1-x^2)/x for x in (0,1] -- one sqrt+divide instead
    // of two transcendentals, per shadowed light per fragment. max() floors the
    // divide so grazing (x->0) lands on the same clamp ceiling.
    float ndl = clamp(NdotL, 0.0, 1.0);
    float slope = clamp(sqrt(1.0 - ndl * ndl) / max(ndl, 1e-3), 0.0, 12.0);
    float bias = clamp(0.0006 * slope, 0.0004, 0.008);
    // 3x3 PCF, which the directional cascades have always had and this never
    // did: a single tap quantizes the edge to the texel grid, and reads as a
    // staircase on any silhouette not aligned to it.
    vec2 texel = vec2(1.0 / PUNCTUAL_SHADOW_MAP_SIZE);
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float d =
                texture(punctualShadowMaps, vec3(pc.xy + vec2(x, y) * texel, float(layer))).r;
            sum += (pc.z - bias > d) ? 0.0 : 1.0;
        }
    }
    return sum / 9.0;
}
