
#ifndef _EMISSIVE_LIGHT_H_
#define _EMISSIVE_LIGHT_H_

#include <cglm/cglm.h>
#include <stdbool.h>

struct Material;
struct Mesh;
struct Scene;

// The derived panels a Scene owns. Opaque: it holds each panel's LOCAL fit,
// which is this module's business and nobody else's.
struct EmissivePanels;
void emissive_panels_free(struct EmissivePanels* panels);

/*
 * Deriving an LTC area light from an emissive mesh (spec 11.49, roadmap C2).
 *
 * An emissive mesh is bright and lights nothing. Everything needed to make it a
 * real source already exists -- Light carries position, direction, up and size,
 * its canonical unit for a panel is nits, and pbr_frag already integrates a
 * rectangle analytically -- so what is missing is only the fit and the
 * registration. Nothing here touches a shader.
 *
 * THE FIT IS IN LOCAL SPACE, DELIBERATELY. A plane fit over every vertex is the
 * expensive half and the mesh's own coordinates are the frame it is stable in,
 * so it can be taken once and transformed by the owning node's global_transform
 * every frame. That is what makes a lamp on a moving node cost three vector
 * transforms rather than a refit, and it is why the fit may run at load (where
 * no transform is resolved yet) while placement may not.
 */

// A rectangle fitted to an emissive mesh, in that mesh's LOCAL space.
//
// `normal` is the side the panel lights: ltcPanel returns zero for a fragment
// behind the plane, so a panel whose winding faces away lights nothing rather
// than lighting the wrong side. `up` spans the height axis and width follows
// from cross(up, normal) -- the same frame light_cluster.c packs and
// include/ltc.glsl unpacks, stated once here so the three cannot disagree.
typedef struct EmissivePanelFit {
    vec3 center;
    vec3 normal;
    vec3 up;
    vec2 size; // width along cross(up, normal), height along up

    // |sum(n_i * A_i)| / sum(A_i): exactly 1 for a flat quad however
    // tessellated, and 0 for a closed volume whose faces cancel. One number
    // that rejects every shape a rectangle cannot describe, which is why there
    // is no separate "is it a quad" test.
    float planarity;
    float area; // sum(A_i), local units squared
} EmissivePanelFit;

// Why a mesh produced no panel. Reported rather than swallowed: a lamp that
// silently fails to light is indistinguishable from a lamp nobody wired up, and
// the probe exists because that difference is invisible in the frame.
typedef enum EmissiveFitReject {
    EMISSIVE_FIT_OK = 0,
    EMISSIVE_FIT_NO_GEOMETRY, // not indexed triangles
    EMISSIVE_FIT_DEGENERATE,  // total area is zero
    EMISSIVE_FIT_NOT_PLANAR,  // a box, a tube, a sphere
    EMISSIVE_FIT_TOO_DIM,     // cannot light anything, and would cost a slot
    EMISSIVE_FIT_OPTED_OUT,   // the material says its emissive is decorative
} EmissiveFitReject;

// Never NULL. For logs and the probe.
const char* emissive_fit_reject_name(EmissiveFitReject reject);

// Planarity below this is not a rectangle. A flat quad reads exactly 1.0, so
// the bar is loose enough for a slightly warped card and nowhere near a shape
// whose faces start cancelling.
#define EMISSIVE_FIT_MIN_PLANARITY 0.9f

// Radiance under this cannot light anything and would still occupy a cluster
// slot. In nits, against a diffuse white of ~1.
#define EMISSIVE_FIT_MIN_NITS 1e-3f

// Fit a rectangle to `mesh` in its LOCAL space. Pure geometry -- it asks nothing
// of the material, so a caller wanting the material's verdict tests that
// separately and the two rejections stay distinguishable in the probe.
EmissiveFitReject emissive_panel_fit(const struct Mesh* mesh, EmissivePanelFit* out);

// The radiance a material emits, in nits.
//
// This is NOT a conversion. material_emissive_factor is already the value
// pbr_frag adds to Lo before the pre-exposure multiply, so the number is already
// scene radiance and already in the unit an area light's intensity is measured
// in. What this adds on top is the texture's mean, which is the one thing the
// panel needs and the shader does not.
//
// The three lines of factor arithmetic used to be duplicated here, justified by
// a comment claiming render.c "encodes a texture fallback the fit reads
// differently". It did not -- the fallback was identical and only the mean
// multiply differed. Both call material.c now.
//
// Returns false when the answer is PROVISIONAL: the material has an emissive
// texture whose mean could not be read yet, so this is the factor alone and will
// change. Textures stream in on a worker (async_loader.c), so that is the normal
// state for the first frames rather than an error -- the per-frame reconcile
// recomputes and settles on its own. A caller that reports rather than shades
// has to say which of the two it is holding.
bool emissive_material_radiance(struct Material* material, vec3 out_nits);

// Split a radiance into the (color, intensity) pair Light stores, so that
// color * intensity reconstructs it exactly and `intensity` reads as real nits
// an author can check against a datasheet. A radiance of zero yields intensity
// zero and leaves the colour white rather than dividing by it.
void emissive_radiance_to_light(const vec3 nits, vec3 out_color, float* out_intensity);

// Bring the scene's derived panels into agreement with its emissive geometry,
// and place them in world space from their owning nodes' CURRENT transforms.
// Returns how many derived panels are live. `enabled` false removes every one,
// so the feature toggles cleanly rather than leaving orphans behind.
//
// CALL IT UNCONDITIONALLY and pass the flag -- do not guard the call. Guarding it
// is what made that promise unreachable: with the call skipped, turning the
// feature off left every derived light in scene->lights and every mesh still
// marked. Nothing collected on it because the only way in was a CLI flag, and
// every comparable Engine bool has a GUI checkbox.
//
// RECONCILE, not rebuild. A panel whose mesh survives keeps the same Light
// object, so anything set on it -- a scene file's light_overrides entry above
// all -- persists across a graph change. A rebuild would silently drop that.
//
// Per frame it is one graph walk plus one matrix-vector product per panel. The
// plane fit and the texture mean are both cached, so neither is on that path.
// (This used to say "one matrix-vector product per panel" alone, which was three
// full graph walks short of the truth.)
//
// Safe to call at load as well as per frame, and the app SHOULD, because
// light_overrides resolves names once at init and can only name a light that
// already exists. Forgetting costs the override, not the lights.
int scene_build_emissive_lights(struct Scene* scene, bool enabled);

// Report what a panel would be for every mesh in the graph, and why any
// candidate was rejected, on stdout in the --water-fft-probe idiom.
//
// The instrument exists BEFORE anything shades from a derived panel, because a
// wrong fit hides inside a plausible image: a panel half a metre off or sqrt(2)
// too wide still lights the room, and the frame gives no way to tell. It reads
// the same three functions the reconcile does, so it cannot drift from what
// actually ships.
void emissive_lights_probe(const struct Scene* scene);

#endif // _EMISSIVE_LIGHT_H_
