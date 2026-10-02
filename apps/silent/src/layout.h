#ifndef _SILENT_LAYOUT_H_
#define _SILENT_LAYOUT_H_

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

#define HALL_X0 (-1.5f) // the hall runs from the front door to the great hall between these
#define HALL_X1 0.0f    // and this is also the kitchen's west wall

// The front band's back wall, which is the great hall's front.
#define KITCHEN_BACK_Z 13.8f

// The kitchen window, in the front wall, and the front door.
#define KITCHEN_WIN_X0   1.5f
#define KITCHEN_WIN_X1   3.7f
#define KITCHEN_WIN_SILL (FLOOR_Y + 1.02f)
#define KITCHEN_WIN_HEAD (FLOOR_Y + 2.18f)

#define FRONT_DOOR_X0 (-1.15f)
#define FRONT_DOOR_X1 (-0.30f)
#define DOOR_HEAD     (FLOOR_Y + 2.05f)
// The front door's pointed head: where it springs, and how far it rises above that.
#define FRONT_DOOR_SPRING (FLOOR_Y + 1.95f)
#define FRONT_DOOR_RISE   0.45f

// The doorway from the hall into the kitchen, in the x = HALL_X1 wall.
#define KITCHEN_DOOR_Z0 12.75f
#define KITCHEN_DOOR_Z1 13.55f

// The kitchen's inner faces: where the room actually is.
#define KITCHEN_X0 (HALL_X1 + 0.5f * INT_WALL)
#define KITCHEN_X1 (HOUSE_X1 - 0.5f * EXT_WALL)
#define KITCHEN_Z0 (HOUSE_FRONT_Z + 0.5f * EXT_WALL)
#define KITCHEN_Z1 (KITCHEN_BACK_Z - 0.5f * INT_WALL)

// The hall wall's run, from just short of the kitchen doorway toward the
// window wall: the fridge first, then the stove, whose centre the hood tube
// hangs over.
#define STOVE_RUN_Z (KITCHEN_DOOR_Z0 - 0.2f)
#define STOVE_Z     (STOVE_RUN_Z - 1.12f)

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
#define HEARTH_X (0.5f * (HALL_X0 + HALL_X1))

// The tower: a regular octagon centred on the front corner west of the door,
// its faces' centre lines TOWER_APOTHEM from the middle. The house's front and
// west walls butt into the middles of its east and north faces.
#define TOWER_X       HOUSE_X0
#define TOWER_Z       HOUSE_FRONT_Z
#define TOWER_APOTHEM 2.3f
#define TOWER_HALF    (TOWER_APOTHEM * 0.41421356f) // half a face: apothem x tan 22.5
#define TOWER_TOP     9.6f                          // the walls' top, under the spire
#define TOWER_CEIL_Y  9.0f                          // the study's tower bay rises to this
#define TOWER_SPIRE_Y 15.5f

#define PORCH_X0 (-2.4f)
#define PORCH_X1 0.8f
#define PORCH_Z0 8.5f

// The street.
#define ROAD_HALF_WIDTH 4.0f     // asphalt, centre line at z = 0
#define ROAD_Y          (-0.15f) // below the kerb
#define SIDEWALK_WIDTH  2.0f
#define STREET_HALF_LEN 45.0f // along X; the fog ends it well before this

#endif // _SILENT_LAYOUT_H_
