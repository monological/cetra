#version 330 core
in vec2 TexCoords;
out vec4 FragColor; // .r visibility toward the scene's lights (1 = lit, 0 = occluded)

// Screen-space contact shadows (Uncharted 4 / Bavoil): a short ray march through
// the depth buffer toward a light, darkening the near-contact seams a shadow
// map's texels are too coarse to resolve (under a foot, a collar, a hand on a
// surface). It is a SHORT-RANGE supplement to a shadow map, not a replacement --
// an object floating well clear of a surface is grounded by the map, not by
// this. Runs bilaterally blurred and temporally accumulated like GTAO, and
// composites in tonemap as a direct-light occlusion multiplier.
//
// It marches the KEY DIRECTIONAL and every CLUSTERED LOCAL LIGHT THAT HAS NO
// SHADOW MAP (spec 11.56), and the second half is the larger one: the punctual
// atlas holds 8 layers and a point light spends 6, so past the first point light
// in a scene there is no map to be had at all, and a derived area panel or any
// light authored cast_shadows false never had one. For that population this is
// not a sharpening of an existing shadow, it is the only occlusion there is.
//
// A light that DOES have a map is skipped, and its 3x3-PCF perspective map is
// why: at 2 to 6 punctual layers that map is 2048 square over a 90 degree cube
// face, about a millimetre per texel at a metre, so it already resolves the
// contact. (The hairline that once did not was a depth-STORAGE defect, fixed in
// 10.3/10.4, not a resolution one. The size is layer-count dependent -- a lone
// spot gets 4096, and 7 or more layers drop to 1024 and ~2 mm -- and every rung
// is finer than the gaps this pass draws.) Skipped for MARCHING only: the light
// still reaches the pixel, so it still counts in the fold's denominator, which
// is what stops one blocked practical darkening a bright mapped lamp with it.
//
// AREA PANELS ARE SKIPPED THE SAME WAY, and not as an oversight. A direction to
// march does exist -- dirType.xyz carries the panel normal -- but pbr_frag shades
// a panel through an LTC integral over its whole area, so marching one ray at its
// centre would be a DIFFERENT approximation from the one the shading used, which
// is worse than no ray at all. A centre is still good enough to weigh how much
// light arrives, so a panel counts in the denominator like any other lamp.
//
// Positions come from the aux G-buffer's LINEAR view-space Z (.z, negative in
// front, 0 = sky), the same source GTAO uses: the non-linear DEPTH24 buffer
// staircases at grazing angles and the march would read the cliffs as
// occlusion. The march itself is projected per step, but projection is linear
// in homogeneous coordinates, so the clip-space ray point is two MADs
// (clipP + t*clipL), not a matrix multiply per step.
uniform sampler2D linDepthTex; // aux G-buffer; .z = linear view-space Z (<0), 0 = sky
uniform sampler2D normalsTex;  // view-space normals (xyz); opportunistic N.L cull
uniform int useNormalsTex;     // 1 = normals resolved this frame, safe to sample
uniform mat4 projection;       // full matrix: focal terms reconstruct P, near plane clamps the ray
uniform mat4 view;             // world -> view; the cluster list stores world positions
uniform vec3 lightDirVS;       // unit vector, view space, pointing TOWARD the key light
uniform vec3 keyRadiance;      // key light colour * intensity, in the same lux the
                               // punctual lights reach through getDistanceAtt
uniform int hasKeyLight;       // 1 iff the two uniforms above describe a real directional
uniform float csDistance;      // march reach in view-space units (C guarantees > 0 when we run)
uniform int temporal;          // 1 iff the accumulation pass is active (frozen otherwise)
uniform int frameIndex;        // rotates the start jitter, only under temporal

#include "depth.glsl"      // viewPosFromLinZ; requires the projection uniform above
#include "lights_ubo.glsl" // the cluster list, getDistanceAtt, spotConeFactor

const int STEPS = 16;          // march samples over the (clamped) reach; a finer stride catches a
                               // thin near-contact without over-marching
const float BIAS_FRAC = 0.02;  // near-side slack (self-contact quantization), a fraction of csDistance
const float THICK_FRAC = 1.0;  // occluder thickness, x csDistance. The canonical SSCS test: a
                               // surface occludes only if the ray passes BEHIND it by less than this.
                               // It is what stops a ray grazing its own silhouette from reading the
                               // far side of the depth cliff as an occluder -- a near contact sits a
                               // SMALL depth behind its receiver, a silhouette straddle a LARGE one.
