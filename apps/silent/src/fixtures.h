#ifndef _SILENT_FIXTURES_H_
#define _SILENT_FIXTURES_H_

#include <cglm/cglm.h>

#include "kit.h"

/*
 * The home hall's light fittings (spec 13.52): square lanterns of aged brass and clear glass round
 * a filament bulb, one hung from the ceiling on a chain and one on a bracket from the wall. The
 * glass is the jars' and casts nothing for a cached light, so the bulb's light leaves through the
 * panes and only the brass shadows it. And the dock's (spec 13.56): a round lantern of the same
 * brass, its glass a cylinder.
 */

// The filament bulb's light: its body's radius, the squirrel cage it glows from, and the near
// plane its cached shadow takes, past the filament.
#define FIXTURE_BULB_BODY 0.012f
#define FIXTURE_BULB_NEAR 0.025f
// How far over the bulb's middle a lantern's crown stands, the socket and its rod between: the
// bulb hangs this far below a lantern's top.
#define FIXTURE_BULB_DROP 0.12f
// A lantern crown's height over the cage's top, to its finial's ring.
#define FIXTURE_CROWN_H 0.1f

// A clear filament bulb in a brass socket, its middle at (a, y, d) in frame `f`, its filament of
// material `filament`.
void fixture_bulb(Kit* kit, const KitFrame* f, float a, float y, float d, int filament);
// A square lantern, `r` its half width and `h` its cage's height, the cage's top at (a, y, d):
// brass posts, frames and a pyramid crown with a ring on its finial, four clear panes, and a bulb
// hung FIXTURE_BULB_DROP under the top.
void fixture_lantern(Kit* kit, const KitFrame* f, float a, float y, float d, float r, float h);
// A round lantern, `r` its glass's radius and `h` its height, the glass's top at (a, y, d): a brass
// dish with a drop finial under a glass cylinder of material `glass`, brass straps and bands on
// it, a domed cap with a ring on its finial, FIXTURE_CROWN_H tall like the square lantern's crown,
// and a bulb hung FIXTURE_BULB_DROP under the top, its filament of material `filament`.
void fixture_lantern_round(Kit* kit, const KitFrame* f, float a, float y, float d, float r, float h,
                           int glass, int filament);
// A chain of oval brass links from `y0` down to `y1` on the kit's upright through its origin, each
// link turned a quarter from the last and hung through it.
void fixtures_chain(Kit* kit, float y0, float y1);

// How far a hung bulb swings from straight down before its light's kept shadow follows it rather
// than stepping after it, metres.
#define FIXTURE_SWING_FOLLOW 0.015f
// A lamp hung from `pivot`, turned there by `turn`: its node placed, and where its bulb, `drop`
// straight under the pivot hanging still, has gone in `bulb`.
void fixture_hang(SceneNode* node, const vec3 pivot, const mat4 turn, float drop, vec3 bulb);

#endif // _SILENT_FIXTURES_H_
