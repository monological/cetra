// The working-space contract for the scene HDR buffer, and the per-frame view
// state that defines it. This file is the authority; C mirrors it at ubo.h
// (UBO_BINDING_VIEW, UBO_VIEW_BLOCK_SIZE) and uploads it once per frame.
//
// THREE SPACES, and confusing them is the failure this file exists to prevent:
//
//   scene radiance  photometric, unbounded. A sun is ~1e5 lux, a bulb ~127 cd.
//                   Lights, materials and import live here.
//   working space   pre-exposed. 1.0 IS DIFFUSE WHITE at the current exposure.
//                   The scene HDR buffer and every post pass live here.
//   display         tonemapped, 0..1. After tonemap; GUI and overlays too.
//
// Every pass that WRITES scene radiance into the HDR buffer must multiply by
// preExposure first. Passes that DERIVE from that buffer (SSGI, SSR, OIT
// resolve) inherit it and must not apply it again. Passes that need absolute
// radiance back -- auto-exposure metering is the only one -- multiply by
// oneOverPreExposure.
//
// Why this is a UBO and not a uniform: the writers are spread across shaders
// that each do their own glUseProgram (skybox, sky background, particles, the
// fog composite) and are NOT reached by render.c's scene-traversal uniform
// block. A per-program upload would be eight hand-maintained sites, and the
// failure mode of missing one is a plausible-looking brightness difference
// rather than an error.
//
// Why it matters at all: before this existed, every stage carried its own
// absolute constant assuming ~1.0 was white, each a bare literal in the pass
// that applied it -- the WS_* ceilings below are those same numbers, gathered.
// Photometric lights (spec 9.9/10.0) invalidated all of them at once and
// nothing errored; a red cube just quietly rendered grey, because a 522-nit red
// channel and a 33-nit green one both clamped to 10. Thresholds expressed
// against THIS contract stay correct at any light magnitude.

layout(std140) uniform ViewParams {
    float preExposure;        // scene radiance -> working space
    float oneOverPreExposure; // working space -> scene radiance (metering only)
    float exposureEV100;      // the camera's EV100; debug and GUI readout
    float _viewPad;
};

// Working-space ceilings. Each is a headroom budget: how many stops over
// diffuse white a pass keeps before it stops tracking brightness. They belong
// here rather than in the passes that apply them because the number is only
// meaningful against the contract above -- as a bare literal in a pass, it is
// the same trap this file documents, and the same one that spec 10.0 sprang.
//
// Values carried over unchanged from when they were scattered literals, so
// naming them was inert. Stops are the unit to retune in: raising one keeps
// more range and more fireflies, lowering it the reverse.
//
// A ceiling belongs here ONLY if the thing it bounds is genuinely measured in
// multiples of white. pbr_frag's per-light clamp was not -- it bounded a number
// of nits -- and moved out to become a dimensionless BRDF_MAX.
//
// The ones that remain all sit upstream of the auto-exposure meter, which reads
// through them: the metered value contains min(radiance, C / preExposure), so
// where a ceiling BINDS it moves the equilibrium the loop settles on. That is
// survivable in a way the terms spec 10.2 phase 1 removed were not -- a clamp
// releases as the gain closes, so the loop still converges, where an
// un-pre-exposed light is an unconditional 1/preExposure factor with no
// positive solution at all.
//
// So the rule is: these must be OVERFLOW AND FIREFLY GUARDS, sitting far enough
// above anything a scene legitimately produces that they engage on outliers
// only. They are not brightness controls. Measured on a sky-lit fixture with a
// 40000 cd key and SSR/SSGI/fog live, disabling all three entirely moves peak
// output by 2/255 -- so what they buy is small, and a ceiling tight enough to
// shape the image would be buying that at the cost of the loop's fixed point.
//
// WS_REFLECT_MAX was +1 stop and WS_BOUNCE_MAX +2, which are look controls by
// that standard: a mirror reflecting a lamp is legitimately hundreds of times
// white, and clipping it there both dims the reflection and drags the exposure.
const float WS_SCENE_MAX = 60000.0; // +15.9 stops: fp16 guard on the finished pixel
const float WS_REFLECT_MAX = 64.0;  // +6 stops: SSR firefly guard
const float WS_BOUNCE_MAX = 64.0;   // +6 stops: SSGI gather firefly guard
const float WS_MEDIA_MAX = 500.0;   // +9 stops: fog in-scatter, per froxel and integrated
