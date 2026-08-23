#ifndef _DECAL_H_
#define _DECAL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * What a decal IS, packed for the shader (spec 11.73).
 *
 * The split mirrors the probe set's: this file states what a decal means and
 * fills the descriptors, and light_cluster.c owns the froxel masks beside them,
 * because those are a property of the GRID and the camera rather than of the
 * decal. The two halves land in one UBO block and are uploaded together.
 *
 * The Decal itself lives on the Scene (scene.h) -- it is a thing placed in the
 * world in world units, like a fog volume and unlike a transformable citizen.
 */

/*
 * The cap is small and deliberate, the roads argument. A room of posters,
 * scorches and logos is a dozen marks; a scene wanting fifty wants them baked
 * into the paged content the composite cache already serves, not fifty box
 * tests per fragment.
 *
 * It is also what the mask width is sized from, so it is not free to be
 * generous: at 32 the froxel masks alone are 12288 bytes and would pin the
 * descriptor at six vec4 rows forever, and at 64 the block does not fit GL
 * 4.1's guaranteed 16 KB at all. Every scene pays the compiled loop's width
 * whether or not it has a decal in it.
 *
 * Declared here rather than in scene.h because light_cluster.h sizes its mask
 * array from it and cannot include that -- the probe set and the roads own
 * their own ceilings for the same reason.
 */
#define DECAL_MAX 16

struct Scene;
struct Decal;
struct GpuDecalBlock;

// Whether this decal reaches the shader at all: enabled, and with an image that
// has been assigned an array layer.
//
// ONE statement of the rule, because two walks consume it -- the descriptor
// pack here and the froxel mask loop in light_cluster.c -- and they index the
// same slot. A predicate that differed between them would not fail loudly; it
// would give mask bit i and descriptor i to different decals, and every mark in
// the scene would be projected by its neighbour's box.
bool decal_is_live(const struct Decal* decal);

// Pack every enabled decal that has an image into `out`, writing the count into
// its info row. Descriptors only: the mask array is the cluster build's and is
// left alone.
//
// The world->local rows are built here rather than in the shader for the reason
// every other descriptor is: a frame that resolves differently on the two sides
// aims the same authored numbers two ways, and both halves still render.
void decal_fill_descriptors(const struct Scene* scene, struct GpuDecalBlock* out);

// FNV-1a over the froxel masks, handed back the way the probe set takes its
// own. Reported rather than written from the cluster build for the same reason:
// that module reads decals and owns the grid, and reaching sideways to mutate a
// subsystem's fields is what forced a const Scene* to be laundered once already.
//
// The digest is over the MASKS alone -- descriptors are a function of the
// authored scene, where the masks are a function of the camera path, which is
// what a determinism question is about.
uint32_t decal_mask_digest(const void* masks, size_t bytes);

#endif // _DECAL_H_
