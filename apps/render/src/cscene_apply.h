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

// Build the scene's ambient dust particle system (if the .cscn declares a dust
// block), sized to the scene bounds. Replaces the old hardcoded filename gate.
void apply_cscene_dust(struct Engine* engine, Scene* scene, const CetraSceneDesc* cscn, vec3 center,
                       float radius);

#endif // CSCENE_APPLY_H
