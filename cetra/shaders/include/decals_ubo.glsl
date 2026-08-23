// Clustered decals (spec 11.73): marks projected onto whatever surface lies
// inside their box, selected by a per-froxel mask and composited onto the
// surface values before any lighting runs.
//
// The atlas is a PARAMETER for the reason probe_specular.glsl's is: a decal
// image is a tenant of the material texture array, which pbr_frag declares as
// materialArray, and passing it keeps this file free of a seventeenth sampler
// in a program that has sixteen.
//
// What a decal is made of lives here and in decal.c. The rest of the engine
// knows only that decals exist.

layout(std140) uniform DecalBlock {
    ivec4 decalInfo; // x count (0 = disarmed), yzw unused
    // [5i + 0..2] world->local rows, each scaled by 1/half_extent with the
    //             translation in .w -- so local xyz is in [-1,1] inside the box
    // [5i + 3]    x albedo layer, y surface layer (-1 = none), z opacity,
    //             w cos(angle fade)
    // [5i + 4]    x edge feather (local units), y normal strength, zw unused
    vec4 decalDesc[16 * 5];
    // One 16-bit mask per froxel, two to a word. std140 gives a scalar array a
    // vec4 stride, so a uint[3072] would be four times this block.
    uvec4 decalClusterMasks[384];
};

// Must match DECAL_MAX (decal.h). Held by the driver's own std140 size check
// against UBO_DECALS_BLOCK_SIZE, which decalDesc's declaration rides.
const int DECAL_MAX = 16;

// Which decals reach this froxel. The decode is lightIndexAt's halfword unpack
// rather than probeMaskAt's byte one, because the mask is twice as wide.
uint decalMaskAt(uint ci) {
    return (decalClusterMasks[ci >> 3u][(ci >> 1u) & 3u] >> ((ci & 1u) * 16u)) & 0xFFFFu;
}

// What a fragment accumulated from every decal that reached it, RESOLVED --
// every value is what it means, and the two alphas say how much of it to take.
//
// The accumulation inside is premultiplied and the divide happens once before
// this is returned, which is probeSetSpecular's arrangement (`sum / total`) and
// LayerSurface's. Handing back premultiplied data instead needs a warning
// comment telling every consumer to divide, and the branch shipped exactly the
// bug that warning describes: a decal at opacity 0.5 was wrong everywhere it
// drew, with six arms green. A type whose obvious use is wrong is the defect.
//
// Albedo is carried in STORED sRGB codes, not linear: the caller decodes once
// after the blend, which is the layered-surface arrangement (layers.glsl) and
// exists because the material array is linear RGBA8 and holds the codes as
// authored.
//
// The normal is WORLD-space, already rotated out of the decal's own tangent
// frame -- so where a decal is opaque its relief REPLACES the surface's rather
// than stacking on it, which is what a poster does to the wall's plaster. That
// is why it is not the whiteout compose layers.glsl uses for macro over detail:
// there the two perturbations describe one surface, here the mark is a second
// surface lying on top of the first.
struct DecalSurface {
    vec3 albedo;
    float alpha;
    vec3 normal;
    float roughness;
    float occlusion;
    float surfaceAlpha; // coverage from decals that carried a surface map
};

// The accumulator's seed, and the identity a fragment with no decal gets back:
// zero coverage, so every caller-side mix is a no-op.
//
// Zero rather than any neutral value, INCLUDING the normal, because the sum
// below runs premultiplied -- a +Z seed would be a phantom contribution that a
// partly-covering decal blends against. What a zero-coverage channel should
// READ as is decided in the resolve at the end, not here.
DecalSurface decalSurfaceNone() {
    return DecalSurface(vec3(0.0), 0.0, vec3(0.0), 0.0, 0.0, 0.0);
}

