#ifndef _GROUND_H_
#define _GROUND_H_

#include "cetra/mesh.h"

/*
 * The island, in one place (spec 11.35 phase 5, reprofiled in 11.44).
 *
 * A BEACH SECTION, not a dome. Three pieces, which is what a real shore is made of:
 *
 *      berm          beach face        terrace
 *   ___________
 *              \____
 *                   \____                        crown, rounded, where the tree stands
 *   ~~~~~~~~~~~~~~~~~~~~~\~~~~~~~~~~~~~~~~~~~    still water
 *                         \______
 *                                \______         one straight slope, through the
 *                                       \____    waterline and on under it
 *
 *   The BERM is a rounded crown that flattens as it runs out to the beach: a cubic Hermite
 *   from slope 0 at the centre to the beach slope at the shore, so the tree stands on
 *   something slightly domed and the sand is already flat by the time it meets the water.
 *
 *   The FACE and the TERRACE are ONE straight slope, continuing unbroken under the water.
 *   That continuity is the point: a beach does not stop at the waterline, and a profile
 *   that changes there puts a crease exactly where the eye is looking.
 *
 * A PARABOLOID was the shape until 11.44 and it is the wrong one, for a reason its own
 * header states: its slope is `2*rise / (t*R)` and therefore STEEPEST at the shore, which
 * is backwards. At H 190 that was 0.31 -- 17 degrees, a 2.16 m rise over 14 m -- so a
 * walker at the water's edge looked OVER a hill and the frame showed a crown silhouette
 * curving away a couple of metres off. Flattening the paraboloid does not fix it either;
 * it is still steepest where a beach is flattest.
 *
 * The old header argued the paraboloid earned its place by having a closed-form shoreline.
 * It does not need one: the direction anyone asks for is level FROM radius, which is the
 * profile evaluated at a point -- see ground_shore_height.
 *
 * THE SURF BAND FOLLOWS THE SLOPE. The water's shoal window is 2.56 METRES of depth, so on
 * a slope s the surf zone is that over s -- 64 m at this slope, which is why
 * TREE_WATER_EXTENT is what it is. A flatter beach is a wider surf zone, always.
 */
#define GROUND_RADIUS 620.0f

// The crown's height above the still water, in metres, and the slope of the face that runs
// down to it. 1:11 is a moderately steep sand beach; flatter reads better and costs surf
// zone that has to fit inside the bed's domain.
//
// The crown sits where the SEA puts a berm: at the run-up limit. The default sea here is
// Hs 3.7 m at Tp 6.7 s, and Stockdon's 2% run-up on a 1:11 face from that is 1.38 m
// (ocean.glsl, oceanSurf) -- so the biggest set of the surf climbs to there, and the tree
// stands on a berm a third of a metre clear of it. At the 0.9 m this was, every set
// overtopped the island. A calmer sea is a lower berm, and it is the sea that would change.
#define GROUND_CROWN_M     1.70f
#define GROUND_BEACH_SLOPE 0.09f

// Where the shore sits, in units -- the radius the profile crosses the still level at.
#define GROUND_SHORE_R (GROUND_SHORE_T * GROUND_RADIUS)
// The crown's rise over the water, in units.
#define GROUND_CROWN_RISE (GROUND_CROWN_M * GROUND_UNITS_PER_METRE)
/*
 * How far the rim sits below the crown -- DERIVED now, where it used to be the paraboloid's
 * one free parameter. The island mesh is built about its crown and translated down by this,
 * the seabed hangs off it, and GROUND_SEABED_DROP is taken from it, so it stays a name even
 * though nothing chooses it any more.
 */
#define GROUND_HEIGHT (GROUND_CROWN_RISE + GROUND_BEACH_SLOPE * (GROUND_RADIUS - GROUND_SHORE_R))

/*
 * SCALE. The anchor is the grass: blades are authored at 5.5 units and grass stands about
 * 0.25 m, which squares with the tree at 250 units for a mature 11 m.
 *
 * Here rather than in `player.h`, which documented the same derivation first, because it is
 * a property of the WORLD and not of the walker -- there are frames with no player in them.
 */
#define GROUND_UNITS_PER_METRE 22.0f

