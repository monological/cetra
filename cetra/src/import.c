#include <string.h>
#include "compat.h" // strcasecmp/strncasecmp
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <ctype.h>

#include <assimp/scene.h>
#include <assimp/light.h>
#include <assimp/material.h>
#include <assimp/cimport.h>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/GltfMaterial.h>
#include <assimp/config.h>
#include <GL/glew.h>

#define STB_IMAGE_IMPLEMENTATION
#include "ext/stb_image.h"
#include "ext/log.h"

#include "animation.h"
#include "rigging.h"
#include "scene.h"
#include "mesh.h"
#include "light.h"
#include "camera.h"
#include "util.h"
#include "texture.h"
#include "material.h"
#include "async_loader.h"

// Forward declarations
static void copy_aiMatrix_to_mat4(const struct aiMatrix4x4* from, mat4 to);

// POM (§4.11): depth applied to a material when its height map is resolved by
// filename convention (glTF/FBX carry no POM scale). Auto-enabling POM wherever
// a height map exists is the "drop a _height.png next to the textures and it
// works" model; --parallax-scale overrides the default, --no-parallax disables
// the whole path. 0 leaves POM off even with a height map.
static float g_parallax_default_scale = 0.05f;
void set_parallax_default_scale(float scale) {
    g_parallax_default_scale = scale;
}

// Default the texture directory to the model file's own directory when the
// caller passes none, so a glTF/OBJ with EXTERNAL textures resolves its siblings
// (and the POM height convention) without an explicit texture dir. Returns
// texture_directory unchanged when set, else dirname(path) written into buf (or
// NULL when path has no directory component -- unchanged behaviour there).
static const char* effective_texture_dir(const char* path, const char* texture_directory, char* buf,
                                         size_t bufsize) {
    if (texture_directory || !path)
        return texture_directory;
    const char* slash = strrchr(path, '/');
    if (!slash || (size_t)(slash - path) >= bufsize)
        return NULL;
    snprintf(buf, bufsize, "%.*s", (int)(slash - path), path);
    return buf;
}

/*
 * UV V-flip policy. glTF specifies a top-left UV origin while FBX data
 * arrives bottom-left (verified against Blender: assimp's FBX UVs match
 * Blender's raw values exactly), so relative to this engine's texture upload
 * the V flip is needed for glTF and wrong for FBX. Default: decide per
 * format. Bakes authored against the opposite convention exist in the wild
 * (symptom: surfaces sample wrong atlas regions, labels mirror), so the
 * application can override per asset.
 */
typedef enum UVFlipMode { UV_FLIP_AUTO = -1, UV_FLIP_OFF = 0, UV_FLIP_ON = 1 } UVFlipMode;
static int import_flip_uvs = UV_FLIP_AUTO;

void set_import_flip_uvs(bool flip) {
    import_flip_uvs = flip ? UV_FLIP_ON : UV_FLIP_OFF;
}

static unsigned int uv_flip_flag(const char* path) {
    if (import_flip_uvs != UV_FLIP_AUTO)
        return import_flip_uvs == UV_FLIP_ON ? aiProcess_FlipUVs : 0u;
    const char* dot = strrchr(path, '.');
    if (dot && strcasecmp(dot, ".fbx") == 0)
        return 0u;
    return aiProcess_FlipUVs;
}

// glTF punctual lights use photometric units (directional in lux, point/spot
// in candela) per KHR_lights_punctual; every other format we import carries
// renderer-scale radiometric-ish intensities.
static bool is_gltf_path(const char* path) {
    const char* dot = path ? strrchr(path, '.') : NULL;
    return dot && (strcasecmp(dot, ".gltf") == 0 || strcasecmp(dot, ".glb") == 0);
}

// Ceiling for imported light intensity. Formats without spec-defined units
// (FBX "percent" scales, watt-baked Blender FBX exports) can arrive orders of
// magnitude above renderer scale (~1-10); anything past this bound would blow
// out every lit pixel regardless of exposure.
static const float IMPORTED_LIGHT_INTENSITY_MAX = 100.0f;

/*
 * Import a scene with FBX pivot preservation disabled. Assimp then collapses
 * the $AssimpFbx$ pseudo-node chains (Translation/PreRotation/Rotation/...)
 * and bakes pre/post rotations into node transforms and animation channels.
 * Without this, Mixamo animation channels only carry the animated rotation
 * curve (missing each joint's pre-rotation), which breaks retargeting.
 */
static const struct aiScene* import_ai_scene(const char* path, unsigned int flags) {
    struct aiPropertyStore* props = aiCreatePropertyStore();
    if (!props) {
        return aiImportFile(path, flags);
    }
    aiSetImportPropertyInteger(props, AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, 0);
    const struct aiScene* ai_scene = aiImportFileExWithProperties(path, flags, NULL, props);
    aiReleasePropertyStore(props);
    return ai_scene;
}

// Extract rotation quaternion from a mat4 transform
static void extract_rotation_quat(mat4 transform, versor out) {
    glm_mat4_quat(transform, out);
    glm_quat_normalize(out);
}

/*
 * Texture mapping table for material loading
 */
typedef struct TextureMapping {
    enum aiTextureType ai_type;
    void (*setter)(Material*, Texture*);
    const char* name;
    bool is_srgb; // Color data; everything else is linear (normals, ORM, ...)
} TextureMapping;

static const TextureMapping texture_mappings[] = {
    // Legacy/FBX texture types
    {aiTextureType_DIFFUSE, set_material_albedo_tex, "Diffuse", true},
    {aiTextureType_NORMALS, set_material_normal_tex, "Normal", false},
    {aiTextureType_METALNESS, set_material_metalness_tex, "Metalness", false},
    {aiTextureType_DIFFUSE_ROUGHNESS, set_material_roughness_tex, "Roughness", false},
    {aiTextureType_AMBIENT_OCCLUSION, set_material_ambient_occlusion_tex, "AO", false},
    {aiTextureType_EMISSIVE, set_material_emissive_tex, "Emissive", true},
    {aiTextureType_HEIGHT, set_material_height_tex, "Height", false},
    {aiTextureType_OPACITY, set_material_opacity_tex, "Opacity", false},
    {aiTextureType_SHEEN, set_material_sheen_tex, "Sheen", true}, // KHR sheen color (sRGB)
    {aiTextureType_REFLECTION, set_material_reflectance_tex, "Reflectance", false},
    // glTF/GLB-specific texture types
    {aiTextureType_BASE_COLOR, set_material_albedo_tex, "BaseColor", true},
    {aiTextureType_NORMAL_CAMERA, set_material_normal_tex, "NormalCamera", false},
    {aiTextureType_EMISSION_COLOR, set_material_emissive_tex, "EmissionColor", true},
};

static const size_t texture_mapping_count = sizeof(texture_mappings) / sizeof(texture_mappings[0]);

/*
 * Extract material properties from assimp material
 */
