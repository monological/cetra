#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// DDGI probe projection (spec 9.7): turn one probe's captured cube into the
// octahedral tile the scene shader samples. Two modes, one shader, because they
// walk the identical hemisphere and differ only in what they integrate.
//
//   mode 0 -- IRRADIANCE. Cosine-convolve the captured radiance. Output rgb is
//             irradiance for the tile texel's direction; a is unused.
//   mode 1 -- VISIBILITY. Mean distance and mean distance squared, cosine-power
//             weighted, for the Chebyshev leak test. Output rg = (mean, mean^2),
//             both in units of farZ rather than world units: the atlas is fp16,
//             and a squared distance in metres overflows its 65504 ceiling from
//             a scene barely 256 units across.
//
// The pass covers the whole BORDERED tile. The 1px gutter exists so a bilinear
// tap at a tile edge reads the octahedral map's wrapped neighbour rather than
// whatever tile is packed beside it -- and since a gutter texel's value is a
// pure function of a direction, computing that direction is the same work as
// copying the interior texel that holds it, minus a second pass that would have
// had to read the atlas while the atlas was the render target (undefined in
// GL 4.1, which has no texture barrier).
//
// The caller blends this into the atlas with a constant alpha (hysteresis), so
// nothing here knows about history.

uniform samplerCube captureColor;
uniform samplerCube captureDepth;
uniform vec2 tileOrigin; // atlas texel of this tile's BORDER, lower-left
uniform float tileRes;   // interior edge length in texels (border adds 1 each side)
uniform float nearZ;
uniform float farZ;
uniform int mode;

#include "octahedral.glsl"

// The cube is only 16^2 per face, so a fixed lat/long sweep over the hemisphere
// never misses a face. 8x24 = 192 samples per output texel, over the whole
// BORDERED tile: 10^2 = 100 fragments for irradiance and 18^2 = 324 for
// visibility, so ~19k and ~62k cube taps per probe respectively. The visibility
// pass is the expensive one by 3.2x, which is the opposite of what tile
// resolution alone suggests.
const int PHI_STEPS = 24;
const int THETA_STEPS = 8;

// Depth buffer value -> distance along the ray. The capture used a symmetric
// 90-degree perspective, so the depth is the standard non-linear z/w; this
// inverts it to a view-space distance and then to a true ray length.
float rayDistance(vec3 dir, float depth01) {
    if (depth01 >= 1.0)
        return 1.0; // nothing hit: treat as the far wall
    float ndc = depth01 * 2.0 - 1.0;
    float viewZ = (2.0 * nearZ * farZ) / (farZ + nearZ - ndc * (farZ - nearZ));
    // viewZ is the PLANAR depth along the face axis; the cube face's dominant
    // axis is the largest component, so dividing by it converts to ray length.
    float axis = max(max(abs(dir.x), abs(dir.y)), abs(dir.z));
    return min(viewZ / (max(axis, 1e-4) * farZ), 1.0);
}

// Border texel -> the interior texel it mirrors. The octahedral square wraps in
// a way no sampler understands: crossing an edge re-enters the SAME tile,
// mirrored along that edge; corners wrap to the diagonally opposite corner.
// Interior texels pass through untouched.
vec2 wrapToInterior(vec2 local, float outer) {
    bool cx = (local.x == 0.0 || local.x == outer);
    bool cy = (local.y == 0.0 || local.y == outer);
    if (cx && cy)
        return vec2(local.x == 0.0 ? tileRes : 1.0, local.y == 0.0 ? tileRes : 1.0);
    if (cx) // vertical edge: mirror in y, step one column in from the far side
        return vec2(local.x == 0.0 ? 1.0 : tileRes, outer - local.y);
    if (cy) // horizontal edge: mirror in x, step one row in from the far side
        return vec2(outer - local.x, local.y == 0.0 ? 1.0 : tileRes);
    return local;
}

void main() {
    // Position within the bordered tile: 0 and tileRes+1 are the gutter ring.
    vec2 local = wrapToInterior(floor(gl_FragCoord.xy - tileOrigin), tileRes + 1.0);
    // ...then to the interior's own 0-based coordinates, which is what the
    // octahedral mapping and every consumer's UV are expressed in.
    vec3 N = octDirFromTexel(local - 1.0, tileRes);

    // Tangent frame for N, built ONCE. It depends on nothing the loops vary, and
    // living inside them cost 192 normalizes and 384 cross products per output
    // texel -- about 20M redundant iterations across an opening sweep.
    vec3 up = abs(N.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 T = normalize(cross(up, N));
    vec3 B = cross(N, T);

    vec3 sum = vec3(0.0);
    float sumDist = 0.0;
    float sumDist2 = 0.0;
    float weightSum = 0.0;

    for (int t = 0; t < THETA_STEPS; ++t) {
        // Cosine-distributed polar angle: equal-area in the cosine measure, so
        // the plain weighted mean below is already the correct estimator.
        float u = (float(t) + 0.5) / float(THETA_STEPS);
        float cosTheta = sqrt(1.0 - u);
        float sinTheta = sqrt(u);
        for (int p = 0; p < PHI_STEPS; ++p) {
            float phi = (float(p) + 0.5) / float(PHI_STEPS) * 6.28318530718;

            // The sample direction in N's tangent frame, hoisted above.
            vec3 dir = normalize(T * (sinTheta * cos(phi)) + B * (sinTheta * sin(phi)) +
                                 N * cosTheta);

            if (mode == 0) {
                // Cosine weight is already in the distribution, so this is a
                // straight mean of radiance over the cosine-weighted hemisphere.
                sum += texture(captureColor, dir).rgb;
                weightSum += 1.0;
            } else {
                // Sharpened toward N: the Chebyshev test wants the distance to
                // what lies ALONG the sampled direction, not a hemisphere-wide
                // average that a distant wall would drag outward.
                float w = pow(max(dot(N, dir), 0.0), 8.0);
                float d = rayDistance(dir, texture(captureDepth, dir).r);
                sumDist += d * w;
                sumDist2 += d * d * w;
                weightSum += w;
            }
        }
    }

    float inv = 1.0 / max(weightSum, 1e-6);
    if (mode == 0)
        FragColor = vec4(sum * inv, 1.0);
    else
        FragColor = vec4(sumDist * inv, sumDist2 * inv, 0.0, 1.0);
}
