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

// Colliders (mirror particle_module.c). Up to MAX_COLLIDERS of any shape/mode,
// packed 4 vec4 each: P=(shape,mode,radius,restitution), A=(a.xyz,wake),
// B=(b.xyz,_), V=(shapeVel.xyz,_). a/b are shape anchors (see the C enum).
#define MAX_COLLIDERS 8
uniform int colliderCount;
uniform vec4 colliderP[MAX_COLLIDERS];
uniform vec4 colliderA[MAX_COLLIDERS];
uniform vec4 colliderB[MAX_COLLIDERS];
uniform vec4 colliderV[MAX_COLLIDERS];

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

// --- Colliders: mirror the CPU resolve helpers in particle_module.c exactly.
// mode: 0 = KEEP_OUT, 1 = KEEP_IN. shape: 0=sphere 1=box 2=plane 3=capsule 4=cylinder.
// These ordinals are a wire contract with the ColliderShape/ColliderMode enums
// (particle_module.h); a _Static_assert in particle_sim_tf.c pins them.

// Sphere/capsule core: keep the particle on the correct side of a sphere of
// radius `radius` centered at `c`; reflect the into-surface velocity.
void resolveRadial(inout vec3 pos, inout vec3 vel, vec3 c, float radius, int mode, float rest,
                   float wake, vec3 svel) {
    vec3 d = pos - c;
    float dist = length(d);
    bool wrong = (mode == 0) ? (dist < radius) : (dist > radius);
    if (!wrong)
        return;
    vec3 n = dist > 1e-5 ? d / dist : vec3(0.0, 1.0, 0.0);
    pos = c + n * radius;
    float vn = dot(vel, n);
    bool into = (mode == 0) ? (vn < 0.0) : (vn > 0.0);
    if (into)
        vel -= n * (vn * (1.0 + rest));
    if (mode == 0)
        vel += svel * wake;
}

vec3 closestOnSeg(vec3 s0, vec3 s1, vec3 p) {
    vec3 ab = s1 - s0;
    float denom = dot(ab, ab);
    float u = denom > 1e-8 ? clamp(dot(p - s0, ab) / denom, 0.0, 1.0) : 0.0;
    return s0 + ab * u;
}

void resolveBox(inout vec3 pos, inout vec3 vel, vec3 lo, vec3 hi, int mode, float rest) {
    if (mode == 1) { // keep-in: clamp inside, reflect the outward component
        for (int k = 0; k < 3; k++) {
            if (pos[k] < lo[k]) {
                pos[k] = lo[k];
                if (vel[k] < 0.0)
                    vel[k] *= -rest;
            } else if (pos[k] > hi[k]) {
                pos[k] = hi[k];
                if (vel[k] > 0.0)
                    vel[k] *= -rest;
            }
        }
        return;
    }
    // keep-out: only when inside, eject through the least-penetrated face
    if (pos.x <= lo.x || pos.x >= hi.x || pos.y <= lo.y || pos.y >= hi.y || pos.z <= lo.z ||
        pos.z >= hi.z)
        return;
    int bk = 0, bs = -1;
    float bpen = pos.x - lo.x, bface = lo.x;
    for (int k = 0; k < 3; k++) {
        float plo = pos[k] - lo[k], phi = hi[k] - pos[k];
        if (plo < bpen) {
            bpen = plo;
            bk = k;
            bface = lo[k];
            bs = -1;
        }
        if (phi < bpen) {
            bpen = phi;
            bk = k;
            bface = hi[k];
            bs = 1;
        }
    }
    pos[bk] = bface;
    if ((bs < 0 && vel[bk] < 0.0) || (bs > 0 && vel[bk] > 0.0))
        vel[bk] *= -rest;
}

void resolvePlane(inout vec3 pos, inout vec3 vel, vec3 pt, vec3 nrm, float rest, float wake,
                  vec3 svel) {
    float sd = dot(pos - pt, nrm);
    if (sd >= 0.0)
        return;
    pos -= nrm * sd; // sd < 0 -> lift along +normal onto the plane
    float vn = dot(vel, nrm);
    if (vn < 0.0)
        vel -= nrm * (vn * (1.0 + rest));
    vel += svel * wake;
}

void resolveCylinder(inout vec3 pos, inout vec3 vel, vec3 p0, vec3 p1, float radius, int mode,
                     float rest, float wake, vec3 svel) {
    vec3 axis = p1 - p0;
    float axlen = length(axis);
    if (axlen < 1e-6)
        return;
    axis /= axlen;
    float h = dot(pos - p0, axis);
    vec3 foot = p0 + axis * h;
    vec3 radial = pos - foot;
    float rdist = length(radial);
    vec3 rn = rdist > 1e-5 ? radial / rdist : vec3(1.0, 0.0, 0.0);
    if (mode == 0) { // keep-out
        if (rdist >= radius || h <= 0.0 || h >= axlen)
            return;
        if (radius - rdist <= min(h, axlen - h)) { // eject through the curved side
            pos = foot + rn * radius;
            float vn = dot(vel, rn);
            if (vn < 0.0)
                vel -= rn * (vn * (1.0 + rest));
        } else { // eject through the nearer cap (foot + radial == pos, so move axially only)
            float capH = (h < axlen - h) ? 0.0 : axlen;
            pos += axis * (capH - h);
            float va = dot(vel, axis);
            bool into = (h < axlen - h) ? (va > 0.0) : (va < 0.0);
            if (into)
                vel -= axis * (va * (1.0 + rest));
        }
        vel += svel * wake;
        return;
    }
    // keep-in: clamp radially into the tube and axially between the caps
    if (rdist > radius) {
        pos = foot + rn * radius;
        float vn = dot(vel, rn);
        if (vn > 0.0)
            vel -= rn * (vn * (1.0 + rest));
    }
    if (h < 0.0 || h > axlen) {
        float capH = (h < 0.0) ? 0.0 : axlen;
        pos += axis * (capH - h);
        float va = dot(vel, axis);
        if ((h < 0.0 && va < 0.0) || (h > axlen && va > 0.0))
            vel -= axis * (va * (1.0 + rest));
    }
}

void applyColliders(inout vec3 pos, inout vec3 vel) {
    for (int ci = 0; ci < colliderCount; ci++) {
        int shape = int(colliderP[ci].x + 0.5);
        int mode = int(colliderP[ci].y + 0.5);
        float radius = colliderP[ci].z;
        float rest = colliderP[ci].w;
        vec3 a = colliderA[ci].xyz;
        float wake = colliderA[ci].w;
        vec3 b = colliderB[ci].xyz;
        vec3 svel = colliderV[ci].xyz;
        if (shape == 0)
            resolveRadial(pos, vel, a, radius, mode, rest, wake, svel);
        else if (shape == 1)
            resolveBox(pos, vel, a, b, mode, rest);
        else if (shape == 2)
            resolvePlane(pos, vel, a, b, rest, wake, svel);
        else if (shape == 3)
            resolveRadial(pos, vel, closestOnSeg(a, b, pos), radius, mode, rest, wake, svel);
        else if (shape == 4)
            resolveCylinder(pos, vel, a, b, radius, mode, rest, wake, svel);
        // no catch-all: an unknown shape is a safe no-op, matching the CPU switch
    }
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
        applyColliders(pos, vel); // after integrate, mirroring the CPU module order
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
