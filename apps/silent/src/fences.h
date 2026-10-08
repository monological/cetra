#ifndef _SILENT_FENCES_H_
#define _SILENT_FENCES_H_

#include "kit.h"
#include "street.h"

#define FENCE_BREACH_MAX 4

// Where a length of fence has come down: the gap from a to b in plan, (x, z), on ground y, and
// the side it fell toward, for something to stand across it so it reads as closed.
typedef struct FenceBreach {
    vec2 a, b;
    vec2 out;
    float y;
} FenceBreach;

typedef struct FenceBreaches {
    FenceBreach at[FENCE_BREACH_MAX];
    int count;
} FenceBreaches;

/*
 * Every fence round the yards, both sides of the street (spec 13.35): a front fence along some
 * lots, a fence down every lot line, a short return from each house's sides to its lot's lines
 * with a gate in one, and back fences along the woods. Our side's yard is reached through its
 * return's open gate; its back gate is padlocked. Each fence collides along its whole length but
 * an open gate, a fallen section included -- `breaches` gets where those are.
 */
void fences_build(Kit* kit, unsigned int seed, const StreetPlots* plots, FenceBreaches* breaches);

#endif // _SILENT_FENCES_H_
