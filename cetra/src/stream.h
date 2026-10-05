#ifndef _STREAM_H_
#define _STREAM_H_

#include <stdbool.h>
#include <cglm/cglm.h>

/*
 * Which of a world's items hold the few resident slots (spec 13.24): the nearest to the
 * camera, by distance to each item's box. GI volumes and reflection probes are captured once
 * and kept, so what streams is only WHERE they sit in the shared atlas; this decides that and
 * nothing else. No GL, no clock.
 *
 * Two rules make it usable from a renderer.
 *
 * SLOTS ARE STABLE. An item keeps the slot it holds for as long as it stays resident, so a
 * slot's texels are rewritten only when its holder changes. A ranking that re-packed the
 * nearest K into slots 0..K-1 every frame would move every resident item's data whenever any
 * one of them changed rank.
 *
 * A HOLDER IS EVICTED ONLY BY A CLEARLY NEARER ITEM: one more than `margin` nearer. Two items
 * at nearly equal distance, the camera between them, would otherwise trade the last slot every
 * frame and each trade is an upload.
 *
 * Ties go to the lower index, so one run repeats itself.
 */

// Distance from p to the box, 0 inside it.
float stream_box_distance(const vec3 p, const vec3 box_min, const vec3 box_max);

// Update which items hold the `capacity` slots.
//
// dist[i] is item i's distance; a negative one marks an item that may not be resident (it is
// switched off) and frees any slot it holds. slot_of[i] is the slot item i holds or -1, and
// holder[s] the item slot s holds or -1; both are read and rewritten, and must agree on entry.
// Returns how many slots changed holder.
int stream_assign(const float* dist, int count, int capacity, float margin, int* slot_of,
                  int* holder);

#endif // _STREAM_H_