static void extract_material_properties(struct aiMaterial* ai_mat, Material* material) {
    struct aiColor4D color;

    // Authored material name -- scene files (.cscn) match overrides on it
    struct aiString mat_name;
    if (AI_SUCCESS == aiGetMaterialString(ai_mat, AI_MATKEY_NAME, &mat_name) &&
        mat_name.length > 0) {
        material->name = safe_strdup(mat_name.data);
    }

    // Try glTF baseColorFactor first, fall back to diffuse color
    if (AI_SUCCESS == aiGetMaterialColor(ai_mat, AI_MATKEY_BASE_COLOR, &color)) {
        material->albedo[0] = color.r;
        material->albedo[1] = color.g;
        material->albedo[2] = color.b;
        // Use alpha from baseColorFactor for opacity
        if (color.a < 1.0f) {
            material->opacity = color.a;
        }
    } else if (AI_SUCCESS == aiGetMaterialColor(ai_mat, AI_MATKEY_COLOR_DIFFUSE, &color)) {
        material->albedo[0] = color.r;
        material->albedo[1] = color.g;
        material->albedo[2] = color.b;
    } else {
        // White: the factor multiplies the albedo texture (glTF semantics),
        // so a missing factor must be the identity
        material->albedo[0] = 1.0;
        material->albedo[1] = 1.0;
        material->albedo[2] = 1.0;
    }

    // Extract emissive factor
    if (AI_SUCCESS == aiGetMaterialColor(ai_mat, AI_MATKEY_COLOR_EMISSIVE, &color)) {
        material->emissive[0] = color.r;
        material->emissive[1] = color.g;
        material->emissive[2] = color.b;
    }

    // HDR emissive multiplier (glTF KHR_materials_emissive_strength)
    ai_real emissive_strength;
    if (AI_SUCCESS ==
        aiGetMaterialFloat(ai_mat, AI_MATKEY_EMISSIVE_INTENSITY, &emissive_strength)) {
        material->emissive_strength = emissive_strength;
    }

    // The factors multiply their textures (glTF semantics); glTF assets
    // always carry them, so the fallbacks only apply to legacy formats:
    // rough dielectric, with roughness at the multiplicative identity
    ai_real metallic, roughness;

    if (AI_SUCCESS == aiGetMaterialFloat(ai_mat, AI_MATKEY_METALLIC_FACTOR, &metallic)) {
        material->metallic = metallic;
    } else {
        material->metallic = 0.0;
    }

    if (AI_SUCCESS == aiGetMaterialFloat(ai_mat, AI_MATKEY_ROUGHNESS_FACTOR, &roughness)) {
        material->roughness = roughness;
    } else {
        material->roughness = 1.0;
    }

    // Extract doubleSided flag
    int twoSided = 0;
    if (AI_SUCCESS == aiGetMaterialInteger(ai_mat, AI_MATKEY_TWOSIDED, &twoSided)) {
        material->doubleSided = (twoSided != 0);
    }

    // Extract normal map scale (glTF normalTexture.scale)
    ai_real normalScale;
    if (AI_SUCCESS == aiGetMaterialFloat(ai_mat,
                                         AI_MATKEY_GLTF_TEXTURE_SCALE(aiTextureType_NORMALS, 0),
                                         &normalScale)) {
        material->normalScale = normalScale;
    }

    // Extract occlusion strength (glTF occlusionTexture.strength)
    ai_real aoStrength;
    if (AI_SUCCESS == aiGetMaterialFloat(ai_mat,
                                         AI_MATKEY_GLTF_TEXTURE_STRENGTH(aiTextureType_LIGHTMAP, 0),
                                         &aoStrength)) {
        material->aoStrength = aoStrength;
    }

    // Extract glTF alpha mode and cutoff for hair/foliage transparency.
    // Formats without alphaMode fall back to the opacity heuristic in
    // material_finalize_alpha_mode once textures are loaded.
    struct aiString alphaMode;
    if (AI_SUCCESS == aiGetMaterialString(ai_mat, AI_MATKEY_GLTF_ALPHAMODE, &alphaMode)) {
        if (strcmp(alphaMode.data, "MASK") == 0) {
            material->alpha_mode = ALPHA_MASK;
            ai_real cutoff;
            if (AI_SUCCESS == aiGetMaterialFloat(ai_mat, AI_MATKEY_GLTF_ALPHACUTOFF, &cutoff)) {
                material->alphaCutoff = cutoff;
            } else {
                material->alphaCutoff = 0.5f; // glTF default
            }
            log_info("Material uses alpha mask mode with cutoff=%.2f", material->alphaCutoff);
        } else if (strcmp(alphaMode.data, "BLEND") == 0) {
            material->alpha_mode = ALPHA_BLEND;
        }
    }

    // KHR_materials_transmission: see-through glass. IOR and volume
    // thickness are read ONLY for transmissive materials: only glTF
    // produces transmission, so this scopes them to glTF for free -- FBX
    // and OBJ exporters commonly bake a meaningless refracti (Ni 1.0 is a
    // stock default) that would silently kill dielectric specular if
    // imported unconditionally.
    ai_real transmission;
    if (AI_SUCCESS == aiGetMaterialFloat(ai_mat, AI_MATKEY_TRANSMISSION_FACTOR, &transmission)) {
        material->transmission = transmission;
        if (transmission > 0.0f) {
            ai_real ior;
            if (AI_SUCCESS == aiGetMaterialFloat(ai_mat, AI_MATKEY_REFRACTI, &ior)) {
                material->ior = ior;
            }
            ai_real thickness;
            if (AI_SUCCESS ==
                aiGetMaterialFloat(ai_mat, AI_MATKEY_VOLUME_THICKNESS_FACTOR, &thickness)) {
                material->thickness = thickness;
            }
            log_info("Material is transmissive: transmission=%.2f ior=%.2f thickness=%.2f",
                     material->transmission, material->ior, material->thickness);
        }
    }

    // KHR_materials_clearcoat: a thin smooth dielectric coat over the base
    // BRDF (car paint, lacquer, varnish). glTF-only, so the guarded reads
    // leave FBX/OBJ at the scalar defaults (no coat).
    ai_real clearcoat;
    if (AI_SUCCESS == aiGetMaterialFloat(ai_mat, AI_MATKEY_CLEARCOAT_FACTOR, &clearcoat)) {
        material->clearcoat = clearcoat;
        if (clearcoat > 0.0f) {
            ai_real coat_rough;
            if (AI_SUCCESS ==
                aiGetMaterialFloat(ai_mat, AI_MATKEY_CLEARCOAT_ROUGHNESS_FACTOR, &coat_rough)) {
                material->clearcoat_roughness = coat_rough;
            }
            log_info("Material has clearcoat: factor=%.2f roughness=%.2f", material->clearcoat,
                     material->clearcoat_roughness);
        }
    }

    // KHR_materials_specular: re-parameterizes the dielectric specular --
    // specularFactor weights it, specularColorFactor tints F0. The factor key
    // ($mat.specularFactor) is glTF-KHR-specific, so it gates the read of the
    // legacy-shared specular COLOR key ($clr.specular); otherwise an FBX/OBJ
    // Phong specular color would be imported as a bogus F0 tint. specular_factor
    // stays -1 (extension absent) for non-glTF, leaving the base BRDF unchanged.
    ai_real specular;
    if (AI_SUCCESS == aiGetMaterialFloat(ai_mat, AI_MATKEY_SPECULAR_FACTOR, &specular)) {
        material->specular_factor = specular;
        struct aiColor4D spec_color;
        if (AI_SUCCESS == aiGetMaterialColor(ai_mat, AI_MATKEY_COLOR_SPECULAR, &spec_color)) {
            material->specular_color_factor[0] = spec_color.r;
            material->specular_color_factor[1] = spec_color.g;
            material->specular_color_factor[2] = spec_color.b;
        }
        log_info("Material has KHR specular: factor=%.2f color=(%.2f %.2f %.2f)",
                 material->specular_factor, material->specular_color_factor[0],
                 material->specular_color_factor[1], material->specular_color_factor[2]);
    }

    // KHR_materials_sheen: a retroreflective cloth lobe (velvet / satin). The
    // color factor ($clr.sheen.factor) is a dedicated glTF-KHR key, so it cleanly
    // scopes to glTF; sheen_color_factor stays (0,0,0) -> no lobe for non-glTF.
    struct aiColor4D sheen_color;
    if (AI_SUCCESS == aiGetMaterialColor(ai_mat, AI_MATKEY_SHEEN_COLOR_FACTOR, &sheen_color)) {
        material->sheen_color_factor[0] = sheen_color.r;
        material->sheen_color_factor[1] = sheen_color.g;
        material->sheen_color_factor[2] = sheen_color.b;
        ai_real sheen_rough;
        if (AI_SUCCESS ==
            aiGetMaterialFloat(ai_mat, AI_MATKEY_SHEEN_ROUGHNESS_FACTOR, &sheen_rough)) {
            material->sheen_roughness_factor = sheen_rough;
        }
        log_info("Material has KHR sheen: color=(%.2f %.2f %.2f) roughness=%.2f",
                 material->sheen_color_factor[0], material->sheen_color_factor[1],
                 material->sheen_color_factor[2], material->sheen_roughness_factor);
    }

    // Extract UV transform (KHR_texture_transform) from base color texture
    struct aiUVTransform uvTransform;
    if (AI_SUCCESS == aiGetMaterialFloatArray(ai_mat,
                                              AI_MATKEY_UVTRANSFORM(aiTextureType_DIFFUSE, 0),
                                              (float*)&uvTransform, NULL)) {
        material->uvOffset[0] = uvTransform.mTranslation.x;
        material->uvOffset[1] = uvTransform.mTranslation.y;
        material->uvScale[0] = uvTransform.mScaling.x;
        material->uvScale[1] = uvTransform.mScaling.y;
        material->uvRotation = uvTransform.mRotation;
    }
}

