#version 330 core

// Water surface vertex stage (spec 11.32). The mesh is a plain indexed grid over
// [-0.5, 0.5]^2 in the XZ plane, positions only: a displaced surface's normal is
// the derivative of the displacement, so a stored normal would only be
// overwritten.
layout(location = 0) in vec2 aGrid;

uniform mat4 view;
uniform mat4 projection; // TAA-jittered, like every other raster in the frame
uniform mat4 uCurrViewProjNoJitter;
uniform mat4 uPrevViewProj;
uniform float waterExtent;
uniform float time;
uniform float uDeltaTime;

out vec3 WorldPos;
out vec3 ViewPos;
out vec3 Normal;
out vec4 CurrClip;
out vec4 PrevClip;

// Required for the same reason pbr_vert declares it: without the qualifier the
// driver may schedule this position's arithmetic differently from another
// program's, and under a depth test a fragment landing a bit behind the depth it
// just wrote disappears rather than shading subtly wrong.
invariant gl_Position;

#include "ocean.glsl"

void main() {
    vec2 p = aGrid * (waterExtent * 2.0);

    OceanSurface s = oceanEvaluate(p, time);
    WorldPos = s.world;
    Normal = s.normal;

    vec4 viewPos = view * vec4(s.world, 1.0);
    ViewPos = viewPos.xyz;

    // Motion vectors from the UN-JITTERED pair while gl_Position below uses the
    // jittered projection. The jitter is a sub-pixel sampling offset; letting it
    // into the velocity would report it to TAA and motion blur as scene motion.
    //
    // The previous position re-evaluates the surface at t - dt rather than
    // reusing this one, so an animated surface reports the motion it actually
    // has. On a still plane the two agree exactly and the velocity is zero,
    // which is correct and not a special case.
    CurrClip = uCurrViewProjNoJitter * vec4(s.world, 1.0);
    OceanSurface sPrev = oceanEvaluate(p, time - uDeltaTime);
    PrevClip = uPrevViewProj * vec4(sPrev.world, 1.0);

    gl_Position = projection * viewPos;
}
