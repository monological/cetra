#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include <assimp/scene.h>
#include <assimp/light.h>
#include <assimp/material.h>
#include <assimp/cimport.h>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <GL/glew.h>

#define STB_IMAGE_IMPLEMENTATION
#include "ext/stb_image.h"
#include "ext/log.h"

#include "scene.h"
#include "mesh.h"
#include "light.h"
#include "camera.h"
#include "util.h"
#include "texture.h"
#include "material.h"
#include "async_loader.h"

/*
 * Texture mapping table for material loading
 */
typedef struct TextureMapping {
    enum aiTextureType ai_type;
    void (*setter)(Material*, Texture*);
    const char* name;
} TextureMapping;

static const TextureMapping texture_mappings[] = {
    {aiTextureType_DIFFUSE, set_material_albedo_tex, "Diffuse"},
    {aiTextureType_NORMALS, set_material_normal_tex, "Normal"},
    {aiTextureType_METALNESS, set_material_metalness_tex, "Metalness"},
    {aiTextureType_DIFFUSE_ROUGHNESS, set_material_roughness_tex, "Roughness"},
    {aiTextureType_AMBIENT_OCCLUSION, set_material_ambient_occlusion_tex, "AO"},
    {aiTextureType_EMISSIVE, set_material_emissive_tex, "Emissive"},
    {aiTextureType_HEIGHT, set_material_height_tex, "Height"},
    {aiTextureType_OPACITY, set_material_opacity_tex, "Opacity"},
    {aiTextureType_SHEEN, set_material_sheen_tex, "Sheen"},
    {aiTextureType_REFLECTION, set_material_reflectance_tex, "Reflectance"},
};

static const size_t texture_mapping_count = sizeof(texture_mappings) / sizeof(texture_mappings[0]);

/*
 * Extract material properties (color, metallic, roughness) from assimp material
 */
static void extract_material_properties(struct aiMaterial* ai_mat, Material* material) {
    struct aiColor4D color;

    if (AI_SUCCESS == aiGetMaterialColor(ai_mat, AI_MATKEY_COLOR_DIFFUSE, &color)) {
        material->albedo[0] = color.r;
        material->albedo[1] = color.g;
        material->albedo[2] = color.b;
    } else {
        material->albedo[0] = 0.1;
        material->albedo[1] = 0.1;
        material->albedo[2] = 0.1;
    }

    ai_real metallic, roughness;

    if (AI_SUCCESS == aiGetMaterialFloat(ai_mat, AI_MATKEY_METALLIC_FACTOR, &metallic)) {
        material->metallic = metallic;
    } else {
        material->metallic = 0.5;
    }

    if (AI_SUCCESS == aiGetMaterialFloat(ai_mat, AI_MATKEY_ROUGHNESS_FACTOR, &roughness)) {
        material->roughness = roughness;
    } else {
        material->roughness = 0.5;
    }
}

Material* process_ai_material(struct aiMaterial* ai_mat, TexturePool* tex_pool) {
    if (!ai_mat || !tex_pool)
        return NULL;

    Material* material = create_material();
    extract_material_properties(ai_mat, material);

    struct aiString str;

    for (size_t i = 0; i < texture_mapping_count; i++) {
        const TextureMapping* mapping = &texture_mappings[i];
        if (AI_SUCCESS == aiGetMaterialTexture(ai_mat, mapping->ai_type, 0, &str, NULL, NULL, NULL,
                                               NULL, NULL, NULL)) {
            Texture* tex = load_texture_path_into_pool(tex_pool, str.data);
            if (tex) {
                mapping->setter(material, tex);
                log_info("%s texture loaded: %s", mapping->name, tex->filepath);
            } else {
                log_warn("Failed to load %s texture '%s'", mapping->name, str.data);
            }
        }
    }

    return material;
}

/*
 * Async texture load callback context
 */
typedef struct AsyncTexCallback {
    Material* material;
    void (*setter)(Material*, Texture*);
    const char* tex_type;
} AsyncTexCallback;

static void async_tex_callback(Texture* tex, void* user_data) {
    AsyncTexCallback* ctx = (AsyncTexCallback*)user_data;
    if (tex && ctx->material && ctx->setter) {
        ctx->setter(ctx->material, tex);
        log_info("%s texture loaded async: %s", ctx->tex_type, tex->filepath);
    }
    free(ctx);
}

