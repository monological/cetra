#ifndef _SILENT_LAYOUT_H_
#define _SILENT_LAYOUT_H_

/*
 * Where everything is, in metres. The street runs along X with its centre line
 * at z = 0; the player's house stands on the +z side, its front wall facing the
 * road. Wall coordinates are wall CENTRE lines.
 *
 *     z
 *     ^   back wall (HOUSE_BACK_Z)
 *     |  +---------------+-----+-------------+
 *     |  |  living room  |hall |  back room  |
 *     |  |               |     +-------------+  KITCHEN_BACK_Z
 *     |  |               |     =  kitchen    |
 *     |  +----[win]------+-[d]-+---[window]--+  HOUSE_FRONT_Z
 *     |                  porch
 *     |  ======================= sidewalk
 *     |  -----------------------  road (centre z = 0)
 *     +--------------------------------------------> x
 */

#define FLOOR_Y 0.30f // the house floor; the porch is level with it
#define CEIL_Y  2.90f

#define EXT_WALL 0.18f // exterior wall thickness
#define INT_WALL 0.10f // interior

#define HOUSE_X0      (-5.0f)
#define HOUSE_X1      5.0f
#define HOUSE_FRONT_Z 10.0f
#define HOUSE_BACK_Z  16.0f

#define HALL_X0 (-1.5f) // the hall runs front to back between these
#define HALL_X1 0.0f    // and this is also the kitchen's west wall

#define KITCHEN_BACK_Z 13.8f

// The kitchen window, in the front wall, and the front door.
#define KITCHEN_WIN_X0   1.5f
#define KITCHEN_WIN_X1   3.7f
#define KITCHEN_WIN_SILL (FLOOR_Y + 1.02f)
#define KITCHEN_WIN_HEAD (FLOOR_Y + 2.18f)

#define FRONT_DOOR_X0 (-1.15f)
#define FRONT_DOOR_X1 (-0.30f)
#define DOOR_HEAD     (FLOOR_Y + 2.05f)

// The doorway from the hall into the kitchen, in the x = HALL_X1 wall.
#define KITCHEN_DOOR_Z0 12.75f
#define KITCHEN_DOOR_Z1 13.55f

#define PORCH_X0 (-2.4f)
#define PORCH_X1 0.8f
#define PORCH_Z0 8.5f

// The street.
#define ROAD_HALF_WIDTH 4.0f     // asphalt, centre line at z = 0
#define ROAD_Y          (-0.15f) // below the kerb
#define SIDEWALK_WIDTH  2.0f
#define STREET_HALF_LEN 45.0f // along X; the fog ends it well before this

#endif // _SILENT_LAYOUT_H_
