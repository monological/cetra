#ifndef LAYERS_GLSL
#define LAYERS_GLSL

#include "triplanar.glsl"
// authoredPos: the tiling lattice and the splat rectangle are both locked to the
// WORLD, so both read a position as an identity rather than as a location.
#include "world_origin.glsl"

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
 *
 * EVERY LOOP HERE IS BOUNDED BY THE CONSTANT, not by layerCount, and every tap
 * is textureGrad. Both are requirements rather than taste. A uniform trip count
 * leaves the local arrays dynamically indexed, which the GL 4.1 back-ends this
 * engine targets spill to scratch memory instead of registers; and an implicit
 * -LOD fetch inside fragment-varying control flow is UNDEFINED in GLSL 3.30,
 * which shows up as a wrong-mip band along every splat boundary and triplanar
 * seam. pbr_frag and stochastic.glsl both already hoist derivatives for exactly
 * this; this is the third consumer of that rule.
 */

// Four, and it is a vec4 rather than an array so there is no constant to keep in
// sync with MATERIAL_MAX_LAYERS -- a vec4 is four on both sides by definition.
#define LAYERS_MAX 4

// Width of the height-blend transition, in weight units. Narrow enough that one
// layer usually wins outright, which is the point: the wide blend is the mud
// this feature exists to avoid.
#define LAYER_BLEND_RANGE 0.12

uniform int layerCount; // 0 = not a layered surface; every consumer skips
uniform ivec4 layerAlbedoLayer;
uniform ivec4 layerSurfaceLayer;
uniform vec4 layerUvScale;         // world units per texture tile
uniform int splatLayer;            // -1 = no weights, layer 0 covers everything
uniform int splatSpace;            // 0 = UV1, 1 = world XZ (MaterialSplatSpace)
uniform vec4 splatDomain;          // world XZ origin.xy, size.zw
uniform float layerBlendSharpness; // 0 = a plain weighted average
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

// Where the splat is read, in its own space. See Material.splat_space: neither
// reading generalises, so the material states which one it means.
//
// The world rectangle is authored, and stays authored: reconstructing here is
// what lets `splat_origin` be a fixed property of the material rather than
// something an origin shift has to chase across every material in the scene.
vec2 layerSplatUV(vec3 worldPos, vec2 uv1) {
    if (splatSpace == 1)
        return (authoredPos(worldPos).xz - splatDomain.xy) / max(splatDomain.zw, vec2(1e-4));
    return uv1;
}

/*
 * Per-texel weights, from the splat map's rgb with layer 0 taking the remainder.
 *
 * Layer 0 is the remainder rather than a fourth channel so the weights cannot
 * fail to sum to one -- a four-channel splat has a redundant degree of freedom,
 * and every texel where it disagrees with itself is a texel that renders darker
 * or brighter than any of its layers.
 *
 * The inset is a HALF TEXEL, not a clamp to [0,1]. The array is GL_REPEAT for
 * the sake of the tiling layer maps, so a splat addressed exactly at 0 or 1
 * lands half a texel outside the edge texel's centre and bilinear wraps it
 * against the far side of the map. Clamping the coordinate does not fix that;
 * only insetting past the first texel centre does.
 */
vec4 layerWeights(sampler2DArray arr, vec2 splatUV, int n) {
    vec3 sp = vec3(0.0);
    if (splatLayer >= 0) {
        vec2 inset = 0.5 / vec2(textureSize(arr, 0).xy);
        sp = texture(arr, vec3(clamp(splatUV, inset, 1.0 - inset), float(splatLayer))).rgb;
    }
    // A layer the material does not have cannot claim weight; layer 0 absorbs it.
    if (n < 4)
        sp.z = 0.0;
    if (n < 3)
        sp.y = 0.0;
    if (n < 2)
        sp.x = 0.0;

    float rest = sp.x + sp.y + sp.z;
    vec4 w = vec4(max(1.0 - rest, 0.0), sp);
    // A splat whose channels oversubscribe still has to produce a convex blend.
    float sum = w.x + rest;
    return sum > 1.0 ? w * (1.0 / sum) : w;
}

