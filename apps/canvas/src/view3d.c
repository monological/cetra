#include "view3d.h"

#include <stdio.h>

#include "cetra/geometry.h"
#include "cetra/ibl.h"
#include "cetra/light.h"
#include "cetra/material.h"
#include "cetra/mesh.h"

// The environment, from the corpus, named from the repository's root.
#define VIEW3D_ENVIRONMENT "assets/abandoned_window/abandoned_window_env.hdr"

static void add_mesh(Scene* scene, Mesh* mesh) {
    SceneNode* node = create_node();
    node_add_mesh(node, mesh);
    node_add_child(scene->root_node, node);
}

static Material* surface(ShaderProgram* pbr, const float albedo[3], float metallic,
                         float roughness) {
    Material* material = create_material();
    material_set_program(material, pbr);
    glm_vec3_copy((float*)albedo, material->albedo);
    material->metallic = metallic;
    material->roughness = roughness;
    return material;
}

SceneView* view3d_build(Engine* engine, Scene* scene) {
    ShaderProgram* pbr = engine_get_program(engine, CETRA_PROGRAM_PBR);
    if (!pbr)
        return NULL;
    IBLResources* ibl = create_ibl_resources();
    if (!ibl || load_hdr_environment(ibl, VIEW3D_ENVIRONMENT) != 0 ||
        precompute_ibl(ibl, engine) != 0) {
        fprintf(stderr, "canvas: the 3D pane's environment could not be loaded\n");
        free_ibl_resources(ibl);
    } else {
        scene->ibl = ibl;
        scene->render_skybox = true;
        scene->skybox_brightness = 1.0f;
    }

    static const struct {
        float x;
        float albedo[3];
        float metallic, roughness;
    } balls[] = {{-1.2f, {1.0f, 0.78f, 0.34f}, 1.0f, 0.25f},
                 {0.0f, {0.8f, 0.1f, 0.08f}, 0.0f, 0.35f},
                 {1.2f, {0.9f, 0.9f, 0.88f}, 0.0f, 0.9f}};
    for (size_t i = 0; i < sizeof(balls) / sizeof(balls[0]); i++) {
        Mesh* mesh = create_mesh();
        mesh->material = surface(pbr, balls[i].albedo, balls[i].metallic, balls[i].roughness);
        mesh_generate_sphere(mesh, &(Sphere){.position = {balls[i].x, 0.5f, 0.0f},
                                             .radius = 0.5f,
                                             .segments_lon = 64,
                                             .segments_lat = 32});
        add_mesh(scene, mesh);
    }
    Mesh* floor_mesh = create_mesh();
    floor_mesh->material = surface(pbr, (const float[3]){0.5f, 0.5f, 0.5f}, 0.0f, 0.8f);
    mesh_generate_plane(floor_mesh,
                        &(Plane){.width = 40.0f, .depth = 40.0f, .segments_w = 1, .segments_d = 1});
    add_mesh(scene, floor_mesh);

    LightDesc key = {.name = "key",
                     .type = LIGHT_DIRECTIONAL,
                     .direction = {-0.4f, -0.7f, -0.6f},
                     .intensity = 1.0f,
                     .cast_shadows = true};
    if (scene->ibl && scene->ibl->light_count > 0)
        glm_vec3_negate_to(scene->ibl->light_dirs[0], key.direction);
    Light* light = create_light(&key);
    if (light) {
        scene_add_light(scene, light);
        SceneNode* light_node = create_node();
        node_set_light(light_node, light);
        node_add_child(scene->root_node, light_node);
    }

    // Framed for a pane taller than it is wide: all three spheres across it, the floor beneath.
    const SceneViewDesc desc = {.camera = {.position = {0.0f, 3.0f, 7.5f},
                                           .look_at = {0.0f, 0.5f, 0.0f},
                                           .fov = glm_rad(40.0f),
                                           .near = 0.05f,
                                           .far = 100.0f}};
    SceneView* view = create_scene_view(engine, &desc);
    if (!view || !engine_add_view(engine, view)) {
        fprintf(stderr, "canvas: the 3D pane's view could not be made\n");
        free_scene_view(view);
        return NULL;
    }
    return view;
}
