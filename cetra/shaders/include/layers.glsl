#ifndef LAYERS_GLSL
#define LAYERS_GLSL

#include "triplanar.glsl"
// authoredPos: the tiling lattice and the splat rectangle are both locked to the
// WORLD, so both read a position as an identity rather than as a location.
#include "world_origin.glsl"
// The composite cache's page table (spec 11.67) -- a UBO, not a sampler.
#include "vt_pages.glsl"
// Road segments (spec 11.68) -- a UBO for the same reason.
#include "roads_ubo.glsl"

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
// Whether this material is the scene's road bearer (spec 11.68). Uploaded for
// every layered material, so it is also what clears the override on the others.
uniform int roadsActive;
// Bake-only (spec 11.66): every layer tap reads its own top mip -- its mean --
// so the composite the cache stores carries no tile grain at ANY atlas
// resolution. Without this the runtime detail ratio would re-apply grain the
// atlas already resolved, squaring the pattern wherever the two densities meet.
// The splat tap is untouched: it goes through texture() above, and the splat IS
// the content being cached.
uniform int layerGrainFrozen;

struct LayerSurface {
    vec3 albedo; // STORED codes, not linear -- the caller decodes (see below)
    vec3 normal; // world space
    float roughness;
    float ao;
    float dominant; // index of the layer with the largest blend weight
};

LayerSurface layerSurfaceNeutral(vec3 worldNormal) {
    LayerSurface s;
    s.albedo = vec3(1.0);
    s.normal = worldNormal;
    s.roughness = 1.0;
    s.ao = 1.0;
    s.dominant = 0.0;
    return s;
}

