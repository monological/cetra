#version 330 core

/*
 * Refracted-grid caustics (spec 13.2), after Evan Wallace, first half: trace every lattice
 * corner once. One texel per corner of a (G+1)^2 lattice laid over the water: the key light
 * enters the surface there, refracts through the normal the water is shaded with, and lands on
 * the floor. The second half, water_caustic_vert, draws the lattice from these and measures
 * each corner's concentration from its neighbours.
 *
 * Out, in METRES from the target's corner -- small numbers, because the next pass differences
 * neighbouring corners and an absolute world position of a few thousand units leaves a
 * difference with no precision in it: .xy where the light LANDED, .zw where in the beam it came
 * from.
 */

layout(location = 0) out vec4 Landed;

uniform vec2 causticGridOrigin;   // world xz of lattice corner (0, 0)
uniform float causticCell;        // world units between lattice corners
uniform vec2 causticTargetOrigin; // world xz of the target's corner
uniform vec3 causticKeyDir;       // the direction the key light TRAVELS, unit, y < 0
uniform float causticFloorY;      // the reference plane rays land on where there is no baked bed
uniform float time;
uniform float waterIor;           // the surface's own, so the trace bends as the lookup does

#include "ocean.glsl"

void main() {
    vec2 p = causticGridOrigin + floor(gl_FragCoord.xy) * causticCell;

    // The surface through which the light enters, and the normal it meets -- the same one the
    // water is shaded with, short band included, or the lens is not the water that is drawn.
    // The footprint is the lattice cell: what a cell cannot resolve it cannot focus either.
    OceanSurface s = oceanEvaluateAt(p, time, oceanBed(p), causticCell);
    vec3 n = oceanShadingNormal(s.normal, s.world.xz, 1.0, causticCell);
    vec3 r = refract(causticKeyDir, n, 1.0 / waterIor);

    /*
     * The floor: the baked bed where there is one, which is exact at every depth, and the
     * reference plane elsewhere. From the plane's hit, fixed-point steps along the ray onto the
     * bed's height; a step moves the hit by |grad bed| tan(refracted angle), and the refracted
     * angle is under 41 degrees, so this closes on any bed shallower than about 48 degrees. A
     * bed standing above the entry point, a dry shoal, holds the light at the entry.
     *
     * Past the critical angle there is no transmitted ray, and `down` lands it far off the target.
     */
    float down = min(r.y, -1.0e-4);
    float t = (causticFloorY - s.world.y) / down;
    for (int i = 0; i < 4; i++) {
        float bedY;
        float floorY = oceanBedHeight(s.world.xz + r.xz * t, bedY) ? bedY : causticFloorY;
        t = max((floorY - s.world.y) / down, 0.0);
    }
    vec3 landed = s.world + r * t;

    /*
     * The SOURCE is the entry point carried back along the incoming beam to the still plane,
     * not the lattice parameter. A choppy or Gerstner surface moves its points sideways, so the
     * parameter does not say where in the beam's cross-section this light was; the beam does.
     * Over flat water the two are the same point, which is what makes the flat answer 1.
     */
    vec2 src = s.world.xz + causticKeyDir.xz * ((waterLevel - s.world.y) / causticKeyDir.y);

    Landed = vec4((landed.xz - causticTargetOrigin) / waterUnitsPerMetre,
                  (src - causticTargetOrigin) / waterUnitsPerMetre);
}
