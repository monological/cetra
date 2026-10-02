#version 330 core

// A GRID fire's burning and forces (spec 13.14), one step, after Nguyen, Fedkiw and Jensen 2002
// as GPU Gems 3 ch. 30 runs it -- written as the two targets the advection reads next.
//
// Scalars are (temperature above ambient K, reaction coordinate Y, soot ppm, blue core weight).
//   - Where a source is alight, gas crosses the reaction front: Y is set to 1 and the gas
//     leaves at the fire's peak temperature -- Nguyen's shortcut for flames too large to resolve
//     the temperature's rise across the front (sec. 4.3).
//   - Y then falls at k a second (eq. 16), so 1 - Y over k is the time since the gas ignited.
//     The blue core is the gas within `core` seconds of the front (sec. 3.1); soot forms in the
//     reacting gas after it.
//   - The gas cools as the fourth power of its rise above ambient (eq. 17), solved exactly over
//     the step; soot oxidises fast in the flame's hot zone and thins slowly as smoke.
//   - Heat lifts the air (Boussinesq), and vorticity confinement puts back the curls the grid's
//     own dissipation takes out (Fedkiw, Stam and Jensen 2001).

#include "fire_constants.glsl"
#include "fire_sim.glsl"
#include "pcg4d.glsl"

const float FIRE_GRAVITY = 9.81; // m/s^2
// 1/s a burning source's gas is brought to its lift at, and the flue's air to the draft: fast
// against a step, so both hold their speed rather than nudging toward it.
const float FIRE_SOURCE_RELAX = 20.0;
const float FIRE_DRAFT_RELAX = 10.0;
// How soft the noise's edge is about a source's coverage threshold, in the noise's own 0..1.
const float FIRE_COVERAGE_BAND = 0.25;
// Seconds between one source's noise and the next's, so no two sources flicker together.
const float FIRE_SOURCE_STAGGER = 17.0;
// K either side of sootBurnoutAt over which soot's oxidation turns on.
const float FIRE_BURNOUT_BAND = 100.0;

uniform sampler2D velocityTex;
uniform sampler2D scalarTex;
uniform sampler2D curlTex;
uniform float dt;
uniform float cell;
uniform vec3 boxMin; // metres, local to the fire's origin, as every position here is
uniform float time;  // seconds of the fire's own clock

uniform float ambient;
uniform float peakRise; // the peak temperature's rise above ambient, K
uniform float reactionRate;
uniform float coolingRate; // fire_cooling_rate
uniform float entrainment;
uniform float core;
uniform float sootYield;
uniform float sootBurnout;
uniform float sootBurnoutAt;
uniform float smokeFade;
uniform float buoyancy;
uniform float vorticity;
uniform vec3 draftMin; // the chimney's flue
uniform vec3 draftMax;
uniform float draftSpeed; // m/s; 0 = no chimney

// The sources: a = (centre or first end, shape), b = (half-extents or second end, radius),
// params = (coverage, lift, unused, unused).
uniform int sourceCount;
uniform vec4 sourceA[FIRE_MAX_SOURCES];
uniform vec4 sourceB[FIRE_MAX_SOURCES];
uniform vec4 sourceParams[FIRE_MAX_SOURCES];

layout(location = 0) out vec4 outVelocity;
layout(location = 1) out vec4 outScalars;

// Signed distance to a source's shape, metres: negative inside.
float sourceDistance(int i, vec3 w) {
    vec3 a = sourceA[i].xyz;
    vec3 b = sourceB[i].xyz;
    int shape = int(sourceA[i].w);
    if (shape == 0) {
        vec3 d = abs(w - a) - b;
        return max(d.x, max(d.y, d.z));
    }
    if (shape == 1)
        return length(w - a) - sourceB[i].w;
    vec3 ab = b - a;
    float s = clamp(dot(w - a, ab) / max(dot(ab, ab), 1e-12), 0.0, 1.0);
    return length(w - (a + s * ab)) - sourceB[i].w;
}

float latticeHash(ivec4 p) {
    return float(pcg4d(uvec4(p)).x) / 4294967296.0;
}

