#include <math.h>
#include <stdio.h> // wind_bound_probe prints to stdout, like the other probes
#include <stdlib.h>
#include <string.h>

#include "wind.h"
#include "material.h"
#include "mesh.h"
#include "scene.h"
#include "util.h" // safe_strdup

// The amplitude coefficients windOffset() uses, from the file the shader reads
// them out of. Included rather than restated so the bound below cannot drift
// from the displacement it is a bound ON.
#include "../shaders/include/wind_bounds.glsl"

Wind* create_wind(const char* name) {
    Wind* wind = malloc(sizeof(Wind));
    if (!wind)
        return NULL;
    wind->name = NULL;
    set_wind_name(wind, name ? name : "wind");
    wind->type = WIND_DIRECTIONAL;
    glm_vec3_copy((vec3){0.0f, 0.0f, 1.0f}, wind->direction);
    wind->strength = 0.02f;
    wind->speed = 1.5f;
    wind->gust_frequency = 0.15f;
    wind->gust_amount = 0.8f;
    wind->turbulence = 0.2f;
    // Lockstep, which is what every scene authored before this existed had.
    wind->phase_variation = 0.0f;
    return wind;
}

void free_wind(Wind* wind) {
    if (!wind)
        return;
    free(wind->name);
    free(wind);
}

void set_wind_name(Wind* wind, const char* name) {
    if (!wind)
        return;
    free(wind->name);
    wind->name = safe_strdup(name);
}

void wind_upload_to_program(const Wind* wind, UniformManager* u) {
    if (!u)
        return;
    // No wind (or none on this scene): strength 0 makes every wind-aware shader
    // early-out, so the scene renders exactly as it did before the feature.
    if (!wind) {
        uniform_set_float(u, "uWindStrength", 0.0f);
        return;
    }
    uniform_set_vec3(u, "uWindDir", (const float*)wind->direction);
    uniform_set_float(u, "uWindStrength", wind->strength);
    uniform_set_float(u, "uWindSpeed", wind->speed);
    uniform_set_float(u, "uWindGustFreq", wind->gust_frequency);
    uniform_set_float(u, "uWindGustAmount", wind->gust_amount);
    uniform_set_float(u, "uWindTurbulence", wind->turbulence);
    uniform_set_float(u, "uWindPhaseVariation", wind->phase_variation);
}

// |vec3(sin a, 0, cos b)| at its worst, which is what both turbulence terms
// displace along -- the two sines are independently phased, so they can peak
// together and the bound has to assume they do.
#define WIND_LATERAL_MAX GLM_SQRT2f

// |vec3(1, WIND_LEAF_FLUTTER_Y, -WIND_LEAF_FLUTTER_Z)|, the fixed direction the
// leaf flutter rides along. Constant-folded, and out here rather than inside
// the branch that uses it because it is a property of the model, not of a call.
#define WIND_LEAF_DIR_MAX                                                                      \
    sqrtf(1.0f + WIND_LEAF_FLUTTER_Y * WIND_LEAF_FLUTTER_Y +                                   \
          WIND_LEAF_FLUTTER_Z * WIND_LEAF_FLUTTER_Z)

