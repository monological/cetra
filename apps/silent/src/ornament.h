#ifndef _SILENT_ORNAMENT_H_
#define _SILENT_ORNAMENT_H_

#include "kit.h"

/*
 * What makes the Gothic house Gothic (spec 13.13), laid on its walls: a rubble base, board-
 * and-batten, windows cased and hooded with tracery in the lancets, cusped bargeboards down
 * the gables, finials, iron cresting, and the porch's arches, posts and balustrades.
 */

// A wall's outside as ornament is laid on it: the wall's frame, the d of its outer face and of
// its middle, where its glass is, and which way along d is out of it.
typedef struct Facade {
    KitFrame f;
    float face;
    float mid;
    float out; // +1 or -1
} Facade;

// An axis-aligned wall's outside, and its inner side -- the one its `inner` points to -- as a
// room sees it.
Facade facade_of(const KitWall* w);
Facade facade_inner(const KitWall* w);
// The outside of a wall built in frame `f` (kit_frame_wall).
Facade facade_of_frame(const KitFrame* f, const KitWall* w);

// A casing round an opening: a band this wide, standing this far off the face.
#define ORNAMENT_CASING_W 0.08f
#define ORNAMENT_CASING_T 0.04f
void ornament_casing(Kit* kit, const Facade* s, int mat, const KitOpening* o);

// The rubble base along the foot of a wall from a0 to a1, and the dressed water table that
// sheds the rain off it; across a doorway it stops at the sill.
void ornament_base(Kit* kit, const Facade* s, float a0, float a1, const KitOpening* openings,
                   int count);

// Battens from a0 to a1 between y0 and y1, round the openings and their casings.
void ornament_battens(Kit* kit, const Facade* s, float a0, float a1, float y0, float y1,
                      const KitOpening* openings, int count);
// Battens up a gable from y0 to under the roof, whose line falls `pitch` a metre either side of
// its apex at (apex_a, apex_y).
void ornament_gable_battens(Kit* kit, const Facade* s, float a0, float a1, float y0, float apex_a,
                            float apex_y, float pitch);

// A window's casing, sill and hood moulding with its stops; a lancet wide enough is parted
// into two lights under a ring.
void ornament_window(Kit* kit, const Facade* s, const KitOpening* o);

// One rake's bargeboard, from d0 to d1: its top along the roof's edge from `eave` to `apex`,
// both (a, y), and its foot a row of cusped arches.
void ornament_bargeboard(Kit* kit, const KitFrame* f, const vec2 eave, const vec2 apex, float d0,
                         float d1);

// A turned finial `h` tall standing at (a, y, d), and a pendant `drop` long hanging under it.
void ornament_finial(Kit* kit, const KitFrame* f, int mat, float a, float y, float d, float h,
                     float drop);

// Iron cresting along a ridge, from a0 to a1 at height y and distance d.
void ornament_cresting(Kit* kit, const KitFrame* f, float a0, float a1, float y, float d);

// A board between two posts, a0 to a1, from its springing line up to `top` at distance d, with
// an arch cut from its whole width.
void ornament_arch_board(Kit* kit, const KitFrame* f, int mat, float a0, float a1, float spring,
                         float top, KitArchShape arch, float rise, float d);

// Turned balusters under a handrail from a0 to a1, standing on a floor at y, at distance d, and
// a body so nobody falls past it.
void ornament_balustrade(Kit* kit, const KitFrame* f, int mat, float a0, float a1, float y,
                         float d);
// One turned baluster standing at (a, y, d), `h` tall.
void ornament_baluster(Kit* kit, const KitFrame* f, int mat, float a, float y, float d, float h);

// A turned post from y0 to y1 at (a, d), `r` its shaft's radius, and its body.
void ornament_post(Kit* kit, const KitFrame* f, int mat, float a, float d, float y0, float y1,
                   float r);

#endif // _SILENT_ORNAMENT_H_