// The surface map's two-channel tangent normal, z reconstructed. ONE decode of
// the storage format: it is a contract with the array packing and the bake,
// and it used to be hand-written at seven sites in this file.
vec3 layerTangentNormal(vec2 rg) {
    vec3 t = vec3(rg * 2.0 - 1.0, 0.0);
    t.z = sqrt(max(1.0 - dot(t.xy, t.xy), 0.0));
    return t;
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

/*
 * Roads (spec 11.68): a procedural override of the splat weights, applied
 * BEFORE the height blend.
 *
 * Before, not after, and that is the whole design. A road blended in afterwards
 * is a stripe painted over the ground; reshaped here it is MADE of one of the
 * material's own layers, so the shoulder height-interlocks with what it runs
 * over exactly as a painted layer boundary does, the dominant index follows it,
 * and the composite bake inherits roads because it calls this same function
 * unchanged. Gravel that interlocks with the grass it displaces is the whole
 * argument for the height blend, and a road is the case that wants it most.
 *
 * Positions are AUTHORED: a road is an identity, not a location, and it may not
 * slide when the engine re-centres.
 *
 * The loops are constant-bounded with early breaks, which is the file's rule
 * above satisfied rather than dodged -- they index UBO arrays, never the local
 * arrays that rule is about (iesProfile walks a uniform block the same way).
 * The lane write is a constant-lane compare rather than w[L], because THAT
 * would be the dynamic local index the rule forbids.
 *
 * The CPU twin of the segment distance is roads_polyline_distance_xz (roads.c);
 * the two cannot share a token, and the road gate arms are what tie them.
 */
vec4 roadReshapedWeights(vec4 w, vec2 apos, int n) {
    if (roadsActive <= 0 || roadsInfo.x <= 0)
        return w;
    for (int r = 0; r < ROADS_MAX; r++) {
        if (r >= roadsInfo.x)
            break;
        vec4 d0 = roadDesc[r * 2];
        int first = int(d0.x);
        int segs = int(d0.y);
        float dist = 1.0e9;
        for (int s = 0; s < ROAD_SEGS_PER_ROAD; s++) {
            if (s >= segs)
                break;
            vec4 seg = roadSegs[first + s];
            vec2 ab = seg.zw - seg.xy;
            float t = clamp(dot(apos - seg.xy, ab) / max(dot(ab, ab), 1e-8), 0.0, 1.0);
            dist = min(dist, length(apos - (seg.xy + ab * t)));
        }
        // Feather 0 is a hard edge, which smoothstep gives for free: the two
        // edges coincide and it steps.
        float m = 1.0 - smoothstep(d0.z, d0.z + d0.w, dist);
        if (m <= 0.0)
            continue;
        int lane = clamp(int(roadDesc[r * 2 + 1].x), 0, n - 1);
        // Convexity survives: a mix of two vectors that each sum to one sums to
        // one, which is what the whole blend below rests on.
        w = w * (1.0 - m) + vec4(equal(ivec4(0, 1, 2, 3), ivec4(lane))) * m;
    }
    return w;
}

LayerSurface sampleLayeredSurface(sampler2DArray arr, vec3 worldPos, vec3 worldNormal,
                                  vec2 uv1) {
    LayerSurface s = layerSurfaceNeutral(worldNormal);
    int n = min(layerCount, LAYERS_MAX);
    if (n <= 0)
        return s;

    // The tiling lattice is locked to the world, so it reads the AUTHORED
    // position: a ground whose gravel slides a third of a tile because the
    // engine re-centred is a ground that is not locked to anything. Taken once
    // here rather than per layer -- the derivatives below are of the same value
    // and a constant offset does not change them, and the roads read it too.
    vec3 tilePos = authoredPos(worldPos);

    vec4 w = layerWeights(arr, layerSplatUV(worldPos, uv1), n);
    w = roadReshapedWeights(w, tilePos.xz, n);
    vec3 tw = triplanarWeights(worldNormal, layerTriplanarSharpness);
    vec3 sgn = triplanarAxisSign(worldNormal);

    // The two world-position derivatives every tap's gradients are built from,
    // taken ONCE in fully uniform control flow. Everything below is affine in
    // worldPos, so no further dFdx is needed anywhere. Frozen grain replaces
    // them with a footprint far past any tile, driving every textureGrad below
    // to its top mip.
    vec3 dpx = layerGrainFrozen > 0 ? vec3(1.0e6, 0.0, 0.0) : dFdx(worldPos);
    vec3 dpy = layerGrainFrozen > 0 ? vec3(0.0, 0.0, 1.0e6) : dFdy(worldPos);

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
    float bestB = -1.0;
    for (int i = 0; i < LAYERS_MAX; i++) {
        if (w[i] > 0.0)
            b[i] = max(w[i] + alb[i].a * layerBlendSharpness - cut, 0.0);
        total += b[i];
        if (b[i] > bestB) {
            bestB = b[i];
            s.dominant = float(i);
        }
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
        vec3 tnx = layerTangentNormal(sx.rg);
        vec3 tny = layerTangentNormal(sy.rg);
        vec3 tnz = layerTangentNormal(sz.rg);

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

/*
 * The composite-cache read (spec 11.66): the blend above, pre-resolved into two
 * 2D textures over the material's world-XZ splat domain, plus ONE triplanar
 * detail tap of the dominant layer's own maps at full tiling density.
 *
 * The macro holds the blend with every layer at its tile MEAN (the bake's
 * layerGrainFrozen), and the detail term is the dominant layer's map over that
 * same mean -- so on a flat map the ratio is exactly one and the macro passes
 * through byte-exact, and where the atlas is coarse the grain still arrives at
 * full density because it never lived in the atlas at all.
 */
uniform int layersVtActive; // 0 = take the per-texel blend above
uniform vec3 layerMeanAlbedo[LAYERS_MAX]; // per-layer top-mip means, STORED codes
uniform vec4 layerMeanRough;              // component i = layer i's mean roughness
uniform vec4 layerMeanAo;

LayerSurface sampleCachedSurface(sampler2D vtAlbedo, sampler2D vtSurface,
                                 sampler2D vtPageAlbedo, sampler2D vtPageSurface,
                                 sampler2DArray arr, vec3 worldPos, vec3 worldNormal) {
    LayerSurface s = layerSurfaceNeutral(worldNormal);
    int n = min(layerCount, LAYERS_MAX);
    if (n <= 0)
        return s;

    // THE splat mapping, not a restatement of it -- the cache is armed only
    // for world-XZ materials, so the helper's world arm is the one taken.
    // CLAMP_TO_EDGE on the cache makes the half-texel inset the splat read
    // itself needs unnecessary here.
    vec2 uv = layerSplatUV(worldPos, vec2(0.0));
    // Hoisted here, in fully uniform flow, because three consumers need them:
    // the page footprint and gradients below, and the detail taps at the end.
    vec3 dpx = dFdx(worldPos);
    vec3 dpy = dFdy(worldPos);
    // Implicit LOD on purpose, against this file's own rule: the caller's gate
    // is a uniform, so this is uniform control flow, and implicit gradients are
    // what keep the cache's anisotropic filtering intact.
    vec4 macroA = texture(vtAlbedo, uv);
    vec4 macroS = texture(vtSurface, uv);

    /*
     * The paged near field (spec 11.67). A resident page holds the SAME macro
     * at VT_PAGE_DENSITY_RATIO x the density, so it takes over exactly where
     * the fallback is magnified -- a footprint under 4 page texels -- and hands
     * back over smoothstep(2, 4), where page mip 2 and fallback mip 0 are the
     * same signal at the same effective density: the band is invisible by
     * construction, not by tuning. textureGrad, never implicit LOD: atlas UV
     * is discontinuous at page borders, and the gutter inset keeps bilinear and
     * the capped trilinear inside the tile. The dominant-layer index below
     * stays FALLBACK-owned -- a page/fallback index disagreement inside the
     * band would flicker the detail term through the blend.
     */
    {
        vec2 pf;
        int slot = vtPageSlot(uv, splatDomain.zw, pf);
        float usable = float(vtPageInfo.z - 2 * vtPageInfo.w);
        float pageTexel = vtPageParams.y / max(usable, 1.0);
        vec2 fp = vec2(length(dpx.xz), length(dpy.xz)) / pageTexel;
        // The band's endpoints ARE the density ratio and its half, uploaded
        // rather than restated: pages beat the fallback exactly while the
        // fallback is magnified, i.e. under one fallback texel = ratio page
        // texels, and at the far end page mip 2 meets fallback mip 0.
        float ratio = vtPageParams.z;
        float pageW = 1.0 - smoothstep(0.5 * ratio, ratio, max(fp.x, fp.y));
        if (slot >= 0 && pageW > 0.0) {
            vec2 slotOrigin = vec2(ivec2(slot % vtPageInfo.y, slot / vtPageInfo.y)
                                   * vtPageInfo.z);
            vec2 texel = slotOrigin + float(vtPageInfo.w) + fract(pf) * usable;
            vec2 auv = texel / vtPageParams.x;
            vec2 gx = dpx.xz / (pageTexel * vtPageParams.x);
            vec2 gy = dpy.xz / (pageTexel * vtPageParams.x);
            vec4 pageA = textureGrad(vtPageAlbedo, auv, gx, gy);
            vec4 pageS = textureGrad(vtPageSurface, auv, gx, gy);
            // rgb only: macroA.a is the dominant index, fallback-owned.
            macroA.rgb = mix(macroA.rgb, pageA.rgb, pageW);
            macroS = mix(macroS, pageS, pageW);
        }
    }

    // The dominant layer index rides the alpha and is read UNFILTERED: bilinear
    // between two indices passes through every index between them, and a mip
    // average of indices is not an index at all.
    ivec2 vtSize = textureSize(vtAlbedo, 0);
    ivec2 tc = clamp(ivec2(uv * vec2(vtSize)), ivec2(0), vtSize - ivec2(1));
    int k = clamp(int(round(texelFetch(vtAlbedo, tc, 0).a * 3.0)), 0, n - 1);

    // The macro's normal, composed onto the geometric normal exactly as a
    // Y-projection tangent normal is -- which is what the bake stored it as.
    vec3 mtn = layerTangentNormal(macroS.rg);
    vec3 tw = triplanarWeights(worldNormal, layerTriplanarSharpness);
    vec3 sgn = triplanarAxisSign(worldNormal);
    vec3 normal = triplanarBlendNormal(mtn, mtn, mtn, worldNormal, vec3(0.0, 1.0, 0.0), sgn);

    vec3 albedo = macroA.rgb;
    float rough = macroS.b;
    float occ = macroS.a;

    // The detail term. k is fragment-varying, so from here on the file's
    // textureGrad rule is load-bearing again; the gradients were hoisted at
    // the top, in flow that was still uniform.
    float s2 = 1.0 / max(layerUvScale[k], 1e-4);
    vec3 p = authoredPos(worldPos) * s2;

    int ka = layerAlbedoLayer[k];
    if (ka >= 0) {
        vec4 d = triplanarSampleArray(arr, float(ka), p, tw, sgn, dpx * s2, dpy * s2);
        albedo = min(albedo * (d.rgb / max(layerMeanAlbedo[k], vec3(1.0 / 255.0))), vec3(1.0));
    }

    int ks = layerSurfaceLayer[k];
    if (ks >= 0) {
        vec4 sx, sy, sz;
        triplanarTapsArray(arr, float(ks), p, tw, sgn, dpx * s2, dpy * s2, sx, sy, sz);
        vec3 tnx = layerTangentNormal(sx.rg);
        vec3 tny = layerTangentNormal(sy.rg);
        vec3 tnz = layerTangentNormal(sz.rg);
        // Detail composes onto the macro-composed normal, not the geometric
        // one, so the two perturbations stack instead of competing.
        normal = triplanarBlendNormal(tnx, tny, tnz, normal, tw, sgn);
        // Roughness and AO are ADDITIVE deviations from the layer mean, where
        // the albedo is a ratio: an additive grain survives a dark mean, and a
        // ratio near zero would amplify quantisation instead of detail.
        rough = clamp(rough + (tw.x * sx.b + tw.y * sy.b + tw.z * sz.b) - layerMeanRough[k],
                      0.0, 1.0);
        occ = clamp(occ + (tw.x * sx.a + tw.y * sy.a + tw.z * sz.a) - layerMeanAo[k], 0.0, 1.0);
    }

    s.albedo = albedo;
    // Already unit length: on both paths `normal` is triplanarBlendNormal's
    // return, which normalizes -- unlike the per-texel blend above, which sums
    // weighted normals and must renormalize.
    s.normal = normal;
    s.roughness = rough;
    s.ao = occ;
    s.dominant = float(k);
    return s;
}

#endif // LAYERS_GLSL