float wind_max_offset(const Wind* wind, float response, int mode, float flex_max, float leaf_max) {
    // The shader's own early-out, mirrored. Not just an optimisation: a
    // negative response would otherwise come back as a negative margin and
    // SHRINK the bound, which is the one direction a bound must never move.
    if (!wind || wind->strength <= 0.0f || response <= 0.0f)
        return 0.0f;

    // Every factor below is bounded by its own range rather than by a constant.
    // gust = mix(1 - gust_amount, 1, cubed 0..1) never exceeds 1 for the
    // documented gust_amount in [0,1]; the fabsf arm covers a scene that
    // authored one outside it. `dir` is normalized in the shader, so direction
    // contributes exactly 1 however the author wrote it. Every sin/cos is in
    // [-1,1], and `sway`, `mask` and h*h are in [0,1] and reach it.
    float gust = fmaxf(1.0f, fabsf(1.0f - wind->gust_amount));
    float amp = wind->strength * response * gust;
    float turb = wind->turbulence;

    if (mode == 0) {
        // Cloth: a forward billow of at most `amp` along the wind, plus a
        // lateral flutter whose vec3(sin, 0, cos) has magnitude up to sqrt(2).
        return amp * (1.0f + WIND_LATERAL_MAX * WIND_CLOTH_FLUTTER * turb);
    }

    // Vegetation: whole-body lean, plus a per-branch sway and a turbulent
    // flutter that both scale with the vertex flex weight. flex and the leaf
    // term's uv0.y are raw unclamped attributes, so they arrive measured from
    // the mesh rather than assumed to be within [0,1].
    float bound =
        amp * (WIND_VEG_LEAN +
               flex_max * (WIND_VEG_SWAY + WIND_LATERAL_MAX * WIND_VEG_TURB * turb));

    if (mode == 2) {
        // Leaf flutter rides on top of that.
        bound += amp * leaf_max * WIND_LEAF_DIR_MAX * turb;
    }
    return bound;
}

// --- the bound's instrument (spec 11.54) ------------------------------------
//
// wind_max_offset above is a conservative bound on what windOffset displaces a
// vertex by, and the two are written in different languages. The coefficients
// are shared (wind_bounds.glsl); the arithmetic is not, and cannot be. So the
// probe drives the REAL shader through transform feedback and compares what it
// measures against what the bound claims -- rather than against a C mirror of
// the displacement, which would leave the GLSL a third copy and pass straight
// through a term added to it.

// The grid's resolution. windOffset mixes six incommensurate rates -- t*speed,
// and that times 0.35, 1.7, 1.3 and 6.0, plus t*gustFreq -- so there is no
// period to walk one of. These are a budget, and the probe PRINTS them: a
// measured maximum is only as credible as the grid that found it.
//
// The step matters more than the span. The fastest term advances 6*speed per
// unit t, so at speed pi and this step it moves about 0.06 rad a sample, fine
// enough that the peak of a sine is not stepped over.
#define WIND_PROBE_TIME_SPAN 40.0f
#define WIND_PROBE_TIME_STEPS 4000
#define WIND_PROBE_UV_STEPS 5
#define WIND_PROBE_POS_STEPS 5
// Distinct object origins, hashed by the shader into distinct phases. More than
// one only matters where phase_variation is non-zero, but costing four vertices
// is cheaper than a branch that would make the grid depend on the scene.
#define WIND_PROBE_ORIGINS 4
// Vertices per draw. The sweep is millions of samples; batching keeps the
// upload and the readback bounded rather than sizing them off the grid.
#define WIND_PROBE_BATCH 65536

// One sample's inputs, interleaved exactly as wind_probe_vert declares them.
typedef struct WindProbeVertex {
    float rest[3];
    float uv0[2];
    float uv1[2];
    float t;
    float origin[3];
} WindProbeVertex;

// The GL objects one probe run needs, built and torn down around it.
typedef struct WindProbeRig {
    ShaderProgram* program;
    GLuint vao, vbo, tfbo;
    WindProbeVertex* batch;
    float* readback;
    size_t filled;
    vec3 best_abs;
    float best_l2;
} WindProbeRig;

