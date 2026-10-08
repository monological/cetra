/*
 * The loading screen's ident, as a timeline in seconds since it was shown (spec 13.34).
 *
 * INCLUDED BY BOTH LANGUAGES, shore_constants.glsl's technique and for its reason: the mark's
 * shader plays the ident and loading_screen.c will not lift the screen until it has played, so
 * two copies of when it ends is a screen that switches off over a letter still turning. Numbers
 * only, `f`-suffixed; no types, functions or qualifiers.
 */

// CETRA: its letters turn in from the left, each a quarter turn from edge on, each starting while
// the one before is still turning.
#define LOADING_TITLE_LETTERS 5
#define LOADING_TITLE_START 0.55f
#define LOADING_TITLE_STAGGER 0.3f
#define LOADING_TURN_SECONDS 1.0f
#define LOADING_TITLE_END \
    (LOADING_TITLE_START + (LOADING_TITLE_LETTERS - 1) * LOADING_TITLE_STAGGER + LOADING_TURN_SECONDS)

// ENGINE, a letter at a time once CETRA has settled, then the rule beneath it.
#define LOADING_ENGINE_START (LOADING_TITLE_END + 0.15f)
#define LOADING_ENGINE_STAGGER 0.08f
#define LOADING_RULE_START (LOADING_ENGINE_START + 0.5f)
#define LOADING_RULE_GROW 0.4f

// A light runs once along the rule from its left end to its right, and a sparkle bursts at the
// right end as it arrives.
#define LOADING_SWEEP_START (LOADING_RULE_START + LOADING_RULE_GROW)
#define LOADING_SWEEP_SECONDS 0.9f
#define LOADING_SPARKLE_START (LOADING_SWEEP_START + LOADING_SWEEP_SECONDS - 0.1f)
#define LOADING_SPARKLE_SECONDS 0.7f

// The tape's glitch, the ident's last beat, a while after the sparkle has gone; the tape's later
// glitches, at random, are as long.
#define LOADING_GLITCH_START (LOADING_SPARKLE_START + LOADING_SPARKLE_SECONDS + 0.8f)
#define LOADING_GLITCH_SECONDS 0.9f

// The ident has played once the glitch has passed.
#define LOADING_IDENT_END (LOADING_GLITCH_START + LOADING_GLITCH_SECONDS)