static void load_material_texture_async(AsyncLoader* loader, TexturePool* tex_pool, Material* mat,
                                        const char* filepath, void (*setter)(Material*, Texture*),
                                        const char* tex_type) {
    AsyncTexCallback* ctx = malloc(sizeof(AsyncTexCallback));
    if (!ctx) {
        log_error("Failed to allocate AsyncTexCallback");
        return;
    }
    ctx->material = mat;
    ctx->setter = setter;
    ctx->tex_type = tex_type;
    load_texture_async(loader, tex_pool, filepath, async_tex_callback, ctx);
}

Material* process_ai_material_async(struct aiMaterial* ai_mat, TexturePool* tex_pool,
                                    AsyncLoader* loader) {
    if (!ai_mat || !tex_pool || !loader) {
        return NULL;
    }

    Material* material = create_material();
    extract_material_properties(ai_mat, material);

    struct aiString str;

    for (size_t i = 0; i < texture_mapping_count; i++) {
        const TextureMapping* mapping = &texture_mappings[i];
        if (AI_SUCCESS == aiGetMaterialTexture(ai_mat, mapping->ai_type, 0, &str, NULL, NULL, NULL,
                                               NULL, NULL, NULL)) {
            load_material_texture_async(loader, tex_pool, material, str.data, mapping->setter,
                                        mapping->name);
        }
    }

    return material;
}

