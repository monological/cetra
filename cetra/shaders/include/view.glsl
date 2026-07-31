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
// absolute constant assuming ~1.0 was white -- pbr_frag's vec3(10.0), ssr's
// min(2.0), gtao's GI_MAX_RADIANCE, the froxel min(500.0) pair, bloom's
// threshold. Photometric lights (spec 9.9/10.0) invalidated all of them at once
// and nothing errored; a red cube just quietly rendered grey, because a 522-nit
// red channel and a 33-nit green one both clamped to 10. Thresholds expressed
// against THIS contract stay correct at any light magnitude.

layout(std140) uniform ViewParams {
    float preExposure;        // scene radiance -> working space
    float oneOverPreExposure; // working space -> scene radiance (metering only)
    float exposureEV100;      // the camera's EV100; debug and GUI readout
    float _viewPad;
};