static bool _wind_rig_init(WindProbeRig* rig) {
    memset(rig, 0, sizeof(*rig));
    rig->program = create_wind_probe_program();
    if (!rig->program)
        return false;
    rig->batch = malloc(WIND_PROBE_BATCH * sizeof(WindProbeVertex));
    rig->readback = malloc(WIND_PROBE_BATCH * 3 * sizeof(float));
    if (!rig->batch || !rig->readback) {
        free(rig->batch);
        free(rig->readback);
        free_program(rig->program);
        return false;
    }

    glGenVertexArrays(1, &rig->vao);
    glGenBuffers(1, &rig->vbo);
    glGenBuffers(1, &rig->tfbo);

    glBindVertexArray(rig->vao);
    glBindBuffer(GL_ARRAY_BUFFER, rig->vbo);
    glBufferData(GL_ARRAY_BUFFER, WIND_PROBE_BATCH * sizeof(WindProbeVertex), NULL,
                 GL_DYNAMIC_DRAW);
    const GLsizei stride = sizeof(WindProbeVertex);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(WindProbeVertex, rest));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(WindProbeVertex, uv0));
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(WindProbeVertex, uv1));
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(WindProbeVertex, t));
    glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, stride,
                          (void*)offsetof(WindProbeVertex, origin));
    for (int i = 0; i < 5; ++i)
        glEnableVertexAttribArray(i);
    glBindVertexArray(0);

    glBindBuffer(GL_ARRAY_BUFFER, rig->tfbo);
    glBufferData(GL_ARRAY_BUFFER, WIND_PROBE_BATCH * 3 * sizeof(float), NULL, GL_DYNAMIC_READ);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return true;
}

static void _wind_rig_free(WindProbeRig* rig) {
    glDeleteVertexArrays(1, &rig->vao);
    glDeleteBuffers(1, &rig->vbo);
    glDeleteBuffers(1, &rig->tfbo);
    free(rig->batch);
    free(rig->readback);
    free_program(rig->program);
}

// Run whatever is queued through the shader and fold it into the running
// maxima. Both norms, because the bound is derived as an L2 magnitude while
// culling inflates the box per AXIS -- so max_abs is the operational question
// and max_l2 is the one the derivation answers.
static void _wind_rig_flush(WindProbeRig* rig) {
    if (rig->filled == 0)
        return;

    glBindBuffer(GL_ARRAY_BUFFER, rig->vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(rig->filled * sizeof(WindProbeVertex)),
                    rig->batch);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glEnable(GL_RASTERIZER_DISCARD);
    glUseProgram(rig->program->id);
    glBindVertexArray(rig->vao);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, rig->tfbo);
    glBeginTransformFeedback(GL_POINTS);
    glDrawArrays(GL_POINTS, 0, (GLsizei)rig->filled);
    glEndTransformFeedback();
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
    glBindVertexArray(0);
    glUseProgram(0);
    glDisable(GL_RASTERIZER_DISCARD);

    glBindBuffer(GL_ARRAY_BUFFER, rig->tfbo);
    glGetBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(rig->filled * 3 * sizeof(float)),
                       rig->readback);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    for (size_t i = 0; i < rig->filled; ++i) {
        const float* o = &rig->readback[i * 3];
        for (int k = 0; k < 3; ++k) {
            float a = fabsf(o[k]);
            if (a > rig->best_abs[k])
                rig->best_abs[k] = a;
        }
        float l2 = sqrtf(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
        if (l2 > rig->best_l2)
            rig->best_l2 = l2;
    }
    rig->filled = 0;
}

static void _wind_rig_push(WindProbeRig* rig, const vec3 rest, float uv0y, float uv1x, float uv1y,
                           float t, int origin_index) {
    WindProbeVertex* v = &rig->batch[rig->filled++];
    v->rest[0] = rest[0];
    v->rest[1] = rest[1];
    v->rest[2] = rest[2];
    v->uv0[0] = 0.0f;
    v->uv0[1] = uv0y;
    v->uv1[0] = uv1x;
    v->uv1[1] = uv1y;
    v->t = t;
    // Arbitrary but spread, so the shader's own hash lands on different phases.
    v->origin[0] = (float)origin_index * 13.37f;
    v->origin[1] = (float)origin_index * 7.11f;
    v->origin[2] = (float)origin_index * 3.77f;
    if (rig->filled == WIND_PROBE_BATCH)
        _wind_rig_flush(rig);
}

