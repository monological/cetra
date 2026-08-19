// The amplitude coefficients windOffset() displaces by, in the one place both
// the shader and the CPU can read them.
//
// INCLUDED BY BOTH LANGUAGES, the same technique and for the same reason as
// shore_constants.glsl (included from C at shore_runup.h): the numbers live
// once and both sides include this file, leaving only the arithmetic
// duplicated, which is reviewable by eye where a bare literal is not. Here the
// CPU copy is wind.c's wind_max_offset, a conservative BOUND on the
// displacement -- which is what lets a wind-driven mesh be frustum-culled at
// all -- and a bound built from a stale copy of these is not conservative, it
// is wrong: too small a margin culls geometry that is on screen, and the
// symptom is a mesh popping at the frame edge on the one path with no test that
// would catch it.
//
// WRITING FOR TWO PREPROCESSORS, per shore_constants.glsl's rules. The `f`
// suffix is load-bearing on the C side: an unsuffixed 0.3 is a double there,
// and would promote every expression it touches to a precision the GPU copy
// does not have. And nothing here may use a type, a function or a qualifier --
// `const`, `static` and `float[3](...)` each belong to one side only. Numbers
// only.
//
// Only the fixed multipliers live here. Everything else in the bound -- the
// gust envelope, the height mask, the sines -- is bounded by its own range
// rather than by a constant, and is derived in wind.c beside the derivation.
#define WIND_CLOTH_FLUTTER   0.3f
#define WIND_VEG_LEAN        0.6f
#define WIND_VEG_SWAY        0.5f
#define WIND_VEG_TURB        0.25f
#define WIND_LEAF_FLUTTER_Y  0.4f
#define WIND_LEAF_FLUTTER_Z  0.6f