LayerSurface sampleLayeredSurface(sampler2DArray arr, vec3 worldPos, vec3 worldNormal,
                                  vec2 uv1) {
    LayerSurface s = layerSurfaceNeutral(worldNormal);
    int n = min(layerCount, LAYERS_MAX);
    if (n <= 0)
        return s;

    vec4 w = layerWeights(arr, layerSplatUV(worldPos, uv1), n);
    vec3 tw = triplanarWeights(worldNormal, layerTriplanarSharpness);
    // The tiling lattice is locked to the world, so it reads the AUTHORED
    // position: a ground whose gravel slides a third of a tile because the
    // engine re-centred is a ground that is not locked to anything. Taken once
    // here rather than per layer -- the derivatives below are of the same value
    // and a constant offset does not change them.
    vec3 tilePos = authoredPos(worldPos);
    vec3 sgn = triplanarAxisSign(worldNormal);

    // The two world-position derivatives every tap's gradients are built from,
    // taken ONCE in fully uniform control flow. Everything below is affine in
    // worldPos, so no further dFdx is needed anywhere.
    vec3 dpx = dFdx(worldPos);
    vec3 dpy = dFdy(worldPos);

    // Pass one: the albedo taps, which also carry the HEIGHT the blend needs.
    // The blend cannot be decided before this, so the surface maps wait for pass
    // two -- where most layers have already been weighted to zero and skipped.
    vec4 alb[LAYERS_MAX];
    vec3 p[LAYERS_MAX];
    for (int i = 0; i < LAYERS_MAX; i++) {
        alb[i] = vec4(1.0, 1.0, 1.0, 0.5);
        float s2 = 1.0 / max(layerUvScale[i], 1e-4);
        p[i] = tilePos * s2;
        if (w[i] <= 0.0 || layerAlbedoLayer[i] < 0)
            continue;
        alb[i] = triplanarSampleArray(arr, float(layerAlbedoLayer[i]), p[i], tw, sgn,
                                      dpx * s2, dpy * s2);
    }

    /*
     * The blend, as ONE expression rather than two limbs.
     *
     * `cut` is 0 when sharpness is 0, and at cut 0 the height form collapses to
     * the plain weighted average exactly: `alb.a * 0 == 0`, `w + 0 == w`, and
     * `max(w, 0) == w` because every weight is non-negative by construction. So
     * the linear case the gate compares against is an exact identity rather than
     * a limit, with no second code path to keep in step.
     *
     * Note the parameter is DISCONTINUOUS at 0 and it is worth knowing before
     * dragging a slider: the sharpness -> 0+ limit of the height form is a hard
     * LAYER_BLEND_RANGE window around the peak, not the linear blend. Only
     * sharpness exactly 0 is linear.
     */
    float cut = 0.0;
    if (layerBlendSharpness > 0.0) {
        float peak = -1e9;
        for (int i = 0; i < LAYERS_MAX; i++)
            if (w[i] > 0.0)
                peak = max(peak, w[i] + alb[i].a * layerBlendSharpness);
        cut = peak - LAYER_BLEND_RANGE;
    }

    float total = 0.0;
    vec4 b = vec4(0.0);
    for (int i = 0; i < LAYERS_MAX; i++) {
        if (w[i] > 0.0)
            b[i] = max(w[i] + alb[i].a * layerBlendSharpness - cut, 0.0);
        total += b[i];
    }
    if (total <= 0.0)
        return s;
    float inv = 1.0 / total;

    // Pass two. The albedo blends in STORED codes and the caller decodes once
    // rather than four decodes here -- the height blend hands almost every pixel
    // to a single layer, so the band where the space could matter is a couple of
    // texels wide. Note this does NOT extend to the mip chain, which averages
    // the same codes at every minified texel; see material_texture_array.h.
    vec3 albedo = vec3(0.0);
    vec3 normal = vec3(0.0);
    float rough = 0.0;
    float occ = 0.0;
    for (int i = 0; i < LAYERS_MAX; i++) {
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

        float s2 = 1.0 / max(layerUvScale[i], 1e-4);
        vec4 sx, sy, sz;
        triplanarTapsArray(arr, float(layerSurfaceLayer[i]), p[i], tw, sgn, dpx * s2, dpy * s2,
                           sx, sy, sz);

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
