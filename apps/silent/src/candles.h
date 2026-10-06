#ifndef _SILENT_CANDLES_H_
#define _SILENT_CANDLES_H_

#include "cetra/fire.h"
#include "cetra/scene.h"

#include "kit.h"
#include "ornament.h"

/*
 * Candles in polished brass (spec 13.15), and the flames on them. Each holder stands its candle
 * in its socket and leaves the candle's wick on the kit; candles_light burns a flame on every
 * wick. A candle is burnt down to `wax` metres.
 */

// A bare taper standing on y at (a, d) in frame `f`, for a holder built elsewhere.
void candle_taper(Kit* kit, const KitFrame* f, float a, float y, float d, float wax);
// A table candlestick standing on y at (a, d) in frame `f`, holding a taper.
void candle_stick(Kit* kit, const KitFrame* f, float a, float y, float d, float wax);
// A chamberstick the same way: a wide drip pan, a short socket, and a ring for a finger at +a.
void candle_chamber(Kit* kit, const KitFrame* f, float a, float y, float d, float wax);
// A floor stand the same way, a little under a metre and a third, holding a pillar. It collides.
void candle_stand(Kit* kit, const KitFrame* f, float a, float y, float d, float wax);
// A sconce on the face of `wall` at a along it: a lancet backplate centred at height y, and an
// arm out into the room to a pan holding a taper. It collides.
void candle_sconce(Kit* kit, const Facade* wall, float a, float y, float wax);

// A flame on every wick the kit holds, added to `fs`, each driving a point light it adds to the
// scene: one with a cached shadow under `shadows`, an unshadowed one otherwise.
void candles_light(FireSystem* fs, Scene* scene, const Kit* kit, bool shadows);

#endif // _SILENT_CANDLES_H_
