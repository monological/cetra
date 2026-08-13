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
 * Surface height at a world XZ, world space, translation included.
 *
 * Beyond the rim it holds at -GROUND_HEIGHT rather than at 0: the dome is a solid that ends,
 * so the ground outside it continues at the depth the rim reached. (This said "Flat (0)
 * beyond the rim" for three specs while the code did the other thing.)
 */
float ground_height_at(float x, float z);

// Curvature-matched sphere for the dome's crown: centre in `out_center`, returning the
// radius. A paraboloid with H far under R is a sphere to within a couple of units over the
// WHOLE dome here, so this is what an analytic collider can use where a plane cannot follow
// the surface at all.
float ground_sphere_fit(vec3 out_center);

// The island mesh: `rings` steps from crown to rim, `segments` around, UVs tiled `uv_tiles`
// times so a terrain-sized disc keeps texel detail. Positions, normals, tangents and the
// AABB; the caller owns upload.
void ground_build_mesh(Mesh* mesh, int rings, int segments, float uv_tiles);

#endif // _GROUND_H_