const float MAX_UV_LEN = 0.15; // clamp screen reach so near-camera pixels don't stride the frame
const float SKY_Z = -1e-4;     // linZ at or above this is the sky/shadow-catcher sentinel
const float GRAZE_LIFT = 4.0;  // slope-scaled start-offset gain: a ray grazing its own surface
                               // (low N.L) skims and self-shadows, so lift it up to (1+this)x the
                               // base offset; a face-on ray (high N.L, the one that must catch a
                               // real contact) keeps the base offset. Shadow-map slope bias, on
                               // the march. Ramps in only below N.L ~0.5, so a well-lit up-facing
                               // receiver's contact is untouched -- that is what keeps a ground
                               // pool while erasing the terminator/silhouette self-shadow.
// Rec.709, on linear radiance. The fold weight is PHOTOMETRIC, which is a
// choice with a cost: a saturated blue lamp weighs 0.0722 x its intensity
// against 0.7152 for a green one of equal intensity, so its contact shadow is
// diluted about tenfold. That is the defensible reading for a scalar that ends
// up multiplying an RGB image -- a per-channel term would need three channels
// the consumer cannot use (see the fold below).
const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722);

// Occlusion along `dirVS`, 0 = this light reaches the point, 1 = fully blocked.
//
// `reach` is the light's own march length and `csDistance` is the feature's: they
// differ for a punctual close enough that the full reach would march PAST it, and
// the shaping terms below deliberately keep using csDistance. distFade is "how
// near the contact is relative to what the feature can see", not relative to how
// far this particular lamp happens to be, and the acceptance window has to mean
// the same depth on every light or one lamp's silhouette straddle becomes
// another's contact.
// `n` is exactly zero where the normals buffer carries none, which is what lets
// the ray-start offset below skip a "do we have one" test rather than select
// between a vector and itself.
float marchOcclusion(vec3 P, vec3 n, float ndl, vec3 dirVS, float reach, float jitter) {
    // Surfaces facing away from the light are already dark from direct
    // shading; marching them only manufactures terminator acne.
    if (ndl < 0.05)
        return 0.0;

    // Start the ray a step off the surface along its normal so a grazing ray
    // does not immediately test against its own surface -- the classic
    // self-shadow acne. Scale with view distance (a fixed offset is
    // sub-quantization far from the camera; SSR biases the same way), and
    // slope-scale it: lift MORE where the light grazes (low N.L), since that is
    // where a ray skims its own surface into a self-shadow. Without a normal, a
    // distance-only floor still lifts the ray off the depth buffer.
    float grazeLift = 1.0 + GRAZE_LIFT * smoothstep(0.5, 0.1, ndl);
    float startBias = max(0.02, 0.01 * (-P.z)) * grazeLift;
    vec3 startV = P + n * startBias;

    // Clamp a camera-facing ray so its far end stays in front of the near
    // plane (recovered from the projection: the app's near clip varies).
    float nearV = projection[3][2] / (projection[2][2] - 1.0);
    float tMax = reach;
    if (dirVS.z > 0.0)
        tMax = min(tMax, (-nearV - startV.z) / dirVS.z);
    if (tMax <= 0.0)
        return 0.0;

    // Project the ray once; each step is clipP + t*clipL (projection is linear
    // in homogeneous coords, so a per-step matrix multiply is wasted work).
    vec4 clipP = projection * vec4(startV, 1.0);
    vec4 clipL = projection * vec4(dirVS, 0.0);

    // Near-camera pixels can otherwise skip across the whole frame in 8 steps,
    // stepping clean over every occluder. Shrink tMax so the screen reach caps
    // at MAX_UV_LEN (GTAO's MAX_SCREEN_RADIUS idiom).
    vec4 clipEnd = clipP + tMax * clipL;
    if (clipEnd.w > 1e-4) {
        vec2 uvEnd = (clipEnd.xy / clipEnd.w) * 0.5 + 0.5;
        float uvLen = length(uvEnd - TexCoords);
        if (uvLen > MAX_UV_LEN)
            tMax *= MAX_UV_LEN / uvLen;
    }

    // Near-side bias: just enough to absorb sub-step quantization, relative to the
    // reach (an absolute epsilon is wrong when scene scales span meters to
    // hundreds). Self-intersection at grazing angles is handled by the
    // slope-scaled START offset above, NOT here -- a depth-gradient (fwidth) term
    // here spikes at the very ground-contact seam it is meant to protect and
    // rejects the true near-contact, the thing the feature exists to draw.
    float bias = BIAS_FRAC * csDistance;

    // Occluder thickness: a surface blocks the ray only if the ray passes behind
    // it by less than this. Larger reads leak through (the ray is past the object,
    // in free space beyond it) -- and crucially reject a ray grazing its own
    // silhouette, which reads the FAR side of the depth cliff, a jump far larger
    // than any near-contact occluder.
    float thick = THICK_FRAC * csDistance;
    bool hit = false;
    float hitT = 0.0;    // t at the occluder, for contact-hardening
    vec2 hitUV = vec2(0.0);
    for (int i = 0; i < STEPS; i++) {
        float t = (float(i) + jitter) / float(STEPS) * tMax;
        vec4 clipS = clipP + t * clipL;
        if (clipS.w <= 1e-4)
            break; // reached the camera plane
        vec2 uv = (clipS.xy / clipS.w) * 0.5 + 0.5;
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
            break; // marched off-screen: no depth information out there

        float sceneZ = texture(linDepthTex, uv).z;
        float rayZ = startV.z + t * dirVS.z;
        // Both negative. delta > 0 means the scene surface is in front of the ray;
        // (bias, thick) is the window where it genuinely blocks the light -- nearer
        // than bias is self-contact quantization, deeper than thick is the far side
        // of a silhouette, not an occluder on this ray's path.
        float delta = sceneZ - rayZ;
        if (sceneZ < SKY_Z && delta > bias && delta < thick) {
            hit = true;
            hitT = t;
            hitUV = uv;
            break; // first blocker wins; contact shadows are binary + short
        }
    }

    if (!hit)
        return 0.0;

    // Contact-hardening: an occluder at the shading point darkens fully, one at
    // the march limit not at all. Fading over the full csDistance (not the
    // clamped tMax) decays to 0 as csDistance -> 0.
    float distFade = 1.0 - hitT / csDistance;
    vec2 edge = min(hitUV, 1.0 - hitUV);
    // Screen-edge fade hides the frame border; the N.L ramp keeps the term
    // on the lit hemisphere only (a direct-light-occlusion proxy) and fades
    // the grazing terminator where a lit-side ray skims its own surface.
    return distFade * smoothstep(0.0, 0.05, min(edge.x, edge.y)) * smoothstep(0.05, 0.25, ndl);
}

