#ifndef _BACKGROUND_H_
#define _BACKGROUND_H_

#include <stdbool.h>
#include <cglm/cglm.h>

#include "postfx.h"

/*
 * An exact background (spec 14.38): the working-space value a view clears to so that its tone
 * curve and the display encode give back the display colour asked for. Exposure takes no part --
 * it is applied where the scene is drawn, never to a cleared texel -- so the curve and the encode
 * are all there is to undo.
 */

// `display` (0..1 per channel) back through `mode`'s curve and the 2.2 encode to the value to
// clear to. False when the curve cannot reach it, `working` then the nearest it can.
bool background_working(const float display[3], PostFXTonemapMode mode, vec3 working);

// The forward map, as tonemap_frag runs it on a cleared texel: the curve, then the encode.
// Passthrough copies the value unchanged.
void background_display(const float working[3], PostFXTonemapMode mode, vec3 display);

#endif
