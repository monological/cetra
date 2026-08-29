// Which optional features a lit-surface variant carries, in the one place both
// the shader and the CPU can read them (spec 11.93).
//
// INCLUDED BY BOTH LANGUAGES, the same technique and for the same reason as
// wind_bounds.glsl and shore_constants.glsl: the numbers live once and both
// sides include this file. Here the CPU copy is program.c, which emits
// `#define CETRA_PBR_FEATURES <mask>` into the source, and the shader reads the
// mask back through the same bits.
//
// The first shape passed a set of macro NAMES instead -- C emitted the text
// "#define CETRA_NO_SHEEN 1" and the shader tested `#ifdef CETRA_NO_SHEEN`. That
// is the hand-mirrored copy this technique exists to delete, and its failure was
// silent in the worst way: rename or mistype either side and the guard simply
// never fires, so the variant keeps the feature it was built to drop. The
// picture stays correct, every golden stays green, and the only symptom is that
// the optimisation stopped happening. Sharing a NUMBER cannot fail that way --
// a bit renamed here breaks both compiles.
//
// WRITING FOR TWO PREPROCESSORS, per shore_constants.glsl's rules: nothing here
// may use a type, a function or a qualifier. Numbers only. No `f` suffixes,
// unlike wind_bounds.glsl, because these are mask bits rather than amplitudes --
// an integer means the same thing to both preprocessors.
#define PBR_FEAT_DECALS   1
#define PBR_FEAT_AREA     2
#define PBR_FEAT_SHEEN    4
#define PBR_FEAT_ANISO    8
#define PBR_FEAT_PARALLAX 16

// The union, written out rather than OR-ed together, because an expression here
// would have to parse identically in C, GLSL and the Python that reads this file
// for scripts/gates.py. A literal is the only form all three agree on.
#define PBR_FEAT_ALL 31
