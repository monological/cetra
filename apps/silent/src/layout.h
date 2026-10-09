#ifndef _SILENT_LAYOUT_H_
#define _SILENT_LAYOUT_H_

#include <math.h>

/*
 * Where everything is, in metres. The street runs along X with its centre line
 * at z = 0; the player's house stands on the +z side, its front wall facing the
 * road. Wall coordinates are wall CENTRE lines.
 *
 * Two storeys (spec 13.13). The front band holds the parlour, the hall and the
 * kitchen below, and the study, a box room and a bedroom above; behind it the
 * great hall rises through both, open to the roof, with the stair up its east
 * wall to a gallery along its front. An octagonal tower stands on the front
 * corner west of the door, the parlour's bay below and the study's above.
 *
 *     z            ground floor                         upper floor
 *     ^   +-----------------------------+    +-----------------------------+
 *     |   |   great hall     [hearth]   |    |    (open to the roof)       |
 *     |   |                           S |    |                             |
 *     |   |                           S |    |========= gallery ==========S|
 *     |   +---------+==arch==+----------+    +---------+---------+---------+
 *     |  /  parlour |  hall  =  kitchen |   /  study   | box rm  | bedroom |
 *     | (  tower    |        |          |  (  tower    |         |         |
 *     |  \          |        |          |   \          |         |         |
 *     |   +---------+--[d]---+--[win]---+    +---------+---------+---------+
 *     |                porch                       S = the stair
 *     |  ======================= sidewalk
 *     |  -----------------------  road (centre z = 0)
 *     +--------------------------------------------> x
 */

#define FLOOR_Y  0.30f              // the house floor; the porch is level with it
#define CEIL_Y   2.90f              // the ground storey's ceiling
#define FLOOR2_Y 3.15f              // the upper floor; its boards and joists fill CEIL_Y..FLOOR2_Y
#define CEIL2_Y  5.75f              // the upper storey's ceiling
#define EAVE_Y   5.90f              // where the walls stop and the roof starts
#define SLAB     0.12f              // a ceiling's thickness under a floor or the attic
#define BASE_TOP (FLOOR_Y + 0.4f)   // the rubble base round the outside walls
#define BOARDS_Y (BASE_TOP + 0.09f) // where the boards start, over the base's water table

#define EXT_WALL 0.18f // exterior wall thickness
#define INT_WALL 0.10f // interior

#define HOUSE_X0      (-5.0f)
#define HOUSE_X1      5.0f
#define HOUSE_FRONT_Z 10.0f
#define HOUSE_BACK_Z  19.5f
// The outside walls' outer faces: the footprint a house stands on.
#define HOUSE_OUT_X0 (HOUSE_X0 - 0.5f * EXT_WALL)
#define HOUSE_OUT_X1 (HOUSE_X1 + 0.5f * EXT_WALL)
#define HOUSE_OUT_Z0 (HOUSE_FRONT_Z - 0.5f * EXT_WALL)
#define HOUSE_OUT_Z1 (HOUSE_BACK_Z + 0.5f * EXT_WALL)

#define HALL_X0 (-1.5f) // the hall runs from the front door to the great hall between these
#define HALL_X1 0.0f    // and this is also the kitchen's west wall
// The hall's inner faces, and its west wall's other face, which is the next room's.
#define HALL_IN_X0  (HALL_X0 + 0.5f * INT_WALL)
#define HALL_IN_X1  (HALL_X1 - 0.5f * INT_WALL)
#define HALL_OUT_X0 (HALL_X0 - 0.5f * INT_WALL)

// The front band's back wall, which is the great hall's front.
#define KITCHEN_BACK_Z 13.8f
// The front band's inner faces, which every room in it shares: the kitchen, the hall, the study.
#define BAND_Z0 (HOUSE_FRONT_Z + 0.5f * EXT_WALL)
#define BAND_Z1 (KITCHEN_BACK_Z - 0.5f * INT_WALL)

// The kitchen window, in the front wall, and the front door.
#define KITCHEN_WIN_X0   1.5f
#define KITCHEN_WIN_X1   3.7f
#define KITCHEN_WIN_SILL (FLOOR_Y + 1.02f)
#define KITCHEN_WIN_HEAD (FLOOR_Y + 2.18f)

// The Gothic house's room in the kitchen's footprint is a dining room (spec 13.25), and its
// window, between the kitchen's edges, is pointed and comes down to a seat under it.
#define DINING_WIN_SILL   (FLOOR_Y + 0.6f)
#define DINING_WIN_SPRING (FLOOR_Y + 1.8f)
#define DINING_WIN_RISE   0.6f