/*
 * Load embedded texture from aiScene
 */
static Texture* load_embedded_texture(TexturePool* tex_pool, const struct aiScene* ai_scene,
                                      const char* tex_path, bool is_srgb) {
    if (!ai_scene || !tex_path || tex_path[0] != '*') {
        return NULL;
    }

    // Parse embedded texture index from path (e.g., "*0" -> 0)
    int tex_index = atoi(tex_path + 1);
    if (tex_index < 0 || (unsigned int)tex_index >= ai_scene->mNumTextures) {
        log_error("Invalid embedded texture index: %s (max: %u)", tex_path, ai_scene->mNumTextures);
        return NULL;
    }

    const struct aiTexture* ai_tex = ai_scene->mTextures[tex_index];
    if (!ai_tex) {
        log_error("Embedded texture at index %d is NULL", tex_index);
        return NULL;
    }

    // The pool is keyed by tex_path ("*0", ...) -- the same key
    // load_texture_from_memory uses below. Check it BEFORE decoding, so an
    // embedded texture shared across many meshes decodes once, not once per mesh.
    Texture* cached = get_texture_from_pool(tex_pool, tex_path);
    if (cached)
        return cached;

    unsigned char* pixels = NULL;
    int width, height, channels;
    bool needs_free = false;

    if (ai_tex->mHeight == 0) {
        // Compressed format (PNG/JPG) - mWidth is buffer size in bytes
        pixels = stbi_load_from_memory((const unsigned char*)ai_tex->pcData, ai_tex->mWidth, &width,
                                       &height, &channels, 0);
        if (!pixels) {
            log_error("Failed to decode embedded texture %s (format hint: %.4s)", tex_path,
                      ai_tex->achFormatHint);
            return NULL;
        }
        needs_free = true;
    } else {
        // Raw RGBA data - mWidth/mHeight are actual dimensions
        pixels = (unsigned char*)ai_tex->pcData;
        width = ai_tex->mWidth;
        height = ai_tex->mHeight;
        channels = 4; // Assimp raw textures are always ARGB8888
        needs_free = false;
    }

    Texture* tex =
        load_texture_from_memory(tex_pool, tex_path, pixels, width, height, channels, is_srgb);

    if (needs_free) {
        stbi_image_free(pixels);
    }

    return tex;
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
        // A late-arriving opacity map can flip the material to BLEND
        material_finalize_alpha_mode(ctx->material);
        log_info("%s texture loaded async: %s", ctx->tex_type, tex->filepath);
    }
    free(ctx);
}

static AsyncTexCallback* make_async_tex_ctx(Material* mat, void (*setter)(Material*, Texture*),
                                            const char* tex_type) {
    AsyncTexCallback* ctx = malloc(sizeof(AsyncTexCallback));
    if (!ctx) {
        log_error("Failed to allocate AsyncTexCallback");
        return NULL;
    }
    ctx->material = mat;
    ctx->setter = setter;
    ctx->tex_type = tex_type;
    return ctx;
}

// Load a texture for a material. Both file and (compressed) embedded textures
// decode on the loader's worker pool and attach via callback; only rare raw
// (uncompressed) embedded pixels are handled inline.
static void load_material_texture(Material* material, TexturePool* tex_pool,
                                  const struct aiScene* ai_scene, AsyncLoader* loader,
                                  const char* tex_path, bool is_srgb,
                                  void (*setter)(Material*, Texture*), const char* tex_type_name) {
    if (tex_path[0] == '*' && ai_scene) {
        int idx = atoi(tex_path + 1);
        const struct aiTexture* ai_tex =
            (idx >= 0 && (unsigned int)idx < ai_scene->mNumTextures) ? ai_scene->mTextures[idx]
                                                                     : NULL;
        if (ai_tex && ai_tex->mHeight == 0) {
            // Compressed embedded (PNG/JPG): decode on a worker like a file. For
            // a compressed aiTexture, mWidth is the byte size of pcData.
            AsyncTexCallback* ctx = make_async_tex_ctx(material, setter, tex_type_name);
            if (ctx) {
                load_texture_from_memory_async(loader, tex_pool, tex_path,
                                               (const unsigned char*)ai_tex->pcData,
                                               (int)ai_tex->mWidth, is_srgb, async_tex_callback,
                                               ctx);
            }
        } else {
            // Raw (uncompressed) embedded pixels: decode inline (rare).
            Texture* tex = load_embedded_texture(tex_pool, ai_scene, tex_path, is_srgb);
            if (tex) {
                setter(material, tex);
            }
        }
    } else {
        AsyncTexCallback* ctx = make_async_tex_ctx(material, setter, tex_type_name);
        if (ctx) {
            load_texture_async(loader, tex_pool, tex_path, is_srgb, async_tex_callback, ctx);
        }
    }
}

