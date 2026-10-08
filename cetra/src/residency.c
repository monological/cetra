#include <stdlib.h>
#include <string.h>

#include "residency.h"
#include "util.h"

void residency_init(Residency* res, int slots, float margin) {
    memset(res, 0, sizeof(*res));
    res->slots = slots < RESIDENCY_SLOTS_MAX ? slots : RESIDENCY_SLOTS_MAX;
    res->margin = margin;
    for (int s = 0; s < RESIDENCY_SLOTS_MAX; ++s)
        res->holder[s] = -1;
}

void residency_free(Residency* res) {
    if (!res)
        return;
    for (size_t i = 0; i < res->count; ++i)
        free(res->items[i].kept);
    free(res->items);
    res->items = NULL;
    res->count = 0;
    res->capacity = 0;
}

bool residency_add(Residency* res) {
    if (!grow_array((void**)&res->items, &res->capacity, res->count + 1, sizeof(ResidencyItem), 8))
        return false;
    res->items[res->count++] = (ResidencyItem){.slot = -1, .home = -1, .state = RESIDENCY_OUT};
    return true;
}

// The nearest item that may be resident and is not, or -1.
static int nearest_candidate(const Residency* res, const bool* resident) {
    int best = -1;
    for (int i = 0; i < (int)res->count; ++i) {
        const float d = res->items[i].distance;
        if (resident[i] || d < 0.0f)
            continue;
        if (best < 0 || d < res->items[best].distance)
            best = i;
    }
    return best;
}

// The resident item that is farthest, or -1. Of two at one distance the higher index counts as
// farther, the mirror of the candidates' tie rule.
static int farthest_resident(const Residency* res, const bool* resident) {
    int best = -1;
    for (int i = 0; i < (int)res->count; ++i) {
        if (!resident[i])
            continue;
        // >= since i only grows: a tie goes to the later index.
        if (best < 0 || res->items[i].distance >= res->items[best].distance)
            best = i;
    }
    return best;
}

// Give an arriving item slot `s`.
static void admit(Residency* res, int i, int s) {
    ResidencyItem* item = &res->items[i];
    res->holder[s] = i;
    item->slot = s;
    item->home = s;
    item->state = item->kept ? RESIDENCY_UPLOAD : RESIDENCY_CAPTURE;
    item->moved = true;
}

void residency_assign(Residency* res, ResidencyKeepFn keep, void* user) {
    const int count = (int)res->count;
    for (int i = 0; i < count; ++i)
        res->items[i].moved = false;
    if (count == 0)
        return;
    bool* resident = calloc((size_t)count, sizeof(bool));
    if (!resident)
        return;

    // WHO is resident, first, and only then WHERE: deciding both at once hands an item
    // readmitted beside another whichever slot its eviction happened to free, and its texels
    // land somewhere new for no reason.
    int held = 0;
    for (int s = 0; s < res->slots; ++s) {
        const int h = res->holder[s];
        if (h >= 0 && res->items[h].distance >= 0.0f) {
            resident[h] = true;
            held++;
        }
    }
    // Nearest first, so the loop can stop at the first candidate that cannot get in: every
    // one after it is farther and could not either.
    for (;;) {
        const int c = nearest_candidate(res, resident);
        if (c < 0)
            break;
        if (held < res->slots) {
            resident[c] = true;
            held++;
            continue;
        }
        const int f = farthest_resident(res, resident);
        if (f < 0 || res->items[f].distance <= res->items[c].distance + res->margin)
            break;
        resident[f] = false;
        resident[c] = true;
    }

    // Leaving, while the slot is still theirs: the next holder writes over it.
    for (int s = 0; s < res->slots; ++s) {
        const int h = res->holder[s];
        if (h < 0 || resident[h])
            continue;
        ResidencyItem* item = &res->items[h];
        if (item->state == RESIDENCY_LOADED && keep && !item->kept)
            item->kept = keep(user, (size_t)h, s);
        item->state = RESIDENCY_OUT;
        item->slot = -1;
        item->moved = true;
        res->holder[s] = -1;
    }
    // Arriving: every item whose home is free takes it before any takes a free slot, which
    // might otherwise be the home of one arriving after it.
    for (int i = 0; i < count; ++i) {
        const ResidencyItem* item = &res->items[i];
        if (resident[i] && item->slot < 0 && item->home >= 0 && item->home < res->slots &&
            res->holder[item->home] < 0)
            admit(res, i, item->home);
    }
    for (int i = 0; i < count; ++i) {
        if (!resident[i] || res->items[i].slot >= 0)
            continue;
        int s = 0;
        while (res->holder[s] >= 0)
            ++s;
        admit(res, i, s);
    }
    free(resident);
}

void residency_loaded(ResidencyItem* item) {
    free(item->kept);
    item->kept = NULL;
    item->state = RESIDENCY_LOADED;
}

bool residency_loaded_where(const Residency* res, ResidencyBearsFn bears, const void* user) {
    for (size_t i = 0; i < res->count; ++i)
        if (res->items[i].state != RESIDENCY_LOADED && bears(user, i))
            return false;
    return true;
}

void residency_forget(Residency* res, bool recapture) {
    for (size_t i = 0; i < res->count; ++i) {
        ResidencyItem* item = &res->items[i];
        free(item->kept);
        item->kept = NULL;
        if (item->state == RESIDENCY_UPLOAD || (recapture && item->state == RESIDENCY_LOADED))
            item->state = RESIDENCY_CAPTURE;
    }
}
