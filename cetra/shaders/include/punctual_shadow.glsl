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

uniform sampler2DArray punctualShadowMaps;
uniform mat4 punctualShadowMatrix[MAX_PUNCTUAL_SHADOW_LAYERS];
// Layers rendered this frame. 0 disables every punctual shadow at once, which
// is what the shadow system's master switch and an absent depth pass both
// reduce to; a light carries its own base layer in its UBO entry, so this is
// the only global the lookup needs.
uniform int punctualShadowCount;

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

    // 3x3 PCF, which the directional cascades have always had and this never
    // did: a single tap quantizes the edge to the texel grid, and reads as a
    // staircase on any silhouette not aligned to it.
    float bias = max(0.0015 * (1.0 - NdotL), 0.0004);
    vec2 texel = vec2(1.0 / float(textureSize(punctualShadowMaps, 0).x));
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
