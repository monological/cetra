/*
 * The touch simulation's drop queue (spec 13.4), INCLUDED BY BOTH LANGUAGES: water.c uploads up
 * to WATER_TOUCH_MAX_DROPS drops a step and water_touch_frag declares that array. An array length
 * spelled once per language is an upload that silently stops short, or runs past the end.
 *
 * Numbers only, for shore_constants.glsl's reasons: `#define`s, no type, function or qualifier.
 * Integers, which mean the same thing to both preprocessors.
 */
#define WATER_TOUCH_MAX_DROPS 4
