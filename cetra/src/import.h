#ifndef IMPORT_H
#define IMPORT_H

#include <string.h>
#include <stdio.h>

#include <GL/glew.h>
#include <assimp/scene.h>

#include "mesh.h"
#include "light.h"
#include "camera.h"
#include "scene.h"
#include "texture.h"

// Forward declaration
struct AsyncLoader;

Material* process_ai_material(struct aiMaterial* ai_mat, TexturePool* tex_pool);

// Async variant - textures loaded in parallel, set via callbacks
Material* process_ai_material_async(struct aiMaterial* ai_mat, TexturePool* tex_pool,
                                    struct AsyncLoader* loader);

void process_ai_mesh(Mesh* mesh, struct aiMesh* ai_mesh);

void process_ai_lights(const struct aiScene* scene, Light*** lights, uint32_t* num_lights);

void process_ai_cameras(const struct aiScene* scene, Camera*** cameras, uint32_t* num_cameras);

Scene* create_scene_from_fbx_path(const char* path, const char* texture_directory);

// Async variant - textures loaded in parallel
Scene* create_scene_from_fbx_path_async(const char* path, const char* texture_directory,
                                        struct AsyncLoader* loader);

#endif // IMPORT_H
