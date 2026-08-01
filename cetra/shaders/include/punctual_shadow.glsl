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
// One texel's world width per unit of axial distance (shadow.c writes
// 2*tan(fov/2)/size). Per layer, because a point light's 90-degree face and a
// panel's 120-degree cone do not cover the same ground at the same distance.
uniform float punctualTexelScale[MAX_PUNCTUAL_SHADOW_LAYERS];
// The near/far pair every punctual projection is built with. One vec2 rather
// than a per-layer array because compute_punctual_matrices takes both from the
// shadow system, so every layer shares them.
uniform vec2 punctualNearFar;
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

// Receiver bias: how far toward the light the sample point moves before it is
// projected, in shadow texels at the receiver's own distance. A world-space
// displacement rather than an epsilon in the projection's NDC z, which is worth
// ~d^2/near world units and so is only ever right at one distance and one scene
// scale.
//
// The slope term carries it. One texel of a surface tilted away from the light
// spans tan(theta) texel-widths of depth, and it is the 3x3 kernel's reach
// across THAT which the bias has to clear. The cap is where the tangent stops
// describing anything: past it the surface is edge-on and GRAZING_FADE has
// already taken over.
#define PUNCTUAL_BIAS_TEXELS 3.0
#define PUNCTUAL_BIAS_FLOOR  0.5
#define PUNCTUAL_MAX_SLOPE   8.0
// Below this NdotL the map is not answering the question asked of it. One
// perspective map tests whether the light's CENTRE is visible, and for a
// receiver nearly edge-on to that centre the test is undefined in both
// directions at once: the receiver compresses into a sliver of the map where its
// own silhouette is inside the PCF kernel (spurious occlusion no bias reaches),
// while the real answer for an AREA source is "half the panel", which a binary
// test cannot express. So the term is faded out rather than trusted.
//
// It is a no-op on the point/spot path, whose radiance is already multiplied by
// the same NdotL and is therefore zero wherever the fade is not one. Only the
// LTC path, which has no NdotL because the form factor carries orientation, has
// anything here to lose -- and there the fade is what stops a hard binary edge
// being drawn across a face that genuinely sees part of the panel.
#define PUNCTUAL_GRAZING_FADE 0.2

// Penumbra cap in shadow-UV, and the tap budget's limit. Past roughly ten
// texels of a 2048 map the 16-tap disk stops being dense enough and the
// gradient breaks into rings, so this is a cleanliness bound rather than a
// look control -- same reasoning and same value as the cascade path's.
#define PUNCTUAL_MAX_PENUMBRA_UV 0.005

// View distance from a stored depth. The punctual maps are perspective, so
// depth is not linear in distance and a penumbra built from raw depth
// differences would be wrong everywhere except one plane. Inverting the
// projection is two multiplies and makes blocker-receiver separation a real
// length, which is what the similar-triangles penumbra below needs.
float linearizePunctualDepth(float d01, vec2 nf) {
    float zn = 2.0 * d01 - 1.0;
    return 2.0 * nf.x * nf.y / ((nf.y + nf.x) - zn * (nf.y - nf.x));
}