// Value noise in 0..1 over world metres and seconds: a lattice FIRE_SOURCE_NOISE_METRES and
// FIRE_SOURCE_NOISE_SECONDS across, smoothly interpolated, so a source's tongues are regions that
// come and go rather than a per-cell, per-step speckle.
float sourceNoise(vec3 w, float t) {
    vec4 p = vec4(w / FIRE_SOURCE_NOISE_METRES, t / FIRE_SOURCE_NOISE_SECONDS);
    ivec4 i = ivec4(floor(p));
    vec4 f = p - floor(p);
    f = f * f * (3.0 - 2.0 * f);
    float slices[2];
    for (int w = 0; w < 2; w++) {
        float c00 = mix(latticeHash(i + ivec4(0, 0, 0, w)), latticeHash(i + ivec4(1, 0, 0, w)), f.x);
        float c10 = mix(latticeHash(i + ivec4(0, 1, 0, w)), latticeHash(i + ivec4(1, 1, 0, w)), f.x);
        float c01 = mix(latticeHash(i + ivec4(0, 0, 1, w)), latticeHash(i + ivec4(1, 0, 1, w)), f.x);
        float c11 = mix(latticeHash(i + ivec4(0, 1, 1, w)), latticeHash(i + ivec4(1, 1, 1, w)), f.x);
        slices[w] = mix(mix(c00, c10, f.y), mix(c01, c11, f.y), f.z);
    }
    return mix(slices[0], slices[1], f.w);
}

void main() {
    ivec3 c;
    if (!fireFragmentCell(c) || fireSolid(c)) {
        outVelocity = vec4(0.0);
        outScalars = vec4(0.0);
        return;
    }
    ivec2 t = fireAtlasTexel(c);
    vec3 u = texelFetch(velocityTex, t, 0).xyz;
    FireGas gas = fireGas(texelFetch(scalarTex, t, 0));
    float theta = max(gas.rise, 0.0);
    float Y = max(gas.Y, 0.0);
    float soot = max(gas.soot, 0.0);
    vec3 w = boxMin + (vec3(c) + 0.5) * cell;

    for (int i = 0; i < sourceCount; i++) {
        float cover = clamp(0.5 - sourceDistance(i, w) / cell, 0.0, 1.0);
        if (cover <= 0.0)
            continue;
        // Alight where the noise clears the share not burning: `coverage` of the shape at once.
        float n = sourceNoise(w, time + float(i) * FIRE_SOURCE_STAGGER);
        float alight = cover * smoothstep(1.0 - sourceParams[i].x - FIRE_COVERAGE_BAND,
                                          1.0 - sourceParams[i].x + FIRE_COVERAGE_BAND, n);
        if (alight <= 0.0)
            continue;
        Y = max(Y, alight);
        theta = max(theta, peakRise * alight);
        u = mix(u, vec3(0.0, sourceParams[i].y, 0.0), alight * min(dt * FIRE_SOURCE_RELAX, 1.0));
    }

    // The reaction coordinate runs down; soot forms in the reacting gas past the core.
    float coreDepth = max(reactionRate * core, 1e-4);
    float blue = clamp((Y - (1.0 - coreDepth)) / coreDepth, 0.0, 1.0);
    float reacting = Y > 0.0 ? 1.0 - blue : 0.0;
    soot += sootYield * reacting * dt;
    Y = max(Y - reactionRate * dt, 0.0);

    // d(theta)/dt = -c_T (theta / peak)^4, exactly over the step.
    theta = theta / pow(1.0 + 3.0 * coolingRate * theta * theta * theta * dt, 1.0 / 3.0);

    // Entrainment: room air mixed in, diluting the heat and the soot alike.
    float diluted = exp(-entrainment * dt);
    theta *= diluted;
    soot *= diluted;

    float hot = smoothstep(sootBurnoutAt - FIRE_BURNOUT_BAND, sootBurnoutAt + FIRE_BURNOUT_BAND,
                           ambient + theta);
    soot *= exp(-mix(smokeFade, sootBurnout, hot) * dt);

    // Buoyancy, g (T - T_amb) / T_amb.
    float lift = buoyancy * FIRE_GRAVITY * theta / ambient;
    // Vorticity confinement: push along N x omega, N the direction |omega| grows in.
    vec4 omega = texelFetch(curlTex, t, 0);
    vec3 grad;
    for (int a = 0; a < 3; a++) {
        ivec3 e = ivec3(0);
        e[a] = 1;
        grad[a] = fireFetch(curlTex, c + e, vec4(0.0)).w - fireFetch(curlTex, c - e, vec4(0.0)).w;
    }
    vec3 confine = vec3(0.0);
    float gl = length(grad);
    if (gl > 1e-6)
        confine = vorticity * cell * cross(grad / gl, omega.xyz);
    u += dt * (vec3(0.0, lift, 0.0) + confine);

    // The chimney's draw: the flue's air relaxed toward rising at the draft speed. The
    // projection that follows is what turns this into a room drawing in through the mouth.
    if (draftSpeed > 0.0 && all(greaterThanEqual(w, draftMin)) && all(lessThanEqual(w, draftMax)))
        u = mix(u, vec3(0.0, max(u.y, draftSpeed), 0.0), min(dt * FIRE_DRAFT_RELAX, 1.0));

    outVelocity = vec4(u, 0.0);
    outScalars = fireGasPack(FireGas(theta, Y, soot, blue));
}
