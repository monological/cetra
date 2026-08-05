// The skin diffusion profile: three Gaussians per channel.
//
// This file is the single definition of what "skin" scatters like, and
// tools/gen_skin_preint_fit.py integrates against these exact numbers to produce
// the coefficient table in include/preintegrated_skin.glsl. Change the peaks or
// the multipliers and that table silently describes a different material -- the
// tool's default (verify) mode reports the error of the checked-in table against
// a fresh integration, so run it if you touch them.
//
// Peaks and width multipliers are separate because they mean different things: a
// peak is the height at t = 0 and the three sum to 1.0 exactly, which is what
// lets a centre tap be seeded with weight 1. The AREA each term carries is
// peak * multiplier, normalised -- (0.0995, 0.3792, 0.5213) -- so the profile is
// tail-dominated even though the tail has the smallest peak. Reading the peaks as
// if they were masses is the mistake that fits systematically sharp.
const vec3 SSS_PROFILE_PEAK = vec3(0.35, 0.40, 0.25);
const vec3 SSS_PROFILE_MULT = vec3(0.30, 1.00, 2.20);

// Per-channel weight at distance t, for a base width sigma. Used by any
// evaluation that samples the profile directly rather than through a pyramid.
vec3 profileWeight(float t, vec3 sigma) {
    vec3 s1 = sigma * SSS_PROFILE_MULT.x;
    vec3 s2 = sigma * SSS_PROFILE_MULT.y;
    vec3 s3 = sigma * SSS_PROFILE_MULT.z;
    float t2 = t * t;
    vec3 g1 = exp(-t2 / (2.0 * s1 * s1));
    vec3 g2 = exp(-t2 / (2.0 * s2 * s2));
    vec3 g3 = exp(-t2 / (2.0 * s3 * s3));
    return SSS_PROFILE_PEAK.x * g1 + SSS_PROFILE_PEAK.y * g2 + SSS_PROFILE_PEAK.z * g3;
}

// Normalised area weights, i.e. how much of the delivered scatter each term
// carries. Derived rather than written down a second time, so the two can never
// drift apart.
vec3 sssProfileMasses() {
    vec3 area = SSS_PROFILE_PEAK * SSS_PROFILE_MULT;
    return area / dot(area, vec3(1.0));
}