// Every vertex the mesh actually holds, over time. Proves the geometry that
// ships; the sweep below proves the bound.
static void _wind_probe_mesh(WindProbeRig* rig, const Mesh* mesh, int origins) {
    const float dt = WIND_PROBE_TIME_SPAN / (float)WIND_PROBE_TIME_STEPS;
    for (size_t v = 0; v < mesh->vertex_count; ++v) {
        vec3 rest = {mesh->vertices[v * 3 + 0], mesh->vertices[v * 3 + 1],
                     mesh->vertices[v * 3 + 2]};
        float uv0y = mesh->tex_coords ? mesh->tex_coords[v * 2 + 1] : 0.0f;
        float uv1x = mesh->tex_coords2 ? mesh->tex_coords2[v * 2 + 0] : 0.0f;
        float uv1y = mesh->tex_coords2 ? mesh->tex_coords2[v * 2 + 1] : 0.0f;
        for (int oi = 0; oi < origins; ++oi)
            for (int i = 0; i <= WIND_PROBE_TIME_STEPS; ++i)
                _wind_rig_push(rig, rest, uv0y, uv1x, uv1y, (float)i * dt, oi);
    }
}

// A synthetic grid, and this is the half that tests the BOUND: its ceilings are
// the mesh's own measured maxima, which are exactly the numbers wind_max_offset
// is handed. A vertex the mesh happens not to have -- flex at its maximum where
// uv0.y is also high, say -- is one the bound still promises to cover.
static void _wind_probe_sweep(WindProbeRig* rig, const Mesh* mesh, int origins) {
    const float dt = WIND_PROBE_TIME_SPAN / (float)WIND_PROBE_TIME_STEPS;
    const float lo[3] = {mesh->aabb.min[0], mesh->aabb.min[1], mesh->aabb.min[2]};
    const float hi[3] = {mesh->aabb.max[0], mesh->aabb.max[1], mesh->aabb.max[2]};

    for (int yi = 0; yi < WIND_PROBE_POS_STEPS; ++yi) {
        float fy = (float)yi / (float)(WIND_PROBE_POS_STEPS - 1);
        for (int xi = 0; xi < WIND_PROBE_POS_STEPS; ++xi) {
            float fx = (float)xi / (float)(WIND_PROBE_POS_STEPS - 1);
            vec3 rest = {glm_lerp(lo[0], hi[0], fx), glm_lerp(lo[1], hi[1], fy),
                         glm_lerp(lo[2], hi[2], fx)};
            for (int fi = 0; fi < WIND_PROBE_UV_STEPS; ++fi) {
                float flex = mesh->wind_flex_max * (float)fi / (float)(WIND_PROBE_UV_STEPS - 1);
                // uv0.y only ever appears multiplied by flex, and the bound
                // reads their JOINT maximum -- so capping uv0.y at
                // leaf_max/flex stays inside what was measured rather than
                // inventing a vertex the mesh cannot have.
                float uv0y = flex > 1e-6f ? fminf(1.0f, mesh->wind_leaf_max / flex) : 1.0f;
                for (int bi = 0; bi < WIND_PROBE_UV_STEPS; ++bi) {
                    float uv1x = (float)bi / (float)WIND_PROBE_UV_STEPS;
                    for (int oi = 0; oi < origins; ++oi)
                        for (int i = 0; i <= WIND_PROBE_TIME_STEPS; ++i)
                            _wind_rig_push(rig, rest, uv0y, uv1x, flex, (float)i * dt, oi);
                }
            }
        }
    }
}

