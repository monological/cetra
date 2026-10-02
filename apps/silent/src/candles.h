#ifndef _SILENT_CANDLES_H_
#define _SILENT_CANDLES_H_

#include "cetra/scene.h"

#include "kit.h"

/*
 * Candles in polished brass (spec 13.15), and the flames on them. A holder stands on a surface
 * whose builder owns it -- the hearth's cornice, a newel's cap, a shelf -- so that builder places
 * it, and the holder leaves its wick on the kit; candles_light then burns a flame on every wick.
 */
typedef enum CandleHolder {
    CANDLE_STICK,   // a table candlestick: a turned stem on a round foot, holding a taper
    CANDLE_CHAMBER, // a chamberstick: a wide drip pan, a short socket and a ring for a finger
    CANDLE_STAND,   // a floor stand: a stepped foot, a knopped stem and a pan, holding a pillar
    CANDLE_SCONCE,  // a wall sconce: a round backplate and an arm curving out to a pan
} CandleHolder;

/*
 * A holder of `kind` with its candle, burnt down to `wax` metres, standing on y at (a, d) in
 * frame `f`. A sconce's backplate is on a wall whose face is at d, centred at height y, and its
 * arm reaches out along +d; a chamberstick's ring is along +a. A floor stand and a sconce
 * collide.
 */
void candle_build(Kit* kit, const KitFrame* f, CandleHolder kind, float a, float y, float d,
                  float wax);

// A flame on every wick the kit holds, each driving an unshadowed point light it adds to the
// scene, in a fire system the scene takes. Call it once the kit is built.
void candles_light(Scene* scene, const Kit* kit);

#endif // _SILENT_CANDLES_H_
