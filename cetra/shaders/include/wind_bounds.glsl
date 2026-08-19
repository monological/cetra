// The amplitude coefficients windOffset() displaces by, in the one place both
// the shader and the CPU can read them.
//
// This file is valid GLSL and valid C: the shader build splices it (any name
// under include/), and wind.c includes it by relative path. That is the whole
// reason it exists. The CPU needs these numbers to compute a conservative bound
// on the displacement -- which is what lets a wind-driven mesh be frustum-culled
// at all -- and a bound built from a stale copy of them is not conservative, it
// is wrong: too small a margin culls geometry that is on screen, and the symptom
// is a mesh popping out at the frame edge, on the one path with no test that
// would catch it. The house pattern of "must match, checked by eye" (see
// stochastic.glsl) is not good enough for a failure that silent.
//
// Only the fixed multipliers live here. Everything else in the bound -- the
// gust envelope, the height mask, the sines -- is bounded by its own range
// rather than by a constant, and is derived in wind.c beside the derivation.
#define WIND_CLOTH_FLUTTER   0.3
#define WIND_VEG_LEAN        0.6
#define WIND_VEG_SWAY        0.5
#define WIND_VEG_TURB        0.25
#define WIND_LEAF_FLUTTER_Y  0.4
#define WIND_LEAF_FLUTTER_Z  0.6
