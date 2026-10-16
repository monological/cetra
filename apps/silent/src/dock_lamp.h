#ifndef _SILENT_DOCK_LAMP_H_
#define _SILENT_DOCK_LAMP_H_

#include <stdbool.h>
#include <cglm/cglm.h>

#include "cetra/light.h"
#include "cetra/scene.h"

#include "kit.h"

/*
 * The dock lamp (spec 13.56): a brass post on a flanged foot on the deck's edge, bending over in
 * a shepherd's crook, and from its hook on a short chain a round lantern over the moored boat,
 * swinging in the breeze. The post is the lake's kit's. The chain and the lantern are a kit of
 * their own that turns about the hook, the hall pendant's way: it casts nothing, since swinging it
 * would be a mover in its own light's kept views every frame, and no capture keeps it. The light
 * is the bulb's, cached, and follows it. It has no body, unlike the hall's bulbs: a filament's
 * softening is nothing two metres over the deck, and a body is eight views to draw again every
 * frame the breeze keeps it swinging.
 */

typedef struct DockLamp {
    SceneNode* lantern; // the chain and the lantern, turned about the pivot under the crook's end
    Light* light;       // at the bulb
    const Scene* scene; // whose breeze swings it; none hangs it still
    vec3 pivot;
    float drop;                    // the bulb under the pivot
    float swing[2], swing_rate[2]; // radians leaning downwind and swaying across, and their rates
} DockLamp;

// The lamp standing at `at`, its post's foot on the deck, its d axis out over the water: the post
// in `kit`, the chain and the lantern in a kit beside it, and its light in `scene`.
void dock_lamp_build(DockLamp* lamp, Kit* kit, Scene* scene, const KitFrame* at);
// Per frame: swung `dt` on by the breeze at `time`.
void dock_lamp_update(DockLamp* lamp, double time, float dt);
// Hanging straight down while `rest`, for a capture that keeps what it sees, and where its swing
// has it after.
void dock_lamp_rest(DockLamp* lamp, bool rest);

#endif // _SILENT_DOCK_LAMP_H_
