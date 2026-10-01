/*
 * The rain's numbers that the CPU and the GPU must agree on (spec 13.9).
 *
 * INCLUDED BY BOTH LANGUAGES, shore_constants.glsl's technique and for its reason: rain.c
 * derives the fall speed the occlusion map looks along and the streak shader derives the
 * fall speed each drop is drawn with, and shadow.c renders the map at a depth range the
 * lookup has to invert. Two copies of any of these is a streak that falls at a different
 * angle from the cover it is clipped by. Numbers only, `f`-suffixed; no types, functions or
 * qualifiers.
 */

// Terminal velocity of a drop of diameter D mm, in m/s: A - B exp(-C D) (Atlas, Srivastava and
// Sekhon 1973). The fit goes negative below 0.11 mm, where nothing it describes falls.
#define RAIN_ATLAS_A 9.65f
#define RAIN_ATLAS_B 10.3f
#define RAIN_ATLAS_C 0.6f

// The drop sizes drawn as streaks, in mm. Below the floor a drop is too slow and too small to
// streak; above the ceiling it breaks up before it reaches terminal velocity.
#define RAIN_DROP_MIN_MM 0.5f
#define RAIN_DROP_MAX_MM 6.0f
// Nested boxes of falling drops round the camera, each RAIN_STREAK_BOX_SCALE times as wide as the
// last: where the outermost ends is where the rain in the air takes over from the streaks.
#define RAIN_STREAK_BOXES 3
#define RAIN_STREAK_BOX_SCALE 3.0f

// The drops that SPLASH. On a wet surface a drop throws a crown once We Oh^-0.4 passes about
// 2100 (Cossali, Coghe and Marengo 1997), which at terminal velocity is 2063 for a 1 mm drop;
// smaller ones merge into the film. A splash is drawn as the droplets its crown sheds, each a
// small drop on a ballistic arc, and a splash slot lives as long as its highest droplet flies.
#define RAIN_SPLASH_MIN_MM 1.0f
#define RAIN_SPLASH_DROPLETS 6
#define RAIN_SPLASH_LIFE 0.4f

// Half the depth the occlusion map spans along the rain's travel, either side of the camera,
// in metres; and how far above the surface the map holds a point may sit and still count as
// open sky. The map stores the surface nearest the sky, so a point ON it compares equal.
#define RAIN_OCCLUSION_REACH 250.0f
#define RAIN_EXPOSED_BIAS 0.02f
// The same two in the map's own units: the metres one unit of stored depth spans, and the bias
// as stored depth.
#define RAIN_MAP_DEPTH_METRES (2.0f * RAIN_OCCLUSION_REACH)
#define RAIN_EXPOSED_DEPTH_BIAS (RAIN_EXPOSED_BIAS / RAIN_MAP_DEPTH_METRES)
// The polygon offset the map is drawn with, glPolygonOffset's factor and units: the shadow
// pass's. A surface under slanted rain is stored this many depth steps a texel deeper than it
// is, which a lookup that needs the SURFACE itself, not only which side of it a point is on,
// has to take back out.
#define RAIN_MAP_SLOPE_BIAS 2.0f
#define RAIN_MAP_CONSTANT_BIAS 2.0f
// How far off a surface, along its normal, its cover is asked for, in metres: a texel of the
// map (9.4 cm at the default extent) and a little over. Asked AT the surface, a wall the rain
// strikes at a grazing angle compares against its own depth quantised across a texel it
// spans a third of a metre of, and shadows itself in stripes.
#define RAIN_NORMAL_OFFSET 0.12f

// The share of the light a drop scatters that it scatters by REFRACTION, into a forward lobe;
// the rest -- external reflection and the internal bounces -- goes everywhere. Geometric
// optics puts refraction at most of it, which is why rain between the eye and a lamp glitters
// and the same rain lit from the side is barely there.
#define RAIN_REFRACT_SHARE 0.85f

// Where the clock beads on glass live by wraps, in seconds of that clock. Every bead's life
// divides it, so nothing jumps when it wraps.
#define RAIN_BEAD_CLOCK_WRAP 1024.0f

// Drips (spec 13.12): the most lines a rain carries, set by the vertex stage's uniform
// arrays -- three of vec4, 576 of the 1024 components GL guarantees that stage -- rather than
// by anything a scene would want; the diameter a drop leaves an edge at,
// where its weight overcomes the surface tension holding it (about 4.5 mm for water); and the
// shortest cycle a drip point runs and the least time a drop hangs growing before it falls.
#define RAIN_DRIP_MAX 48
#define RAIN_DRIP_MM 4.5f
#define RAIN_DRIP_PERIOD_MIN 1.5f
#define RAIN_DRIP_HANG_MIN 0.25f
#define RAIN_GRAVITY 9.81f

// A falling drop oscillates, and the image of a lamp in it flashes as it does (Garg and Nayar
// 2006): Rayleigh's lowest mode rings at (1/2pi) sqrt(8 sigma / (rho a^3)) for radius a, so a
// 1 mm drop flashes about eleven times across a 1/60 s streak and a 4 mm drop once or twice.
// Surface tension of water in N/m, and its density in kg/m^3.
#define RAIN_SURFACE_TENSION 0.0728f
#define RAIN_WATER_DENSITY 1000.0f
