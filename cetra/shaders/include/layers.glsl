#ifndef LAYERS_GLSL
#define LAYERS_GLSL

#include "triplanar.glsl"

/*
 * Layered surfaces, spec 11.60.
 *
 * N material layers blended per texel, where the layers live as tenants of the
 * material texture array and cost the program ZERO new sampler declarations --
 * 11.45's rule (ocean.glsl) says a cap counted in declarations is a cap on how
 * many distinct SHAPES of data a program reads, and a layer map is the same
 * shape as a mask.
 *
 * Two textures per layer, because the array is RGBA8 and the alpha channel of
 * each is otherwise wasted:
 *   albedo  rgb = albedo in STORED codes, a = height
 *   surface rg  = normal xy, b = roughness, a = ambient occlusion
 *
 * The normal is two-channel with z reconstructed. That is not a compression
 * compromise: it is what pays for the roughness and the AO, and a tangent normal
 * is a unit vector whose z is positive by construction, so the third channel was
 * never carrying information.
 */

// Must equal MATERIAL_MAX_LAYERS in material.h. Two copies rather than a
// generated constant because it is one integer, in the same arrangement
// STOCHASTIC_LUT_SIZE already lives in -- but the number is load-bearing on both
// sides, so a change to either is a change to both.
#define LAYERS_MAX 4

// Width of the height-blend transition, in weight units. Narrow enough that one
// layer usually wins outright, which is the point: the wide blend is the mud
// this feature exists to avoid.
#define LAYER_BLEND_RANGE 0.12

uniform int layerCount; // 0 = not a layered surface; every consumer skips
uniform int layerAlbedoLayer[LAYERS_MAX];
uniform int layerSurfaceLayer[LAYERS_MAX];
uniform float layerUvScale[LAYERS_MAX]; // world units per texture tile
uniform int splatLayer;                 // -1 = no weights, layer 0 covers everything
uniform float layerBlendSharpness;      // 0 = a plain weighted average
uniform float layerTriplanarSharpness;

struct LayerSurface {
    vec3 albedo; // STORED codes, not linear -- the caller decodes (see below)
    vec3 normal; // world space
    float roughness;
    float ao;
};

LayerSurface layerSurfaceNeutral(vec3 worldNormal) {
    LayerSurface s;
    s.albedo = vec3(1.0);
    s.normal = worldNormal;
    s.roughness = 1.0;
    s.ao = 1.0;
    return s;
}

// Per-texel weights, from the splat map's rgb with layer 0 taking the remainder.
//
// Layer 0 is the remainder rather than a fourth channel so the weights cannot
// fail to sum to one -- a four-channel splat has a redundant degree of freedom,
// and every texel where it disagrees with itself is a texel that renders darker
// or brighter than any of its layers.
void layerWeights(sampler2DArray arr, vec2 splatUV, int n, out float w[LAYERS_MAX]) {
    vec3 sp = vec3(0.0);
    if (splatLayer >= 0) {
        // Clamped because the array is GL_REPEAT for the sake of the tiling
        // layer maps, and a splat map is addressed over [0,1] exactly once. One
        // texel past the edge would wrap to the far side of the terrain.
        sp = texture(arr, vec3(clamp(splatUV, 0.0, 1.0), float(splatLayer))).rgb;
    }

    float rest = 0.0;
    for (int i = 0; i < LAYERS_MAX; i++)
        w[i] = 0.0;
    for (int i = 1; i < n; i++) {
        w[i] = sp[i - 1];
        rest += sp[i - 1];
    }
    w[0] = max(1.0 - rest, 0.0);

    // A splat whose channels oversubscribe still has to produce a convex blend.
    float sum = w[0] + rest;
    if (sum > 1.0) {
        float inv = 1.0 / sum;
        for (int i = 0; i < n; i++)
            w[i] *= inv;
    }
}

