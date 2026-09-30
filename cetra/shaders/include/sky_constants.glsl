/*
 * The sky's photometric anchors (spec 13.7), INCLUDED BY BOTH LANGUAGES: sky.h and
 * atmosphere.glsl. An app matching its sun light to the sky computes with these, and the
 * shaders scale every sun-driven term by the first, so a copy per language is the half that
 * would drift -- and a drift here puts every photometric app's sun off its own sky with
 * nothing reporting it. Numbers only, `f`-suffixed; shore_constants.glsl states the rules.
 */

// The atmosphere integral is computed per unit sun illuminance, which puts a clear zenith
// near 0.04; this carries it to the relative scale the sky has always drawn at, where a
// noon zenith is a couple of units.
#define SKY_SUN_ILLUMINANCE 3.0f

// What an unattenuated sun delivers, in klux: what the relative scale is measured against
// to put the sky in nits.
#define SKY_SUN_KLUX 127.5f

// Under RGBA16F's 65504. Every sky target that stores radiance is held under it at the
// write, since a sum of bounded terms is not itself bounded.
#define SKY_STORE_FP16_MAX 60000.0f
