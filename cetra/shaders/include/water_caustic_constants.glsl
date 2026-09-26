/*
 * The refracted-grid caustics' shape (spec 13.2), INCLUDED BY BOTH LANGUAGES: water.c sizes the
 * targets, places the window and allocates the lattice from these; the lattice shaders draw it;
 * water_frag looks it up. A window size or a ceiling spelled once per language drifts silently,
 * and a lookup that disagrees with the trace about where the target lies reads the wrong texel
 * everywhere, plausibly.
 *
 * Numbers only, for shore_constants.glsl's reasons: `#define`s, `f` on every float literal, no
 * type, function or qualifier.
 *
 * All lengths in METRES; each consumer converts by the world's units per metre.
 */

/*
 * The window: a square ahead of the camera rather than a periodic patch, because the three
 * bands tile at 240, 64 and 12 m and share no period.
 *
 * The lattice cell is the short band's own texel -- 12 m over 128 -- so the lattice samples the
 * band that does most of the focusing at its resolution and no coarser. The target is finer than
 * that, since a focused line is much narrower than the wave that focused it. The lattice overhangs
 * the target on every side, because the rays that land on a target edge left the surface upstream
 * of it.
 *
 * The window moves in steps of WATER_CAUSTIC_SNAP_CELLS cells, which must be a whole number of
 * target texels too, or the target's texels land somewhere new each step and the pattern swims:
 * 5 x 12/128 = 12 x 40/1024. Change any of the four sizes and re-derive this.
 */
#define WATER_CAUSTIC_TARGET_M     40.0f
#define WATER_CAUSTIC_TARGET_RES   1024
#define WATER_CAUSTIC_CELL_M       0.09375f
#define WATER_CAUSTIC_GRID_N       512
#define WATER_CAUSTIC_SNAP_CELLS   5

/*
 * The LEVELS (spec 13.3): the window above is level 0, and each further level is the one before
 * with every length divided by WATER_CAUSTIC_LEVEL_DIV -- window, cell and target together, the
 * lattice and target counts unchanged. Dividing all four by one number keeps the snap identity
 * above true at every level, so none of them swims.
 *
 * Level 1 is a 2.34 cm cell over 10 m: two ripple texels (water_ripple_constants.glsl), which is
 * the finest lattice the ripple band can focus through, laid where the pixels are densest. The
 * coarse level still reaches 40 m; the fine one resolves what a 9.4 cm cell cannot.
 */
#define WATER_CAUSTIC_LEVELS    2
#define WATER_CAUSTIC_LEVEL_DIV 4.0f

// Where the rays land when there is no baked bed: the focal length of the short band's ripples,
// n / ((n - 1) a k^2) for a centimetre over half a metre to a metre, is 2.5 to 5 m.
#define WATER_CAUSTIC_PLANE_M 3.0f

// A true focus is a singularity; this is the ceiling on one cell's concentration, and the one
// place the pass knowingly loses energy.
#define WATER_CAUSTIC_MAX 40.0f
