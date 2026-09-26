/*
 * The surface query's shape (spec 13.1), INCLUDED BY BOTH LANGUAGES: water.h sizes the slot
 * table and the readback from these, water_probe_frag declares its point array and runs its
 * inversion with them, and water_waves.c's CPU solve runs the same loop. A slot count or a step
 * cap spelled once in each language drifts silently -- a larger C table against a smaller
 * uniform array answers the extra slots with whatever the texel was cleared to.
 *
 * Numbers only, for shore_constants.glsl's reasons: `#define`s, `f` on every float literal, no
 * type, function or qualifier.
 */

// Query slots, one texel each. The render app's probe grid is 4x4.
#define WATER_PROBE_MAX 16

// The horizontal map's inversion: a step cap, and a tolerance as a fraction of the longest
// wave's amplitude, below which the parameter has stopped moving by anything the surface can
// express.
#define WATER_WAVES_INVERSE_MAX_STEPS 8
#define WATER_WAVES_INVERSE_EPS_FRAC  0.002f