/*
 * Accumulate every decal covering this fragment, front of the list to the back
 * of it -- authored order is paint order, so a later decal draws over an
 * earlier one.
 *
 * The loop is bounded by a CONSTANT and broken on a dynamically uniform count,
 * the probeSetSpecular shape: a scene with no decals takes one branch, and no
 * fragment can be made to iterate more than the authored cap however the boxes
 * overlap. There is deliberately no per-pixel cap under that -- a silent
 * truncation would make a mark vanish where two others happened to cover it --
 * so the cap IS the bound, and it is the one the author chose.
 *
 * Every cheap reject runs before any fetch: the mask bit, the box test, the
 * facing test, then the edge. `ng` is the GEOMETRIC normal rather than the
 * mapped one, because what the facing test asks is which way the SURFACE lies,
 * and a normal map answering that question would let relief punch holes in a
 * poster.
 *
 * Taps are textureGrad with gradients carried in from the caller: the projected
 * coordinate is discontinuous at a box edge and the loop is divergent, where an
 * implicit-LOD fetch is undefined in GLSL 3.30.
 */
DecalSurface decalAccumulate(sampler2DArray atlas, uint mask, vec3 worldPos, vec3 ng,
                             vec3 ddxWorld, vec3 ddyWorld) {
    DecalSurface acc = decalSurfaceNone();
    if (decalInfo.x <= 0 || mask == 0u)
        return acc;

    // Half a texel of the array's own size, so a tap at the very edge of an
    // image cannot bilinear against the opposite edge: the array wraps with
    // GL_REPEAT, and every layer shares one canonical size.
    //
    // DEFENSIVE, and measured as such: removing it moves 0 px on decal_fixture,
    // whose images carry a transparent margin -- a wrapped blend between two
    // transparent texels is still transparent. What it protects is an image
    // opaque to its own border, where the band is half a texel wide and the
    // colour comes from the far side of the mark. No arm covers it; see the
    // decals gate's docstring for why one was written and then removed.
    vec2 inset = 0.5 / vec2(textureSize(atlas, 0).xy);

    for (int i = 0; i < DECAL_MAX; ++i) {
        if (i >= decalInfo.x)
            break;
        if ((mask & (1u << uint(i))) == 0u)
            continue;

        vec4 r0 = decalDesc[i * 5 + 0];
        vec4 r1 = decalDesc[i * 5 + 1];
        vec4 r2 = decalDesc[i * 5 + 2];
        vec4 p0 = decalDesc[i * 5 + 3];
        vec4 p1 = decalDesc[i * 5 + 4];

        vec4 wp = vec4(worldPos, 1.0);
        vec3 local = vec3(dot(r0, wp), dot(r1, wp), dot(r2, wp));
        if (any(greaterThan(abs(local), vec3(1.0))))
            continue;

        // Local +Z is the way the projector faces, so a surface takes the mark
        // where its normal opposes that. Outside the authored angle it takes
        // none -- which is what stops a projector smearing its image down every
        // wall it happens to graze.
        vec3 axis = normalize(vec3(r2.x, r2.y, r2.z));
        float facing = dot(normalize(ng), -axis);
        // The smoothstep below is what REJECTS -- it returns 0 for any facing
        // at or under the cutoff -- so this compare saves the fetches rather
        // than deciding anything. Worth knowing before mutating it to prove the
        // fade works: deleting this line changes no pixel.
        if (facing <= p0.w)
            continue;
        // Ramp over the last of the authored angle rather than switching, or a
        // curved surface takes a hard-edged bite out of the mark.
        float angleWeight = smoothstep(p0.w, mix(p0.w, 1.0, 0.25), facing);

        // The edge, feathered inward from all three faces in the box's own
        // units. Depth counts: a mark should fade out as the surface it lands
        // on leaves the volume, not stop dead at the back plane.
        float feather = max(p1.x, 1e-4);
        vec3 edge = (vec3(1.0) - abs(local)) / feather;
        float edgeWeight = clamp(min(edge.x, min(edge.y, edge.z)), 0.0, 1.0);

        // v is NEGATED and u is not, because the two axes disagree about which
        // way is forward: row1 is the decal's up in the WORLD, where v runs DOWN
        // an image from row 0. Without it the picture arrives upside down --
        // which, together with a mirrored `right`, is a clean 180-degree
        // rotation that a symmetric test image cannot see.
        const vec2 uvAxis = vec2(0.5, -0.5);
        vec2 uv = clamp(local.xy * uvAxis + 0.5, inset, 1.0 - inset);
        // The projected coordinate's gradients, by chain rule through the same
        // rows that produced it -- exact, because the projection is affine, and
        // carrying uvAxis so the v gradient keeps the sign its coordinate has.
        vec2 duvdx = vec2(dot(r0.xyz, ddxWorld), dot(r1.xyz, ddxWorld)) * uvAxis;
        vec2 duvdy = vec2(dot(r0.xyz, ddyWorld), dot(r1.xyz, ddyWorld)) * uvAxis;

        vec4 tex = textureGrad(atlas, vec3(uv, p0.x), duvdx, duvdy);
        float a = tex.a * p0.z * angleWeight * edgeWeight;
        if (a <= 0.0)
            continue;

        // Over, PREMULTIPLIED, in stored codes. Written as the explicit sum
        // rather than mix() only to say so out loud -- the two are the same
        // expression, and reading the mix form as "not premultiplied" is what
        // hid the defect for a round.
        //
        // The coverage is divided back out in the resolve at the end, so this
        // form never leaves the function. Blending a premultiplied value by its
        // own alpha applies the coverage twice, which darkens a mark toward its
        // feathered edge instead of fading it to the surface under it -- and is
        // invisible at a == 1, which is every opaque interior and so every
        // reading that looks right. That shipped.
        acc.albedo = tex.rgb * a + acc.albedo * (1.0 - a);
        acc.alpha = a + acc.alpha * (1.0 - a);

        if (p0.y >= 0.0) {
            vec4 surf = textureGrad(atlas, vec3(uv, p0.y), duvdx, duvdy);
            vec3 tn = layerTangentNormal(surf.rg);
            // Strength scales the perturbation, not the normal: flattening xy
            // and renormalising is what "less relief" means.
            tn = normalize(vec3(tn.xy * p1.y, max(tn.z, 1e-4)));
            // Out of the decal's own tangent frame -- u along row0, v along
            // row1, n along the facing direction -- into world space, where the
            // caller mixes it against the surface normal by surfaceAlpha.
            vec3 t = normalize(vec3(r0.x, r0.y, r0.z));
            vec3 b = normalize(vec3(r1.x, r1.y, r1.z));
            vec3 worldNormal = normalize(t * tn.x + b * tn.y + (-axis) * tn.z);
            // Premultiplied too, and for the albedo's reason. The normal needs
            // no un-premultiply because normalize removes the scale, but it is
            // accumulated the same way so overlapping marks weigh correctly.
            acc.normal = worldNormal * a + acc.normal * (1.0 - a);
            acc.roughness = surf.b * a + acc.roughness * (1.0 - a);
            acc.occlusion = surf.a * a + acc.occlusion * (1.0 - a);
            acc.surfaceAlpha = a + acc.surfaceAlpha * (1.0 - a);
        }
    }

    /*
     * RESOLVE, here and once, so nothing outside this file can hold a
     * premultiplied value and use it by mistake. probeSetSpecular's shape.
     *
     * The zero-coverage answers differ by what each channel IS: occlusion
     * multiplies, so its neutral is 1; roughness replaces, so it has no neutral
     * this side of the substrate and the caller's mix weight of 0 is what
     * actually protects it.
     */
    if (acc.alpha > 0.0)
        acc.albedo /= acc.alpha;
    if (acc.surfaceAlpha > 0.0) {
        acc.roughness /= acc.surfaceAlpha;
        acc.occlusion /= acc.surfaceAlpha;
        // Normalised here rather than by the caller, and GUARDED: two marks
        // whose relief resolves to opposed world normals cancel exactly, and
        // normalize(0) is a NaN straight into the normals target -- the failure
        // triplanar.glsl's floored cutoff exists to prevent.
        // Falls back to the surface's own normal, which makes the caller's mix
        // a no-op rather than a wrong answer.
        float len2 = dot(acc.normal, acc.normal);
        acc.normal = len2 > 1e-12 ? acc.normal * inversesqrt(len2) : normalize(ng);
    } else {
        acc.occlusion = 1.0;
    }
    return acc;
}