LayerSurface sampleLayeredSurface(sampler2DArray arr, vec3 worldPos, vec3 worldNormal,
                                  vec2 splatUV) {
    LayerSurface s = layerSurfaceNeutral(worldNormal);
    int n = min(layerCount, LAYERS_MAX);
    if (n <= 0)
        return s;

    float w[LAYERS_MAX];
    layerWeights(arr, splatUV, n, w);

    vec3 tw = triplanarWeights(worldNormal, layerTriplanarSharpness);
    vec3 sgn = triplanarAxisSign(worldNormal);

    // Pass one: the albedo taps, which also carry the HEIGHT the blend needs.
    // The blend cannot be decided before this, so the surface maps wait for pass
    // two -- where most layers have already been weighted to zero and skipped.
    vec4 alb[LAYERS_MAX];
    for (int i = 0; i < LAYERS_MAX; i++)
        alb[i] = vec4(1.0, 1.0, 1.0, 0.5);
    for (int i = 0; i < n; i++) {
        if (w[i] <= 0.0 || layerAlbedoLayer[i] < 0)
            continue;
        vec3 p = worldPos / max(layerUvScale[i], 1e-4);
        alb[i] = triplanarSampleArray(arr, float(layerAlbedoLayer[i]), p, tw, sgn);
    }

    // The blend itself. Sharpness 0 is a plain weighted average and is EXACT
    // rather than a limit of the height form -- the gate has to be able to
    // compare the interlock against the naive blend, and a naive leg that is
    // merely nearly-linear cannot separate the two.
    float b[LAYERS_MAX];
    float total = 0.0;
    if (layerBlendSharpness > 0.0) {
        float peak = -1e9;
        for (int i = 0; i < n; i++) {
            if (w[i] <= 0.0)
                continue;
            peak = max(peak, w[i] + alb[i].a * layerBlendSharpness);
        }
        float cut = peak - LAYER_BLEND_RANGE;
        for (int i = 0; i < LAYERS_MAX; i++)
            b[i] = 0.0;
        for (int i = 0; i < n; i++) {
            if (w[i] <= 0.0)
                continue;
            b[i] = max(w[i] + alb[i].a * layerBlendSharpness - cut, 0.0);
            total += b[i];
        }
    } else {
        for (int i = 0; i < LAYERS_MAX; i++)
            b[i] = 0.0;
        for (int i = 0; i < n; i++) {
            b[i] = w[i];
            total += b[i];
        }
    }
    if (total <= 0.0)
        return s;
    float inv = 1.0 / total;

    // Pass two. The albedo blends in STORED codes and the caller decodes once,
    // rather than four decodes here: the height blend hands almost every pixel
    // to a single layer, so the band where the space could matter is a couple of
    // texels wide, and the stochastic path already establishes that a decode
    // belongs at the call site.
    vec3 albedo = vec3(0.0);
    vec3 normal = vec3(0.0);
    float rough = 0.0;
    float occ = 0.0;
    for (int i = 0; i < n; i++) {
        if (b[i] <= 0.0)
            continue;
        float k = b[i] * inv;
        albedo += k * alb[i].rgb;

        if (layerSurfaceLayer[i] < 0) {
            normal += k * worldNormal;
            rough += k;
            occ += k;
            continue;
        }

        vec3 p = worldPos / max(layerUvScale[i], 1e-4);
        float layer = float(layerSurfaceLayer[i]);
        vec4 sx = tw.x > 0.0 ? texture(arr, vec3(triplanarUvX(p, sgn), layer)) : vec4(0.5, 0.5, 1.0, 1.0);
        vec4 sy = tw.y > 0.0 ? texture(arr, vec3(triplanarUvY(p, sgn), layer)) : vec4(0.5, 0.5, 1.0, 1.0);
        vec4 sz = tw.z > 0.0 ? texture(arr, vec3(triplanarUvZ(p, sgn), layer)) : vec4(0.5, 0.5, 1.0, 1.0);

        // Reconstruct each projection's tangent normal from two channels.
        vec3 tnx = vec3(sx.rg * 2.0 - 1.0, 0.0);
        vec3 tny = vec3(sy.rg * 2.0 - 1.0, 0.0);
        vec3 tnz = vec3(sz.rg * 2.0 - 1.0, 0.0);
        tnx.z = sqrt(max(1.0 - dot(tnx.xy, tnx.xy), 0.0));
        tny.z = sqrt(max(1.0 - dot(tny.xy, tny.xy), 0.0));
        tnz.z = sqrt(max(1.0 - dot(tnz.xy, tnz.xy), 0.0));

        normal += k * triplanarBlendNormal(tnx, tny, tnz, worldNormal, tw, sgn);
        rough += k * (tw.x * sx.b + tw.y * sy.b + tw.z * sz.b);
        occ += k * (tw.x * sx.a + tw.y * sy.a + tw.z * sz.a);
    }

    s.albedo = albedo;
    // Blended world normals, so a length below one means the layers disagreed
    // about which way the surface faces -- normalising is what makes that read
    // as a flatter surface rather than as a shorter one.
    s.normal = normalize(normal);
    s.roughness = rough;
    s.ao = occ;
    return s;
}

#endif // LAYERS_GLSL
