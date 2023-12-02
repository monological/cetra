#ifndef IMPORT_H
#define IMPORT_H

#include <GL/glew.h>
#include <assimp/scene.h>
#include <string.h>
#include <stdio.h>

#include "mesh.h"
#include "light.h"
#include "camera.h"
#include "scene.h"

// Function prototypes

GLuint load_gl_texture(const char* path, const char* directory);

// Processes an individual Assimp mesh and converts it into your Mesh structure
void process_ai_mesh(Mesh* mesh, struct aiMesh* ai_mesh);

// Processes an Assimp material and converts it into your Material structure
Material* process_ai_material(struct aiMaterial* ai_mat, const char* directory);

// Processes all lights in an Assimp scene and converts them into your Light structures
void process_ai_lights(const struct aiScene* scene, Light ***lights, uint32_t *num_lights);

// Processes all cameras in an Assimp scene and converts them into your Camera structures
void process_ai_cameras(const struct aiScene* scene, Camera ***cameras, uint32_t *num_cameras);

// Main function to import an FBX file and create a Scene structure
Scene* import_fbx(const char* path, const char* textureDirectory);

#endif // IMPORT_H

