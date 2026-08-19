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
// Skipping a light that DOES have a map is the same line and is a correctness
// requirement rather than a budget: its 3x3-PCF perspective map already resolves
// the contact (that hairline was a depth-storage defect, fixed in 10.3/10.4, not
// a resolution one), so marching it as well would darken the seam twice.
//
// AREA PANELS ARE SKIPPED, and not as an oversight: posRange.xyz is a panel
// CENTRE, a panel has no single direction to march along, and pbr_frag shades it
// through an LTC branch that never computes one. Marching to the centre would be
// a different approximation from the one the shading uses.
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
const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722); // Rec.709, on linear radiance

// Occlusion along `dirVS`, 0 = this light reaches the point, 1 = fully blocked.
//
// `reach` is the light's own march length and `csDistance` is the feature's: they
// differ for a punctual close enough that the full reach would march PAST it, and
// the shaping terms below deliberately keep using csDistance. distFade is "how
// near the contact is relative to what the feature can see", not relative to how
// far this particular lamp happens to be, and the acceptance window has to mean
// the same depth on every light or one lamp's silhouette straddle becomes
// another's contact.
float marchOcclusion(vec3 P, vec3 n, bool hasN, float ndl, vec3 dirVS, float reach, float jitter) {
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
    vec3 startV = P + (hasN ? n : vec3(0.0)) * startBias;

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
     * One R8 channel cannot say "light A is blocked here and light B is not", and
     * the blur, the accumulator and the tonemap multiply are all single-channel.
     * So the per-light visibilities fold into one scalar WEIGHTED BY EACH LIGHT'S
     * OWN CONTRIBUTION -- the fraction of the direct light this pixel loses, which
     * is the quantity a direct-light occlusion multiplier is supposed to carry.
     * Everything the weight needs is already here: radiance from the cluster list,
     * falloff from the same getDistanceAtt pbr_frag uses, N.L from the normals
     * buffer.
     *
     * The residual, stated because 9.4 stated the one-light version of it: the
     * multiply lands on a summed image that also holds ambient and, on a lit
     * frame, light this pass never marched. For one key light 9.4 called that
     * inherent and benign; adding practicals makes it a new error, bounded by
     * csStrength and by the weight of a light that is by definition dim enough
     * not to have earned a shadow map.
     *
     * `occ` STARTS as the key light's own answer and is only replaced when
     * something else actually contributes. That is not an optimisation -- it is
     * what makes a sun-only frame bit-identical to the pre-11.56 pass, since
     * (w*occ)/w is not an identity in floating point and this feature is not
     * worth re-baking a golden over.
     */
    float occ = 0.0;
    float wsum = 0.0;
    float osum = 0.0;
    int folded = 0;

    if (hasKeyLight == 1) {
        float ndl = hasN ? dot(n, lightDirVS) : 1.0;
        occ = marchOcclusion(P, n, hasN, ndl, lightDirVS, csDistance, jitter);
        wsum = dot(keyRadiance, LUMA) * max(ndl, 0.0);
        osum = wsum * occ;
    }

    // The froxel volume reads the same list through the same UV entry point and
    // for the same reason: this pass does not render at the scene pass's
    // resolution, so it has no pixel coordinate in the space clusterParams.zw is
    // expressed in.
    uvec2 list = clusterLightListUv(TexCoords, -P.z);
    for (uint k = 0u; k < list.y; k++) {
        uint li = lightIndexAt(list.x + k);
        if (clusterLights[li].shadowMisc.y >= 0.0)
            continue; // has a punctual map; marching it too would shadow it twice
        float typeF = clusterLights[li].dirType.w;
        if (typeF != 1.0 && typeF != 2.0)
            continue; // point and spot only -- see the area-panel note at the top

        vec3 posVS = (view * vec4(clusterLights[li].posRange.xyz, 1.0)).xyz;
        vec3 toL = posVS - P;
        float sqrDist = dot(toL, toL);
        float dist = sqrt(max(sqrDist, 1e-8));
        vec3 dirVS = toL / dist;
        float ndl = hasN ? dot(n, dirVS) : 1.0;

        // The cone axis is a world direction like the position, and view is
        // rigid, so the cone survives the transform unchanged.
        vec3 coneDirVS = (view * vec4(clusterLights[li].dirType.xyz, 0.0)).xyz;
        float w = dot(clusterLights[li].colorIntensity.xyz, LUMA) *
                  getDistanceAtt(sqrDist, clusterLights[li].attenCutoff.x) *
                  spotConeFactor(typeF, coneDirVS, clusterLights[li].attenCutoff.w,
                                 clusterLights[li].shadowMisc.x, dirVS) *
                  max(ndl, 0.0);
        if (w <= 0.0)
            continue; // out of range, outside the cone, or on the unlit hemisphere

        // A ray must stop AT the lamp: past it there is no occluder, only more
        // scene, and marching the full reach through a lamp sitting in a corner
        // reads the wall behind it as blocking its own light.
        wsum += w;
        osum += w * marchOcclusion(P, n, hasN, ndl, dirVS, min(csDistance, dist), jitter);
        folded++;
    }

    // Every folded light passed w > 0, so a nonzero count is a positive sum.
    if (folded > 0)
        occ = osum / wsum;

    FragColor = vec4(vec3(1.0 - occ), 1.0);
}
