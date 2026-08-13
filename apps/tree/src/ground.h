#ifndef _GROUND_H_
#define _GROUND_H_

#include "cetra/mesh.h"

/*
 * The island, in one place (spec 11.34 phase 5).
 *
 * A paraboloid of revolution: y = H*(1 - t^2) - H over t = d/R, translated so the CROWN
 * sits at y = 0 where the tree roots start and the rim reaches -H. That single expression
 * is what the mesh, the grass, the water's shoaling bed and the leaf litter all read, so
 * the surface and everything standing on it cannot drift apart.
 *
 * WHY A PARABOLOID and not a nicer island silhouette: its shoreline has a closed form.
 * A still-water level of -H*t^2 puts the waterline at exactly t*R, so the level is derived
 * from the shape rather than tuned against it, and it stays derived when the shape changes.
 * A profile without that inverse would need the level re-tuned by eye every time.
 *
 * THE THREE NUMBERS ARE NOT INDEPENDENT, and working that out is most of phase 5. Write
 * the rise of the crown above the waterline as `rise = H*t^2` and the island's width above
 * water as `2*t*R`; then the beach's slope is `2*rise / (t*R)` -- fixed by those two alone,
 * whatever H and R separately are.
 *
 *   The island has to FIT THE FRAME. At the camera's distance the frame spans about 620
 *   units across, so `2*t*R ~ 620`. Wider and the shore leaves frame on both sides and it
 *   reads as a coastline rather than as an island -- measured, at t = 0.5 over R = 900.
 *
 *   The beach has to be SHOAL-ABLE. The water's shoal window is 2.56 units of DEPTH, so on
 *   a slope s it is 2.56/s wide, and the bed is baked at 2*extent/WATER_BED_RES per texel.
 *   Under about three texels the shore-foam band is one linear segment wide.
 *
 * Those two pin the slope at ~0.31 (17 degrees) and the rise at ~48 units, which is 14% of
 * the frame height -- a hill the eye reads as a hill. 11.32's dome rose 2.45 units at 2.5
 * degrees and read as a sandbar, which is what this phase was called to fix.
 */
#define GROUND_RADIUS 620.0f
#define GROUND_HEIGHT 190.0f

// Where the waterline lands, as a fraction of GROUND_RADIUS -- so the shore is at 310 units,
// which is where 11.32 had it and where the camera (600 back) frames it as an island rather
// than as a beach it is standing on.
#define GROUND_SHORE_T 0.5f

/*
 * How far the seabed mesh reaches, and how deep it goes past the rim (spec 11.34 phase 6).
 *
 * The sea itself reaches the horizon, so the bed cannot cover all of it and does not need to:
 * it only has to cover what can be SEEN through the water, and this app's own camera far
 * plane is 3000.
 *
 * The DROP is derived, not chosen. `ground_height_at` eases the bed down with 1 - (1 - u)^2,
 * whose slope at the rim is 2*DROP/span; the dome's own slope arriving there is 2H/R. Setting
 * those equal gives DROP = (H/R) * span, so the flank and the bed meet with no kink -- one
 * surface rather than a disc sitting on a plate, which is what the eye reads the moment the
 * camera goes under. Pick DROP by hand and that continuity is silently gone.
 */
#define GROUND_SEABED_RADIUS 2800.0f
#define GROUND_SEABED_DROP \
    ((GROUND_HEIGHT / GROUND_RADIUS) * (GROUND_SEABED_RADIUS - GROUND_RADIUS))

/*
 * Surface height at a world XZ, world space, translation included.
 *
 * Continuous everywhere, including across the rim: inside it is the dome, outside it descends
 * to the seabed and flattens. ONE authority, so the island mesh, the grass, the seabed mesh,
 * the water's shoaling bed and the leaf litter cannot disagree about where the ground is.
 * (The header said "Flat (0) beyond the rim" for three specs while the code returned
 * -GROUND_HEIGHT, which was neither.)
 */
float ground_height_at(float x, float z);

// Surface normal at a world XZ, by central differences on ground_height_at -- so a consumer
// cannot derive a normal from a different surface than the one it stands on.
void ground_normal_at(float x, float z, vec3 out);

// Curvature-matched sphere for the dome's crown: centre in `out_center`, returning the
// radius. A paraboloid with H far under R is a sphere to within a couple of units over the
// WHOLE dome here, so this is what an analytic collider can use where a plane cannot follow
// the surface at all.
float ground_sphere_fit(vec3 out_center);

// The island mesh: `rings` steps from crown to rim, `segments` around, UVs tiled `uv_tiles`
// times so a terrain-sized disc keeps texel detail. Positions, normals, tangents and the
// AABB; the caller owns upload.
void ground_build_mesh(Mesh* mesh, int rings, int segments, float uv_tiles);

/*
 * The seabed: an annulus from the dome's rim out to GROUND_SEABED_RADIUS.
 *
 * Rings are spaced GEOMETRICALLY rather than evenly, because what it covers spans a factor of
 * four and half of it is at grazing incidence under saturated water -- even spacing would put
 * most of the vertices where nothing can be seen. Shares the rim vertices' height with the
 * island by construction, since both read ground_height_at.
 *
 * Returns false if the builder ran out of memory, leaving the mesh untouched.
 */
bool ground_build_seabed(Mesh* mesh, int rings, int segments, float uv_tiles);

#endif // _GROUND_H_
