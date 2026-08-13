// The water surface, evaluated in ONE place.
//
// Both the position the vertex stage rasterizes and the normal the fragment
// stage shades with come from here, and that is the point: a normal derived from
// a different height than the raster used prints as shading that does not match
// the silhouette, and the shadow pass evaluating its own copy casts a shadow
// from a surface that is not where the surface is. wind.glsl makes the same
// argument for the same reason.
//
// Derivatives are ANALYTIC, never finite differences. The models this seam is
// built for both hand them over for free: a Gerstner term differentiates in
// closed form, and an FFT cascade transforms its derivative fields alongside its
// height in the same pass.

uniform float waterLevel;

struct OceanSurface {
    vec3 world;  // displaced world position
    vec3 normal; // unit normal from the analytic derivatives
};

// A still plane: the degenerate case of the wave sum that replaces it, with an
// exact and constant derivative. What this file establishes is the seam, not the
// current contents of it -- every consumer below reads world and normal and does
// not care which model produced them.
OceanSurface oceanEvaluate(vec2 p, float t) {
    OceanSurface s;
    s.world = vec3(p.x, waterLevel, p.y);
    s.normal = vec3(0.0, 1.0, 0.0);
    return s;
}