static Material* process_ai_material(struct aiMaterial* ai_mat, TexturePool* tex_pool,
                                     const struct aiScene* ai_scene, AsyncLoader* loader) {
    if (!ai_mat || !tex_pool || !loader) {
        return NULL;
    }

    Material* material = create_material();
    extract_material_properties(ai_mat, material);

    struct aiString str;

    // Load textures from the mapping table
    for (size_t i = 0; i < texture_mapping_count; i++) {
        const TextureMapping* mapping = &texture_mappings[i];
        if (AI_SUCCESS == aiGetMaterialTexture(ai_mat, mapping->ai_type, 0, &str, NULL, NULL, NULL,
                                               NULL, NULL, NULL)) {
            load_material_texture(material, tex_pool, ai_scene, loader, str.data, mapping->is_srgb,
                                  mapping->setter, mapping->name);
        }
    }

    // Handle glTF combined metallic-roughness texture (uses same texture for both)
    if (AI_SUCCESS == aiGetMaterialTexture(ai_mat, aiTextureType_UNKNOWN, 0, &str, NULL, NULL, NULL,
                                           NULL, NULL, NULL)) {
        // glTF often stores metallicRoughness as UNKNOWN type
        // Only use if we don't already have metalness/roughness textures
        if (!material->metalness_tex) {
            load_material_texture(material, tex_pool, ai_scene, loader, str.data, false,
                                  set_material_metalness_tex, "MetallicRoughness(metalness)");
        }
        if (!material->roughness_tex) {
            load_material_texture(material, tex_pool, ai_scene, loader, str.data, false,
                                  set_material_roughness_tex, "MetallicRoughness(roughness)");
        }
    }

    // Clearcoat normal at aiTextureType_CLEARCOAT index 2 (the index-0 table
    // can't reach it); linear normal data.
    if (AI_SUCCESS == aiGetMaterialTexture(ai_mat, aiTextureType_CLEARCOAT, 2, &str, NULL, NULL,
                                           NULL, NULL, NULL, NULL)) {
        load_material_texture(material, tex_pool, ai_scene, loader, str.data, false,
                              set_material_clearcoat_normal_tex, "ClearcoatNormal");
    }

    material_finalize_alpha_mode(material);

    return material;
}

