#include <math.h>
#include <stdlib.h>

#include "stream.h"

float stream_box_distance(const vec3 p, const vec3 box_min, const vec3 box_max) {
    float sum = 0.0f;
    for (int c = 0; c < 3; ++c) {
        float d = fmaxf(fmaxf(box_min[c] - p[c], p[c] - box_max[c]), 0.0f);
        sum += d * d;
    }
    return sqrtf(sum);
}

// The nearest item that may be resident and is not, or -1.
static int nearest_candidate(const float* dist, int count, const bool* resident) {
    int best = -1;
    for (int i = 0; i < count; ++i) {
        if (resident[i] || dist[i] < 0.0f)
            continue;
        if (best < 0 || dist[i] < dist[best])
            best = i;
    }
    return best;
}

// The resident item that is farthest, or -1. Of two at one distance the higher index counts as
// farther, the mirror of the candidates' tie rule.
static int farthest_resident(const float* dist, int count, const bool* resident) {
    int best = -1;
    for (int i = 0; i < count; ++i) {
        if (!resident[i])
            continue;
        // >= since i only grows: a tie goes to the later index.
        if (best < 0 || dist[i] >= dist[best])
            best = i;
    }
    return best;
}

int stream_assign(const float* dist, int count, int capacity, float margin, int* slot_of,
                  int* holder, int* home) {
    if (!dist || !slot_of || !holder || capacity <= 0 || count <= 0)
        return 0;
    bool* resident = calloc((size_t)count, sizeof(bool));
    if (!resident)
        return 0;

    // WHO is resident, first, and only then WHERE: deciding both at once hands an item
    // readmitted beside another whichever slot its eviction happened to free, and its texels
    // land somewhere new for no reason.
    int held = 0;
    for (int s = 0; s < capacity; ++s) {
        const int h = holder[s];
        if (h >= 0 && h < count && dist[h] >= 0.0f) {
            resident[h] = true;
            held++;
        }
    }
    // Nearest first, so the loop can stop at the first candidate that cannot get in: every
    // one after it is farther and could not either.
    for (;;) {
        const int c = nearest_candidate(dist, count, resident);
        if (c < 0)
            break;
        if (held < capacity) {
            resident[c] = true;
            held++;
            continue;
        }
        const int f = farthest_resident(dist, count, resident);
        if (f < 0 || dist[f] <= dist[c] + margin)
            break;
        resident[f] = false;
        resident[c] = true;
    }

    int changed = 0;
    for (int s = 0; s < capacity; ++s) {
        const int h = holder[s];
        if (h < 0)
            continue;
        if (h < count && resident[h])
            continue;
        if (h < count)
            slot_of[h] = -1;
        holder[s] = -1;
        changed++;
    }
    for (int i = 0; i < count; ++i) {
        if (!resident[i] || slot_of[i] >= 0)
            continue;
        int s = home && home[i] >= 0 && home[i] < capacity && holder[home[i]] < 0 ? home[i] : -1;
        for (int f = 0; s < 0 && f < capacity; ++f)
            if (holder[f] < 0)
                s = f;
        holder[s] = i;
        slot_of[i] = s;
        if (home)
            home[i] = s;
        changed++;
    }
    free(resident);
    return changed;
}
