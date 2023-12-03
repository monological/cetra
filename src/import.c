#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include <assimp/scene.h>
#include <assimp/light.h>
#include <assimp/material.h>
#include <GL/glew.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "scene.h"
#include "mesh.h"
#include "light.h"
#include "camera.h"
#include "util.h"
#include "texture.h"


Material* process_ai_material(struct aiMaterial* ai_mat, const char* directory) {
    Material* material = create_material();

    struct aiColor4D color;
    float floatVal;
    struct aiString str;

      // Load Albedo/Base Color
    if (AI_SUCCESS == aiGetMaterialColor(ai_mat, AI_MATKEY_COLOR_DIFFUSE, &color)) {
        material->albedo[0] = color.r;
        material->albedo[1] = color.g;
        material->albedo[2] = color.b;
    }else{
        material->albedo[0] = 1.0;
        material->albedo[1] = 0.2;
        material->albedo[2] = 0.2;
    }

    /*
    if (AI_SUCCESS == aiGetMaterialFloat(ai_mat, AI_MATKEY_METALLIC_FACTOR, &floatVal)) {
        material->metallic = floatVal;
    }else{
        material->metallic = 0.5;
    }
    if (AI_SUCCESS == aiGetMaterialFloat(ai_mat, AI_MATKEY_ROUGHNESS_FACTOR, &floatVal)) {
        material->roughness = floatVal;
    }else{
        material->roughness = 0.5;
    }*/
        material->metallic = 0.5;
        material->roughness = 0.5;

         // Load Albedo/Base Color Texture
    if (AI_SUCCESS == aiGetMaterialTexture(ai_mat, aiTextureType_DIFFUSE, 0, &str, NULL, NULL, NULL, NULL, NULL, NULL)) {
        material->albedoTex = load_texture(str.data, directory);
        printf("Diffuse texture loaded: %s\n", str.data);
    }

    // Load Normal Texture
    if (AI_SUCCESS == aiGetMaterialTexture(ai_mat, aiTextureType_NORMALS, 0, &str, NULL, NULL, NULL, NULL, NULL, NULL)) {
        material->normalTex = load_texture(str.data, directory);
        printf("Normals texture loaded: %s\n", str.data);
    }

    // Load Metalness Texture
    if (AI_SUCCESS == aiGetMaterialTexture(ai_mat, aiTextureType_METALNESS, 0, &str, NULL, NULL, NULL, NULL, NULL, NULL)) {
        material->metalnessTex = load_texture(str.data, directory);
        printf("Metalness texture loaded: %s\n", str.data);
    }

    // Load Diffuse Roughness Texture
    if (AI_SUCCESS == aiGetMaterialTexture(ai_mat, aiTextureType_DIFFUSE_ROUGHNESS, 0, &str, NULL, NULL, NULL, NULL, NULL, NULL)) {
        material->roughnessTex = load_texture(str.data, directory);
        printf("Diffuse Roughness texture loaded: %s\n", str.data);
    }

    // Load Ambient Occlusion Texture
    if (AI_SUCCESS == aiGetMaterialTexture(ai_mat, aiTextureType_AMBIENT_OCCLUSION, 0, &str, NULL, NULL, NULL, NULL, NULL, NULL)) {
        material->ambientOcclusionTex = load_texture(str.data, directory);
        printf("Ambient Occlusion texture loaded: %s\n", str.data);
    }

    // Load Emissive Texture
    if (AI_SUCCESS == aiGetMaterialTexture(ai_mat, aiTextureType_EMISSIVE, 0, &str, NULL, NULL, NULL, NULL, NULL, NULL)) {
        material->emissiveTex = load_texture(str.data, directory);
        printf("Emissive texture loaded: %s\n", str.data);
    }

    // Load Height Texture
    if (AI_SUCCESS == aiGetMaterialTexture(ai_mat, aiTextureType_HEIGHT, 0, &str, NULL, NULL, NULL, NULL, NULL, NULL)) {
        material->heightTex = load_texture(str.data, directory);
        printf("Height texture loaded: %s\n", str.data);
    }

    // Load Opacity Texture
    if (AI_SUCCESS == aiGetMaterialTexture(ai_mat, aiTextureType_OPACITY, 0, &str, NULL, NULL, NULL, NULL, NULL, NULL)) {
        material->opacityTex = load_texture(str.data, directory);
        printf("Opacity texture loaded: %s\n", str.data);
    }

    // Load Sheen Texture
    if (AI_SUCCESS == aiGetMaterialTexture(ai_mat, aiTextureType_SHEEN, 0, &str, NULL, NULL, NULL, NULL, NULL, NULL)) {
        material->sheenTex = load_texture(str.data, directory);
        printf("Sheen texture loaded: %s\n", str.data);
    }

    // Load Reflectance Texture
    if (AI_SUCCESS == aiGetMaterialTexture(ai_mat, aiTextureType_REFLECTION, 0, &str, NULL, NULL, NULL, NULL, NULL, NULL)) {
        material->reflectanceTex = load_texture(str.data, directory);
        printf("Reflectance texture loaded: %s\n", str.data);
    }

    return material;
}


