#version 330 core

/*
 * Refracted-grid caustics (spec 13.2), after Evan Wallace: a lattice laid over the water,
 * each vertex refracting the key light through the surface normal and landing on the floor.
 * The displaced lattice is rasterised into the caustics target, where the fragment stage
 * turns how much each cell shrank or spread into how much light it concentrates.
 *
 * No attributes: the vertex is its index into a (G+1)^2 lattice, which the index buffer
 * stitches into triangles.
 */

uniform vec2 causticGridOrigin;   // world xz of lattice vertex (0, 0)
uniform float causticCell;        // world units between lattice vertices
uniform int causticGridN;         // cells per side
uniform vec2 causticTargetOrigin; // world xz of the target's corner
uniform float causticTargetSize;  // world units the target spans
uniform vec3 causticKeyDir;       // the direction the key light TRAVELS, unit, y < 0
uniform float causticFloorY;      // world y the rays land on
uniform float time;

// Where this ray's light came from, in METRES from the target's corner -- small numbers,
// because the fragment stage differentiates it and an absolute world position of a few
// thousand units leaves a derivative with no precision left.
out vec2 vSrc;

#include "ocean.glsl"

const float WATER_IOR = 1.3335;

void main() {
    int side = causticGridN + 1;
    vec2 ij = vec2(float(gl_VertexID % side), float(gl_VertexID / side));
    vec2 p = causticGridOrigin + ij * causticCell;

    // The surface through which the light enters, and the normal it meets -- the same one the
    // water is shaded with, short band included, or the lens is not the water that is drawn.
    // The footprint is the lattice cell: what a cell cannot resolve it cannot focus either.
    OceanSurface s = oceanEvaluateAt(p, time, oceanBed(p), causticCell);
    vec3 n = oceanShadingNormal(s.normal, s.world.xz, 1.0);
    vec3 r = refract(causticKeyDir, n, 1.0 / WATER_IOR);

    // Past the critical angle there is no transmitted ray; land it far off the target.
    float down = min(r.y, -1.0e-4);
    vec3 landed = s.world + r * ((causticFloorY - s.world.y) / down);

    /*
     * The SOURCE is the entry point carried back along the incoming beam to the still plane,
     * not the lattice parameter. A choppy or Gerstner surface moves its points sideways, so the
     * parameter does not say where in the beam's cross-section this light was; the beam does.
     * Over flat water the two are the same point, which is what makes the flat answer 1.
     */
    vec2 src = s.world.xz + causticKeyDir.xz * ((waterLevel - s.world.y) / causticKeyDir.y);
    vSrc = (src - causticTargetOrigin) / waterUnitsPerMetre;

    vec2 uv = (landed.xz - causticTargetOrigin) / causticTargetSize;
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
