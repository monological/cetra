#ifndef _RESIDENCY_H_
#define _RESIDENCY_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The life every streamed lighting item lives (spec 13.24). GI volumes and reflection probes are
 * the same thing to this: the nearest few hold a slot of the scene's lighting atlas, each is
 * captured once into its slot, its texels go to the CPU when it leaves and come back when it
 * returns, and a change in the light it saw drops what was kept. This owns that life -- who
 * holds which slot, and what state each item's texels are in. Each kind supplies its distances,
 * its capture and the rectangle a slot is. No GL, no clock.
 *
 * Three rules make it usable from a renderer.
 *
 * SLOTS ARE STABLE. An item keeps the slot it holds for as long as it stays resident, so a
 * slot's texels are rewritten only when its holder changes. A ranking that re-packed the
 * nearest K into slots 0..K-1 every frame would move every resident item's data whenever any
 * one of them changed rank.
 *
 * A HOLDER IS EVICTED ONLY BY A CLEARLY NEARER ITEM: one more than the margin nearer. Two items
 * at nearly equal distance, the camera between them, would otherwise trade the last slot every
 * frame and each trade is an upload.
 *
 * AN ITEM COMES BACK TO ITS HOME, the slot it last held, when that slot is free. Its texels then
 * land where they were, and so does every coordinate computed from where they are: a kept probe
 * readmitted one column over samples its texels at UVs rounded differently.
 *
 * Ties go to the lower index, so one run repeats itself.
 *
 * TEXELS ARE KEPT AT EVICTION, read from the slot before its next holder writes it, and given
 * back to memory once they are in a slot again. A world that never evicts reads nothing back.
 */

// The most slots a residency has: the probe set's sixteen, the froxel mask's width.
#define RESIDENCY_SLOTS_MAX 16

typedef enum ResidencyState {
    RESIDENCY_OUT,     // holds no slot
    RESIDENCY_CAPTURE, // holds a slot nothing has been captured into
    RESIDENCY_UPLOAD,  // holds a slot its kept texels are owed to
    RESIDENCY_LOADED,  // holds a slot with its texels in it
} ResidencyState;

typedef struct ResidencyItem {
    // SETTINGS: the kind writes it before each assignment.
    float distance; // from the camera to the item's box; negative = may not be resident

    // ENGINE-OWNED: read, never write; residency_loaded is how a kind says its texels are in.
    int slot;   // the slot held, or -1
    int home;   // the slot last held, or -1
    bool moved; // the last assignment admitted or evicted it
    ResidencyState state;
    uint16_t* kept; // its texels while it holds no slot, RGBA half floats; NULL when none
} ResidencyItem;

typedef struct Residency {
    ResidencyItem* items; // one per item, in the kind's order
    size_t count;
    size_t capacity;
    int slots;                       // how many there are, at most RESIDENCY_SLOTS_MAX
    int holder[RESIDENCY_SLOTS_MAX]; // each slot's item, or -1
    float margin;                    // metres nearer a challenger must be
} Residency;

// Read a LOADED item's texels out of the slot it is about to give up: NULL keeps nothing, and
// the item is captured again when it is next admitted.
typedef uint16_t* (*ResidencyKeepFn)(void* user, size_t item, int slot);

void residency_init(Residency* res, int slots, float margin);
void residency_free(Residency* res);

// Append an item, out of residency. False on out of memory.
bool residency_add(Residency* res);

// Decide which items hold the slots from their distances, keeping the texels of every loaded
// item that leaves through `keep` before its slot changes hands. An admitted item is owed an
// upload when something was kept for it and a capture otherwise.
void residency_assign(Residency* res, ResidencyKeepFn keep, void* user);

// The item's texels are in its slot: captured there, or put back. What was kept is given back.
void residency_loaded(ResidencyItem* item);

// The light every item saw has changed: what was kept is dropped, and an item owed an upload
// is owed a capture instead. So is a loaded one when `recapture`; otherwise it stays loaded,
// for a kind that converges over its stale texels in place.
void residency_forget(Residency* res, bool recapture);

#endif // _RESIDENCY_H_
