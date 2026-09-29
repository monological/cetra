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
    MAT_ENAMEL, // cabinets, the fridge, the stove
    MAT_TRIM,   // door and window frames, skirting
    MAT_STEEL,  // sink, hood, handles
    MAT_WOOD,   // table, chairs, doors
    MAT_SIDING, // three paints, one per house in turn
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
    MAT_COUNT
} MatId;

// Loads every surface's photo maps into the scene's pool and registers the
// materials with the kit, slot for slot with MatId.
bool mats_register(Kit* kit, Engine* engine, Scene* scene);

#endif // _SILENT_MATS_H_
