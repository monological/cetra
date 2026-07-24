#version 330 core
in vec2 TexCoords;
out vec4 FragColor; // .r visibility toward the key light (1 = lit, 0 = occluded)

// Screen-space contact shadows (Uncharted 4 / Bavoil): an 8-step ray march
// through the depth buffer toward the key light, darkening the near-contact
// gaps a cascaded shadow map's texels are too coarse to resolve (the seam
// under a foot, a collar, a hand on a surface). Runs at AO resolution, is
// bilaterally blurred and temporally accumulated like GTAO, and composites in
// tonemap as a direct-light occlusion multiplier -- so a shadow map is not
// required for the technique, only a key-light direction.
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

const int STEPS = 8;
const float BIAS_FRAC = 0.02;  // step-quantization slack, as a fraction of csDistance
const float MAX_UV_LEN = 0.15; // clamp screen reach so near-camera pixels don't stride the frame
const float SKY_Z = -1e-4;     // linZ at or above this is the sky/shadow-catcher sentinel

void main() {
    float linZ = texture(linDepthTex, TexCoords).z;
    // fwidth before any divergent branch: it needs the neighbouring fragments'
    // linZ, which a return/continue in this quad would leave undefined
    float zGrain = fwidth(linZ);
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
    // sub-quantization far from the camera; SSR biases the same way). Without a
    // normal, a distance-only floor still lifts the ray off the depth buffer.
    float startBias = max(0.02, 0.01 * (-P.z));
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
    vec2 fc = gl_FragCoord.xy;
    if (temporal == 1)
        fc += vec2(float(frameIndex) * 5.588238);
    float jitter = fract(52.9829189 * fract(0.06711056 * fc.x + 0.00583715 * fc.y));

    // Two-term bias: the slope term (per-texel depth gradient, which grows with
    // |z| under perspective on its own) stops the ray hugging its own surface;
    // the distance term absorbs sub-step quantization. Both are relative -- an
    // absolute epsilon is wrong when scene scales span meters to hundreds.
    float bias = BIAS_FRAC * csDistance + 2.0 * zGrain;

    float occ = 0.0;
    for (int i = 0; i < STEPS; i++) {
        float t = (float(i) + jitter) / float(STEPS) * tMax;
        vec4 clipS = clipP + t * clipL;
        if (clipS.w <= 1e-4)
            break; // reached the camera plane
        vec2 uv = (clipS.xy / clipS.w) * 0.5 + 0.5;
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
            break; // marched off-screen: no depth information out there

        float sceneZ = texture(linDepthTex, uv).z;
        if (sceneZ >= SKY_Z)
            continue; // sky/catcher sample is not an occluder
        float rayZ = startV.z + t * lightDirVS.z;
        // Both negative; diff > 0 means the ray passed BEHIND the sampled
        // surface -- something blocks the path to the light, so the receiver is
        // occluded. There is deliberately no upper bound: the grounding case (a
        // receiver marching behind a much-closer occluder, e.g. a plane behind
        // a cube) and the haloing case (a distant surface behind a foreground
        // object) share this signature, and the discriminator between them is
        // the MARCH LENGTH, not a depth slab. csDistance is short, so a far
        // receiver's march projects only a sub-pixel screen band past a
        // foreground silhouette -- haloing stays thin -- while a near receiver
        // reaches its occluder. A symmetric thickness window instead rejects
        // the grounding case outright (the occluder is "too far in front"),
        // which is why contact shadows were invisible before.
        float diff = sceneZ - rayZ;
        if (diff > bias) {
            // Contact-hardening: an occluder at the shading point darkens fully,
            // one at the march limit not at all. Fading over the full csDistance
            // (not the clamped tMax) makes the strength decay continuously to 0
            // as csDistance -> 0. Screen-edge smoothstep hides the frame border.
            float distFade = 1.0 - t / csDistance;
            vec2 edge = min(uv, 1.0 - uv);
            // Grazing-angle fade: near the terminator a lit-side ray skims its
            // own surface and self-occludes (a dark rim on smooth geometry).
            // Ramp the shadow in over N.L so those grazing pixels contribute
            // little, while well-lit contact gaps keep the full term.
            occ = distFade * smoothstep(0.0, 0.05, min(edge.x, edge.y)) *
                  smoothstep(0.05, 0.25, ndl);
            break;
        }
    }

    FragColor = vec4(vec3(1.0 - occ), 1.0);
}
