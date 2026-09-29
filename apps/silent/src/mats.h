#ifndef _SILENT_MATS_H_
#define _SILENT_MATS_H_

#include "cetra/engine.h"

#include "kit.h"

// Every surface in the world, as a kit slot. mats_register adds them in this
// order, so the enum value IS the slot.
typedef enum {
    MAT_PLASTER,
    MAT_CEILING,
    MAT_KITCHEN_FLOOR,
    MAT_WOOD_FLOOR,
    MAT_SIDING,
    MAT_TRIM,
    MAT_PORCH,
    MAT_DIRT,
    MAT_ASPHALT,
    MAT_CONCRETE,
    MAT_ROOF,
    MAT_COUNT
} MatId;

// Registers every material with the kit, slot for slot with MatId.
bool mats_register(Kit* kit, Engine* engine);

#endif // _SILENT_MATS_H_