static void _wind_probe_row(WindProbeRig* rig, const char* kind, const Wind* wind,
                            const Mesh* mesh, const char* name, int origins) {
    const Material* mat = mesh->material;
    UniformManager* u = rig->program->uniforms;

    glUseProgram(rig->program->id);
    // Through the SAME function the real passes use, so the probe cannot be fed
    // a wind the renderer would not have -- including the normalize the struct
    // does not do and the shader does.
    wind_upload_to_program(wind, u);
    uniform_set_float(u, "uWindResponse", mat->wind_response);
    uniform_set_int(u, "uWindMode", mat->wind_mode);
    uniform_set_float(u, "uWindMaskMinY", mesh->aabb.min[1]);
    uniform_set_float(u, "uWindMaskMaxY", mesh->aabb.max[1]);
    glUseProgram(0);

    glm_vec3_zero(rig->best_abs);
    rig->best_l2 = 0.0f;
    rig->filled = 0;

    if (strcmp(kind, "sweep") == 0)
        _wind_probe_sweep(rig, mesh, origins);
    else
        _wind_probe_mesh(rig, mesh, origins);
    _wind_rig_flush(rig);

    float bound = wind_max_offset(wind, mat->wind_response, mat->wind_mode, mesh->wind_flex_max,
                                  mesh->wind_leaf_max);
    float max_abs = fmaxf(rig->best_abs[0], fmaxf(rig->best_abs[1], rig->best_abs[2]));
    printf("wind-bound-probe %s mesh=%s mode=%d response=%.4f flex_max=%.4f leaf_max=%.4f "
           "max_abs=%.6f max_l2=%.6f bound=%.6f abs_ratio=%.4f l2_ratio=%.4f\n",
           kind, name, mat->wind_mode, mat->wind_response, mesh->wind_flex_max,
           mesh->wind_leaf_max, max_abs, rig->best_l2, bound, max_abs / fmaxf(bound, 1e-12f),
           rig->best_l2 / fmaxf(bound, 1e-12f));
}

static void _wind_probe_node(WindProbeRig* rig, const Wind* wind, const SceneNode* node,
                             int origins, int* counted) {
    if (!node)
        return;
    for (size_t i = 0; i < node->mesh_count; ++i) {
        const Mesh* mesh = node->meshes ? node->meshes[i] : NULL;
        if (!mesh || !mesh->material || mesh->material->wind_response <= 0.0f)
            continue;
        const char* name = mesh->material->name ? mesh->material->name : "unnamed";
        _wind_probe_row(rig, "mesh", wind, mesh, name, origins);
        _wind_probe_row(rig, "sweep", wind, mesh, name, origins);
        (*counted)++;
    }
    for (size_t i = 0; i < node->children_count; ++i)
        _wind_probe_node(rig, wind, node->children[i], origins, counted);
}

void wind_bound_probe(const struct Scene* scene) {
    const char* declined = NULL;
    if (!scene || !scene->root_node)
        declined = "noscene";
    else if (!scene->wind)
        declined = "nowind";
    else if (scene->wind->strength <= 0.0f)
        declined = "calm";
    if (declined) {
        printf("wind-bound-probe header available=0 reason=%s\n", declined);
        return;
    }

    WindProbeRig rig;
    if (!_wind_rig_init(&rig)) {
        printf("wind-bound-probe header available=0 reason=noprogram\n");
        return;
    }

    const Wind* wind = scene->wind;
    const int origins = wind->phase_variation > 0.0f ? WIND_PROBE_ORIGINS : 1;
    printf("wind-bound-probe header available=1 time_span=%.1f time_steps=%d origins=%d "
           "uv_steps=%d pos_steps=%d strength=%.4f turbulence=%.4f gust_amount=%.4f "
           "phase_variation=%.4f\n",
           (double)WIND_PROBE_TIME_SPAN, WIND_PROBE_TIME_STEPS, origins, WIND_PROBE_UV_STEPS,
           WIND_PROBE_POS_STEPS, wind->strength, wind->turbulence, wind->gust_amount,
           wind->phase_variation);

    int counted = 0;
    _wind_probe_node(&rig, wind, scene->root_node, origins, &counted);
    printf("wind-bound-probe total meshes=%d\n", counted);

    _wind_rig_free(&rig);
    check_gl_error("wind bound probe");
}
