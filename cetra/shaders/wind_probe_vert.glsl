#version 330 core

// The wind bound's instrument (spec 11.54). Vertex-only: it captures windOffset
// into a transform-feedback buffer under GL_RASTERIZER_DISCARD and never
// rasterizes.
//
// WHY THIS EXISTS RATHER THAN A CPU PORT. wind.c's wind_max_offset is a
// conservative BOUND on what windOffset displaces a vertex by, and the two are
// written in different languages from the same coefficients. A C mirror of the
// displacement would check the bound against the mirror, leaving the GLSL a
// third hand-maintained copy -- and a term added HERE would then pass, which is
// exactly the failure the probe is for. Measured at: a term added to a C port
// moved the reading 0.966 -> 1.258, and the same term added to this file moved
// it not at all.
//
// So the probe drives the real chunk. `wind.glsl` below is the same include the
// four rasterizing vertex programs pull, so there is nothing to keep in step.
//
// Every input is an ATTRIBUTE, including time and the object origin. windOffset
// already takes both as parameters, so feeding them per-vertex is the identical
// function -- and it is what lets one draw cover the whole grid of positions,
// UVs, times and origins instead of one draw per time step.
layout(location = 0) in vec3 aRest;
layout(location = 1) in vec2 aUV0;
layout(location = 2) in vec2 aUV1;
layout(location = 3) in float aTime;
// Hashed by windObjectPhase into a per-object phase. Fed rather than swept
// because the GPU's own fract(sin(...)) is the value under test; a CPU cannot
// reproduce its low bits and would have had to sweep the phase to stay honest.
layout(location = 4) in vec3 aOrigin;

// uWindMaskMinY / uWindMaskMaxY arrive with the rest of the wind uniforms.
#include "wind.glsl"

out vec3 oOffset;

void main() {
    oOffset = windOffset(aRest, aUV0, aUV1, aTime, aOrigin);
}
