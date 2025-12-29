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

Material* process_ai_material(struct aiMaterial* ai_mat, TexturePool* tex_pool,
                              const struct aiScene* ai_scene);

// Async variant - textures loaded in parallel, set via callbacks
Material* process_ai_material_async(struct aiMaterial* ai_mat, TexturePool* tex_pool,
                                    const struct aiScene* ai_scene, struct AsyncLoader* loader);

void process_ai_mesh(Mesh* mesh, struct aiMesh* ai_mesh);

void process_ai_lights(const struct aiScene* scene, Light*** lights, uint32_t* num_lights);

void process_ai_cameras(const struct aiScene* scene, Camera*** cameras, uint32_t* num_cameras);

Scene* create_scene_from_model_path(const char* path, const char* texture_directory);

// Async variant - textures loaded in parallel
Scene* create_scene_from_model_path_async(const char* path, const char* texture_directory,
                                          struct AsyncLoader* loader);

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
