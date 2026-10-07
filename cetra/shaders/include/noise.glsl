// Interleaved-gradient noise in [0,1) (Jimenez 2014) -- the cheap per-pixel
// dither every stochastic pass keys its pattern from. Advance a pattern per
// frame by offsetting the input: ign(gl_FragCoord.xy + vec2(frame * 5.588238))
// (an irrational-ish stride so no frame repeats).
//
// ign is for SCREEN SPACE. It is keyed off gl_FragCoord and is no substitute
// for hashing a world position -- hash21 and hash13 below are that, and new
// stochastic code includes this file for whichever of the four it needs rather
// than inlining a fifth copy. A stride added to both of ign's axes still moves
// one pattern along a line, which is fine under a temporal filter that averages
// it away; noise SEEN as new each frame is frameNoise's.
//
// TWO SHADERS ARE EXEMPT and must stay that way:
//   ssr_frag.glsl:211        inline IGN. The include's dot() form rounds
//                            differently from the explicit multiply-add, the
//                            hash's low bits steer a traced ray, and migrating
//                            was MEASURED at 31,800 px.
//   contact_shadow_frag.glsl inline IGN, same explicit form, same reason -- and
//                            unlike ssr it is covered by the contact_debug
//                            golden, so a migration there is a 0 px bet against
//                            a transformation already known to be hostile.
float ign(vec2 p) {
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

// The classic sin-fract value hash, vec2 -> [0,1).
//
// `k` IS A PARAMETER because its callers disagree on it and both are right:
// water's foam noise keys off (127.1, 311.7) and the rain's ripples off their
// own. Sharing one constant would have changed one of their patterns for no
// reason, where sharing the FORM -- which is what was actually duplicated --
// costs neither of them a value.
float hash21(vec2 p, vec2 k) {
    return fract(sin(dot(p, k)) * 43758.5453);
}

// The same, vec3 -> [0,1). Lives here rather than in wind.glsl because that
// chunk is spliced into four vertex programs that must agree bit-for-bit -- a
// shared definition they all pull is one fewer thing to keep in step than four
// copies that are meant to match.
float hash13(vec3 p, vec3 k) {
    return fract(sin(dot(p, k)) * 43758.5453);
}

// Jarzynski & Olano, "Hash Functions for GPU Rendering", JCGT 2020, section 6.1, as written:
// a (3 -> 3) hash in which every output word changes when any input word does.
uvec3 pcg3d(uvec3 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return v;
}

// [0, 1) at an integer cell on a given frame, independent of every other cell and of the
// same cell on every other frame: noise that is new each frame and goes nowhere. Seeding a
// pattern by adding the frame to both of its coordinates, as the grain once did, moves ONE
// field one cell along the diagonal each frame, which the eye follows as crawling lines. The
// top 24 bits, which a float holds exactly.
float frameNoise(uvec2 cell, uint frame) {
    return float(pcg3d(uvec3(cell, frame)).x >> 8u) * (1.0 / 16777216.0);
}