#define FRONT_DOOR_X0 (-1.15f)
#define FRONT_DOOR_X1 (-0.30f)
#define DOOR_HEAD     (FLOOR_Y + 2.05f)
// The front door's pointed head: where it springs, and how far it rises above that.
#define FRONT_DOOR_SPRING (FLOOR_Y + 1.95f)
#define FRONT_DOOR_RISE   0.45f
// A door leaf's thickness. Every leaf hangs on its wall's centre line but the front door, which
// hangs against the front wall's inner face, set 5 mm into its frame: this is its middle.
#define DOOR_THICK   0.05f
#define FRONT_DOOR_Z (HOUSE_FRONT_Z + 0.5f * EXT_WALL - 0.5f * DOOR_THICK - 0.005f)

// The doorway from the hall into the kitchen, in the x = HALL_X1 wall.
#define KITCHEN_DOOR_Z0 12.75f
#define KITCHEN_DOOR_Z1 13.55f

// The kitchen's inner faces: where the room actually is.
#define KITCHEN_X0 (HALL_X1 + 0.5f * INT_WALL)
#define KITCHEN_X1 (HOUSE_X1 - 0.5f * EXT_WALL)
#define KITCHEN_Z0 BAND_Z0
#define KITCHEN_Z1 BAND_Z1

// The hall wall's run, from just short of the kitchen doorway toward the
// window wall: the fridge first, then the stove, whose centre the hood tube
// hangs over.
#define STOVE_RUN_Z (KITCHEN_DOOR_Z0 - 0.2f)
#define STOVE_Z     (STOVE_RUN_Z - 1.12f)

// How far the mahogany panelling stands off a wall: what stands against a panelled wall
// stands this far out.
#define PANEL_DEPTH 0.021f

// The great hall's inner faces.
#define GREAT_X0 (HOUSE_X0 + 0.5f * EXT_WALL)
#define GREAT_X1 (HOUSE_X1 - 0.5f * EXT_WALL)
#define GREAT_Z0 (KITCHEN_BACK_Z + 0.5f * INT_WALL)
#define GREAT_Z1 (HOUSE_BACK_Z - 0.5f * EXT_WALL)

// The gallery along the great hall's front at the upper floor, out to its rail.
#define GALLERY_Z1 15.05f

// The stair up the great hall's east wall: STAIR_RISERS of STAIR_RISE from the
// floor to the gallery, climbing toward -z and landing at its edge. Its going
// leaves room at its foot, by the back wall, to stand and face up it.
#define STAIR_X0     3.9f
#define STAIR_RISERS 15
#define STAIR_RISE   ((FLOOR2_Y - FLOOR_Y) / (float)STAIR_RISERS)
#define STAIR_GOING  0.25f
#define STAIR_FOOT_Z (GALLERY_Z1 + (float)(STAIR_RISERS - 1) * STAIR_GOING)

// The hearth, centred on the back wall on the hall's axis, so it is what the
// arch frames as you come down the hall.
#define HEARTH_X     (0.5f * (HALL_X0 + HALL_X1))
#define HEARTH_HALF  1.75f // the fireplace's half width along the wall, which the panelling meets
#define HEARTH_FRONT 0.92f // how far its stone runs out into the hall, short of the rug

// The great hall's two trusses, clear of the lancets down its sides, and how far each one's
// timbers reach either side of its middle.
#define TRUSS_Z0   16.25f
#define TRUSS_Z1   18.8f
#define TRUSS_HALF 0.1f

// The tower: a regular octagon centred on the front corner west of the door,
// its faces' centre lines TOWER_APOTHEM from the middle. The house's front and
// west walls butt into the middles of its east and north faces.
#define TOWER_X       HOUSE_X0
#define TOWER_Z       HOUSE_FRONT_Z
#define TOWER_APOTHEM 2.3f
#define TOWER_TAN     0.41421356f // tan 22.5: half a face over the apothem, for any apothem
#define TOWER_HALF    (TOWER_APOTHEM * TOWER_TAN)       // half a face
#define TOWER_OUTER   (TOWER_APOTHEM + 0.5f * EXT_WALL) // the apothem of the walls' outer faces
#define TOWER_TOP     9.6f                              // the walls' top, under the spire
#define TOWER_CEIL_Y  9.0f                              // the study's tower bay rises to this
#define TOWER_SPIRE_Y 15.5f

