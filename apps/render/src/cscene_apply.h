#ifndef CSCENE_APPLY_H
#define CSCENE_APPLY_H

#include "cetra/cscene.h"
#include "cetra/scene.h"

#include "render_args.h"

struct Engine;

/*
 * App-side policy for cetra scene files (.cscn): merge the parsed description
 * into RenderArgs (CLI wins) and apply the scene-graph-dependent pieces at
 * main's hook points. Parsing lives in the engine (cetra/cscene.c); this
 * module owns what the values MEAN to the viewer.
 */

// Resolve the input (a .cscn itself, or one sitting next to a bare model)
// and merge its look into args, leaving CLI-given values untouched.
// Returns 0 (with *out_cscn possibly NULL when no scene file is involved)
// or -1 when a .cscn input is unreadable.
int cscene_setup(RenderArgs* args, CetraSceneDesc** out_cscn);

// Create the scene file's point lights (area-fill conversions) and attach
// them to the graph. Must run before the auto-key-light decision so they
// count as "model ships lights".
void apply_cscene_ambient(Scene* scene, const CetraSceneDesc* cscn);
void add_cscene_lights(Scene* scene, const CetraSceneDesc* cscn);

// Apply per-light overrides (penumbra from authored sun angle, intensity).
// Needs scene_radius, so it runs inside main's scene-scaled block, after
// the global light sizing pass.
void apply_cscene_light_overrides(Scene* scene, const CetraSceneDesc* cscn, float scene_radius);

// Create the scene's directional wind (if any) and opt matching materials into
// it (windResponse), deriving each cloth mesh's sway mask from its AABB. Runs
// after the scene graph and materials exist.
void apply_cscene_wind(Scene* scene, const CetraSceneDesc* cscn);

// Apply the scene file's material overrides onto matching materials by authored name: their
// parameters, textures, layers and roads, and their shaders (spec 13.29) -- a late-draw program,
// a surface hook from shaderHooks, and the params either reads. The engine owns the programs and
// the hooks. Subsurface is NOT applied here: it also has to register a scatter profile with
// PostFX, so it stays in configure_sss_materials where that engine handle is in scope.
void apply_cscene_material_overrides(struct Engine* engine, Scene* scene,
                                     const CetraSceneDesc* cscn);

// Compile each of post.passes (spec 13.29) and add it at its location. The engine owns the
// programs. A file that does not read or compile is reported by name and skipped, and the frame
// renders without it.
void apply_cscene_post_passes(struct Engine* engine, const CetraSceneDesc* cscn);

// Attach the scene file's water surface (if the .cscn declares a water block).
// Runs BEFORE the CLI water block, which overrides whatever it finds.
void apply_cscene_water(Scene* scene, const CetraSceneDesc* cscn);
// Attach the scene file's rain (spec 13.9). Runs BEFORE --rain / --no-rain, which
// override it.
void apply_cscene_rain(Scene* scene, const CetraSceneDesc* cscn);

// Attach the scene file's fires (spec 13.14), each bound to the light it drives by name -- so
// after the lights are created. --no-fire removes them after.
void apply_cscene_fire(Scene* scene, const CetraSceneDesc* cscn);
void apply_cscene_fog_volumes(Scene* scene, const CetraSceneDesc* cscn);
void apply_cscene_occluders(Scene* scene, const CetraSceneDesc* cscn);

// Attach the scene file's decals (spec 11.73), loading each image into the
// texture pool. Marks the material texture array dirty, since a decal image
// becomes one of its layers.
void apply_cscene_decals(Scene* scene, const CetraSceneDesc* cscn);

// Build the scene file's reflection probes (spec 11.70) and install them for the
// engine to capture. True when it took the scene's probes, which is what tells the
// caller to skip the single auto-placed probe --probe would otherwise install. Must
// run after the model recenter, since the probes are placed in world space.
bool apply_cscene_probes(Scene* scene, const CetraSceneDesc* cscn, int row0);

// GI volumes (spec 13.24), one grid per authored box. False when the file authored none.
bool apply_cscene_gi_volumes(Scene* scene, const CetraSceneDesc* cscn);

// Build the scene's ambient dust particle system (if the .cscn declares a dust
// block), sized to the scene bounds. Replaces the old hardcoded filename gate.
void apply_cscene_dust(struct Engine* engine, Scene* scene, const CetraSceneDesc* cscn, vec3 center,
                       float radius);

#endif // CSCENE_APPLY_H