void process_ai_mesh(Mesh* mesh, struct aiMesh* ai_mesh) {
    size_t vert_count = ai_mesh->mNumVertices;
    size_t idx_count = ai_mesh->mNumFaces * 3; // Assuming the mesh is triangulated

    // Allocate memory for vertices and normals
    mesh->vertices = malloc(vert_count * 3 * sizeof(float));
    mesh->normals = malloc(vert_count * 3 * sizeof(float));

    // Validate critical allocations
    if (!mesh->vertices || !mesh->normals) {
        log_error("Failed to allocate mesh vertex/normal buffers");
        free(mesh->vertices);
        free(mesh->normals);
        mesh->vertices = NULL;
        mesh->normals = NULL;
        mesh->vertex_count = 0;
        mesh->index_count = 0;
        return;
    }

    mesh->vertex_count = vert_count;
    mesh->index_count = idx_count;

    // vec4: xyz tangent, w handedness derived from Assimp's bitangent below.
    if (ai_mesh->mTangents && ai_mesh->mBitangents) {
        mesh->tangents = malloc(mesh->vertex_count * 4 * sizeof(float));
    } else {
        mesh->tangents = NULL;
    }

    // Check for texture coordinates (UV0)
    if (ai_mesh->mTextureCoords[0]) {
        mesh->tex_coords = malloc(mesh->vertex_count * 2 * sizeof(float));
    } else {
        mesh->tex_coords = NULL;
    }

    // Check for texture coordinates (UV1) for lightmaps/AO
    if (ai_mesh->mTextureCoords[1]) {
        mesh->tex_coords2 = malloc(mesh->vertex_count * 2 * sizeof(float));
    } else {
        mesh->tex_coords2 = NULL;
    }

    // Check for vertex colors
    if (ai_mesh->mColors[0]) {
        mesh->colors = malloc(mesh->vertex_count * 4 * sizeof(float));
    } else {
        mesh->colors = NULL;
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
            mesh->tangents[i * 4] = ai_mesh->mTangents[i].x;
            mesh->tangents[i * 4 + 1] = ai_mesh->mTangents[i].y;
            mesh->tangents[i * 4 + 2] = ai_mesh->mTangents[i].z;

            // Handedness: Assimp derives the bitangent from the real UV
            // gradient, so on mirrored UV islands it points opposite
            // cross(N, T). That sign is the only part of its bitangent the
            // renderer ever used, and it is the only thing here that carries
            // information a procedural generator could not have produced.
            vec3 n = {ai_mesh->mNormals[i].x, ai_mesh->mNormals[i].y, ai_mesh->mNormals[i].z};
            vec3 t = {ai_mesh->mTangents[i].x, ai_mesh->mTangents[i].y, ai_mesh->mTangents[i].z};
            vec3 b = {ai_mesh->mBitangents[i].x, ai_mesh->mBitangents[i].y,
                      ai_mesh->mBitangents[i].z};
            vec3 derived;
            glm_vec3_cross(n, t, derived);
            mesh->tangents[i * 4 + 3] = glm_vec3_dot(derived, b) < 0.0f ? -1.0f : 1.0f;
        }

        if (mesh->tex_coords) {
            mesh->tex_coords[i * 2] = ai_mesh->mTextureCoords[0][i].x;
            mesh->tex_coords[i * 2 + 1] = ai_mesh->mTextureCoords[0][i].y;
        }

        if (mesh->tex_coords2) {
            mesh->tex_coords2[i * 2] = ai_mesh->mTextureCoords[1][i].x;
            mesh->tex_coords2[i * 2 + 1] = ai_mesh->mTextureCoords[1][i].y;
        }

        if (mesh->colors) {
            mesh->colors[i * 4] = ai_mesh->mColors[0][i].r;
            mesh->colors[i * 4 + 1] = ai_mesh->mColors[0][i].g;
            mesh->colors[i * 4 + 2] = ai_mesh->mColors[0][i].b;
            mesh->colors[i * 4 + 3] = ai_mesh->mColors[0][i].a;
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

/*
 * Find aiNode by name in hierarchy (for bone lookup)
 */
static struct aiNode* find_ai_node_by_name(struct aiNode* root, const char* name) {
    if (!root || !name)
        return NULL;

    if (strcmp(root->mName.data, name) == 0)
        return root;

    for (unsigned int i = 0; i < root->mNumChildren; i++) {
        struct aiNode* result = find_ai_node_by_name(root->mChildren[i], name);
        if (result)
            return result;
    }

    return NULL;
}

/*
 * Extract skeleton from aiMesh bone data
 */
Skeleton* process_ai_skeleton(const struct aiScene* ai_scene, const struct aiMesh* ai_mesh) {
    if (!ai_scene || !ai_mesh || ai_mesh->mNumBones == 0)
        return NULL;

    // Create skeleton with mesh name
    char skel_name[256];
    snprintf(skel_name, sizeof(skel_name), "%s_skeleton", ai_mesh->mName.data);
    Skeleton* skeleton = create_skeleton(skel_name);
    if (!skeleton)
        return NULL;

    // First pass: add all bones to skeleton
    // We need to determine parent relationships from the scene hierarchy
    for (unsigned int i = 0; i < ai_mesh->mNumBones; i++) {
        struct aiBone* ai_bone = ai_mesh->mBones[i];

        // Get inverse bind pose matrix
        mat4 inverse_bind = GLM_MAT4_IDENTITY_INIT;
        copy_aiMatrix_to_mat4(&ai_bone->mOffsetMatrix, inverse_bind);

        // Find bone node in scene hierarchy
        const struct aiNode* bone_node =
            find_ai_node_by_name(ai_scene->mRootNode, ai_bone->mName.data);

        mat4 local_transform = GLM_MAT4_IDENTITY_INIT;
        if (bone_node) {
            copy_aiMatrix_to_mat4(&bone_node->mTransformation, local_transform);
        }

        // Add bone (parent will be resolved in second pass)
        add_bone_to_skeleton(skeleton, ai_bone->mName.data, -1, inverse_bind, local_transform);
    }

    // Second pass: resolve parent indices using scene hierarchy
    // Walk up the node tree to find ancestor that is a bone (skipping intermediate nodes)
    for (size_t i = 0; i < skeleton->bone_count; i++) {
        Bone* bone = &skeleton->bones[i];
        const struct aiNode* bone_node = find_ai_node_by_name(ai_scene->mRootNode, bone->name);

        if (bone_node) {
            // Walk up the parent chain until we find a node that matches a bone
            const struct aiNode* parent = bone_node->mParent;
            while (parent) {
                int parent_idx = get_bone_index_by_name(skeleton, parent->mName.data);
                if (parent_idx >= 0) {
                    bone->parent_index = parent_idx;
                    break;
                }
                parent = parent->mParent;
            }
        }
    }

    log_info("Extracted skeleton '%s' with %zu bones", skeleton->name, skeleton->bone_count);
    return skeleton;
}

/*
 * Fill mesh bone_ids and bone_weights from aiMesh bone data
 */
void process_ai_mesh_bones(Mesh* mesh, const struct aiMesh* ai_mesh, Skeleton* skeleton) {
    if (!mesh || !ai_mesh || !skeleton || ai_mesh->mNumBones == 0)
        return;

    mesh->skeleton = skeleton;
    mesh->is_skinned = true;

    // Allocate per-vertex bone data
    size_t vert_count = mesh->vertex_count;
    mesh->bone_ids = calloc(vert_count * BONES_PER_VERTEX, sizeof(int));
    mesh->bone_weights = calloc(vert_count * BONES_PER_VERTEX, sizeof(float));

    if (!mesh->bone_ids || !mesh->bone_weights) {
        log_error("Failed to allocate bone data for mesh");
        free(mesh->bone_ids);
        free(mesh->bone_weights);
        mesh->bone_ids = NULL;
        mesh->bone_weights = NULL;
        mesh->is_skinned = false;
        return;
    }

    // Initialize bone IDs to -1 (no bone)
    for (size_t i = 0; i < vert_count * BONES_PER_VERTEX; i++) {
        mesh->bone_ids[i] = -1;
    }

    // Track how many bones assigned per vertex
    int* bone_counts = calloc(vert_count, sizeof(int));
    if (!bone_counts) {
        log_error("Failed to allocate bone count tracking");
        free(mesh->bone_ids);
        free(mesh->bone_weights);
        mesh->bone_ids = NULL;
        mesh->bone_weights = NULL;
        mesh->is_skinned = false;
        return;
    }

    // Process each bone's vertex weights
    for (unsigned int b = 0; b < ai_mesh->mNumBones; b++) {
        struct aiBone* ai_bone = ai_mesh->mBones[b];
        int bone_index = get_bone_index_by_name(skeleton, ai_bone->mName.data);

        if (bone_index < 0) {
            log_warn("Bone '%s' not found in skeleton", ai_bone->mName.data);
            continue;
        }

        for (unsigned int w = 0; w < ai_bone->mNumWeights; w++) {
            const struct aiVertexWeight* weight = &ai_bone->mWeights[w];
            unsigned int vertex_id = weight->mVertexId;
            float bone_weight = weight->mWeight;

            if (vertex_id >= vert_count)
                continue;

            int slot = bone_counts[vertex_id];
            if (slot < BONES_PER_VERTEX) {
                mesh->bone_ids[vertex_id * BONES_PER_VERTEX + slot] = bone_index;
                mesh->bone_weights[vertex_id * BONES_PER_VERTEX + slot] = bone_weight;
                bone_counts[vertex_id]++;
            }
        }
    }

    // Normalize weights (ensure they sum to 1.0). Track which vertices end up
    // with no effective weight: Assimp can report weight entries of 0.0, so
    // slot counts alone cannot identify them.
    unsigned char* has_weights = calloc(vert_count, 1);
    size_t unweighted = 0;
    for (size_t v = 0; v < vert_count; v++) {
        float total = 0.0f;
        for (int i = 0; i < BONES_PER_VERTEX; i++) {
            total += mesh->bone_weights[v * BONES_PER_VERTEX + i];
        }
        if (total > 0.0f) {
            for (int i = 0; i < BONES_PER_VERTEX; i++) {
                mesh->bone_weights[v * BONES_PER_VERTEX + i] /= total;
            }
            if (has_weights)
                has_weights[v] = 1;
        } else {
            unweighted++;
        }
    }

    // Vertices with no effective weights get the weights of a triangle
    // neighbor. Left untransformed they pin at their bind-pose position
    // during animation, stretching their triangles into long spikes; any
    // other bone choice (e.g. the mesh's dominant bone) does the same because
    // the vertex must move with the surface patch it is part of.
    if (unweighted > 0 && has_weights && mesh->indices && mesh->index_count >= 3) {
        size_t fixed = 0;
        for (size_t v = 0; v < vert_count; v++) {
            if (has_weights[v])
                continue;

            // Find a co-triangle vertex that has weights and copy them
            for (size_t t = 0; t + 2 < mesh->index_count && !has_weights[v]; t += 3) {
                if (mesh->indices[t] != v && mesh->indices[t + 1] != v && mesh->indices[t + 2] != v)
                    continue;
                for (int e = 0; e < 3; e++) {
                    size_t n = mesh->indices[t + e];
                    if (n == v || n >= vert_count || !has_weights[n])
                        continue;
                    for (int i = 0; i < BONES_PER_VERTEX; i++) {
                        mesh->bone_ids[v * BONES_PER_VERTEX + i] =
                            mesh->bone_ids[n * BONES_PER_VERTEX + i];
                        mesh->bone_weights[v * BONES_PER_VERTEX + i] =
                            mesh->bone_weights[n * BONES_PER_VERTEX + i];
                    }
                    has_weights[v] = 1;
                    fixed++;
                    break;
                }
            }
        }
        log_warn("%zu vertices had no bone weights; %zu inherited a triangle neighbor's weights",
                 unweighted, fixed);
    }

    free(has_weights);
    free(bone_counts);
    log_info("Processed %u bones for mesh with %zu vertices", ai_mesh->mNumBones, vert_count);
}

/*
 * Compute global rest pose for a bone by accumulating parent transforms
 * This is needed for retargeting: delta must use GLOBAL rest poses, not local,
 * because bone_matrix = global * inverse_bind_pose uses GLOBAL coordinates.
 */
static void compute_bone_global_rest(Skeleton* skeleton, int bone_idx, mat4 out) {
    if (!skeleton || bone_idx < 0 || (size_t)bone_idx >= skeleton->bone_count) {
        glm_mat4_identity(out);
        return;
    }
    Bone* bone = &skeleton->bones[bone_idx];
    if (bone->parent_index < 0) {
        // Root bone: global = local
        glm_mat4_copy(bone->local_transform, out);
    } else {
        // Child bone: global = parent_global * local
        mat4 parent_global = GLM_MAT4_IDENTITY_INIT;
        compute_bone_global_rest(skeleton, bone->parent_index, parent_global);
        glm_mat4_mul(parent_global, bone->local_transform, out);
    }
}

/*
 * Extract animations from aiScene with optional retargeting support
 * source_skeleton: if provided, used to get source rest poses for proper delta computation
 */
static void process_ai_animations_internal(const struct aiScene* ai_scene, Scene* scene,
                                           Skeleton* skeleton, bool enable_retargeting,
                                           Skeleton* source_skeleton) {
    if (!ai_scene || !scene || ai_scene->mNumAnimations == 0)
        return;

    for (unsigned int a = 0; a < ai_scene->mNumAnimations; a++) {
        struct aiAnimation* ai_anim = ai_scene->mAnimations[a];

        float duration = (float)ai_anim->mDuration;
        float tps = (float)ai_anim->mTicksPerSecond;
        if (tps <= 0.0f)
            tps = 25.0f;

        Animation* animation = create_animation(ai_anim->mName.data, duration, tps);
        if (!animation)
            continue;

        animation->skeleton = skeleton;

        // Process each channel (bone animation)
        int matched_channels = 0;
        int retargeted_channels = 0;
        for (unsigned int c = 0; c < ai_anim->mNumChannels; c++) {
            struct aiNodeAnim* ai_channel = ai_anim->mChannels[c];

            // Find bone index in skeleton
            int bone_index = -1;
            bool used_smart_match = false;

            if (skeleton) {
                // Try exact match first
                bone_index = get_bone_index_by_name(skeleton, ai_channel->mNodeName.data);

                // Fall back to smart matching if enabled and exact match failed
                if (bone_index < 0 && enable_retargeting) {
                    bone_index = find_matching_bone_smart(skeleton, ai_channel->mNodeName.data);
                    if (bone_index >= 0) {
                        used_smart_match = true;
                    }
                }

                if (bone_index >= 0) {
                    matched_channels++;
                } else {
                    log_debug("Animation channel '%s' has no matching bone in skeleton",
                              ai_channel->mNodeName.data);
                }
            }

            AnimationChannel* channel =
                create_animation_channel(bone_index, ai_channel->mNodeName.data);
            if (!channel)
                continue;

            // Position keys
            for (unsigned int k = 0; k < ai_channel->mNumPositionKeys; k++) {
                struct aiVectorKey* key = &ai_channel->mPositionKeys[k];
                vec3 pos = {key->mValue.x, key->mValue.y, key->mValue.z};
                add_position_key(channel, (float)key->mTime, pos);
            }

            // Rotation keys
            for (unsigned int k = 0; k < ai_channel->mNumRotationKeys; k++) {
                struct aiQuatKey* key = &ai_channel->mRotationKeys[k];
                // Assimp quaternion: w, x, y, z
                // cGLM versor: x, y, z, w
                versor rot = {key->mValue.x, key->mValue.y, key->mValue.z, key->mValue.w};
                add_rotation_key(channel, (float)key->mTime, rot);
            }

            // Compute retargeting for ALL matched bones when retargeting is enabled
            // Not just smart-matched - even exact name matches may have different rest poses
            // due to coordinate system conversion during FBX export/import
            // We ALWAYS set needs_retargeting=true for matched bones so we use bind pose position
            if (enable_retargeting && bone_index >= 0 && ai_channel->mNumRotationKeys > 0) {
                const Bone* target_bone = &skeleton->bones[bone_index];

                // Extract LOCAL rest pose rotation from target bone
                versor target_local_rest;
                glm_mat4_quat(target_bone->local_transform, target_local_rest);
                glm_quat_normalize(target_local_rest);

                // Initialize global retarget fields
                channel->use_global_retarget = false;
                channel->source_bone_index = -1;
                channel->source_parent_bone_index = -1;
                glm_quat_identity(channel->source_local_rest);

                // Compute proper delta: delta = inv(source_rest) * target_rest
                // Without a source skeleton, assume the animation comes from a skeleton
                // with the same rest pose as the target, so the delta becomes identity
                // and keyframes pass through unchanged.
                versor source_local_rest;
                glm_quat_copy(target_local_rest, source_local_rest);

                if (source_skeleton) {
                    // Find matching bone in source skeleton
                    int source_idx =
                        find_matching_bone_smart(source_skeleton, ai_channel->mNodeName.data);
                    if (source_idx >= 0) {
                        const Bone* source_bone = &source_skeleton->bones[source_idx];
                        glm_mat4_quat(source_bone->local_transform, source_local_rest);
                        glm_quat_normalize(source_local_rest);

                        // Enable global-space retargeting with source hierarchy info
                        channel->use_global_retarget = true;
                        channel->source_bone_index = source_idx;
                        channel->source_parent_bone_index = source_bone->parent_index;
                        glm_quat_copy(source_local_rest, channel->source_local_rest);
                    }
                }

                // delta = inv(source_rest) * target_rest
                // This transforms: "undo source orientation, apply target orientation"
                // When applied as: result = keyframe * delta
                // We get: keyframe * inv(source_rest) * target_rest
                // = "apply keyframe, undo source, apply target"
                // NOTE: This is kept as fallback but use_global_retarget takes precedence
                versor source_inv;
                glm_quat_inv(source_local_rest, source_inv);
                glm_quat_mul(source_inv, target_local_rest, channel->rotation_delta);
                glm_quat_normalize(channel->rotation_delta);

                // Always enable retargeting for matched bones - this ensures:
                // 1. Rotation delta is applied
                // 2. Bind pose position is used instead of animation position
                channel->needs_retargeting = true;
                retargeted_channels++;

                float delta_angle = 2.0f * acosf(fabsf(channel->rotation_delta[3])) * 57.2958f;
                if (delta_angle > 5.0f) {
                    log_debug("Retarget '%s' -> '%s': delta=%.1f deg%s%s",
                              ai_channel->mNodeName.data, target_bone->name, delta_angle,
                              source_skeleton ? " (with source)" : " (no source)",
                              used_smart_match ? " (smart)" : "");
                }
            }

            // Scale keys
            for (unsigned int k = 0; k < ai_channel->mNumScalingKeys; k++) {
                struct aiVectorKey* key = &ai_channel->mScalingKeys[k];
                vec3 scale = {key->mValue.x, key->mValue.y, key->mValue.z};
                add_scale_key(channel, (float)key->mTime, scale);
            }

            if (add_channel_to_animation(animation, channel) < 0) {
                free_animation_channel(channel);
            } else {
                free(channel); // Content was transferred, free the shell
            }
        }

        add_animation_to_scene(scene, animation);
        if (retargeted_channels > 0) {
            log_info("Extracted animation '%s': %.2f ticks @ %.2f tps (%zu channels, %d matched, "
                     "%d retargeted)",
                     animation->name, animation->duration, animation->ticks_per_second,
                     animation->channel_count, matched_channels, retargeted_channels);
        } else {
            log_info(
                "Extracted animation '%s': %.2f ticks @ %.2f tps (%zu channels, %d matched bones)",
                animation->name, animation->duration, animation->ticks_per_second,
                animation->channel_count, matched_channels);
        }

        // Print bone mapping debug table
        if (enable_retargeting && animation->channel_count > 0) {
            printf("\n==================== BONE MAPPING TABLE ====================\n");
            printf("%-45s -> %-25s %s\n", "SOURCE (Mixamo)", "TARGET", "DELTA");
            printf("-------------------------------------------------------------\n");
            for (size_t ch = 0; ch < animation->channel_count; ch++) {
                AnimationChannel* chan = &animation->channels[ch];
                if (chan->bone_index >= 0) {
                    const Bone* target_bone = &skeleton->bones[chan->bone_index];
                    float delta_angle = 2.0f * acosf(fabsf(chan->rotation_delta[3])) * 57.2958f;
                    printf("%-45s -> %-25s %6.1f deg%s\n",
                           chan->bone_name ? chan->bone_name : "(unknown)", target_bone->name,
                           delta_angle, chan->needs_retargeting ? " [R]" : "");
                } else {
                    printf("%-45s -> (UNMAPPED)\n",
                           chan->bone_name ? chan->bone_name : "(unknown)");
                }
            }
            printf("=============================================================\n\n");
        }
    }
}

// Public wrapper without retargeting (for internal scene loading)
void process_ai_animations(const struct aiScene* ai_scene, Scene* scene, Skeleton* skeleton) {
    process_ai_animations_internal(ai_scene, scene, skeleton, false, NULL);
}

int load_animations_from_file(Scene* scene, Skeleton* skeleton, const char* filepath,
                              bool enable_retargeting, Skeleton* source_skeleton) {
    if (!scene || !skeleton || !filepath) {
        log_error("load_animations_from_file: NULL parameter");
        return -1;
    }

    // Use minimal import flags - we only need animation data
    unsigned int flags = aiProcess_ValidateDataStructure;

    const struct aiScene* ai_scene = import_ai_scene(filepath, flags);
    if (!ai_scene) {
        log_error("Failed to load animation file: %s - %s", filepath, aiGetErrorString());
        return -1;
    }

    if (ai_scene->mNumAnimations == 0) {
        log_warn("Animation file '%s' contains no animations", filepath);
        aiReleaseImport(ai_scene);
        return 0;
    }

    // If no source skeleton was provided, try to extract one from the animation
    // file itself (Mixamo clips exported with skin embed the full rig). Global
    // retargeting needs source rest poses; channels only copy indices and
    // rotations out of the skeleton, so a temporary one is safe to free after.
    Skeleton* own_source = NULL;
    if (enable_retargeting && !source_skeleton) {
        for (unsigned int m = 0; m < ai_scene->mNumMeshes; m++) {
            if (ai_scene->mMeshes[m]->mNumBones > 0) {
                own_source = process_ai_skeleton(ai_scene, ai_scene->mMeshes[m]);
                break;
            }
        }
        if (own_source) {
            source_skeleton = own_source;
            log_info("Using skeleton embedded in '%s' as retarget source (%zu bones)", filepath,
                     own_source->bone_count);
        } else {
            log_warn("No source skeleton in '%s' and none provided (-s); cross-rig "
                     "retargeting will be incorrect",
                     filepath);
        }
    }

    size_t initial_count = scene->animation_count;

    // Process animations with retargeting support
    process_ai_animations_internal(ai_scene, scene, skeleton, enable_retargeting, source_skeleton);

    if (own_source) {
        free_skeleton(own_source);
    }

    size_t loaded = scene->animation_count - initial_count;
    log_info("Loaded %zu animation(s) from '%s'%s", loaded, filepath,
             enable_retargeting ? " (retargeting enabled)" : "");

    aiReleaseImport(ai_scene);
    return (int)loaded;
}

void process_ai_lights(const struct aiScene* scene, Light*** lights, size_t* num_lights,
                       bool photometric_units) {
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
            light->original_direction);
        glm_vec3_copy(light->original_direction, light->direction);
        glm_vec3_copy(
            (vec3){ai_light->mColorAmbient.r, ai_light->mColorAmbient.g, ai_light->mColorAmbient.b},
            light->ambient);
        glm_vec3_copy(
            (vec3){ai_light->mColorDiffuse.r, ai_light->mColorDiffuse.g, ai_light->mColorDiffuse.b},
            light->color);
        glm_vec3_copy((vec3){ai_light->mColorSpecular.r, ai_light->mColorSpecular.g,
                             ai_light->mColorSpecular.b},
                      light->specular);

        // Attenuation: only adopt the file's profile when it is usable. Blender
        // FBX exports physical watts, not OpenGL attenuation coefficients -- its
        // lights arrive with the coefficients zero (or denormal-tiny, which a
        // plain > 0 test accepts but still explodes 1/(c + l*d + q*d^2) into
        // absurd radiance: every lit pixel blows out no matter the exposure).
        // Require magnitudes that keep the denominator sane; otherwise keep
        // create_light()'s defaults.
        bool has_attenuation = ai_light->mAttenuationConstant > 1e-3f ||
                               ai_light->mAttenuationLinear > 1e-4f ||
                               ai_light->mAttenuationQuadratic > 1e-6f;

        // Set intensity, attenuation, and cutoff based on light type
        switch (ai_light->mType) {
            case aiLightSource_DIRECTIONAL:
                light->type = LIGHT_DIRECTIONAL;
                light->intensity = 1.0f;
                break;
            case aiLightSource_POINT:
                light->type = LIGHT_POINT;
                break;
            case aiLightSource_SPOT:
                light->type = LIGHT_SPOT;
                light->cutOff = ai_light->mAngleInnerCone;
                light->outerCutOff = ai_light->mAngleOuterCone;
                break;
            default:
                light->type = LIGHT_AREA;
                light->cutOff = ai_light->mAngleInnerCone;
                light->outerCutOff = ai_light->mAngleOuterCone;
                break;
        }
        if (light->type != LIGHT_DIRECTIONAL && has_attenuation) {
            light->constant = ai_light->mAttenuationConstant;
            light->linear = ai_light->mAttenuationLinear;
            light->quadratic = ai_light->mAttenuationQuadratic;
        }

        // Blender also bakes the light's power into the color (e.g. an 800W
        // point light imports as color=(800,800,800)). Re-express as a
        // normalized color times intensity -- numerically identical (the shader
        // multiplies color * intensity) but sane for the GUI and clamps.
        float peak = fmaxf(light->color[0], fmaxf(light->color[1], light->color[2]));
        if (peak > 1.0f) {
            glm_vec3_divs(light->color, peak, light->color);
            glm_vec3_divs(light->specular, peak, light->specular);
            light->intensity *= peak;
        }

        // glTF punctual lights are photometric (KHR_lights_punctual:
        // directional in lux, point/spot in candela). Blender exports a sun's
        // W/m^2 as lux (x683); undo that so authored intensities land at
        // renderer scale (a 17 W sun imports as 17, not 11611).
        if (photometric_units && light->intensity > 1.0f) {
            float photometric = light->intensity;
            light->intensity /= 683.0f;
            log_info("Light '%s': photometric intensity %.0f -> %.2f (lux/candela to renderer scale)",
                     ai_light->mName.data, photometric, light->intensity);
        }

        // Units without a spec (see IMPORTED_LIGHT_INTENSITY_MAX) can still
        // arrive absurdly hot; clamp rather than blow out every lit pixel.
        if (light->intensity > IMPORTED_LIGHT_INTENSITY_MAX) {
            log_warn("Light '%s': intensity %.0f exceeds sane ceiling, clamped to %.0f",
                     ai_light->mName.data, light->intensity, IMPORTED_LIGHT_INTENSITY_MAX);
            light->intensity = IMPORTED_LIGHT_INTENSITY_MAX;
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
        if (!camera) {
            log_error("Failed to allocate memory for camera\n");
            continue;
        }
        camera->name = safe_strdup(ai_camera->mName.data);

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

static void copy_aiMatrix_to_mat4(const struct aiMatrix4x4* from, mat4 to) {
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

// Walk the aiNode tree into cetra SceneNodes. Textures stream on the loader's
// worker pool; skeletons/bones are extracted synchronously here regardless.
static SceneNode* process_ai_node(Scene* scene, struct aiNode* ai_node,
                                  const struct aiScene* ai_scene, TexturePool* tex_pool,
                                  AsyncLoader* loader, Material** mat_cache) {
    if (!scene || !ai_node || !ai_scene || !tex_pool || !loader || !mat_cache)
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
        struct aiMesh* ai_mesh = ai_scene->mMeshes[meshIndex];

        Mesh* mesh = create_mesh();
        process_ai_mesh(mesh, ai_mesh);

        // Material, deduped by aiMaterial index: one aiMaterial can be shared by
        // hundreds of meshes (e.g. ivy leaves), so build the cetra Material once
        // per index and reuse it -- the ~30 property queries + texture resolution
        // + embedded decodes happen once, not per mesh. Meshes safely share the
        // Material* (textures refcounted; per-mesh AABB on the Mesh; wind mask is
        // a per-mesh uniform).
        unsigned int matIndex = ai_mesh->mMaterialIndex;
        if (matIndex < ai_scene->mNumMaterials) {
            Material* mat = mat_cache[matIndex];
            if (!mat) {
                mat = process_ai_material(ai_scene->mMaterials[matIndex], tex_pool, ai_scene,
                                          loader);
                if (mat) {
                    mat_cache[matIndex] = mat;
                    add_material_to_scene(scene, mat);
                }
            }
            mesh->material = mat;
        }

        // Process skeleton and bone weights if mesh has bones
        if (ai_mesh->mNumBones > 0) {
            // Try to find existing skeleton or create new one
            Skeleton* skeleton = NULL;
            if (scene->skeleton_count > 0) {
                skeleton = scene->skeletons[0]; // Use first skeleton for now
            } else {
                skeleton = process_ai_skeleton(ai_scene, ai_mesh);
                if (skeleton) {
                    add_skeleton_to_scene(scene, skeleton);
                }
            }

            if (skeleton) {
                process_ai_mesh_bones(mesh, ai_mesh, skeleton);
            }
        }

        calculate_aabb(mesh);
        node->meshes[i] = mesh;
    }

    // Recursively process children nodes
    node->children_count = ai_node->mNumChildren;
    node->children = malloc(sizeof(SceneNode*) * node->children_count);
    for (unsigned int i = 0; i < node->children_count; i++) {
        node->children[i] =
            process_ai_node(scene, ai_node->mChildren[i], ai_scene, tex_pool, loader, mat_cache);
        if (node->children[i]) {
            node->children[i]->parent = node;
        }
    }

    node->name = safe_strdup(ai_node->mName.data);

    struct aiMatrix4x4 ai_mat = ai_node->mTransformation;
    copy_aiMatrix_to_mat4(&ai_mat, node->original_transform);

    return node;
}

// glTF carries no height/displacement texture (the KHR_materials_displacement
// draft was abandoned; assimp surfaces nothing), so height maps reach the engine
// by a filename convention -- the same "the pipeline resolves it" model every
// engine uses for POM. For each material that has no height texture yet but does
// have an albedo/normal texture with a file path, derive a "<name>_height.<ext>"
// sibling (swapping a known base-map suffix, else appending) and load it LINEAR
// into height_tex if the file exists. Materials with a height map already (e.g.
// FBX aiTextureType_HEIGHT), no source texture, or no sibling on disk are left
// untouched, so assets without a height map stay byte-identical. Idempotent
// (skips materials that already have height), so the render loop can also call
// it once the async texture loader drains (the render app streams textures on
// background threads, so at import time the *_tex paths are not yet populated).
void resolve_height_maps(Scene* scene) {
    if (!scene)
        return;
    static const char* const SUFFIXES[] = {"_albedo", "_basecolor", "_basecolour", "_diffuse",
                                           "_normal", "_norm",      "_nrm"};
    for (size_t m = 0; m < scene->material_count; m++) {
        Material* mat = scene->materials[m];
        if (!mat || mat->height_tex)
            continue;
        const char* src = mat->albedo_tex ? mat->albedo_tex->filepath
                                          : (mat->normal_tex ? mat->normal_tex->filepath : NULL);
        if (!src || !src[0])
            continue;

        const char* slash = strrchr(src, '/');
        const char* name = slash ? slash + 1 : src;
        const char* dot = strrchr(name, '.');
        size_t stem_len = dot ? (size_t)(dot - name) : strlen(name);
        const char* ext = dot ? dot : ""; // includes the leading '.'
        size_t prefix_len = (size_t)(name - src);

        size_t core_len = stem_len; // stem with a known base-map suffix stripped
        for (size_t s = 0; s < sizeof(SUFFIXES) / sizeof(SUFFIXES[0]); s++) {
            size_t sl = strlen(SUFFIXES[s]);
            if (stem_len >= sl && strncasecmp(name + stem_len - sl, SUFFIXES[s], sl) == 0) {
                core_len = stem_len - sl;
                break;
            }
        }

        char base[1024];
        int n = snprintf(base, sizeof(base), "%.*s%.*s_height", (int)prefix_len, src, (int)core_len,
                         name);
        if (n <= 0 || (size_t)n >= sizeof(base))
            continue;

        const char* try_ext[] = {".png", ext}; // prefer .png, else the source extension
        for (size_t e = 0; e < sizeof(try_ext) / sizeof(try_ext[0]); e++) {
            if (!try_ext[e][0] || (e > 0 && strcasecmp(try_ext[e], try_ext[0]) == 0))
                continue; // empty, or a duplicate of an extension already tried
            char cand[1100];
            if (snprintf(cand, sizeof(cand), "%s%s", base, try_ext[e]) >= (int)sizeof(cand))
                continue;
            if (!path_exists(cand))
                continue;
            Texture* h = load_texture_path_into_pool(scene->tex_pool, cand, false);
            if (h) {
                set_material_height_tex(mat, h);
                // Auto-enable POM with the default depth (glTF/FBX carry no POM
                // scale); an author who set one keeps it.
                if (mat->parallax_scale == 0.0f)
                    mat->parallax_scale = g_parallax_default_scale;
                log_info("POM: resolved height map %s (scale %.3f)", cand, mat->parallax_scale);
            }
            break;
        }
    }
}

// Load a model file into a Scene. Textures stream on the loader's worker pool
// (they may still be decoding when this returns); skeletons/meshes are ready.
Scene* create_scene_from_model_path(const char* path, const char* texture_directory,
                                    AsyncLoader* loader) {
    if (!loader) {
        log_error("create_scene_from_model_path: an AsyncLoader is required");
        return NULL;
    }

    const struct aiScene* ai_scene = import_ai_scene(
        path, aiProcess_Triangulate | aiProcess_CalcTangentSpace | uv_flip_flag(path));
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
        free_scene(scene);
        aiReleaseImport(ai_scene);
        return NULL;
    }

    char texdir_buf[1024];
    texture_directory =
        effective_texture_dir(path, texture_directory, texdir_buf, sizeof(texdir_buf));
    set_texture_pool_directory(tex_pool, texture_directory);

    // Process lights and cameras
    process_ai_lights(ai_scene, &scene->lights, &scene->light_count, is_gltf_path(path));
    process_ai_cameras(ai_scene, &scene->cameras, &scene->camera_count);

    // Process the root node (this also extracts skeletons and bone weights).
    // mat_cache dedups the cetra Material per aiMaterial index across the tree;
    // at least one slot so calloc(0) can't be mistaken for OOM.
    Material** mat_cache =
        calloc(ai_scene->mNumMaterials ? ai_scene->mNumMaterials : 1, sizeof(Material*));
    if (!mat_cache) {
        log_error("import: failed to allocate material cache (%u materials)",
                  ai_scene->mNumMaterials);
        free_scene(scene);
        aiReleaseImport(ai_scene);
        return NULL;
    }
    scene->root_node =
        process_ai_node(scene, ai_scene->mRootNode, ai_scene, tex_pool, loader, mat_cache);
    free(mat_cache);

    associate_cameras_and_lights_with_nodes(scene->root_node, scene);

    // Process animations if any skeleton was extracted
    if (ai_scene->mNumAnimations > 0 && scene->skeleton_count > 0) {
        process_ai_animations(ai_scene, scene, scene->skeletons[0]);
    }

    aiReleaseImport(ai_scene);
    return scene;
}
