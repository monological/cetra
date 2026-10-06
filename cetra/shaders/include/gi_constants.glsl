/*
 * The GI volumes' numbers that the CPU and the GPU must agree on (spec 13.24).
 *
 * INCLUDED BY BOTH LANGUAGES, shore_constants.glsl's technique and for its reason: gi_volume.c
 * publishes one row block per resident volume into a uniform array of this many, and the lookup
 * walks that array. A C cap above the shader's would write past the declared array and leave the
 * volumes beyond it never sampled, with nothing to say so. Numbers only; no types, functions or
 * qualifiers.
 */

// Volumes resident at once: the nearest to the camera, each holding a slot of the scene's
// lighting atlas. Set by the shader's uniform table rather than by memory.
#define GI_RESIDENT_MAX 8
