#version 330 core
in vec2 TexCoords;
out vec4 FragColor; // .r visibility toward the key light (1 = lit, 0 = occluded)

// Screen-space contact shadows (Uncharted 4 / Bavoil): a short ray march through
// the depth buffer toward the key light, darkening the near-contact seams a
// cascaded shadow map's texels are too coarse to resolve (under a foot, a collar,
// a hand on a surface). It is a SHORT-RANGE supplement to the shadow map, not a
// replacement -- an object floating well clear of a surface is grounded by the
// CSM, not by this. Runs bilaterally blurred and temporally accumulated like
// GTAO, and composites in tonemap as a direct-light occlusion multiplier -- so a
// shadow map is not required for the technique, only a key-light direction.
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
uniform vec3 lightDirVS;       // unit vector, view space, pointing TOWARD the key light
uniform float csDistance;      // march reach in view-space units (C guarantees > 0 when we run)
uniform int temporal;          // 1 iff the accumulation pass is active (frozen otherwise)
uniform int frameIndex;        // rotates the start jitter, only under temporal

#include "depth.glsl" // viewPosFromLinZ; requires the projection uniform above

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

void main() {
    float linZ = texture(linDepthTex, TexCoords).z;
    if (linZ >= SKY_Z) {
        FragColor = vec4(1.0); // sky and the shadow-catcher floor: nothing to occlude
        return;
    }

    vec2 invFocal = 1.0 / vec2(projection[0][0], projection[1][1]);
    vec3 P = viewPosFromLinZ(TexCoords, linZ, invFocal);

    // Read the surface normal once; both the back-face cull and the ray-start
    // offset below use it. The zero-normal hair/A2C marker leaves hasN false.
    vec3 n = vec3(0.0);
    bool hasN = false;
    if (useNormalsTex == 1) {
        vec3 nn = texture(normalsTex, TexCoords).xyz;
        if (dot(nn, nn) > 0.01) {
            n = normalize(nn);
            hasN = true;
        }
    }
    // Surfaces facing away from the key light are already dark from direct
    // shading; marching them only manufactures terminator acne.
    float ndl = hasN ? dot(n, lightDirVS) : 1.0;
    if (ndl < 0.05) {
        FragColor = vec4(1.0);
        return;
    }

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
    float tMax = csDistance;
    if (lightDirVS.z > 0.0)
        tMax = min(tMax, (-nearV - startV.z) / lightDirVS.z);
    if (tMax <= 0.0) {
        FragColor = vec4(1.0);
        return;
    }

    // Project the ray once; each step is clipP + t*clipL (projection is linear
    // in homogeneous coords, so a per-step matrix multiply is wasted work).
    vec4 clipP = projection * vec4(startV, 1.0);
    vec4 clipL = projection * vec4(lightDirVS, 0.0);

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

    // Start offset dithered per pixel by interleaved-gradient noise, rotated
    // per frame ONLY under temporal (the accumulator averages the rotations;
    // frozen otherwise so headless renders are byte-deterministic).
    //
    // INLINE, and not include/noise.glsl's ign() -- do not tidy this. The
    // include spells the same function as a dot(), which rounds differently
    // from this explicit multiply-add, and the hash's low bits steer where the
    // march starts. ssr_frag.glsl:187 declined the identical migration on a
    // measured 31,800 px, and unlike ssr this one is covered by the
    // contact_debug golden, so the same edit here is a 0 px bet against a
    // transformation already known to be hostile.
    vec2 fc = gl_FragCoord.xy;
    if (temporal == 1)
        fc += vec2(float(frameIndex) * 5.588238);
    float jitter = fract(52.9829189 * fract(0.06711056 * fc.x + 0.00583715 * fc.y));

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
        float rayZ = startV.z + t * lightDirVS.z;
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

    float occ = 0.0;
    if (hit) {
        // Contact-hardening: an occluder at the shading point darkens fully, one at
        // the march limit not at all. Fading over the full csDistance (not the
        // clamped tMax) decays to 0 as csDistance -> 0.
        float distFade = 1.0 - hitT / csDistance;
        vec2 edge = min(hitUV, 1.0 - hitUV);
        // Screen-edge fade hides the frame border; the N.L ramp keeps the term
        // on the lit hemisphere only (a direct-light-occlusion proxy) and fades
        // the grazing terminator where a lit-side ray skims its own surface.
        occ = distFade * smoothstep(0.0, 0.05, min(edge.x, edge.y)) *
              smoothstep(0.05, 0.25, ndl);
    }

    FragColor = vec4(vec3(1.0 - occ), 1.0);
}
