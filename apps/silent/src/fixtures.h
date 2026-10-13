#ifndef _SILENT_FIXTURES_H_
#define _SILENT_FIXTURES_H_

#include <cglm/cglm.h>

#include "kit.h"

/*
 * The home hall's light fittings (spec 13.52): square lanterns of aged brass and clear glass round
 * a filament bulb, one hung from the ceiling on a chain and one on a bracket from the wall. The
 * glass is the jars' and casts nothing for a cached light, so the bulb's light leaves through the
 * panes and only the brass shadows it.
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

// A clear filament bulb in a brass socket, its middle at (a, y, d) in frame `f`.
void fixture_bulb(Kit* kit, const KitFrame* f, float a, float y, float d);
// A square lantern, `r` its half width and `h` its cage's height, the cage's top at (a, y, d):
// brass posts, frames and a pyramid crown with a ring on its finial, four clear panes, and a bulb
// hung FIXTURE_BULB_DROP under the top.
void fixture_lantern(Kit* kit, const KitFrame* f, float a, float y, float d, float r, float h);

#endif // _SILENT_FIXTURES_H_
