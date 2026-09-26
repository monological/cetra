#version 330 core

/*
 * The surface query (spec 13.1): one texel per query slot, each answering "where is the
 * water over this world (x, z)" from the same oceanEvaluateAt the surface is drawn with.
 *
 * Both wave models displace sideways as well as up, so the water over a world point did not
 * START there: the parameter that lands on the query has to be recovered by inverting the
 * horizontal map. Fixed-point, p <- q - u(p) with u the horizontal part of the shoaled
 * displacement, the loop water_waves.c runs on the CPU -- same step cap, same tolerance,
 * same last-step residual, which is exact at the final iterate for the reason that file
 * gives.
 *
 * Footprint 0, which is full detail on both models: every Gerstner octave, mip 0 of every
 * band. The query answers about the water, not about how coarsely one lattice cell happened
 * to draw it.
 *
 * Out: (height, normal.x, normal.z, residual). normal.y is recoverable from the other two.
 */

layout(location = 0) out vec4 Answer;

#include "water_probe_constants.glsl"

uniform vec2 probePoints[WATER_PROBE_MAX];
uniform int probeCount;
uniform float time;
uniform float probeEps; // world units

#include "ocean.glsl"

void main() {
    int slot = int(gl_FragCoord.x);
    if (slot >= probeCount) {
        Answer = vec4(0.0);
        return;
    }
    vec2 q = probePoints[slot];
    vec2 p = q;
    vec2 prevU = vec2(0.0);
    float residual = 0.0;
    for (int step = 0; step < WATER_WAVES_INVERSE_MAX_STEPS; step++) {
        OceanBed bed = oceanBed(p);
        vec2 u = oceanEvaluateAt(p, time, bed, 0.0).world.xz - p;
        p = q - u;
        if (step > 0) {
            residual = length(u - prevU);
            if (residual <= probeEps)
                break;
        }
        prevU = u;
    }
    OceanSurface s = oceanEvaluateAt(p, time, oceanBed(p), 0.0);
    Answer = vec4(s.world.y, s.normal.x, s.normal.z, residual);
}