void process_ai_mesh(Mesh* mesh, struct aiMesh* ai_mesh) {
    mesh->vertexCount = ai_mesh->mNumVertices;
    mesh->indexCount = ai_mesh->mNumFaces * 3; // Assuming the mesh is triangulated

    // Allocate memory for vertices and normals
    mesh->vertices = malloc(mesh->vertexCount * 3 * sizeof(float));
    mesh->normals = malloc(mesh->vertexCount * 3 * sizeof(float));

    // Check for texture coordinates
    if (ai_mesh->mTextureCoords[0]) {
        mesh->texCoords = malloc(mesh->vertexCount * 2 * sizeof(float));
    } else {
        mesh->texCoords = NULL;
    }

    // Allocate memory for indices
    mesh->indices = malloc(mesh->indexCount * sizeof(unsigned int));

    // Process vertices and normals
    for (unsigned int i = 0; i < mesh->vertexCount; i++) {
        mesh->vertices[i * 3] = ai_mesh->mVertices[i].x;
        mesh->vertices[i * 3 + 1] = ai_mesh->mVertices[i].y;
        mesh->vertices[i * 3 + 2] = ai_mesh->mVertices[i].z;

        mesh->normals[i * 3] = ai_mesh->mNormals[i].x;
        mesh->normals[i * 3 + 1] = ai_mesh->mNormals[i].y;
        mesh->normals[i * 3 + 2] = ai_mesh->mNormals[i].z;

        if (mesh->texCoords) {
            mesh->texCoords[i * 2] = ai_mesh->mTextureCoords[0][i].x;
            mesh->texCoords[i * 2 + 1] = ai_mesh->mTextureCoords[0][i].y;
        }
    }

    // Process indices
    for (unsigned int i = 0; i < ai_mesh->mNumFaces; i++) {
        struct aiFace face = ai_mesh->mFaces[i];
        for (unsigned int j = 0; j < face.mNumIndices; j++) {
            mesh->indices[i * 3 + j] = face.mIndices[j];
        }
    }
}

void process_ai_lights(const struct aiScene* scene, Light ***lights, size_t *num_lights) {
    *num_lights = scene->mNumLights;
    *lights = malloc(sizeof(Light*) * (*num_lights));

    for (unsigned int i = 0; i < scene->mNumLights; i++) {
        const struct aiLight* ai_light = scene->mLights[i];
        Light *light = create_light();
        light->name = strdup(ai_light->mName.data);

        light->name = strdup(ai_light->mName.data);
        glm_vec3_copy((vec3){ai_light->mPosition.x, ai_light->mPosition.y, ai_light->mPosition.z}, light->position);
        glm_vec3_copy((vec3){ai_light->mDirection.x, ai_light->mDirection.y, ai_light->mDirection.z}, light->direction);
        glm_vec3_copy((vec3){ai_light->mColorDiffuse.r, ai_light->mColorDiffuse.g, ai_light->mColorDiffuse.b}, light->color);
        glm_vec3_copy((vec3){ai_light->mColorSpecular.r, ai_light->mColorSpecular.g, ai_light->mColorSpecular.b}, light->specular);
        glm_vec3_copy((vec3){ai_light->mColorAmbient.r, ai_light->mColorAmbient.g, ai_light->mColorAmbient.b}, light->ambient);

        // Set intensity, attenuation, and cutoff based on light type
        switch (ai_light->mType) {
            default:
            case aiLightSource_DIRECTIONAL:
                light->type = LIGHT_DIRECTIONAL;
                light->intensity = 1.0f; // Default intensity for directional light
                break;
            case aiLightSource_POINT:
                light->type = LIGHT_POINT;
                light->constant = ai_light->mAttenuationConstant;
                light->linear = ai_light->mAttenuationLinear;
                light->quadratic = ai_light->mAttenuationQuadratic;
                break;
            case aiLightSource_SPOT:
                light->type = LIGHT_SPOT;
                light->constant = ai_light->mAttenuationConstant;
                light->linear = ai_light->mAttenuationLinear;
                light->quadratic = ai_light->mAttenuationQuadratic;
                light->cutOff = ai_light->mAngleInnerCone;
                light->outerCutOff = ai_light->mAngleOuterCone;
                break;
        }

        (*lights)[i] = light;
    }
}