void main() {
    float linZ = texture(linDepthTex, TexCoords).z;
    if (linZ >= SKY_Z) {
        FragColor = vec4(1.0); // sky and the shadow-catcher floor: nothing to occlude
        return;
    }

    vec2 invFocal = 1.0 / vec2(projection[0][0], projection[1][1]);
    vec3 P = viewPosFromLinZ(TexCoords, linZ, invFocal);

    // Read the surface normal once; every light's back-face cull, ray-start
    // offset and fold weight uses it. The zero-normal hair/A2C marker leaves
    // hasN false.
    vec3 n = vec3(0.0);
    bool hasN = false;
    if (useNormalsTex == 1) {
        vec3 nn = texture(normalsTex, TexCoords).xyz;
        if (dot(nn, nn) > 0.01) {
            n = normalize(nn);
            hasN = true;
        }
    }

    // Start offset dithered per pixel by interleaved-gradient noise, rotated
    // per frame ONLY under temporal (the accumulator averages the rotations;
    // frozen otherwise so headless renders are byte-deterministic). Shared by
    // every light: it decorrelates the march from the PIXEL GRID, which is a
    // property of where we are, not of what we are marching toward.
    //
    // INLINE, and not include/noise.glsl's ign() -- do not tidy this. The
    // include spells the same function as a dot(), which rounds differently
    // from this explicit multiply-add, and the hash's low bits steer where the
    // march starts. ssr_frag.glsl:191 declined the identical migration on a
    // measured 31,800 px, and unlike ssr this one is covered by the
    // contact_debug golden, so the same edit here is a 0 px bet against a
    // transformation already known to be hostile.
    vec2 fc = gl_FragCoord.xy;
    if (temporal == 1)
        fc += vec2(float(frameIndex) * 5.588238);
    float jitter = fract(52.9829189 * fract(0.06711056 * fc.x + 0.00583715 * fc.y));

    /*
     * The R8 target holds ONE number, so the per-light visibilities fold into one
     * scalar WEIGHTED BY EACH LIGHT'S OWN CONTRIBUTION -- the fraction of the
     * direct light this pixel loses, which is what a direct-light occlusion
     * multiplier is supposed to carry. Everything the weight needs is already
     * here: radiance from the cluster list, falloff from the same getDistanceAtt
     * pbr_frag uses, N.L from the normals buffer.
     *
     * More channels would not help, and the reason is the CONSUMER rather than
     * the plumbing: the blur and the accumulator are both RGBA already, but
     * tonemap has one scene sample with no per-light decomposition, so N
     * visibilities collapse to one scalar at the multiply whatever they travelled
     * in. Keeping them apart is 9.4's separated direct-light buffer, which is out
     * of reach for the reason it always was.
     *
     * THE DENOMINATOR IS EVERY LIGHT THAT REACHES THE PIXEL, not every light this
     * pass marched. A light with its own shadow map, and an area panel, still
     * deliver light here -- they just deliver it unoccluded as far as this pass is
     * concerned. Leaving them out makes `occ` mean "fraction of the MARCHED light
     * lost" while tonemap multiplies the whole sample by it, so one blocked
     * practical beside a bright mapped spot took 23% off a pixel that should have
     * lost 1%.
     *
     * Two approximations remain, and neither is bounded by a light being dim --
     * create_light defaults cast_shadows FALSE, so the brightest lamp in a scene
     * is routinely map-less. The sum omits ambient (9.4 called that inherent and
     * benign for one key light and it is unchanged here), and a mapped light
     * counts its UNSHADOWED weight, since this pass cannot read its map. The
     * second one biases the term DOWN where a mapped light is itself in shadow --
     * pixels that are already dark, which is the harmless direction to be wrong
     * in, and the reason to prefer this over the old overshoot on a lit one.
     */
    float keyOcc = 0.0;
    float keyW = 0.0;
    if (hasKeyLight == 1) {
        float ndl = hasN ? dot(n, lightDirVS) : 1.0;
        keyOcc = marchOcclusion(P, n, ndl, lightDirVS, csDistance, jitter);
        keyW = dot(keyRadiance, LUMA) * max(ndl, 0.0);
    }

    float wsum = 0.0; // local direct light reaching this pixel, marched or not
    float osum = 0.0; // ...and the part of that this pass found blocked

    // An empty list contributes neither, so a scene with no local lights skips the
    // cluster lookup itself rather than the loop body -- the exponential-Z slice
    // and its dependent fetch are not free at full internal resolution.
    if (lightCounts.y > 0) {
        // The froxel volume reads the same list through the same UV entry point
        // and for the same reason: this pass does not render at the scene pass's
        // resolution, so it has no pixel coordinate in the space clusterParams.zw
        // is expressed in.
        uvec2 list = clusterLightListUv(TexCoords, -P.z);
        for (uint k = 0u; k < list.y; k++) {
            uint li = lightIndexAt(list.x + k);
            vec3 posVS = (view * vec4(clusterLights[li].posRange.xyz, 1.0)).xyz;
            vec3 toL = posVS - P;
            float sqrDist = dot(toL, toL);
            // getDistanceAtt's window is exactly 0 at and past the range, so this
            // is the same reject as w == 0 -- taken before the reciprocal square
            // root rather than after the whole weight. attenCutoff.x is 0 for an
            // unbounded light, which can never trip it.
            if (sqrDist * clusterLights[li].attenCutoff.x >= 1.0)
                continue;

            float dist = sqrt(max(sqrDist, 1e-8));
            vec3 dirVS = toL / dist;
            float ndl = hasN ? dot(n, dirVS) : 1.0;

            float typeF = clusterLights[li].dirType.w;
            float cone = 1.0;
            if (typeF == 2.0) {
                // The cone axis is a world direction like the position, and view
                // is rigid, so the cone survives the transform unchanged. Behind
                // the type test because spotConeFactor answers 1 for everything
                // else without reading it, and a point light is the common case.
                vec3 coneDirVS = (view * vec4(clusterLights[li].dirType.xyz, 0.0)).xyz;
                cone = spotConeFactor(typeF, coneDirVS, clusterLights[li].attenCutoff.w,
                                      clusterLights[li].shadowMisc.x, dirVS);
            }
            // An area panel is weighted from its CENTRE, which is a crude estimate
            // of an LTC integral and deliberately not the same judgement as the
            // one below: a centre is good enough to say how much light arrives,
            // and not good enough to say which way to march for it.
            float w = dot(clusterLights[li].colorIntensity.xyz, LUMA) *
                      getDistanceAtt(sqrDist, clusterLights[li].attenCutoff.x) * cone *
                      max(ndl, 0.0);
            // Negated rather than `w <= 0.0` so a NaN is rejected too. It cannot
            // arrive from anything this file computes, but spotConeFactor
            // normalizes an authored direction that nothing guards, and a NaN here
            // does not stay here -- it survives the bilateral blur into a 0.9
            // feedback history, where it is both sticky and spreading.
            if (!(w > 0.0))
                continue;
            wsum += w;

            if (clusterLights[li].shadowMisc.y >= 0.0)
                continue; // has a punctual map; marching it too would shadow it twice
            if (typeF == 3.0)
                continue; // area panel -- see the note at the top of the file

            // A ray must stop AT the lamp: past it there is no occluder, only more
            // scene, and marching the full reach through a lamp sitting in a
            // corner reads the wall behind it as blocking its own light.
            osum += w * marchOcclusion(P, n, ndl, dirVS, min(csDistance, dist), jitter);
        }
    }

    // A lone contributor is not divided: (w*occ)/w is not an identity in floating
    // point, and there is nothing to average.
    float occ = wsum > 0.0 ? (osum + keyW * keyOcc) / (wsum + keyW) : keyOcc;

    FragColor = vec4(vec3(1.0 - occ), 1.0);
}
