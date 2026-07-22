#ifndef IMPORT_H
#define IMPORT_H

#include <string.h>
#include <stdio.h>
#include <stdbool.h>

#include <GL/glew.h>
#include <assimp/scene.h>

#include "mesh.h"
#include "light.h"
#include "camera.h"
#include "scene.h"
#include "texture.h"
#include "animation.h"

// Forward declaration
struct AsyncLoader;

void process_ai_mesh(Mesh* mesh, struct aiMesh* ai_mesh);

void process_ai_lights(const struct aiScene* scene, Light*** lights, uint32_t* num_lights,
                       bool photometric_units);

void process_ai_cameras(const struct aiScene* scene, Camera*** cameras, uint32_t* num_cameras);

// Import setting: override the UV V-flip. The default is per-format AUTO
// (glTF flips, FBX does not — each format's spec convention relative to this
// engine's texture upload); some bakes are authored against the opposite
// convention (symptom: scrambled/mirrored textures), so the application can
// pin the flip on or off before create_scene_from_model_path*. Note: pinning
// is one-way for the process — there is no API back to AUTO — so set it per
// run, not per asset.
void set_import_flip_uvs(bool flip);

// Load a model file into a Scene. Textures stream on the loader's worker pool
// and may still be decoding on return -- file paths and compressed embedded
// images alike (only raw embedded pixels, which are rare, decode inline);
// meshes and skeletons are ready. The loader is required; NULL returns NULL.
Scene* create_scene_from_model_path(const char* path, const char* texture_directory,
                                    struct AsyncLoader* loader);

// POM (§4.11): resolve "<name>_height" sibling maps into materials that have an
// albedo/normal texture but no height map yet (glTF carries no height slot).
// Idempotent + no-op unless a sibling is on disk. Import never calls it: the
// render loop does, once the async texture loader drains and the material's
// albedo/normal pointers have actually been attached.
void resolve_height_maps(Scene* scene);

// POM (§4.11): default depth auto-applied to a material whose height map is
// resolved by convention (glTF/FBX have no POM scale). --parallax-scale sets it;
// 0 leaves POM off even where a height map exists. Set before loading a model.
void set_parallax_default_scale(float scale);

// Load animations from a separate file (e.g., Mixamo "Without Skin" FBX)
// Maps animation channels to the provided skeleton by bone name
// If enable_retargeting is true, uses smart bone matching and computes rotation
// deltas to handle different skeleton rest poses (required for Mixamo -> custom rig)
// source_skeleton: optional skeleton providing source rest poses for proper delta computation
//                  (e.g., Mixamo T-pose skeleton). If NULL, uses target skeleton rest poses.
// Returns number of animations loaded, or -1 on error
int load_animations_from_file(Scene* scene, Skeleton* skeleton, const char* filepath,
                              bool enable_retargeting, Skeleton* source_skeleton);

#endif // IMPORT_H
