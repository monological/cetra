/*
 * How many dry shapes a water may have (spec 13.56): places inside its bounds where it is not,
 * such as the inside of a hull floating on it.
 *
 * INCLUDED BY BOTH LANGUAGES, shore_constants.glsl's technique and for its reason: water.c
 * uploads at most this many and the shaders' uniform array holds this many. Numbers only.
 */

#define WATER_DRY_MAX 4
