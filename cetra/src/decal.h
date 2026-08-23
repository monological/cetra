#ifndef _DECAL_H_
#define _DECAL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <cglm/cglm.h>

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

/*
 * A mark projected onto whatever surface is inside its box.
 *
 * ORIENTED, unlike the fog volume it otherwise resembles, and that deferral's
 * reasoning does not carry over: a fog box is a MEDIUM, which an axis-aligned
 * pair of corners describes completely, and it declined a matrix per volume
 * because nothing had asked. A decal is a PICTURE, so which way round it sits is
 * half of what is authored -- there is no unoriented version of a poster -- and
 * the per-fragment transform is the cost this feature exists to pay.
 *
 * What it is NOT is a material: 11.68's roads reshape a layered ground's own
 * splat weights, so a road is MADE of a layer the surface already carries. This
 * projects an image onto surfaces that have no such layer set at all, which is
 * why walls and props are its half of the job.
 *
 * Owned by the Scene as a flat array, like a fog volume and unlike a
 * transformable citizen -- but declared HERE, with its cap and the functions
 * that read it, which is where probe_set.h and roads.h each keep theirs.
 */
typedef struct Decal {
    vec3 position;
    vec3 half_extent; // local x,y span the image covers; z the projection depth
    vec3 direction;   // the way the projector faces; local +Z
    vec3 up;          // roll reference, Gram-Schmidt'd against direction
    float opacity;    // scales the image's own alpha; clamped to [0,1] at pack
    float angle_fade; // degrees off-axis past which a surface takes no mark
    float feather;    // local units the edge ramps over, inward from the box faces
    float normal_strength;
    bool enabled;
    struct Texture* albedo_tex;  // borrowed from the scene's texture pool
    struct Texture* surface_tex; // optional: packed normal.xy + roughness + AO
    // Layer indices in the material texture array, assigned at its build and
    // -1 until then (or where no map was authored). Re-read every frame by the
    // descriptor pack, so a rebuild that renumbers layers heals on the next one.
    int albedo_layer;
    int surface_layer;
} Decal;

struct Scene;
struct GpuDecalBlock;

/*
 * Pack every live decal into `out` -- enabled, with an image the material array
 * has given a layer -- writing the count into its info row and returning it.
 *
 * `bounds` receives, PER PACKED SLOT, the world-space bounding sphere the froxel
 * grid tests: xyz centre, w radius. That is the whole reason it is an out
 * parameter rather than something the caller derives.
 *
 * Descriptor i and mask bit i must name the same decal, and the mask loop used
 * to re-walk the scene under a shared predicate to rediscover the numbering this
 * function had just produced. Three things then had to agree across two files --
 * the predicate, the cap, and where the counter increments relative to the
 * culling rejects -- and only the first was written down. Moving the slot's
 * counter three lines, which is the obvious "do not burn a slot you will not
 * use" edit, shifted every bit above it and projected every mark in the scene
 * through its neighbour's box, silently. Handing the enumeration out makes that
 * unrepresentable rather than documented.
 *
 * The world->local rows are built here rather than in the shader for the reason
 * every other descriptor is: a frame that resolves differently on the two sides
 * aims the same authored numbers two ways, and both halves still render.
 */
int decal_fill_descriptors(const struct Scene* scene, struct GpuDecalBlock* out, float bounds[][4]);

/*
 * One line per frame plus one per decal at the end, the --water-fft-probe idiom.
 *
 * The instrument exists because a decal's correctness is not a question a frame
 * answers: a mark projected through a box half a metre out, or folded by the
 * wrong axis, or taking its image from a neighbour's layer, still lands on a
 * wall and still looks like a poster. What is checkable from outside the process
 * is the froxel mask -- how many cells each decal claimed, and whether two runs
 * claimed the same ones -- and the layers the array actually assigned.
 *
 * The digest and bit count come from the cluster build, which is where the masks
 * are; a Decal has no module owning a set of them to report back to.
 */
void decal_probe_print(const struct Scene* scene, int frame, bool final, uint32_t mask_digest,
                       int mask_bits);

#endif // _DECAL_H_
