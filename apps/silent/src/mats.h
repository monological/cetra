#ifndef _SILENT_MATS_H_
#define _SILENT_MATS_H_

#include "cetra/engine.h"
#include "cetra/scene.h"

#include "kit.h"

// Every surface in the world, as a kit slot. mats_register adds them in this
// order, so the enum value IS the slot.
typedef enum {
    MAT_PLASTER,
    MAT_CEILING,
    MAT_KITCHEN_FLOOR,
    MAT_WOOD_FLOOR,
    MAT_BACKSPLASH,
    MAT_ENAMEL,    // the cabinets: painted steel gone to rust
    MAT_APPLIANCE, // the fridge and the stove: grimy off-white enamel
    MAT_TRIM,      // door and window frames, skirting
    MAT_STEEL,     // sink, hood, handles
    MAT_WOOD,      // table, chairs, doors
    MAT_SIDING,    // three paints, one per house in turn
    MAT_SIDING_B,
    MAT_SIDING_C,
    MAT_PORCH,
    MAT_DIRT,
    MAT_ASPHALT,
    MAT_CONCRETE,
    MAT_ROOF,
    MAT_BRICK,
    MAT_RUG,
    MAT_TOWEL,
    MAT_PAPER,       // notes and photos on the fridge
    MAT_GLASS_AMBER, // jars and bottles
    MAT_GLASS_CLEAR,
    MAT_CONTENTS, // what is in them
    MAT_CONTENTS_PALE,
    MAT_CONTENTS_GREEN,
    MAT_CERAMIC,      // plates and mugs
    MAT_BLACK,        // oven door, burners, knobs
    MAT_TABLE,        // the kitchen table's grey-blue enamel top
    MAT_WINDOW_GLASS, // the kitchen's pane: thin, grimy, transmissive
    MAT_DARK_GLASS,   // the street's unlit windows, and a car's
    MAT_WINDOW_LIT,   // somebody is home: a warm curtained glow
    MAT_LAMP_GLOW,    // a street lamp's lens
    MAT_LAMP_POST,    // painted iron gone to rust
    MAT_POLE,         // creosoted wood utility poles
    MAT_CAR,          // faded maroon paint
    MAT_COUNT
} MatId;

// Loads every surface's photo maps into the scene's pool and registers the
// materials with the kit, slot for slot with MatId. The kit must be empty.
void mats_register(Kit* kit, Engine* engine, Scene* scene);

// Daytime: the street lamps' lenses go dark.
void mats_lamps_out(Kit* kit);

#endif // _SILENT_MATS_H_
