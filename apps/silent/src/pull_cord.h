#ifndef _SILENT_PULL_CORD_H_
#define _SILENT_PULL_CORD_H_

#include <stdbool.h>

#include "cetra/ui.h"

/*
 * Starting the boat's outboard (spec 13.56): a pull on its cord timed against a marker running
 * back and forth along a bar. A pull while the marker is in the small lit zone is a good one: the
 * motor coughs and the zone moves somewhere else along the bar. PULL_CORD_PULLS good pulls catch
 * it. A pull outside the zone is a weak one and costs nothing.
 */

#define PULL_CORD_PULLS 3

typedef struct PullCord {
    bool open;
    float run;      // seconds the marker has been running
    float sweep;    // seconds it takes from one end of the bar to the other
    float zone;     // where the lit zone starts along the bar, 0..1
    int good;       // good pulls so far
    float since;    // seconds since the last pull, for its flash
    bool last_good; // whether the last pull was good
    float alpha;    // the bar's fade in and out
    unsigned int state;
} PullCord;

// The bar up, the marker at its start, no pulls yet; `seed` places the zones.
void pull_cord_open(PullCord* pc, unsigned int seed);
// The bar going; nothing pulled is kept.
void pull_cord_close(PullCord* pc);
// Per frame: the marker run on and the bar faded `dt` on.
void pull_cord_update(PullCord* pc, float dt);
// A pull now: whether it was a good one. The zone moves after a good pull.
bool pull_cord_pull(PullCord* pc);
// Whether enough good pulls have been had to catch.
bool pull_cord_caught(const PullCord* pc);
// The bar, drawn over the window `el` covers: hud_set_gauge's draw, with the PullCord as `user`.
void pull_cord_draw(UIElement* el, UIDrawList* dl, void* user);

#endif // _SILENT_PULL_CORD_H_
