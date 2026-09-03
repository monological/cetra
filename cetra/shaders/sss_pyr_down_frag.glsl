#version 330 core
in vec2 TexCoords;
layout(location = 0) out vec4 ColorOut;
layout(location = 1) out float DepthOut;

// One scatter-pyramid downsample step: the same 13-tap Jimenez pattern the bloom
// chain uses (five overlapping 2x2 boxes, 0.5/0.125), with a depth guard.
//
// The overlap matters more here than it does for bloom. This chain's levels are
// sampled at a CONTINUOUS LOD, so a plain 2x2 box -- whose transfer function has
// deep nulls -- would make the delivered width wobble as the radius sweeps
// across a level boundary, which is the same class of artifact the pyramid
// exists to remove.
//
// Colour arrives already coverage-premultiplied, so it is averaged directly and
// never multiplied by coverage a second time. Depth is NOT premultiplied (it is
// a position, not a quantity that superposes), so it is weighted by coverage
// here and divided back out at the end.
uniform sampler2D srcColor;
uniform sampler2D srcDepth;
uniform vec2 texelSize;    // one SOURCE-level texel
uniform float srcFootprint; // world units per source texel per unit of clip w
uniform float sigmaZFloor;  // authored scatter radius, in world units
uniform mat4 projection;    // for clipWAt: the footprint scales by depth only under perspective
#include "depth.glsl"

const vec2 OFF[13] = vec2[13](
    vec2(-2.0, 2.0), vec2(0.0, 2.0), vec2(2.0, 2.0),
    vec2(-2.0, 0.0), vec2(0.0, 0.0), vec2(2.0, 0.0),
    vec2(-2.0, -2.0), vec2(0.0, -2.0), vec2(2.0, -2.0),
    vec2(-1.0, 1.0), vec2(1.0, 1.0), vec2(-1.0, -1.0), vec2(1.0, -1.0)
);
const float W[13] = float[13](
    0.03125, 0.0625, 0.03125,
    0.0625, 0.125, 0.0625,
    0.03125, 0.0625, 0.03125,
    0.125, 0.125, 0.125, 0.125
);

void main()
{
    // The centre tap is the depth reference. Using the coarse mean instead would
    // let a texel straddling a silhouette drag its own reference across the step
    // and then accept everything.
    float zRef = texture(srcDepth, TexCoords).r;

    // Tolerance grows with the level, because a coarser texel legitimately spans
    // more depth: at this footprint it admits a surface up to ~72 degrees from
    // face-on. Floored at the authored radius so level 0 degrades to the guard
    // the separable blur used. A tolerance that did NOT grow would reject the
    // wide terms on any curved surface, which is exactly where they are wanted.
    float tol = max(sigmaZFloor, 3.0 * srcFootprint * clipWAt(-max(zRef, 0.0)));

    vec4 color = vec4(0.0);
    float depth = 0.0;
    float wsum = 0.0;
    for (int i = 0; i < 13; i++) {
        vec2 uv = TexCoords + texelSize * OFF[i];
        vec4 c = texture(srcColor, uv);
        float z = texture(srcDepth, uv).r;
        // zRef <= 0 means the centre is uncovered, so nothing here has a
        // reference to be near: fall back to accepting covered taps, which lets
        // a mostly-covered texel whose centre happens to miss still carry light.
        float ok = (zRef <= 0.0) ? 1.0 : step(abs(z - zRef), tol);
        float w = W[i] * ok;
        color += c * w;
        depth += z * c.a * w;
        wsum += c.a * w;
    }
    ColorOut = color;
    DepthOut = depth / max(wsum, 1e-6);
}
