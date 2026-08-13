#include <stdlib.h>

#include "water.h"

#include "engine.h"
#include "ext/log.h"
#include "ibl.h"
#include "program.h"
#include "scene.h"
#include "uniform.h"
#include "util.h"

Water* create_water(void) {
    Water* water = calloc(1, sizeof(Water));
    if (!water) {
        log_error("Failed to allocate memory for water");
        return NULL;
    }

    water->enabled = true;
    water->level = 0.0f;
    water->extent = 60.0f;
    // Clear-water extinction per metre, red first. Not a look control at the
    // scale of a pond -- over the couple of metres a lake fixture spans these
    // barely bite -- but it is the physical ordering, and the depth-graded
    // colour that reads as water comes from it rather than from a tint.
    glm_vec3_copy((vec3){0.45f, 0.09f, 0.06f}, water->absorption);
    glm_vec3_copy((vec3){0.02f, 0.10f, 0.12f}, water->scatter);
    water->roughness = 0.04f;
    water->ior = 1.333f;
    // Lake-scale defaults: a 6 m longest wave at 6 cm, which is a light breeze
    // rather than a sea state. An ocean wants both numbers an order up.
    glm_vec2_copy((vec2){0.86f, 0.51f}, water->wind_dir);
    water->amplitude = 0.06f;
    water->wavelength = 6.0f;
    water->steepness = 0.6f;
    water->spread = 0.42f;
    return water;
}

void free_water(Water* water) {
    if (!water)
        return;
    if (water->grid_vao)
        glDeleteVertexArrays(1, &water->grid_vao);
    if (water->grid_vbo)
        glDeleteBuffers(1, &water->grid_vbo);
    if (water->grid_ebo)
        glDeleteBuffers(1, &water->grid_ebo);
    free(water);
}

bool water_active(const Water* water) {
    return water && water->enabled && !water->failed;
}

// One indexed grid in the XZ plane over [-0.5, 0.5]^2, positions only. Where it
// lands and how it is displaced is the vertex shader's business, so the mesh
// carries no normals: a displaced surface's normal is the derivative of the
// displacement, and a stored one would just be overwritten.
static bool water_ensure_grid(Water* water) {
    if (water->grid_vao)
        return true;
    if (water->failed)
        return false;

    const int res = WATER_GRID_RES;
    const int verts_per_side = res + 1;
    const int vert_count = verts_per_side * verts_per_side;
    const int index_count = res * res * 6;

    float* verts = malloc((size_t)vert_count * 2 * sizeof(float));
    unsigned* indices = malloc((size_t)index_count * sizeof(unsigned));
    if (!verts || !indices) {
        log_error("Water grid allocation failed; disabling water");
        free(verts);
        free(indices);
        water->failed = true;
        return false;
    }

    for (int z = 0; z < verts_per_side; z++) {
        for (int x = 0; x < verts_per_side; x++) {
            const int i = (z * verts_per_side + x) * 2;
            verts[i + 0] = (float)x / (float)res - 0.5f;
            verts[i + 1] = (float)z / (float)res - 0.5f;
        }
    }
    int w = 0;
    for (int z = 0; z < res; z++) {
        for (int x = 0; x < res; x++) {
            const unsigned a = (unsigned)(z * verts_per_side + x);
            const unsigned b = a + 1;
            const unsigned c = a + (unsigned)verts_per_side;
            const unsigned d = c + 1;
            indices[w++] = a;
            indices[w++] = c;
            indices[w++] = b;
            indices[w++] = b;
            indices[w++] = c;
            indices[w++] = d;
        }
    }

    glGenVertexArrays(1, &water->grid_vao);
    glBindVertexArray(water->grid_vao);
    glGenBuffers(1, &water->grid_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, water->grid_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)vert_count * 2 * sizeof(float), verts,
                 GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glGenBuffers(1, &water->grid_ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, water->grid_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)index_count * sizeof(unsigned), indices,
                 GL_STATIC_DRAW);
    glBindVertexArray(0);

    free(verts);
    free(indices);
    water->grid_index_count = index_count;
    log_info("Water: %dx%d grid, %d triangles, level %.2f, extent %.1f", res, res,
             index_count / 3, (double)water->level, (double)water->extent);
    return true;
}

