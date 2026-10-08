#ifndef _AUDIO_ZONES_H_
#define _AUDIO_ZONES_H_

/*
 * Audio zones (spec 13.33): what lets a sound through on its way from where it is to where it
 * is heard. A ZONE is a named set of boxes; a LINK joins two zones, or a zone and the world
 * outside every zone, with a `through` -- the gain a sound keeps crossing it, a wall's, a
 * floor's, a door's as it opens. What reaches a listener in one zone from a sound in another is
 * the BEST PATH between them: the largest product of `through` along any chain of links.
 *
 * A graph and not a nesting of boxes, because a storey is the outdoors' neighbour and the
 * neighbour of the storey under it at once: nested, upstairs would reach the outdoors through
 * the floor as well as its own walls. A point belongs to the zone added LAST among those whose
 * boxes hold it, so an inner room is added after the one round it and carves itself out.
 *
 * No miniaudio and no clock: the AudioSystem asks this where things are and what passes, and
 * eases and applies the answer itself.
 */

#include <stdbool.h>
#include <stdint.h>

#include "../mesh.h"

#define AUDIO_ZONE_WORLD   0u // outside every zone
#define AUDIO_ZONE_NO_LINK UINT32_MAX
#define AUDIO_ZONE_MAX     16 // the world included
#define AUDIO_ZONE_BOXES   8
#define AUDIO_ZONE_LINKS   32
#define AUDIO_ZONE_NAME    32

typedef uint32_t AudioZone;
typedef uint32_t AudioZoneLink;

typedef struct AudioZoneDesc {
    const char* name;
    const AABB* boxes; // world space; copied
    int box_count;
} AudioZoneDesc;

typedef struct AudioZones {
    int count; // zones, the world included, so 1 when none was added
    char names[AUDIO_ZONE_MAX][AUDIO_ZONE_NAME];
    AABB boxes[AUDIO_ZONE_MAX][AUDIO_ZONE_BOXES];
    int box_counts[AUDIO_ZONE_MAX];
    int link_count;
    AudioZone link_ends[AUDIO_ZONE_LINKS][2];
    float link_through[AUDIO_ZONE_LINKS];
    // The best path's gain between every pair of zones: 1 from a zone to itself, 0 where no
    // chain of links joins two.
    float heard[AUDIO_ZONE_MAX][AUDIO_ZONE_MAX];
} AudioZones;

// The world alone.
void audio_zones_init(AudioZones* zones);
// A zone; AUDIO_ZONE_WORLD, logged by name, when there is no room for it or it has no box.
AudioZone audio_zones_add(AudioZones* zones, const AudioZoneDesc* desc);
// A link between two zones, `through` clamped to 0..1; AUDIO_ZONE_NO_LINK, logged, when there is
// no room for it or an end is not a zone.
AudioZoneLink audio_zones_link(AudioZones* zones, AudioZone a, AudioZone b, float through);
// A link's `through` changed, clamped to 0..1; false for no such link.
bool audio_zones_link_set(AudioZones* zones, AudioZoneLink link, float through);
// The zone `p` is in: the last added whose boxes hold it, else the world.
AudioZone audio_zones_find(const AudioZones* zones, const vec3 p);

#endif // _AUDIO_ZONES_H_
