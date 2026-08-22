#version 330 core

/*
 * The feedback pass (spec 11.67): every rasterized texel of a paged surface
 * votes for the virtual page under it, into a small RGBA8 target the CPU reads
 * back at fixed latency. GPU feedback is what stays correct when the VT grows
 * past terrain -- decals and meshes the CPU cannot enumerate join this stream
 * without touching the pass -- and what it adds TODAY is occlusion: a page
 * behind a hill rasterizes nothing and casts no vote, so prediction's
 * frustum-only want ranks it below pages actually seen.
 *
 * Depth-tested against the same geometry, so the vote IS visibility. No
 * footprint filter, deliberately: this target is coarser than the render, so a
 * footprint test here would under-request; every covered page votes, and the
 * residency sort spends the extra votes by distance.
 *
 * Encoding: r = vx, g = vz (a page grid is at most 34), b = 1 marks a vote; a
 * cleared texel is no vote. i/255 is exact under the unorm8 round -- the
 * (i+0.5)/255 form lands on the round-to-nearest tie the GL spec leaves
 * implementation-defined, which would shift votes one page on some drivers.
 */

#include "world_origin.glsl"
#include "vt_pages.glsl"

uniform vec4 splatDomain; // the paged material's authored rectangle

in vec3 WorldPos;

layout(location = 0) out vec4 FragColor;

void main() {
    FragColor = vec4(0.0);
    vec2 uv = (authoredPos(WorldPos).xz - splatDomain.xy) / max(splatDomain.zw, vec2(1e-4));
    vec2 pf;
    ivec2 pi;
    if (!vtPageCoord(uv, splatDomain.zw, pf, pi))
        return;
    FragColor = vec4(vec2(pi) / 255.0, 1.0, 1.0);
}