void water_render(Water* water, struct Scene* scene, struct Engine* engine, const mat4 view,
                  const mat4 draw_projection) {
    if (!water_active(water) || !scene || !engine)
        return;
    if (!water_ensure_grid(water))
        return;

    ShaderProgram* program = get_engine_shader_program_by_name(engine, "water");
    if (!program) {
        log_error("Water program missing; disabling water");
        water->failed = true;
        return;
    }

    // The surface's own transmission source. Both resolves re-bind the scene
    // framebuffer on the way out, so this is safe here and the pass below draws
    // into the scene target as usual.
    if (!engine->scene_color_this_frame)
        engine->scene_color_this_frame = engine_resolve_opaque_color(engine);
    const GLuint scene_depth = engine_resolve_scene_depth(engine);

    glUseProgram(program->id);
    UniformManager* u = program->uniforms;

    uniform_set_mat4(u, "view", (const float*)view);
    uniform_set_mat4(u, "projection", (const float*)draw_projection);
    // Motion vectors come from the UN-JITTERED pair while the raster above uses
    // the jittered projection: the jitter is a sub-pixel sampling offset, and
    // letting it into the velocity would report it as scene motion.
    uniform_set_mat4(u, "uCurrViewProjNoJitter", (const float*)engine->view_proj);
    uniform_set_mat4(u, "uPrevViewProj", (const float*)engine->prev_view_proj);

    uniform_set_float(u, "waterLevel", water->level);
    uniform_set_float(u, "waterExtent", water->extent);
    uniform_set_float(u, "waterRoughness", water->roughness);
    uniform_set_float(u, "waterIor", water->ior);
    uniform_set_vec3(u, "waterAbsorption", (const float*)&water->absorption);
    uniform_set_vec3(u, "waterScatter", (const float*)&water->scatter);
    uniform_set_vec2(u, "waterWindDir", (const float*)&water->wind_dir);
    uniform_set_float(u, "waterAmplitude", water->amplitude);
    uniform_set_float(u, "waterWavelength", water->wavelength);
    uniform_set_float(u, "waterSteepness", water->steepness);
    uniform_set_float(u, "waterSpread", water->spread);
    // The animation clock, not the wall clock: frame N must be phase N or a
    // headless run stops being comparable to itself.
    uniform_set_float(u, "time", (float)engine->render_time);
    // The advance of the SAME clock `time` came from, so the previous-frame
    // surface is one step back rather than one wall-clock tick back.
    uniform_set_float(u, "uDeltaTime", (float)engine->render_delta);

    int rw, rh;
    engine_render_size(engine, &rw, &rh);
    uniform_set_vec2(u, "screenSize", (vec2){(float)rw, (float)rh});

    glActiveTexture(GL_TEXTURE0 + TEXUNIT_SCENE_COLOR);
    glBindTexture(GL_TEXTURE_2D, engine->opaque_color_texture);
    uniform_set_int(u, "sceneColorTex", TEXUNIT_SCENE_COLOR);
    uniform_set_int(u, "sceneColorAvailable", engine->scene_color_this_frame ? 1 : 0);

    glActiveTexture(GL_TEXTURE0 + WATER_DEPTH_UNIT);
    glBindTexture(GL_TEXTURE_2D, scene_depth);
    uniform_set_int(u, "sceneDepthTex", WATER_DEPTH_UNIT);
    uniform_set_int(u, "sceneDepthAvailable", scene_depth ? 1 : 0);

    // The split-sum BRDF table is engine-owned and bound for every scene,
    // environment or not, so the Fresnel lobe's lookup is always valid.
    glActiveTexture(GL_TEXTURE0 + IBL_BRDF_LUT_TEXTURE_UNIT);
    glBindTexture(GL_TEXTURE_2D, engine->brdf_lut);
    uniform_set_int(u, "brdfLUT", IBL_BRDF_LUT_TEXTURE_UNIT);

    // The environment reflection: the same split-sum lookup every other material
    // makes, and it follows the sun for free because the procedural sky bakes
    // into this cubemap.
    if (scene->ibl && scene->ibl->precomputed) {
        bind_ibl_textures(scene->ibl, program);
    } else {
        // The samplerCube still has to be POINTED at its unit even with nothing
        // to sample. Left at the default 0, where a 2D texture lives, it is a
        // sampler type mismatch -- undefined for the whole program, not just for
        // the branch that reads it. render.c records the same hazard costing a
        // fixture 62,009 px.
        uniform_set_int(u, "prefilteredMap", IBL_PREFILTER_TEXTURE_UNIT);
        uniform_set_int(u, "iblEnabled", 0);
        uniform_set_float(u, "iblIntensity", 1.0f);
        uniform_set_float(u, "maxReflectionLOD", 0.0f);
    }

    glActiveTexture(GL_TEXTURE0);

    // Depth writes ON, unlike the skybox and the late pass. The particle depth
    // resolve and everything that sorts against the surface read this.
    const GLboolean cull_was_enabled = glIsEnabled(GL_CULL_FACE);
    glDisable(GL_CULL_FACE);
    // Nothing here is translucent -- coverage is a discard, not an alpha -- and
    // the G-buffer list about to be bound carries indexed blend disables that a
    // blanket glEnable(GL_BLEND) would wipe. Same bracket the opaque lane uses.
    glDisable(GL_BLEND);
    engine_set_scene_draw_buffers(engine, true);

    glBindVertexArray(water->grid_vao);
    glDrawElements(GL_TRIANGLES, water->grid_index_count, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);

    engine_set_scene_draw_buffers(engine, false);
    glEnable(GL_BLEND);
    if (cull_was_enabled)
        glEnable(GL_CULL_FACE);

    check_gl_error("water surface");
}