// Where the waterline lands, as a fraction of GROUND_RADIUS -- so the shore is at 310 units,
// which is where 11.32 had it and where the camera (600 back) frames it as an island rather
// than as a beach it is standing on.
#define GROUND_SHORE_T 0.5f

/*
 * How far the shore wanders off the circle, and over what distance.
 *
 * A surface of revolution has a CIRCLE for a waterline, and no amount of shading hides that:
 * it reads as the edge of a dome from every angle, because that is what it is. Two octaves of
 * noise on the HEIGHT is the whole fix -- at the beach slope an amplitude of a few centimetres
 * moves the waterline by tens of units, so the shore bays and points without the profile
 * having to stop being a beach.
 *
 * In the header rather than beside the noise because the island mesh has to be fine enough to
 * CARRY them, which is what GROUND_MESH_SEGMENTS is derived from.
 */
#define GROUND_WOBBLE_M    0.16f
#define GROUND_WOBBLE_SPAN 210.0f
// The second octave, and so the finest feature the ground has anywhere.
#define GROUND_WOBBLE_FINE (GROUND_WOBBLE_SPAN * 0.37f)

/*
 * Segments the island mesh needs around, DERIVED from the noise it has to carry.
 *
 * The waterline is a CONTOUR of ground_height_at, so its shape is only ever what the mesh can
 * represent there -- and the mesh carries the shore at one vertex per segment, spaced
 * 2*pi*GROUND_SHORE_R/segments. At the 64 this app passed until 11.44 that is 30 units
 * against a finest octave of 78: 2.6 samples per period, BELOW Nyquist. So the noise written
 * to break the circle was being aliased into flat facets -- which is why the waterline showed
 * a staircase, and why it still read as an arc even though the field it contours does not.
 * One cause, both symptoms.
 *
 * Eight samples per period is the bar, which is about where a curve stops showing its chords
 * at walking distance. The RINGS need no such rule and did not have the defect: they are 4.8
 * units apart, 16 to a period already, which is why the aliasing was angular only and printed
 * as steps along the shore rather than as rings across it.
 */
#define GROUND_SHORE_SAMPLES 8.0f
#define GROUND_MESH_SEGMENTS \
    ((int)(6.28318531f * GROUND_SHORE_R * GROUND_SHORE_SAMPLES / GROUND_WOBBLE_FINE))

/*
 * The still-water height: the ground's own height at the shore radius (spec 11.44).
 *
 * The sea's level is DERIVED from the ground rather than picked, and this is where that
 * derivation lives so the app and the beach banding below cannot answer it differently.
 * Evaluated rather than solved in closed form -- the direction anyone needs is level FROM
 * radius, which is the profile at a point.
 */
float ground_shore_height(void);

/*
 * Beach banding, in METRES above the still water, and metres because GROUND_UNITS_PER_METRE
 * is right there and a beach is a physical thing (spec 11.44).
 *
 * These drive VERTEX COLOUR, not a second texture, and that is forced rather than chosen:
 * pbr_frag declares sixteen of sixteen samplers, so the terrain gets one albedo map and the
 * large-scale colour has to arrive another way. The map is near-neutral grain (procedural/
 * sand.h) and these bands supply the hue, which is also why the beach can grade into the
 * upland continuously instead of meeting it at a material boundary.
 *
 * The dry sand runs up to the run-up limit, because that is what a beach face IS: the sand
 * the sea reaches. The berm sits just above it (GROUND_CROWN_M) and the grass takes the
 * berm, where the tree is. The wet strip is the always-wet last fifth of a metre at the
 * still line; what is wet ABOVE it at any moment is the swash's business, not a band's.
 */
#define GROUND_WET_SAND_M 0.18f
#define GROUND_DRY_SAND_M 1.45f
#define GROUND_UPLAND_M   1.65f

// Vertex colour for a point on the ground, sRGB, as pbr_frag decodes it. Submerged sand
// below the water, a wet band just above it, dry sand, then the upland it grades into.
void ground_beach_color(float height_above_water, vec4 out);

/*
 * How far the seabed mesh reaches, and how deep it goes past the rim (spec 11.35 phase 6).
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
