#ifndef _CAPTURE_KEY_H_
#define _CAPTURE_KEY_H_

#include "aabb.h"

struct CookKey;
struct Engine;
struct Scene;

/*
 * Fold into `key` everything a light capture of what lies in `box` reads (spec 13.42), so a cooked
 * capture is found only by a scene that would capture the same:
 *  - the engine's own source (a build digest) and the driver;
 *  - every setting a capture reads, through config_snapshot_fold;
 *  - the rain's state, the wind and the environment's source;
 *  - every light that gives off anything and reaches `box`, whole;
 *  - every decal that meets `box`, its placement and its images;
 *  - every item a capture of `box` takes (draw_item_captured_in): its geometry and settings, where
 *    it stands, how it is drawn, and its material -- rows, textures, hook and program.
 * Called inside a capture burst, so what animates is folded at rest. The key is refused when
 * something it reaches cannot say what it is -- a mesh or a texture with no content key -- and the
 * capture is then taken live.
 *
 * What it leaves out: two inputs that are the pacing of a sweep rather than the scene -- the
 * cached shadow tiles' contents, which the frames between bursts draw from the lights' live
 * places, and the rain's cover, mapped round the camera -- and a shadow cast into `box` by
 * something outside it, which only a shadow's caster list could name.
 */
void scene_capture_fold(struct Engine* engine, struct Scene* scene, const AABB* box,
                        struct CookKey* key);

#endif // _CAPTURE_KEY_H_
