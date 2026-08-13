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

// Clipmap placement. waterRingBase is the CENTRE patch's half-extent, so the
// outermost ring reaches waterExtent. waterLevelBase is 0 for the centre draw and 1
// for the instanced ring draw -- the two are separate draws, so the level cannot come
// from gl_InstanceID alone.
uniform float waterRingBase;
uniform int waterRingLevels;
uniform int waterGridRes;
uniform int waterLevelBase;
uniform vec3 waterCamPos;

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

/*
 * Where this vertex sits, and whose neighbour it has to agree with.
 *
 * Level 0 is the centre patch; instance N draws ring N+1 at twice the half-extent.
 * The snap is to the COARSEST level's cell for every level, which is what makes the
 * ring boundaries share one grid and tile exactly -- see water.h.
 */
vec2 clipmapPosition(int level, vec2 grid, out float cell) {
    float halfExtent = waterRingBase * exp2(float(level));
    cell = halfExtent * 2.0 / float(waterGridRes);
    float coarsest = waterRingBase * exp2(float(waterRingLevels - 1)) * 2.0 /
                     float(waterGridRes);
    vec2 snapped = floor(waterCamPos.xz / coarsest) * coarsest;
    return snapped + grid * halfExtent * 2.0;
}

void main() {
    int level = waterLevelBase + gl_InstanceID;
    float cell;
    vec2 p = clipmapPosition(level, aGrid, cell);

    OceanSurface s = oceanEvaluate(p, time);

    /*
     * T-junction stitch, on the patch's OUTER edge only.
     *
     * The level outside this one has cells twice as wide, so every second vertex on
     * this edge falls at the midpoint of one of its edges. Left alone, this vertex
     * sits at the true surface height while the neighbour draws a straight line
     * between its endpoints, and the difference is a crack that shows sky at grazing
     * angles.
     *
     * So the odd vertices are made to agree instead of being hidden under a skirt:
     * evaluate the two even neighbours and average, which is by definition the
     * straight line the coarser edge draws there. Exact, and it costs two extra
     * evaluations on one row of vertices per patch.
     */
    if (level < waterRingLevels - 1) {
        vec2 onEdge = step(0.4999, abs(aGrid));
        if (onEdge.x + onEdge.y > 0.5) {
            // Index ALONG the edge: the free axis is whichever one is not pinned.
            vec2 along = onEdge.x > 0.5 ? vec2(0.0, 1.0) : vec2(1.0, 0.0);
            float idx = dot(aGrid + 0.5, along) * float(waterGridRes);
            if (mod(floor(idx + 0.5), 2.0) > 0.5) {
                vec2 step2 = along * cell;
                OceanSurface lo = oceanEvaluate(p - step2, time);
                OceanSurface hi = oceanEvaluate(p + step2, time);
                s.world = 0.5 * (lo.world + hi.world);
                // The normal is left as this vertex's own. Averaging it too would
                // flatten the shading along every boundary into a visible seam,
                // where a position that matches the neighbour is all the crack needs.
            }
        }
    }
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
