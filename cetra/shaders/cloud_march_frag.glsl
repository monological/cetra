#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Half-res screen march of the cloud shell (spec 11.0). Rays reconstruct
// from the UNJITTERED projection (the aerial/froxel discipline); output is
// (absolute in-scattered radiance, transmittance) -- the background
// composite applies the exposure conversion and the geometry depth test.

uniform sampler3D shapeTex;
uniform sampler3D detailTex;
uniform sampler2D transmittanceLut;
uniform sampler2D skyViewLut;
uniform mat4 invView;  // view -> world
uniform vec2 invFocal; // (1/proj[0][0], 1/proj[1][1])
uniform vec3 camPosKm; // world camera position / unitsPerKm
uniform vec3 sunDir;
uniform float coverage;
uniform float cloudType;
uniform float densityScale;
uniform vec3 windOffsetKm; // accumulated drift; the caller owns the clock

// Screen-tier march quality. Consts, not uniforms: the trip counts must be
// compile-time for the loop-unrolling the march relies on, lightSteps may
// never exceed the light-offset table's length, and the env tier already
// carries its own literals -- a runtime knob here would be a trap dressed
// as flexibility.
const int MARCH_STEPS = 64;
const int MARCH_LIGHT_STEPS = 6;

// Ray-direction temporal reprojection (spec 11.0 phase 3): blend last
// frame's result fetched through the march's OWN previous camera -- sky
// pixels carry zero scene velocity, so the shared accumulators cannot
// reproject them. Rotation-only: clouds are treated as at-infinity, and
// translation parallax is bounded by the blend. NOT TAA-gated: the frame
// stride below is a pure function of frameIndex, so headless goldens stay
// byte-deterministic at a fixed frame count.
uniform sampler2D historyTex;
uniform int temporal;   // 1 = previous march was exactly last frame
uniform int frameIndex; // total_frames % 4096
uniform mat4 prevView;  // rotation-only world->view of the previous march
uniform vec2 prevFocal; // previous (proj[0][0], proj[1][1])

#include "noise.glsl"
#include "clouds.glsl"

void main()
{
    vec2 ndc = TexCoords * 2.0 - 1.0;
    vec3 viewDir = normalize(vec3(ndc * invFocal, -1.0));
    vec3 rd = normalize(mat3(invView) * viewDir);

    // Frame-strided dither only when history is live: static headless
    // pattern, per-frame rotation the blend averages when temporal.
    float dither = ign(gl_FragCoord.xy +
                       (temporal == 1 ? vec2(float(frameIndex) * 5.588238) : vec2(0.0)));

    vec4 result = cloud_march(camPosKm, rd, sunDir, shapeTex, detailTex, transmittanceLut,
                              skyViewLut, MARCH_STEPS, MARCH_LIGHT_STEPS, true, coverage,
                              cloudType, densityScale, windOffsetKm, dither);

    if (temporal == 1) {
        // Fetch history where this ray direction fell last frame. In front
        // of the previous camera and on-screen -> 90/10 blend; otherwise
        // keep the fresh sample.
        vec3 pv = mat3(prevView) * rd;
        if (pv.z < -1e-4) {
            vec2 prevNdc = vec2(prevFocal.x * pv.x, prevFocal.y * pv.y) / -pv.z;
            vec2 prevUv = prevNdc * 0.5 + 0.5;
            if (prevUv.x >= 0.0 && prevUv.x <= 1.0 && prevUv.y >= 0.0 && prevUv.y <= 1.0) {
                vec4 history = texture(historyTex, prevUv);
                result = mix(result, history, 0.9);
            }
        }
    }

    FragColor = result;
}