/*
 * The skirt roof across the front at the upper floor, PENT_PITCH a metre, which both houses
 * carry: a deep one over the porch, and a strip past it over the kitchen window, shallow enough
 * that the wind still drives the rain onto the window's lower half.
 */
#define PENT_Y        3.3f // where it meets the wall
#define PENT_PITCH    0.47f
#define PENT_DEPTH    0.45f
#define PORCH_ROOF_X1 0.9f
#define PENT_THICK    0.08f

#define PORCH_X0 (-2.4f)
#define PORCH_X1 0.8f
#define PORCH_Z0 8.5f
// The front path's edges, from the pavement up to the porch steps.
#define PATH_X0 (-1.35f)
#define PATH_X1 (-0.25f)

// The street.
#define ROAD_HALF_WIDTH   4.0f     // asphalt, centre line at z = 0
#define ROAD_Y            (-0.15f) // below the kerb
#define SIDEWALK_WIDTH    2.0f
#define STREET_HALF_WIDTH (ROAD_HALF_WIDTH + SIDEWALK_WIDTH) // the road and a sidewalk
#define STREET_HALF_LEN   45.0f // along X; the fog ends it well before this
#define GROUND_DEPTH      0.4f  // the boxes the flat ground stands on, below their tops

// Our side's back fences (spec 13.35): the yards stop here, and past them the ground falls away
// through the woods. The far side's stand at FAR_BACK_FENCE_Z, short of the terrace's back.
#define BACK_FENCE_Z     34.0f
#define FAR_BACK_FENCE_Z (-30.0f)

/*
 * The lots either side of the street (spec 13.35), on one grid TERRACE_LOT_WIDTH apart: the far
 * side's lots lie between lines at LOT_GRID_X0 + TERRACE_LOT_WIDTH * k, a house midway along
 * each, and our side's between lines midway between those, its houses on the far side's lines --
 * ours at x = 0. Each side's end lots run on to the street's ends, and each side's end fences
 * stand in from them.
 */
#define LOT_GRID_X0          (-STREET_HALF_LEN + 3.0f)
#define NEAR_LOTS            7 // a vacant lot at each end, and two neighbours either side of ours
#define HOME_LOT             3
#define NEAR_END_FENCE_INSET 0.4f
#define FAR_END_FENCE_INSET  0.2f

// The utility poles down the far sidewalk: the line they stand on, the westmost one's place on
// the cross street's corner, and their two wires' height and offset either side.
#define POLE_Z        (-(STREET_HALF_WIDTH - 0.6f))
#define POLE_WEST_X   (-47.0f)
#define POLE_WIRE_Y   7.95f
#define POLE_WIRE_OFF 0.65f

/*
 * The crossroads at the street's west end (spec 13.35): a cross street, each of its arms closed by
 * a barricade, and straight on, the road breaking off at the lip of a chasm the fog fills. The
 * lip wanders a metre or two either side of CHASM_X, and is straight across the road's end.
 */
#define CROSS_X       (-52.0f) // the cross street's centre line
#define CROSS_NORTH_Z (-18.0f) // the north arm's barricade
#define CROSS_SOUTH_Z 22.0f    // the south arm's, the way the lake road will take
#define CROSS_Z0      (-32.0f) // where the cross street's arms run out into the woods
#define CROSS_Z1      36.0f
#define CROSS_X0      (-67.0f) // the crossroads' flat ground runs from here to the street
#define CHASM_X       (-69.0f)

/*
 * The far side stands on a TERRACE (spec 13.35): six level lots, one a house, behind a concrete
 * retaining wall along the back of the far sidewalk, each lot a step higher than the one east of
 * it. Stairs go up through the wall to each front door. The houses stand further back than ours,
 * since a flight up to the highest lot needs more than four metres.
 */
#define TERRACE_WALL_Z     (-STREET_HALF_WIDTH) // the wall's street face
#define TERRACE_WALL_THICK 0.4f
#define TERRACE_LOTS       6
#define TERRACE_LOT_WIDTH  14.0f // a lot's frontage, the far houses' spacing
#define TERRACE_LOW        1.6f  // the east lot's height above the sidewalk
#define TERRACE_HIGH       2.6f  // the west lot's
#define TERRACE_BACK_Z     (-32.0f)
#define FAR_HOUSE_FRONT_Z  (-13.0f) // the far houses' facades

// Where the Gothic house stands (spec 13.25): past the street's east end and up the hill, its
// front toward the drive that climbs to it. Everything above the street is in the house's OWN
// coordinates, which the mansion's kit and mansion_at move here; the player's house stands at
// the plan's own origin, on the lot the Gothic house used to.
#define MANSION_X 115.0f
#define MANSION_Y 8.0f
#define MANSION_Z 40.0f