void process_ai_cameras(const struct aiScene* scene, Camera ***cameras, size_t *num_cameras) {
    *num_cameras = scene->mNumCameras;
    *cameras = malloc(sizeof(Camera*) * (*num_cameras));

    if (!(*cameras)) {
        fprintf(stderr, "Failed to allocate memory for cameras\n");
        return;
    }

    for (unsigned int i = 0; i < *num_cameras; i++) {
        const struct aiCamera* ai_camera = scene->mCameras[i];
        Camera *camera = malloc(sizeof(Camera));
        camera->name = strdup(ai_camera->mName.data);

        if (!camera) {
            fprintf(stderr, "Failed to allocate memory for camera\n");
            continue;
        }

        glm_vec3_copy((vec3){ai_camera->mPosition.x, ai_camera->mPosition.y, ai_camera->mPosition.z}, camera->position);
        glm_vec3_copy((vec3){ai_camera->mUp.x, ai_camera->mUp.y, ai_camera->mUp.z}, camera->up_vector);
        glm_vec3_copy((vec3){ai_camera->mLookAt.x, ai_camera->mLookAt.y, ai_camera->mLookAt.z}, camera->look_at);

        camera->fov_radians = ai_camera->mHorizontalFOV;
        camera->aspect_ratio = ai_camera->mAspect; // You might need to calculate this differently
        camera->near_clip = ai_camera->mClipPlaneNear;
        camera->far_clip = ai_camera->mClipPlaneFar;
        camera->horizontal_fov = ai_camera->mHorizontalFOV;

        (*cameras)[i] = camera;
    }
}

void associate_cameras_and_lights_with_nodes(SceneNode* node, Scene* scene) {
    if (node->name) {
        node->camera = find_camera_by_name(scene, node->name);
        node->light = find_light_by_name(scene, node->name);
    }
    for (size_t i = 0; i < node->children_count; ++i) {
        associate_cameras_and_lights_with_nodes(node->children[i], scene);
    }
}

void copy_aiMatrix_to_mat4(const struct aiMatrix4x4 *from, mat4 to) {
    to[0][0] = from->a1; to[1][0] = from->a2; to[2][0] = from->a3; to[3][0] = from->a4;
    to[0][1] = from->b1; to[1][1] = from->b2; to[2][1] = from->b3; to[3][1] = from->b4;
    to[0][2] = from->c1; to[1][2] = from->c2; to[2][2] = from->c3; to[3][2] = from->c4;
    to[0][3] = from->d1; to[1][3] = from->d2; to[2][3] = from->d3; to[3][3] = from->d4;
}

SceneNode* process_ai_node(struct aiNode* ai_node, const struct aiScene* ai_scene, const char *texture_directory) {
    assert(ai_node != NULL);
    assert(ai_scene != NULL);
    assert(texture_directory != NULL);

    SceneNode* node = create_node();
    if (!node) {
        return NULL;
    }

    // Process meshes for this node
    node->mesh_count = ai_node->mNumMeshes;
    node->meshes = malloc(sizeof(Mesh*) * node->mesh_count);

    for (unsigned int i = 0; i < node->mesh_count; i++) {
        unsigned int meshIndex = ai_node->mMeshes[i];
        Mesh *mesh = create_mesh();
        process_ai_mesh(mesh, ai_scene->mMeshes[meshIndex]);
        if (ai_scene->mMeshes[meshIndex]->mMaterialIndex >= 0) {
            unsigned int matIndex = ai_scene->mMeshes[meshIndex]->mMaterialIndex;
            mesh->material = process_ai_material(ai_scene->mMaterials[matIndex], texture_directory);
        }
        node->meshes[i] = mesh;
    }

    // Recursively process children nodes
    node->children_count = ai_node->mNumChildren;
    node->children = malloc(sizeof(SceneNode*) * node->children_count);
    for (unsigned int i = 0; i < node->children_count; i++) {
        node->children[i] = process_ai_node(ai_node->mChildren[i], ai_scene, texture_directory);
        node->children[i]->parent = node; // Set parent
    }

    node->name = strdup(ai_node->mName.data);

    struct aiMatrix4x4 ai_mat = ai_node->mTransformation;
    copy_aiMatrix_to_mat4(&ai_mat, node->original_transform);

    return node;
}

Scene* import_fbx(const char* path, const char* texture_directory) {
    const struct aiScene* ai_scene = aiImportFile(path, aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_CalcTangentSpace);
    if (!ai_scene || ai_scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !ai_scene->mRootNode) {
        fprintf(stderr, "Error importing FBX file: %s\n", path);
        return NULL;
    }

    Scene* scene = create_scene();
    if (!scene) {
        aiReleaseImport(ai_scene);
        return NULL;
    }

    set_scene_texture_directory(scene, texture_directory);

    // Process lights and cameras
    process_ai_lights(ai_scene, &scene->lights, &scene->light_count);
    process_ai_cameras(ai_scene, &scene->cameras, &scene->camera_count);

    // Process the root node
    scene->root_node = process_ai_node(ai_scene->mRootNode, ai_scene, scene->texture_directory);

    associate_cameras_and_lights_with_nodes(scene->root_node, scene);

    aiReleaseImport(ai_scene);
    return scene;
}

