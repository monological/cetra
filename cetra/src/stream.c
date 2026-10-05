#include <math.h>

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
static int nearest_candidate(const float* dist, int count, const int* slot_of) {
    int best = -1;
    for (int i = 0; i < count; ++i) {
        if (slot_of[i] >= 0 || dist[i] < 0.0f)
            continue;
        if (best < 0 || dist[i] < dist[best])
            best = i;
    }
    return best;
}

// The slot whose holder is farthest, or -1 when every slot is free. Of two at one distance the
// higher index counts as farther, the mirror of the candidates' tie rule.
static int farthest_slot(const float* dist, int capacity, const int* holder) {
    int best = -1;
    for (int s = 0; s < capacity; ++s) {
        if (holder[s] < 0)
            continue;
        if (best < 0 || dist[holder[s]] > dist[holder[best]] ||
            (dist[holder[s]] == dist[holder[best]] && holder[s] > holder[best]))
            best = s;
    }
    return best;
}

int stream_assign(const float* dist, int count, int capacity, float margin, int* slot_of,
                  int* holder) {
    if (!dist || !slot_of || !holder || capacity <= 0)
        return 0;

    int changed = 0;
    for (int s = 0; s < capacity; ++s) {
        int h = holder[s];
        if (h < 0)
            continue;
        if (h >= count || dist[h] < 0.0f) {
            if (h < count)
                slot_of[h] = -1;
            holder[s] = -1;
            changed++;
        }
    }

    // Nearest first, so the loop can stop at the first candidate that cannot take a slot:
    // every one after it is farther and could not either. An item just evicted is a
    // candidate again, but never displaces anything, since every holder left is nearer.
    for (;;) {
        int c = nearest_candidate(dist, count, slot_of);
        if (c < 0)
            break;
        int s = -1;
        for (int f = 0; f < capacity; ++f) {
            if (holder[f] < 0) {
                s = f;
                break;
            }
        }
        if (s < 0) {
            s = farthest_slot(dist, capacity, holder);
            if (s < 0 || dist[holder[s]] <= dist[c] + margin)
                break;
            slot_of[holder[s]] = -1;
        }
        holder[s] = c;
        slot_of[c] = s;
        changed++;
    }
    return changed;
}
