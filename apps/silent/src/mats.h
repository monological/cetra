#ifndef _SILENT_MATS_H_
#define _SILENT_MATS_H_

#include "cetra/engine.h"
#include "cetra/scene.h"
#include "cetra/texture.h"

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
    MAT_CARDBOARD,    // old boxes on top of the cupboards
    MAT_PLASTIC,      // a dish-soap bottle
    MAT_STAINLESS,    // the sink, the tap, the stove's knobs, pots: polished
    MAT_JAM,          // preserves: a dark red
    MAT_PLUM,         // and a purple nearly black
    MAT_PRUNE,        // and a brown nearly black
    MAT_CARDS,        // pictures placed whole, photos to the clock's dial: see cards.h
    MAT_CUSHION,      // a foam seat pad gone yellow
    MAT_CASE,         // the hall clock's dark lacquered body
    MAT_ROSEWOOD,     // its door, hood and mouldings
    MAT_MAPLE,        // the pale stringing inlaid round its door
    MAT_BRASS,        // capitals, finials, rosettes, weights, the bob: tarnished
    MAT_BULB,         // the hall's bare bulb, lit
    MAT_SIDING_DARK,  // the Gothic house's near-black board-and-batten, and its carpentry
    MAT_SLATE,        // its fish-scale slate
    MAT_STONE,        // dressed grey ashlar: the hearth, the great hall, the gargoyles
    MAT_FOUNDATION,   // the rubble base under it all
    MAT_IRON,         // cresting, finials, straps, the fireback: black paint gone dull
    MAT_LEATHER,      // book covers, the desk's top
    MAT_MAHOGANY,     // panelling's stiles and rails, beams, trusses, doors' casings
    MAT_GOTHIC,       // gothic.h's pictures placed whole: carvings, spines, the rug and runners
    MAT_STAINED,      // gothic.h's lancets: opaque, lit through by its own picture
    MAT_LEADED,       // diamond quarries in lead, laid by the metre
    MAT_SOOT,         // a firebox's blackened stone
    MAT_WAX,          // candles
    MAT_SHADE,        // the study lamp's green cased glass, faintly lit from inside
    MAT_BRASS_BRIGHT, // candlesticks and sconces: brass kept polished
    // The player's house (spec 13.25).
    MAT_CREAM,      // the hall's plaster, gone the colour of old cream
    MAT_HARDWOOD,   // its dark boards under a lacquer that still shines
    MAT_WALLPAPER,  // the living room's faded paper
    MAT_MOULDING,   // crown moulding, skirting and casings, painted white long ago
    MAT_UPHOLSTERY, // the sofa and the armchair
    MAT_MIRROR,     // the bathroom's mirror, its silvering spotted
    MAT_LAMPSHADE,  // the floor lamp's fabric shade, lit from inside
    MAT_FROSTED,    // a lantern's frosted glass, lit from inside
    MAT_SCREEN,     // the television's glass, glowing with its picture
    MAT_TV_STATIC,  // the picture on it, snow, drawn in the late draw (spec 13.30)
    // The home's basement (spec 13.31).
    MAT_CELLAR_WALL,     // the stairwell down, painted a pale colour long ago
    MAT_CELLAR_DADO,     // the dark gloss band painted along the flight
    MAT_CELLAR_CONCRETE, // the basement's own walls: bare poured concrete
    MAT_CELLAR_DAMP,     // and their foot, dark and wet with the damp it draws up
    MAT_CELLAR_FLOOR,    // the slab, stained and never sealed
    MAT_CELLAR_WET,      // standing water on it round the drain
    MAT_JOIST,           // the ground floor's framing seen from below
    // The town's edges (spec 13.35).
    MAT_RETAINING,   // poured concrete holding the far side's lots up, stained by the rain
    MAT_FENCE_BOARD, // a privacy fence's boards, gone grey
    MAT_PICKET,      // a picket fence's paint, white once
    MAT_CINDER,      // cinder block
    MAT_CHAINLINK,   // the mesh, a cutout drawn from both sides
    MAT_GALVANISED,  // a chain-link fence's posts and rails
    MAT_WOODS_FLOOR, // bare earth and twigs under the trees
    MAT_CLIFF,       // broken rock
    MAT_LOG,         // pine bark on the woods' fallen logs and stumps
    MAT_COUNT
} MatId;

// Loads every surface's photo maps into the scene's pool and registers the
// materials with the kit, slot for slot with MatId. The kit must be empty.
void mats_register(Kit* kit, Engine* engine, Scene* scene);

// Daytime: the street lamps' lenses go dark, and the stained glass is lit by the sky.
void mats_daytime(Kit* kit);

// `m` as a cutout drawn from both sides, a hole where its albedo's alpha is under `cutoff`, and
// `albedo` the descriptor its albedo map loads by, keeping that cut's coverage down its mips.
void mats_cutout(Material* m, float cutoff, TextureDesc* albedo);

// Maps baked in memory, `width` by `height`: each a buffer the pool takes over, or NULL for none,
// the albedo with an alpha when it has 4 channels.
typedef struct BakedMaps {
    unsigned char *albedo, *normal, *rough;
    int width, height, albedo_channels;
} BakedMaps;

// `maps` onto `m` through the scene's pool, named `name`_albedo, _normal and _rough, the albedo
// loaded by `albedo`.
void mats_set_baked(Material* m, Scene* scene, const char* name, const BakedMaps* maps,
                    TextureDesc albedo);

#endif // _SILENT_MATS_H_
