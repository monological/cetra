#ifndef _SAND_H_
#define _SAND_H_

// Procedural sand texture synthesis (spec 11.44). Pure CPU, same contract as
// vegetation_tex.h: every entry point returns a malloc'd 8-bit buffer the caller
// uploads and owns, and nothing here touches GL.
//
// Uses that header's 2D noise, and inherits its seeding hazard with it: seed
// immediately before the bake that depends on it, on one thread.

// The sand relief in [0,1], `width * height` floats the caller allocates: 0 in a
// ripple trough, 1 on a crest.
//
// ONE field behind every map below, for the reason veg_bark_height_field states:
// the albedo's shading, the normal's ridges and the roughness all describe the
// same surface rather than three noises that happen to share a texel.
//
// `ripple_angle` in radians orients the ripple train. Wind-blown sand ripples run
// ACROSS the wind, and a beach's run along the shore -- so an app that knows which
// way its shore faces can say so, and one that does not can pass 0.
void sand_height_field(float* out, int width, int height, float ripple_angle);

// RGB. Near-neutral by design, and that is the whole reason this file is small:
// the large-scale colour -- dry sand, wet sand, the upland it grades into -- comes
// from VERTEX COLOUR. So this map carries grain and relief shading only, and
// multiplying it by any hue has to land on plausible sand.
//
// The reason given here used to be that pbr_frag declares sixteen of sixteen
// samplers and a second terrain albedo cannot be bound. That stopped being true
// in spec 11.60: a second albedo goes in the material texture array as a LAYER,
// for no declaration at all. What remains true is that this map is authored to be
// tinted, so a caller wanting a per-texel ground should reach for a layer set
// rather than trying to make this one carry colour.
unsigned char* sand_albedo(int width, int height, const float* field);
unsigned char* sand_normal(int width, int height, const float* field);
// RGB, and not flat: wet-looking specular comes from the vertex-colour band, but
// dry sand is not uniformly rough either -- packed troughs are smoother than the
// loose grain on a crest.
unsigned char* sand_roughness(int width, int height, const float* field);

#endif // _SAND_H_