// Occlusion for one perspective map: 1 = lit, 0 = fully occluded. A layer
// outside the live range, a fragment behind the light, a fragment off the map,
// and a fragment edge-on to the light all read as lit -- the third via the
// array's white border, the last via the fade above.
//
// L points at the light, so the fragment's own incidence angle sizes the bias:
// it vanishes head-on, where a texel spans almost no depth and any displacement
// is pure loss, and grows as the surface turns edge-on.
//
// `sourceRadius` is the emitter's world half-extent, and 0 asks for the hard
// 3x3 result. A point or spot passes 0 because it has no extent to resolve; a
// panel passes its own, which is the whole difference between a shadow that
// staircases along the shadow-map grid and one that softens with distance from
// its caster the way an extended source actually does.
float punctualShadow(int layer, vec3 worldPos, vec3 N, vec3 L, float sourceRadius) {
    if (layer < 0 || layer >= punctualShadowCount)
        return 1.0;

    float ndl = clamp(dot(N, L), 0.0, 1.0);
    float trust = smoothstep(0.0, PUNCTUAL_GRAZING_FADE, ndl);
    if (trust <= 0.0)
        return 1.0;

    // The first projection is for ls.w alone -- the axial distance to the light,
    // which is what the per-layer texel scale is expressed per unit of. The
    // bias is sized in texels, so it cannot be built without it.
    vec4 ls = punctualShadowMatrix[layer] * vec4(worldPos, 1.0);
    if (ls.w <= 0.0)
        return 1.0;

    // tan(acos(x)) == sqrt(1-x^2)/x for x in (0,1] -- one sqrt+divide instead
    // of two transcendentals, per shadowed light per fragment. max() floors the
    // divide so grazing (x->0) lands on the cap rather than on infinity.
    float slope = min(sqrt(1.0 - ndl * ndl) / max(ndl, 1e-3), PUNCTUAL_MAX_SLOPE);
    float texelWorld = punctualTexelScale[layer] * ls.w;
    ls = punctualShadowMatrix[layer] *
         vec4(worldPos + L * (texelWorld * (PUNCTUAL_BIAS_TEXELS * slope + PUNCTUAL_BIAS_FLOOR)),
              1.0);
    if (ls.w <= 0.0)
        return 1.0;

    vec3 pc = ls.xyz / ls.w * 0.5 + 0.5;
    if (pc.z > 1.0 || pc.x < 0.0 || pc.x > 1.0 || pc.y < 0.0 || pc.y > 1.0)
        return 1.0;

    // 3x3 PCF, which the directional cascades have always had and this never
    // did: a single tap quantizes the edge to the texel grid, and reads as a
    // staircase on any silhouette not aligned to it.
    //
    // Taps are clamped to the face. A point light's six faces meet on 45-degree
    // planes, and a fragment sitting near one has taps that fall OFF this face
    // -- where a 2D array has no neighbouring face to sample, only its border,
    // which reads lit. Unclamped that draws a straight lit seam across whatever
    // the boundary crosses, and it is immune to every depth remedy because no
    // depth comparison is involved: the tap simply misses the data. Clamping
    // re-reads this face's edge texel, which is the nearest depth that actually
    // exists; the true neighbour is a face away and unreachable without a
    // samplerCubeArray (GLSL 400, above this shader set's 330).
    vec2 texel = vec2(1.0 / PUNCTUAL_SHADOW_MAP_SIZE);

    // The map's full world width at the receiver, which turns any world length
    // into shadow-UV: one texel is texelWorld across and 1/size in UV.
    float frustumWorld = texelWorld * PUNCTUAL_SHADOW_MAP_SIZE;
    float sourceUV = sourceRadius / frustumWorld;

    if (sourceUV > texel.x) {
        // An emitter wider than a texel has a resolvable penumbra, so run the
        // PCSS pair: find what is blocking, then filter over the width that
        // blocker's distance implies.
        float zReceiver = ls.w;
        float blockerSum = 0.0;
        float blockerCount = 0.0;
        float searchUV = min(sourceUV, PUNCTUAL_MAX_PENUMBRA_UV);
        for (int i = 0; i < 16; ++i) {
            vec2 uv = clamp(pc.xy + POISSON16[i] * searchUV, texel * 0.5, 1.0 - texel * 0.5);
            float d = texture(punctualShadowMaps, vec3(uv, float(layer))).r;
            if (d < pc.z) {
                blockerSum += linearizePunctualDepth(d, punctualNearFar);
                blockerCount += 1.0;
            }
        }
        if (blockerCount < 0.5)
            return 1.0; // nothing between the fragment and the panel

        // Similar triangles: a source of half-width r at distance zR, blocked
        // at zB, throws a penumbra r * (zR - zB) / zB wide. This is the term
        // that makes contacts stay tight while a caster far from its receiver
        // goes soft, and it is what the 3x3 kernel cannot express at any size.
        float zBlocker = blockerSum / blockerCount;
        float penumbraWorld = sourceRadius * max(zReceiver - zBlocker, 0.0) / max(zBlocker, 1e-4);
        float filterUV =
            clamp(penumbraWorld / frustumWorld, texel.x, PUNCTUAL_MAX_PENUMBRA_UV);

        float soft = 0.0;
        for (int i = 0; i < 16; ++i) {
            vec2 uv = clamp(pc.xy + POISSON16[i] * filterUV, texel * 0.5, 1.0 - texel * 0.5);
            float d = texture(punctualShadowMaps, vec3(uv, float(layer))).r;
            soft += (pc.z > d) ? 0.0 : 1.0;
        }
        return mix(1.0, soft / 16.0, trust);
    }

    float sum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 uv = clamp(pc.xy + vec2(x, y) * texel, texel * 0.5, 1.0 - texel * 0.5);
            float d = texture(punctualShadowMaps, vec3(uv, float(layer))).r;
            sum += (pc.z > d) ? 0.0 : 1.0;
        }
    }
    return mix(1.0, sum / 9.0, trust);
}
