// Fire's shared numbers (spec 13.14), compiled by C (fire.h, fire_sim.c) and by GLSL (every
// fire pass). Numbers only: no type, function or qualifier, and every float keeps its `f`
// so C does not promote the expression it lands in to double. See shore_constants.glsl.

// How many fires a scene holds, and what each may carry.
#define FIRE_MAX           8
#define FIRE_MAX_SOURCES   8
#define FIRE_MAX_OBSTACLES 16

// The largest grid one fire simulates, in cells. A grid is stored as a 2D atlas of its z
// slices side by side, so this bounds an atlas at 512 x 1024 texels.
#define FIRE_GRID_MAX_X 64
#define FIRE_GRID_MAX_Y 128
#define FIRE_GRID_MAX_Z 64

// The blackbody table: FIRE_BB_LUT_SIZE texels over [FIRE_BB_T_MIN, FIRE_BB_T_MAX] kelvin,
// each the Rec.709 chromaticity (rgb / luminance) with log10 of the luminance in alpha. Below
// the low end a body glows too faintly to matter and is drawn black.
#define FIRE_BB_LUT_SIZE 256
#define FIRE_BB_T_MIN    600.0f
#define FIRE_BB_T_MAX    4000.0f

// The points a FLAME's spine is drawn through, base to tip.
#define FIRE_SPINE_POINTS 8

// Air's volumetric heat capacity, rho * c_p, J / (m^3 K): what turns the heat the grid's gas
// sheds into the watts a fire releases.
#define FIRE_AIR_RHO_CP 1206.0f

// The source noise's lattice, in cells and in seconds: coarse enough that a log's length breaks
// into a few tongues rather than a cell-by-cell shimmer, and slow enough that a tongue lives
// a moment rather than a step.
#define FIRE_SOURCE_NOISE_CELLS   4.0f
#define FIRE_SOURCE_NOISE_SECONDS 0.15f