// The mansion's grounds: level at MANSION_Y round the house, inside its fence.
#define GROUNDS_X0 (MANSION_X - 17.0f)
#define GROUNDS_X1 (MANSION_X + 17.0f)
#define GROUNDS_Z0 (MANSION_Z - 4.0f)
#define GROUNDS_Z1 (MANSION_Z + 28.0f)

// The walkable world: x from the chasm to WORLD_X1, past the hill the drive climbs to the
// mansion, and z from the woods behind the far side's lots (spec 13.35) to WORLD_Z1.
#define WORLD_X1 150.0f
#define WORLD_Z0 (-80.0f)
#define WORLD_Z1 85.0f

// The woods behind both sides (spec 13.35) run east to here, past the street's end onto the hill;
// a walk into them ends at these, deep enough that the trees carry on past it into the fog.
#define WOODS_EAST_X  50.0f
#define WOODS_EDGE_Z0 (-58.0f)
#define WOODS_EDGE_Z1 64.0f

/*
 * The lake valley (spec 13.41), down the crossroads' south arm. The chasm's east lip turns west at
 * RIDGE_Z, its south lip the north face of a rock ridge, and past the ridge the ground falls to a
 * lake whose water stands at LAKE_Y, with a log cabin on its east shore. WORLD_Z1 stays where it
 * was: the woods behind our side and the hill's dead trees are placed against it, and the valley
 * has its own extent.
 */
#define RIDGE_Z      72.0f    // where the chasm's east lip turns west, on a grid line
#define LAKE_X       (-96.0f) // the shore's centre
#define LAKE_Z       120.0f
#define LAKE_RX      23.0f // its half-widths, before the shore's wobble
#define LAKE_RZ      17.0f
#define LAKE_Y       (-12.0f) // the still water
#define CABIN_PAD_X0 (-71.0f) // the cabin's level pad, cut into the east bank on grid lines
#define CABIN_PAD_X1 (-59.0f)
#define CABIN_PAD_Z0 112.0f
#define CABIN_PAD_Z1 124.0f
#define CABIN_PAD_Y  (-10.8f)
// The ground's extent past the world's: west of the chasm south of the ridge, and south of the
// woods behind our side; east of VALLEY_X1 it is not there at all.
#define VALLEY_X0 (-147.0f) // a whole number of grid steps west of CHASM_X
#define VALLEY_X1 (-23.0f)
#define VALLEY_Z1 168.0f
// The walls round it, well inside the ground so the trees carry on past them into the fog.
#define VALLEY_WALL_X0 (-128.0f)
#define VALLEY_WALL_Z1 146.0f

// How far (x, z) is outside a box from (x0, z0) to (x1, z1) in plan, 0 inside it.
static inline float plan_box_distance(float x, float z, float x0, float x1, float z0, float z1) {
    const float dx = fmaxf(fmaxf(x0 - x, x - x1), 0.0f);
    const float dz = fmaxf(fmaxf(z0 - z, z - z1), 0.0f);
    return sqrtf(dx * dx + dz * dz);
}

// Where line `k` between the far side's TERRACE_LOTS lots stands, 0 and TERRACE_LOTS the
// street's ends, and the far house of `lot`.
static inline float far_lot_line_x(int k) {
    return k <= 0
               ? -STREET_HALF_LEN
               : (k >= TERRACE_LOTS ? STREET_HALF_LEN : LOT_GRID_X0 + TERRACE_LOT_WIDTH * (float)k);
}
static inline float far_lot_house_x(int lot) {
    return LOT_GRID_X0 + TERRACE_LOT_WIDTH * ((float)lot + 0.5f);
}

// The same for our side's NEAR_LOTS lots.
static inline float near_lot_line_x(int k) {
    return k <= 0 ? -STREET_HALF_LEN
                  : (k >= NEAR_LOTS ? STREET_HALF_LEN
                                    : LOT_GRID_X0 + TERRACE_LOT_WIDTH * ((float)k - 0.5f));
}
static inline float near_lot_house_x(int lot) {
    return LOT_GRID_X0 + TERRACE_LOT_WIDTH * (float)lot;
}

// A point of the house's plan, where the mansion puts it.
static inline void mansion_at(const float local[3], float out[3]) {
    out[0] = local[0] + MANSION_X;
    out[1] = local[1] + MANSION_Y;
    out[2] = local[2] + MANSION_Z;
}

#endif // _SILENT_LAYOUT_H_
