#version 330 core

// Transform-feedback particle UPDATE shader (GPU sim backend, spec 5.2).
// Reads one particle's ParticleGpuState as vertex attributes, advances it
// (curl-noise turbulence + drift + integrate + age), and writes the new state
// back as interleaved feedback varyings. Rasterization is discarded; nothing is
// drawn here. The 5 in/out vec4s MUST match the ParticleGpuState struct layout
// (particle_sim.h) field-for-field, in this order.

layout(location = 0) in vec4 iCenter; // xyz = world position, w = pad
layout(location = 1) in vec4 iParams; // x=size, y=rotation, z=lifeFrac, w=seed
layout(location = 2) in vec4 iColor;  // linear HDR rgba
layout(location = 3) in vec4 iVelAge; // xyz = velocity, w = age
layout(location = 4) in vec4 iLife;   // x = lifetime, yzw = free

uniform float dt;
uniform float time;
uniform float curlScale;     // spatial frequency of the noise
uniform float curlStrength;  // acceleration magnitude
uniform float curlTimescale; // rate the noise domain drifts over time
uniform vec3 drift;          // constant acceleration (m/s^2)
uniform float drag;          // per-step velocity damping in (0,1]

out vec4 oCenter;
out vec4 oParams;
out vec4 oColor;
out vec4 oVelAge;
out vec4 oLife;

// --- Hash-based 3D gradient noise (no permutation table; a GPU-native stand-in
// for noise_perlin3 -- the field differs slightly from the CPU version, which is
// fine for an organic cloud). ---
vec3 hash33(vec3 p) {
    p = vec3(dot(p, vec3(127.1, 311.7, 74.7)), dot(p, vec3(269.5, 183.3, 246.1)),
             dot(p, vec3(113.5, 271.9, 124.6)));
    return -1.0 + 2.0 * fract(sin(p) * 43758.5453123);
}

float gnoise(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    vec3 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(dot(hash33(i + vec3(0, 0, 0)), f - vec3(0, 0, 0)),
                       dot(hash33(i + vec3(1, 0, 0)), f - vec3(1, 0, 0)), u.x),
                   mix(dot(hash33(i + vec3(0, 1, 0)), f - vec3(0, 1, 0)),
                       dot(hash33(i + vec3(1, 1, 0)), f - vec3(1, 1, 0)), u.x),
                   u.y),
               mix(mix(dot(hash33(i + vec3(0, 0, 1)), f - vec3(0, 0, 1)),
                       dot(hash33(i + vec3(1, 0, 1)), f - vec3(1, 0, 1)), u.x),
                   mix(dot(hash33(i + vec3(0, 1, 1)), f - vec3(0, 1, 1)),
                       dot(hash33(i + vec3(1, 1, 1)), f - vec3(1, 1, 1)), u.x),
                   u.y),
               u.z);
}

// Three decorrelated scalar potentials (domain-offset), matching noise_curl3.
float potX(vec3 p) { return gnoise(p); }
float potY(vec3 p) { return gnoise(p + vec3(31.416, -47.853, 12.793)); }
float potZ(vec3 p) { return gnoise(p + vec3(-19.264, 33.148, -8.421)); }

// Divergence-free curl of the 3-potential field via central differences.
vec3 curl3(vec3 p) {
    const float e = 0.1;
    const float inv = 1.0 / (2.0 * e);
    float dPz_dy = (potZ(p + vec3(0, e, 0)) - potZ(p - vec3(0, e, 0))) * inv;
    float dPy_dz = (potY(p + vec3(0, 0, e)) - potY(p - vec3(0, 0, e))) * inv;
    float dPx_dz = (potX(p + vec3(0, 0, e)) - potX(p - vec3(0, 0, e))) * inv;
    float dPz_dx = (potZ(p + vec3(e, 0, 0)) - potZ(p - vec3(e, 0, 0))) * inv;
    float dPy_dx = (potY(p + vec3(e, 0, 0)) - potY(p - vec3(e, 0, 0))) * inv;
    float dPx_dy = (potX(p + vec3(0, e, 0)) - potX(p - vec3(0, e, 0))) * inv;
    return vec3(dPz_dy - dPy_dz, dPx_dz - dPz_dx, dPy_dx - dPx_dy);
}

void main() {
    vec3 pos = iCenter.xyz;
    vec3 vel = iVelAge.xyz;
    float age = iVelAge.w;
    float lifetime = iLife.x;
    float size = iParams.x;

    // Only live particles advance. Dead slots freeze (age unchanged so it never
    // overflows) and render invisibly (size 0) until the ring re-inits the slot.
    if (age < lifetime) {
        vec3 sp = pos * curlScale;
        vec3 c = curl3(vec3(sp.x, sp.y, sp.z + time * curlTimescale)); // matches update_curl_run
        vel += c * (curlStrength * dt);                                // curl turbulence
        vel += drift * dt;                                             // update_drift
        pos += vel * dt;                                               // update_integrate
        vel *= drag;
        age += dt;
    }

    float lifeFrac = (lifetime > 0.0) ? clamp(age / lifetime, 0.0, 1.0) : 0.0;
    float outSize = (age < lifetime) ? size : 0.0;

    oCenter = vec4(pos, 0.0);
    oParams = vec4(outSize, iParams.y, lifeFrac, iParams.w);
    oColor = iColor;
    oVelAge = vec4(vel, age);
    oLife = iLife;

    gl_Position = vec4(0.0); // unused: this pass runs under GL_RASTERIZER_DISCARD
}
