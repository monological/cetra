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

// Half the depth the occlusion map spans along the rain's travel, either side of the camera,
// in metres; and how far above the surface the map holds a point may sit and still count as
// open sky. The map stores the surface nearest the sky, so a point ON it compares equal.
#define RAIN_OCCLUSION_REACH 250.0f
#define RAIN_EXPOSED_BIAS 0.02f

// The share of the light a drop scatters that it scatters by REFRACTION, into a forward lobe;
// the rest -- external reflection and the internal bounces -- goes everywhere. Geometric
// optics puts refraction at most of it, which is why rain between the eye and a lamp glitters
// and the same rain lit from the side is barely there.
#define RAIN_REFRACT_SHARE 0.85f

// A falling drop oscillates, and the image of a lamp in it flashes as it does (Garg and Nayar
// 2006): Rayleigh's lowest mode rings at (1/2pi) sqrt(8 sigma / (rho a^3)) for radius a, so a
// 1 mm drop flashes about eleven times across a 1/60 s streak and a 4 mm drop once or twice.
// Surface tension of water in N/m, and its density in kg/m^3.
#define RAIN_SURFACE_TENSION 0.0728f
#define RAIN_WATER_DENSITY 1000.0f