void process_ai_mesh(Mesh* mesh, struct aiMesh* ai_mesh) {
    mesh->vertex_count = ai_mesh->mNumVertices;
    mesh->index_count = ai_mesh->mNumFaces * 3; // Assuming the mesh is triangulated

    // Allocate memory for vertices and normals
    mesh->vertices = malloc(mesh->vertex_count * 3 * sizeof(float));
    mesh->normals = malloc(mesh->vertex_count * 3 * sizeof(float));

    if (ai_mesh->mTangents && ai_mesh->mBitangents) {
        mesh->tangents = malloc(mesh->vertex_count * 3 * sizeof(float));
        mesh->bitangents = malloc(mesh->vertex_count * 3 * sizeof(float));
    } else {
        mesh->tangents = NULL;
        mesh->bitangents = NULL;
    }

    // Check for texture coordinates
    if (ai_mesh->mTextureCoords[0]) {
        mesh->tex_coords = malloc(mesh->vertex_count * 2 * sizeof(float));
    } else {
        mesh->tex_coords = NULL;
    }

    // Allocate memory for indices
    mesh->indices = malloc(mesh->index_count * sizeof(unsigned int));

    // Process vertices and normals
    for (unsigned int i = 0; i < mesh->vertex_count; i++) {
        mesh->vertices[i * 3] = ai_mesh->mVertices[i].x;
        mesh->vertices[i * 3 + 1] = ai_mesh->mVertices[i].y;
        mesh->vertices[i * 3 + 2] = ai_mesh->mVertices[i].z;

        mesh->normals[i * 3] = ai_mesh->mNormals[i].x;
        mesh->normals[i * 3 + 1] = ai_mesh->mNormals[i].y;
        mesh->normals[i * 3 + 2] = ai_mesh->mNormals[i].z;

        if (mesh->tangents) {
            // Tangents
            mesh->tangents[i * 3] = ai_mesh->mTangents[i].x;
            mesh->tangents[i * 3 + 1] = ai_mesh->mTangents[i].y;
            mesh->tangents[i * 3 + 2] = ai_mesh->mTangents[i].z;
        }

        if (mesh->bitangents) {
            // Bitangents
            mesh->bitangents[i * 3] = ai_mesh->mBitangents[i].x;
            mesh->bitangents[i * 3 + 1] = ai_mesh->mBitangents[i].y;
            mesh->bitangents[i * 3 + 2] = ai_mesh->mBitangents[i].z;
        }

        if (mesh->tex_coords) {
            mesh->tex_coords[i * 2] = ai_mesh->mTextureCoords[0][i].x;
            mesh->tex_coords[i * 2 + 1] = ai_mesh->mTextureCoords[0][i].y;
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

void process_ai_lights(const struct aiScene* scene, Light*** lights, size_t* num_lights) {
    *num_lights = scene->mNumLights;
    *lights = malloc(sizeof(Light*) * (*num_lights));

    for (unsigned int i = 0; i < scene->mNumLights; i++) {
        const struct aiLight* ai_light = scene->mLights[i];
        Light* light = create_light();
        light->name = safe_strdup(ai_light->mName.data);

        glm_vec3_copy((vec3){ai_light->mPosition.x, ai_light->mPosition.y, ai_light->mPosition.z},
                      light->original_position);
        glm_vec3_copy((vec3){ai_light->mPosition.x, ai_light->mPosition.y, ai_light->mPosition.z},
                      light->global_position);
        glm_vec3_copy(
            (vec3){ai_light->mDirection.x, ai_light->mDirection.y, ai_light->mDirection.z},
            light->direction);
        glm_vec3_copy(
            (vec3){ai_light->mColorAmbient.r, ai_light->mColorAmbient.g, ai_light->mColorAmbient.b},
            light->ambient);
        glm_vec3_copy(
            (vec3){ai_light->mColorDiffuse.r, ai_light->mColorDiffuse.g, ai_light->mColorDiffuse.b},
            light->color);
        glm_vec3_copy((vec3){ai_light->mColorSpecular.r, ai_light->mColorSpecular.g,
                             ai_light->mColorSpecular.b},
                      light->specular);

        // Set intensity, attenuation, and cutoff based on light type
        switch (ai_light->mType) {
            case aiLightSource_DIRECTIONAL:
                light->type = LIGHT_DIRECTIONAL;
                light->intensity = 1.0f;
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
            default:
                light->type = LIGHT_AREA;
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

void process_ai_cameras(const struct aiScene* scene, Camera*** cameras, size_t* num_cameras) {
    *num_cameras = scene->mNumCameras;
    *cameras = malloc(sizeof(Camera*) * (*num_cameras));

    if (!(*cameras)) {
        log_error("Failed to allocate memory for cameras\n");
        return;
    }

    for (unsigned int i = 0; i < *num_cameras; i++) {
        const struct aiCamera* ai_camera = scene->mCameras[i];
        Camera* camera = malloc(sizeof(Camera));
        camera->name = safe_strdup(ai_camera->mName.data);

        if (!camera) {
            log_error("Failed to allocate memory for camera\n");
            continue;
        }

        glm_vec3_copy(
            (vec3){ai_camera->mPosition.x, ai_camera->mPosition.y, ai_camera->mPosition.z},
            camera->position);
        glm_vec3_copy((vec3){ai_camera->mUp.x, ai_camera->mUp.y, ai_camera->mUp.z},
                      camera->up_vector);
        glm_vec3_copy((vec3){ai_camera->mLookAt.x, ai_camera->mLookAt.y, ai_camera->mLookAt.z},
                      camera->look_at);

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

void copy_aiMatrix_to_mat4(const struct aiMatrix4x4* from, mat4 to) {
    to[0][0] = from->a1;
    to[1][0] = from->a2;
    to[2][0] = from->a3;
    to[3][0] = from->a4;
    to[0][1] = from->b1;
    to[1][1] = from->b2;
    to[2][1] = from->b3;
    to[3][1] = from->b4;
    to[0][2] = from->c1;
    to[1][2] = from->c2;
    to[2][2] = from->c3;
    to[3][2] = from->c4;
    to[0][3] = from->d1;
    to[1][3] = from->d2;
    to[2][3] = from->d3;
    to[3][3] = from->d4;
}

SceneNode* process_ai_node(Scene* scene, struct aiNode* ai_node, const struct aiScene* ai_scene,
                           TexturePool* tex_pool) {
    if (!scene || !ai_node || !ai_scene || !tex_pool)
        return NULL;

    SceneNode* node = create_node();
    if (!node) {
        return NULL;
    }

    // Process meshes for this node
    node->mesh_count = ai_node->mNumMeshes;
    node->meshes = malloc(sizeof(Mesh*) * node->mesh_count);

    for (unsigned int i = 0; i < node->mesh_count; i++) {
        unsigned int meshIndex = ai_node->mMeshes[i];
        Mesh* mesh = create_mesh();
        process_ai_mesh(mesh, ai_scene->mMeshes[meshIndex]);
        if (ai_scene->mMeshes[meshIndex]->mMaterialIndex >= 0) {
            unsigned int matIndex = ai_scene->mMeshes[meshIndex]->mMaterialIndex;
            mesh->material = process_ai_material(ai_scene->mMaterials[matIndex], tex_pool);

            // scene will manage the material
            add_material_to_scene(scene, mesh->material);
        }

        calculate_aabb(mesh);

        node->meshes[i] = mesh;
    }

    // Recursively process children nodes
    node->children_count = ai_node->mNumChildren;
    node->children = malloc(sizeof(SceneNode*) * node->children_count);
    for (unsigned int i = 0; i < node->children_count; i++) {
        node->children[i] = process_ai_node(scene, ai_node->mChildren[i], ai_scene, tex_pool);
        node->children[i]->parent = node; // Set parent
    }

    node->name = safe_strdup(ai_node->mName.data);

    struct aiMatrix4x4 ai_mat = ai_node->mTransformation;
    copy_aiMatrix_to_mat4(&ai_mat, node->original_transform);

    return node;
}

Scene* create_scene_from_fbx_path(const char* path, const char* texture_directory) {
    const struct aiScene* ai_scene =
        aiImportFile(path, aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_CalcTangentSpace);
    if (!ai_scene || ai_scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !ai_scene->mRootNode) {
        log_error("Error importing FBX file: %s\n", path);
        return NULL;
    }

    Scene* scene = create_scene();
    if (!scene) {
        aiReleaseImport(ai_scene);
        return NULL;
    }

    TexturePool* tex_pool = scene->tex_pool;
    if (!tex_pool) {
        log_error("Failed to create texture pool\n");
        aiReleaseImport(ai_scene);
        return NULL;
    }

    set_texture_pool_directory(tex_pool, texture_directory);

    // Process lights and cameras
    process_ai_lights(ai_scene, &scene->lights, &scene->light_count);
    process_ai_cameras(ai_scene, &scene->cameras, &scene->camera_count);

    // Process the root node
    scene->root_node = process_ai_node(scene, ai_scene->mRootNode, ai_scene, tex_pool);

    associate_cameras_and_lights_with_nodes(scene->root_node, scene);

    aiReleaseImport(ai_scene);
    return scene;
}

/*
 * Async variant of process_ai_node - uses async texture loading
 */
static SceneNode* process_ai_node_async(Scene* scene, struct aiNode* ai_node,
                                        const struct aiScene* ai_scene, TexturePool* tex_pool,
                                        AsyncLoader* loader) {
    if (!scene || !ai_node || !ai_scene || !tex_pool || !loader) {
        return NULL;
    }

    SceneNode* node = create_node();
    if (!node) {
        return NULL;
    }

    // Process meshes for this node
    node->mesh_count = ai_node->mNumMeshes;
    node->meshes = malloc(sizeof(Mesh*) * node->mesh_count);

    for (unsigned int i = 0; i < node->mesh_count; i++) {
        unsigned int meshIndex = ai_node->mMeshes[i];
        Mesh* mesh = create_mesh();
        process_ai_mesh(mesh, ai_scene->mMeshes[meshIndex]);
        if (ai_scene->mMeshes[meshIndex]->mMaterialIndex >= 0) {
            unsigned int matIndex = ai_scene->mMeshes[meshIndex]->mMaterialIndex;
            // Use async material processing
            mesh->material =
                process_ai_material_async(ai_scene->mMaterials[matIndex], tex_pool, loader);
            add_material_to_scene(scene, mesh->material);
        }

        calculate_aabb(mesh);
        node->meshes[i] = mesh;
    }

    // Recursively process children nodes
    node->children_count = ai_node->mNumChildren;
    node->children = malloc(sizeof(SceneNode*) * node->children_count);
    for (unsigned int i = 0; i < node->children_count; i++) {
        node->children[i] =
            process_ai_node_async(scene, ai_node->mChildren[i], ai_scene, tex_pool, loader);
        node->children[i]->parent = node;
    }

    node->name = safe_strdup(ai_node->mName.data);

    struct aiMatrix4x4 ai_mat = ai_node->mTransformation;
    copy_aiMatrix_to_mat4(&ai_mat, node->original_transform);

    return node;
}

Scene* create_scene_from_fbx_path_async(const char* path, const char* texture_directory,
                                        AsyncLoader* loader) {
    if (!loader) {
        log_error("AsyncLoader is NULL, falling back to sync loading");
        return create_scene_from_fbx_path(path, texture_directory);
    }

    const struct aiScene* ai_scene =
        aiImportFile(path, aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_CalcTangentSpace);
    if (!ai_scene || ai_scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !ai_scene->mRootNode) {
        log_error("Error importing FBX file: %s\n", path);
        return NULL;
    }

    Scene* scene = create_scene();
    if (!scene) {
        aiReleaseImport(ai_scene);
        return NULL;
    }

    TexturePool* tex_pool = scene->tex_pool;
    if (!tex_pool) {
        log_error("Failed to create texture pool\n");
        aiReleaseImport(ai_scene);
        return NULL;
    }

    set_texture_pool_directory(tex_pool, texture_directory);

    // Process lights and cameras
    process_ai_lights(ai_scene, &scene->lights, &scene->light_count);
    process_ai_cameras(ai_scene, &scene->cameras, &scene->camera_count);

    // Process the root node with async texture loading
    scene->root_node =
        process_ai_node_async(scene, ai_scene->mRootNode, ai_scene, tex_pool, loader);

    associate_cameras_and_lights_with_nodes(scene->root_node, scene);

    aiReleaseImport(ai_scene);
    return scene;
}
