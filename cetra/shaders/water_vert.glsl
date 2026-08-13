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
uniform float time;
uniform float uDeltaTime;
// waterExtent is declared by ocean.glsl, which needs it to index the bed field.

out vec3 WorldPos;
out vec3 ViewPos;
out vec3 Normal;
out vec4 CurrClip;
out vec4 PrevClip;
out float Jacobian;
out float Shoal;

// Kept for symmetry with pbr_vert, where it is load-bearing because the depth
// prepass rasterizes the same triangles from a second program. Nothing rasterizes
// this grid twice today, so here it is harmless rather than required -- and cheap
// insurance if a depth-only water pass ever appears.
invariant gl_Position;

#include "ocean.glsl"

void main() {
    vec2 p = aGrid * (waterExtent * 2.0);

    OceanSurface s = oceanEvaluate(p, time);
    WorldPos = s.world;
    Normal = s.normal;
    Jacobian = s.jacobian;
    Shoal = s.shoal;

    vec4 viewPos = view * vec4(s.world, 1.0);
    ViewPos = viewPos.xyz;

    // Motion vectors from the UN-JITTERED pair while gl_Position below uses the
    // jittered projection. The jitter is a sub-pixel sampling offset; letting it
    // into the velocity would report it to TAA and motion blur as scene motion.
    //
    // Gerstner re-evaluates at t - dt, so its waves report the motion they have.
    // The SPECTRAL path cannot: the cascades hold one instant, the current one, and
    // reading them at t - dt returns the same surface. So a spectral ocean reports
    // CAMERA motion only, and TAA reprojects travelling wave detail as if it were
    // static. Fixing it needs the previous frame's transformed cascades kept alive
    // -- a third buffer, since pass 0 overwrites the one that still holds them --
    // and that is not built. The branch also avoids four texture fetches whose
    // result is known to equal s.world.
    CurrClip = uCurrViewProjNoJitter * vec4(s.world, 1.0);
    vec3 prevWorld = s.world;
    if (waveModel == 0)
        prevWorld = oceanEvaluate(p, time - uDeltaTime).world;
    PrevClip = uPrevViewProj * vec4(prevWorld, 1.0);

    gl_Position = projection * viewPos;
}
