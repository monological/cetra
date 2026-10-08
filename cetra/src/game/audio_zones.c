#include <string.h>

#include "audio_zones.h"
#include "../ext/log.h"

// The best path between every pair of zones, by Floyd-Warshall over the max-product semiring.
// No `through` is above 1, so going round a cycle never improves a path and the closure is
// exact. Two links between one pair -- a wall and a door through it -- count as the better.
static void zones_solve(AudioZones* z) {
    const int n = z->count;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
            z->heard[i][j] = i == j ? 1.0f : 0.0f;
    for (int l = 0; l < z->link_count; l++) {
        const AudioZone a = z->link_ends[l][0], b = z->link_ends[l][1];
        const float t = z->link_through[l];
        if (a != b && t > z->heard[a][b]) {
            z->heard[a][b] = t;
            z->heard[b][a] = t;
        }
    }
    for (int k = 0; k < n; k++)
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++) {
                const float via = z->heard[i][k] * z->heard[k][j];
                if (via > z->heard[i][j])
                    z->heard[i][j] = via;
            }
}

void audio_zones_init(AudioZones* zones) {
    memset(zones, 0, sizeof(*zones));
    zones->count = 1;
    strcpy(zones->names[AUDIO_ZONE_WORLD], "world");
    zones_solve(zones);
}

AudioZone audio_zones_add(AudioZones* zones, const AudioZoneDesc* desc) {
    const char* name = desc && desc->name ? desc->name : "unnamed";
    if (!desc || !desc->boxes || desc->box_count <= 0 || desc->box_count > AUDIO_ZONE_BOXES) {
        log_error("audio: zone '%s' wants 1 to %d boxes; not added", name, AUDIO_ZONE_BOXES);
        return AUDIO_ZONE_WORLD;
    }
    if (zones->count >= AUDIO_ZONE_MAX) {
        log_error("audio: zone '%s' past the %d there is room for; not added", name,
                  AUDIO_ZONE_MAX - 1);
        return AUDIO_ZONE_WORLD;
    }
    const AudioZone z = (AudioZone)zones->count++;
    strncpy(zones->names[z], name, AUDIO_ZONE_NAME - 1);
    memcpy(zones->boxes[z], desc->boxes, (size_t)desc->box_count * sizeof(AABB));
    zones->box_counts[z] = desc->box_count;
    zones_solve(zones);
    return z;
}

AudioZoneLink audio_zones_link(AudioZones* zones, AudioZone a, AudioZone b, float through) {
    if (a >= (AudioZone)zones->count || b >= (AudioZone)zones->count) {
        log_error("audio: a link from zone %u to %u, which are not both zones", a, b);
        return AUDIO_ZONE_NO_LINK;
    }
    if (zones->link_count >= AUDIO_ZONE_LINKS) {
        log_error("audio: a link from '%s' to '%s' past the %d there is room for", zones->names[a],
                  zones->names[b], AUDIO_ZONE_LINKS);
        return AUDIO_ZONE_NO_LINK;
    }
    const AudioZoneLink l = (AudioZoneLink)zones->link_count++;
    zones->link_ends[l][0] = a;
    zones->link_ends[l][1] = b;
    zones->link_through[l] = glm_clamp(through, 0.0f, 1.0f);
    zones_solve(zones);
    return l;
}

bool audio_zones_link_set(AudioZones* zones, AudioZoneLink link, float through) {
    if (link >= (AudioZoneLink)zones->link_count)
        return false;
    const float t = glm_clamp(through, 0.0f, 1.0f);
    if (zones->link_through[link] != t) {
        zones->link_through[link] = t;
        zones_solve(zones);
    }
    return true;
}

AudioZone audio_zones_find(const AudioZones* zones, const vec3 p) {
    for (int z = zones->count - 1; z > (int)AUDIO_ZONE_WORLD; z--)
        for (int i = 0; i < zones->box_counts[z]; i++)
            if (aabb_dist_sq(&zones->boxes[z][i], p) == 0.0f)
                return (AudioZone)z;
    return AUDIO_ZONE_WORLD;
}
