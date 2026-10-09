#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "common.h"
#include "ext/log.h"
#include "program.h"
#include "shader_hook.h"
#include "shadow.h"
#include "ubo.h"
#include "util.h"

// The generated shader sources. This is the only file that reads them. They are
// static string literals, and including them from a public header copied them
// into every file that reached engine.h and rebuilt all of those on a .glsl edit.
#include "shader_strings.h"

// Fullscreen post-pass program helper (defined with the postfx constructors)
static ShaderProgram* create_post_program(const char* name, const char* frag_src);

ShaderProgram* create_program(const char* name) {
    ShaderProgram* program = calloc(1, sizeof(ShaderProgram));
    if (!program) {
        log_error("Failed to allocate memory for shader program");
        return NULL;
    }
    program->id = glCreateProgram();

    if (program->id == 0) {
        log_error("Failed to create program object.");
        free(program);
        return NULL;
    }

    if (!name) {
        log_error("Shader program name is NULL");
        glDeleteProgram(program->id);
        free(program);
        return NULL;
    }

    program->name = safe_strdup(name);
    program->shaders = NULL;
    program->shader_count = 0;
    program->uniforms = NULL;
    // NOT the calloc zero, which is a valid mask meaning "carries no features".
    // Every program starts as "not a variant" and only the variant builder says
    // otherwise, so a program that never heard of this cannot be mistaken for
    // the leanest one and swapped out from under its material.
    program->pbr_features = -1;
    // Same reason, different zero: 0 samplers is a real answer a post pass could
    // give, so "never counted" needs a value of its own.
    program->sampler_count = -1;
    // And GL_POINTS is 0, so "no geometry stage" cannot be the zero either.
    program->geometry_input = -1;

    return program;
}

ShaderProgram* create_program_from_paths(const char* name, const char* vert_path,
                                         const char* frag_path, const char* geo_path) {

    if (!name) {
        log_error("Shader program name is NULL");
        return NULL;
    }

    GLboolean success = GL_TRUE;

    ShaderProgram* program = create_program(name);
    if (program == NULL) {
        log_error("Failed to create program by name %s", name);
        return NULL;
    }

    // Load and compile the vertex shader
    if (vert_path != NULL) {
        Shader* vertex_shader = create_shader_from_path(VERTEX_SHADER, vert_path);
        if (vertex_shader && compile_shader(vertex_shader)) {
            attach_shader_to_program(program, vertex_shader);
        } else {
            log_error("Vertex shader compilation failed");
        }
    } else {
        log_error("Vertex shader path is NULL");
        success = GL_FALSE;
    }

    // Load and compile the geometry shader, if path is provided
    if (geo_path != NULL) {
        Shader* geometry_shader = create_shader_from_path(GEOMETRY_SHADER, geo_path);
        if (geometry_shader && compile_shader(geometry_shader)) {
            attach_shader_to_program(program, geometry_shader);
        } else {
            log_error("Geometry shader compilation failed");
            success = GL_FALSE;
        }
    }

    // Load and compile the fragment shader
    if (frag_path != NULL) {
        Shader* fragment_shader = create_shader_from_path(FRAGMENT_SHADER, frag_path);
        if (fragment_shader && compile_shader(fragment_shader)) {
            attach_shader_to_program(program, fragment_shader);
        } else {
            log_error("Fragment shader compilation failed");
            success = GL_FALSE;
        }
    } else {
        log_error("Fragment shader path is NULL");
        success = GL_FALSE;
    }

    // Link the shader program
    if (success && !link_program(program)) {
        log_error("Shader program linking failed");
        success = GL_FALSE;
    }

    // Setup uniforms and other initializations as needed
    if (success) {
        setup_program_uniforms(program);
    } else {
        free_program(program);
        program = NULL;
    }

    return program;
}

// `source` compiled as a `type` stage and attached to `program`, which then owns it; false, logged
// as `what`, when there is no source or it does not compile, and a stage that did not compile is
// freed here, since nothing else holds it.
static bool _attach_stage(ShaderProgram* program, ShaderType type, const char* source,
                          const char* what) {
    if (!source) {
        log_error("%s shader source is NULL", what);
        return false;
    }
    Shader* shader = create_shader(type, source);
    if (shader && compile_shader(shader)) {
        attach_shader_to_program(program, shader);
        return true;
    }
    free_shader(shader);
    log_error("%s shader compilation failed", what);
    return false;
}

ShaderProgram* create_program_from_source(const char* name, const char* vert_source,
                                          const char* frag_source, const char* geo_source) {

    if (!name) {
        log_error("Shader program name is NULL");
        return NULL;
    }

    ShaderProgram* program = create_program(name);
    if (program == NULL) {
        log_error("Failed to create program by name %s", name);
        return NULL;
    }

    // Every stage is compiled even after one fails, so one build reports every stage's errors.
    bool success = _attach_stage(program, VERTEX_SHADER, vert_source, "Vertex");
    success = _attach_stage(program, FRAGMENT_SHADER, frag_source, "Fragment") && success;
    if (geo_source != NULL)
        success = _attach_stage(program, GEOMETRY_SHADER, geo_source, "Geometry") && success;

    // Link the shader program
    if (success && !link_program(program)) {
        log_error("Shader program linking failed");
        success = false;
    }

    // Setup uniforms and other initializations as needed
    if (success) {
        setup_program_uniforms(program);
    } else {
        free_program(program);
        program = NULL;
    }

    return program;
}

void free_program(ShaderProgram* program) {
    if (program != NULL) {
        if (program->id != 0) {
            glDeleteProgram(program->id);
        }

        if (program->name != NULL) {
            free(program->name);
        }

        if (program->shaders) {
            for (size_t i = 0; i < program->shader_count; ++i) {
                if (program->shaders[i]) {
                    free_shader(program->shaders[i]);
                }
            }
            free(program->shaders);
        }

        if (program->uniforms) {
            free_uniform_manager(program->uniforms);
        }

        free(program);
    }
}

GLboolean reload_program_from_paths(ShaderProgram* program, const char* vert_path,
                                    const char* frag_path, const char* geo_path) {
    if (!program || !vert_path || !frag_path) {
        log_error("Invalid arguments to reload_program_from_paths");
        return GL_FALSE;
    }

    // Compile new shaders first (don't modify program until all succeed)
    Shader* new_vert = create_shader_from_path(VERTEX_SHADER, vert_path);
    if (!new_vert || !compile_shader(new_vert)) {
        log_error("Failed to compile vertex shader: %s", vert_path);
        if (new_vert)
            free_shader(new_vert);
        return GL_FALSE;
    }

    Shader* new_frag = create_shader_from_path(FRAGMENT_SHADER, frag_path);
    if (!new_frag || !compile_shader(new_frag)) {
        log_error("Failed to compile fragment shader: %s", frag_path);
        free_shader(new_vert);
        if (new_frag)
            free_shader(new_frag);
        return GL_FALSE;
    }

    Shader* new_geo = NULL;
    if (geo_path) {
        new_geo = create_shader_from_path(GEOMETRY_SHADER, geo_path);
        if (!new_geo || !compile_shader(new_geo)) {
            log_error("Failed to compile geometry shader: %s", geo_path);
            free_shader(new_vert);
            free_shader(new_frag);
            if (new_geo)
                free_shader(new_geo);
            return GL_FALSE;
        }
    }

    // All shaders compiled successfully - now modify the program
    // Detach and free old shaders
    for (size_t i = 0; i < program->shader_count; ++i) {
        if (program->shaders[i]) {
            glDetachShader(program->id, program->shaders[i]->shaderID);
            free_shader(program->shaders[i]);
        }
    }
    free(program->shaders);
    program->shaders = NULL;
    program->shader_count = 0;

    // Attach new shaders
    glAttachShader(program->id, new_vert->shaderID);
    glAttachShader(program->id, new_frag->shaderID);
    if (new_geo)
        glAttachShader(program->id, new_geo->shaderID);

    // Relink
    if (!link_program(program)) {
        log_error("Failed to relink program after shader reload");
        free_shader(new_vert);
        free_shader(new_frag);
        if (new_geo)
            free_shader(new_geo);
        return GL_FALSE;
    }

    // Store new shaders
    size_t new_count = new_geo ? 3 : 2;
    program->shaders = malloc(new_count * sizeof(Shader*));
    if (!program->shaders) {
        log_error("Failed to allocate shader array");
        free_shader(new_vert);
        free_shader(new_frag);
        if (new_geo)
            free_shader(new_geo);
        return GL_FALSE;
    }
    program->shaders[0] = new_vert;
    program->shaders[1] = new_frag;
    if (new_geo)
        program->shaders[2] = new_geo;
    program->shader_count = new_count;

    // Everything derived from the linked program -- the uniform cache, the
    // block bindings re-linking reset, the counts -- comes from the one setup.
    if (program->uniforms) {
        free_uniform_manager(program->uniforms);
        program->uniforms = NULL;
    }
    setup_program_uniforms(program);

    log_info("Reloaded shader program: %s", program->name);
    return GL_TRUE;
}

void attach_shader_to_program(ShaderProgram* program, Shader* shader) {
    if (program && shader && shader->shaderID) {
        // Attach the shader to the program
        glAttachShader(program->id, shader->shaderID);
        check_gl_error("attach shader");

        // Reallocate the shaders array to accommodate the new shader
        size_t new_count = program->shader_count + 1;
        Shader** new_shaders = realloc(program->shaders, new_count * sizeof(Shader*));
        if (new_shaders == NULL) {
            log_error("Failed to allocate memory for shaders");
            return;
        }

        // Add the new shader to the array and update the shader count
        new_shaders[program->shader_count] = shader;
        program->shaders = new_shaders;
        program->shader_count = new_count;
    } else {
        log_error("Failed to attach shader %i", shader ? shader->shaderID : 0);
    }
}

GLboolean link_program(ShaderProgram* program) {
    int success;

    glLinkProgram(program->id);
    check_gl_error("link program");

    glGetProgramiv(program->id, GL_LINK_STATUS, &success);
    check_gl_error("get program iv");

    if (!success) {
        GLint logLength = 0;
        glGetProgramiv(program->id, GL_INFO_LOG_LENGTH, &logLength);
        check_gl_error("glGetProgramiv log length");

        if (logLength > 0) {
            char* log = (char*)malloc(logLength);
            if (log) {
                glGetProgramInfoLog(program->id, logLength, &logLength, log);
                check_gl_error("glGetProgramInfoLog");

                log_error("Program %s compilation failed: %s", program->name, log);
                free(log);
            } else {
                log_error("Failed to allocate memory for program log.");
            }
        } else {
            log_error("Program compilation failed with no additional information.");
        }
        return GL_FALSE;
    }

    return GL_TRUE;
}

GLboolean validate_program(ShaderProgram* program) {
    GLboolean success = GL_TRUE;

    glValidateProgram(program->id);
    // Note: Some drivers generate spurious GL errors during validation.
    // The validation status (GL_VALIDATE_STATUS) is what actually matters.
    while (glGetError() != GL_NO_ERROR) {
    }

    GLint validationStatus;
    glGetProgramiv(program->id, GL_VALIDATE_STATUS, &validationStatus);
    if (validationStatus == GL_FALSE) {
        log_error("Shader program validation failed");

        // Get and print the validation log
        GLint logLength;
        glGetProgramiv(program->id, GL_INFO_LOG_LENGTH, &logLength);
        char* logMessage = malloc(sizeof(char) * logLength);
        if (logMessage) {
            glGetProgramInfoLog(program->id, logLength, NULL, logMessage);
            log_error("Validation log: %s", logMessage);
            free(logMessage);
        } else {
            log_error("Failed to allocate memory for validation log");
        }

        success = GL_FALSE;
    }
    return success;
}

// Whether a uniform of this type spends one of the program's texture image
// units.
//
// Exhaustive over GL 4.1 rather than over the four types this engine declares,
// and the difference is not pedantry: the count feeds an assertion about the
// sampler budget, and a type missing here undercounts SILENTLY -- the program
// links, the number reads better than the truth, and an arm built on it reports
// a unit that was never freed.
static bool _is_sampler_type(GLenum type) {
    switch (type) {
        case GL_SAMPLER_1D:
        case GL_SAMPLER_2D:
        case GL_SAMPLER_3D:
        case GL_SAMPLER_CUBE:
        case GL_SAMPLER_1D_SHADOW:
        case GL_SAMPLER_2D_SHADOW:
        case GL_SAMPLER_CUBE_SHADOW:
        case GL_SAMPLER_1D_ARRAY:
        case GL_SAMPLER_2D_ARRAY:
        case GL_SAMPLER_1D_ARRAY_SHADOW:
        case GL_SAMPLER_2D_ARRAY_SHADOW:
        case GL_SAMPLER_2D_MULTISAMPLE:
        case GL_SAMPLER_2D_MULTISAMPLE_ARRAY:
        case GL_SAMPLER_BUFFER:
        case GL_SAMPLER_2D_RECT:
        case GL_SAMPLER_2D_RECT_SHADOW:
        case GL_INT_SAMPLER_1D:
        case GL_INT_SAMPLER_2D:
        case GL_INT_SAMPLER_3D:
        case GL_INT_SAMPLER_CUBE:
        case GL_INT_SAMPLER_1D_ARRAY:
        case GL_INT_SAMPLER_2D_ARRAY:
        case GL_INT_SAMPLER_2D_MULTISAMPLE:
        case GL_INT_SAMPLER_2D_MULTISAMPLE_ARRAY:
        case GL_INT_SAMPLER_BUFFER:
        case GL_INT_SAMPLER_2D_RECT:
        case GL_UNSIGNED_INT_SAMPLER_1D:
        case GL_UNSIGNED_INT_SAMPLER_2D:
        case GL_UNSIGNED_INT_SAMPLER_3D:
        case GL_UNSIGNED_INT_SAMPLER_CUBE:
        case GL_UNSIGNED_INT_SAMPLER_1D_ARRAY:
        case GL_UNSIGNED_INT_SAMPLER_2D_ARRAY:
        case GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE:
        case GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE_ARRAY:
        case GL_UNSIGNED_INT_SAMPLER_BUFFER:
        case GL_UNSIGNED_INT_SAMPLER_2D_RECT:
            return true;
        default:
            return false;
    }
}

// The indices of the program's `active` uniforms that are in no uniform block, into `loose`;
// returns how many. Each block is asked for its members, into `member`, which both hold room for
// `active`. A member the blocks fail to report only stays among the loose ones, where its type
// is asked and found not to be a sampler, so a miss costs a query and never a wrong count.
static GLsizei _uniforms_outside_blocks(GLuint program_id, GLint active, GLint* member,
                                        GLuint* loose) {
    for (GLint i = 0; i < active; ++i)
        loose[i] = (GLuint)i;
    GLint blocks = 0;
    glGetProgramiv(program_id, GL_ACTIVE_UNIFORM_BLOCKS, &blocks);
    for (GLint b = 0; b < blocks; ++b) {
        GLint members = 0;
        glGetActiveUniformBlockiv(program_id, (GLuint)b, GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS,
                                  &members);
        if (members <= 0 || members > active)
            continue;
        glGetActiveUniformBlockiv(program_id, (GLuint)b, GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES,
                                  member);
        for (GLint m = 0; m < members; ++m)
            if (member[m] >= 0 && member[m] < active)
                loose[member[m]] = (GLuint)active; // past every index: in a block
    }
    GLsizei n = 0;
    for (GLint i = 0; i < active; ++i)
        if (loose[i] != (GLuint)active)
            loose[n++] = loose[i];
    return n;
}

// How many texture image units the LINKED program spends.
//
// Counted from what the linker KEPT, which is the only authority on it. A driver
// is free to drop a sampler whose every read was compiled away, and whether this
// one does is not a thing the source can answer -- so the question is put to the
// program rather than to a grep over the shader.
//
// A sampler cannot be a member of a uniform block, so only the uniforms outside every block have
// their types asked. An active uniform is every member of every element of every block too, and
// Apple's driver finds a uniform BY INDEX by walking all of them, so asking each its type -- one
// call per uniform, or one batched call over them all -- cost more than compiling and linking the
// program. -1 when it cannot be asked.
static int _count_program_samplers(GLuint program_id) {
    GLint active = 0;
    glGetProgramiv(program_id, GL_ACTIVE_UNIFORMS, &active);
    if (active <= 0)
        return active < 0 ? -1 : 0;
    GLuint* loose = malloc((size_t)active * sizeof(GLuint));
    GLint* member = malloc((size_t)active * sizeof(GLint));
    GLint* type = malloc((size_t)active * sizeof(GLint));
    GLint* size = malloc((size_t)active * sizeof(GLint));
    int samplers = -1;
    if (loose && member && type && size) {
        const GLsizei n = _uniforms_outside_blocks(program_id, active, member, loose);
        samplers = 0;
        if (n > 0) {
            glGetActiveUniformsiv(program_id, n, loose, GL_UNIFORM_TYPE, type);
            glGetActiveUniformsiv(program_id, n, loose, GL_UNIFORM_SIZE, size);
            // `size` is the array length, and an array of samplers spends a unit per element
            // rather than one for the declaration.
            for (GLsizei k = 0; k < n; ++k)
                if (_is_sampler_type((GLenum)type[k]))
                    samplers += size[k];
        }
    }
    free(loose);
    free(member);
    free(type);
    free(size);
    return samplers;
}

// The geometry stage's declared input primitive, or -1 when there is no such
// stage. Asked of the linked program, since the declaration is in the source
// of one of the attached shaders and the linker is what read it.
static GLint _program_geometry_input(const ShaderProgram* program) {
    for (size_t i = 0; i < program->shader_count; ++i) {
        if (program->shaders[i] && program->shaders[i]->type == GEOMETRY_SHADER) {
            GLint input = -1;
            glGetProgramiv(program->id, GL_GEOMETRY_INPUT_TYPE, &input);
            return input;
        }
    }
    return -1;
}

void setup_program_uniforms(ShaderProgram* program) {
    if (program == NULL || program->id == 0) {
        log_error("Invalid shader program.");
        return;
    }

    program->uniforms = create_uniform_manager(program->id);
    if (!program->uniforms) {
        log_error("Failed to create uniform manager");
        return;
    }

    uniform_cache_standard(program->uniforms);
    uniform_cache_shadows(program->uniforms, MAX_SHADOW_LIGHTS, SHADOW_CASCADES,
                          MAX_PUNCTUAL_SHADOW_LAYERS);

    // Clustered-forward blocks (spec 9.1): bind to the global binding points
    // and guard against C/GLSL layout drift. No-ops for programs that don't
    // declare (or strip) them. InstanceBlock is the one whose absence the
    // submitter has to know about, so its answer is kept on the program.
    program->instanced = ubo_wire_blocks(program->id);

    // Beside it for the same reason: a fact about the linked program that every
    // reader wants the answer to rather than the derivation.
    program->sampler_count = _count_program_samplers(program->id);
    program->geometry_input = _program_geometry_input(program);
}

bool program_accepts_draw_mode(const ShaderProgram* program, GLenum draw_mode) {
    if (!program)
        return false;
    switch (program->geometry_input) {
        case -1:
            return true;
        case GL_POINTS:
            return draw_mode == GL_POINTS;
        case GL_LINES:
            return draw_mode == GL_LINES || draw_mode == GL_LINE_STRIP || draw_mode == GL_LINE_LOOP;
        case GL_TRIANGLES:
            return draw_mode == GL_TRIANGLES || draw_mode == GL_TRIANGLE_STRIP ||
                   draw_mode == GL_TRIANGLE_FAN;
        default:
            // The adjacency inputs, which no MeshDrawMode names.
            return false;
    }
}

// The line in pbr_frag and shadow_depth_frag where a surface hook's GLSL goes (spec 13.29): after
// every declaration the hook may name, before main().
#define PBR_SURFACE_HOOK_MARKER "// CETRA_SURFACE_HOOK_CHUNK"
// And in object_position.glsl, where an offset hook's goes, in every stage that includes it.
#define PBR_OFFSET_HOOK_MARKER "// CETRA_OFFSET_HOOK_CHUNK"

// The parts of a hook a stage may take.
enum { HOOK_PART_SURFACE = 1u, HOOK_PART_OFFSET = 2u };

static unsigned _hook_parts(const ShaderHook* hook) {
    return !hook
               ? 0u
               : (hook->surface ? HOOK_PART_SURFACE : 0u) | (hook->offset ? HOOK_PART_OFFSET : 0u);
}

// `base` with `defines`, then the define of each hook part in `defined`, then the source of each
// part in `spliced` at its marker. A stage holding a part's marker tests that part's define there,
// so it takes the source exactly when it takes the define; the one stage that takes a define
// alone is the shadow vertex stage, where the surface's define declares the world position its
// alpha reads, and that stage has no surface marker. NULL, logged, on a failure.
static char* _hooked_stage(const char* base, const char* defines, const ShaderHook* hook,
                           unsigned defined, unsigned spliced) {
    char all[160];
    snprintf(all, sizeof(all), "%s%s%s", defines ? defines : "",
             (defined & HOOK_PART_SURFACE) ? "#define CETRA_SURFACE_HOOK 1\n" : "",
             (defined & HOOK_PART_OFFSET) ? "#define CETRA_OFFSET_HOOK 1\n" : "");
    char* source = shader_source_with_defines(base, all);
    const struct {
        unsigned part;
        const char* marker;
        const char* chunk;
    } splices[] = {
        {HOOK_PART_SURFACE, PBR_SURFACE_HOOK_MARKER, hook ? hook->surface : NULL},
        {HOOK_PART_OFFSET, PBR_OFFSET_HOOK_MARKER, hook ? hook->offset : NULL},
    };
    for (size_t i = 0; source && i < sizeof(splices) / sizeof(splices[0]); i++) {
        if (!(spliced & splices[i].part))
            continue;
        char* with = shader_source_splice(source, splices[i].marker, splices[i].chunk);
        free(source);
        source = with;
    }
    if (!source && hook)
        log_error("shader hook '%s' could not be put into a stage", hook->name);
    return source;
}

// Both stages of the variant carrying exactly `features`, `hook`'s parts each in the stage it
// belongs to: the offset where the position is made, the surface where it is shaded. False, both
// NULL, on a failure.
static bool _pbr_variant_sources(PbrFamily family, unsigned features, const ShaderHook* hook,
                                 char** vert, char** frag) {
    // ONE line, carrying the mask itself. The bits are shared with the shader
    // through pbr_features.glsl, so this cannot drift from what the gates test
    // -- where emitting a set of macro NAMES could, silently and invisibly.
    //
    // Subtractive still, but now structurally: pbr_features.glsl defaults
    // CETRA_PBR_FEATURES to PBR_FEAT_ALL when it is undefined, so a source
    // compiled with no defines at all is the uber-shader rather than a variant
    // with every feature stripped. Both families go through here now, so nothing
    // relies on that default -- but it is what makes the failure direction safe.
    //
    // A surface hook is NOT a bit of the mask, and cannot be (spec 13.29): a bit
    // switches off code the shader already holds, where a hook brings code in.
    // Defaulted on in the uber-shader it would call a function nobody supplied.
    // So its define arrives with its source, from _hooked_stage.
    char mask[48];
    snprintf(mask, sizeof(mask), "#define CETRA_PBR_FEATURES %u\n", features);
    const unsigned parts = _hook_parts(hook);
    // The families differ in the VERTEX stage and nowhere else: same pbr_frag,
    // same mask, same gates. That is what makes a second family an argument here
    // rather than a second copy of the feature system.
    // The mask into BOTH stages: the fur bit gates vertex code too, and a vertex stage that never
    // saw the mask would default it to the full set and carry that code into every variant.
    *vert = _hooked_stage(family == PBR_FAMILY_SKINNED ? pbr_skinned_vert_shader_str
                                                       : pbr_vert_shader_str,
                          mask, hook, parts & HOOK_PART_OFFSET, parts & HOOK_PART_OFFSET);
    *frag = _hooked_stage(pbr_frag_shader_str, mask, hook, parts & HOOK_PART_SURFACE,
                          parts & HOOK_PART_SURFACE);
    if (*vert && *frag)
        return true;
    free(*vert);
    free(*frag);
    *vert = *frag = NULL;
    return false;
}

static bool _stage_compiles(ShaderType type, const char* source) {
    Shader* shader = create_shader(type, source);
    const bool ok = shader && compile_shader(shader);
    free_shader(shader);
    return ok;
}

bool pbr_hook_compiles(const ShaderHook* hook) {
    // Both ends of the mask, since a hook naming something a feature declares -- a sampler, or a
    // chunk the host includes only under the feature's bit -- compiles at every feature and not at
    // none, and a material's variant may be either. The skinned vertex stage too when it moves
    // vertices; the families share the fragment stage.
    const unsigned ends[] = {PBR_FEAT_ALL, 0u};
    bool ok = true;
    for (size_t e = 0; ok && e < sizeof(ends) / sizeof(ends[0]); e++) {
        for (int family = PBR_FAMILY_RIGID; ok && family <= PBR_FAMILY_SKINNED; family++) {
            const bool skinned = family == PBR_FAMILY_SKINNED;
            if (skinned && !hook->offset)
                continue;
            char *vert, *frag;
            if (!_pbr_variant_sources((PbrFamily)family, ends[e], hook, &vert, &frag))
                return false;
            ok = _stage_compiles(VERTEX_SHADER, vert) &&
                 (skinned || _stage_compiles(FRAGMENT_SHADER, frag));
            free(vert);
            free(frag);
        }
    }
    return ok;
}

// Build the variant carrying exactly `features`. Static: every caller goes
// through engine_pbr_variant below, so the name is formatted in one place and is
// only ever a cache key.
static ShaderProgram* _create_pbr_variant(const char* name, PbrFamily family, unsigned features,
                                          const struct ShaderHook* hook) {
    char *vert, *frag;
    if (!_pbr_variant_sources(family, features, hook, &vert, &frag))
        return NULL;
    ShaderProgram* program = create_program_from_source(name, vert, frag, NULL);
    free(vert);
    free(frag);

    if (program == NULL) {
        log_error("Failed to initialize PBR shader program (features %u)", features);
        return NULL;
    }
    program->pbr_features = (int)features;
    program->pbr_family = family;
    program->pbr_hook = hook;

    // Both families take their clip position from object_position.glsl, the same
    // chunk depth_prepass_vert uses, and declare `invariant gl_Position`; the
    // skinned one's skinning matches the prepass's because both call skin.glsl's
    // skinMatrix on the same uniforms. Set here rather than by the caller because
    // every variant owes an answer, and
    // one that missed it lets the prepass stamp depth with no coverage test --
    // which deletes alpha-masked geometry rather than shading it wrong, and is
    // invisible in whichever variant happened to get tested.
    //
    // The one exception is the skinned stage with the fur bit: its shell code sits between the
    // skinning and gl_Position, and on this driver code that never runs for the skin still
    // re-lowers the position the prepass would have to match, so the skin would lose to its
    // own prepass depth.
    //
    // `instanced` is not set here and never was: setup_program_uniforms resolves
    // it from the linked program, for every program in the engine.
    program->fur_shells = family == PBR_FAMILY_SKINNED && (features & PBR_FEAT_FUR);
    // An offset hook moves the position too (spec 13.29), and the lean prepass has no hook in it,
    // so the surface would lose to its own prepass depth. The masked prepass runs this program
    // and needs no exemption.
    program->depth_prepass_safe = !program->fur_shells && !(hook && hook->offset);

    // Parsed by scripts/gates.py::_PBR_VARIANT. It is the only way from outside
    // to see which variant a scene resolved to, because a correct variant and
    // the uber-shader are the same picture.
    //
    // `samplers` is APPENDED rather than woven in, so the existing regex keeps
    // matching. It is the whole instrument for spec 11.95's question: whether a
    // declaration whose reads this mask compiled away still spends a unit.
    log_info("pbr variant %s: features %u of %u samplers %d", name, features, PBR_FEAT_ALL,
             program->sampler_count);
    return program;
}

// The bare family name for the full set, so the cache -- and every log line and
// app lookup that already says pbr or pbr_skinned -- keeps meaning what it
// meant. ONE function because this string is the program cache's key: a second
// site spelling it differently would miss the lookup forever and compile and
// leak a fresh 2,500-line program every frame, rendering correctly the whole
// time.
void pbr_variant_name(PbrFamily family, unsigned features, const struct ShaderHook* hook, char* out,
                      size_t n) {
    const char* prefix =
        family == PBR_FAMILY_SKINNED ? CETRA_PROGRAM_PBR_SKINNED : CETRA_PROGRAM_PBR;
    if (hook)
        snprintf(out, n, "%s-%u-h%u", prefix, features, hook->id);
    else if (features == PBR_FEAT_ALL)
        snprintf(out, n, "%s", prefix);
    else
        snprintf(out, n, "%s-%u", prefix, features);
}

ShaderProgram* create_pbr_program_variant(PbrFamily family, unsigned features,
                                          const struct ShaderHook* hook) {
    char name[PBR_VARIANT_NAME_MAX];
    pbr_variant_name(family, features, hook, name, sizeof(name));
    return _create_pbr_variant(name, family, features, hook);
}

ShaderProgram* create_pbr_program() {
    return create_pbr_program_variant(PBR_FAMILY_RIGID, PBR_FEAT_ALL, NULL);
}

ShaderProgram* create_particle_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source(CETRA_PROGRAM_PARTICLE, particle_vert_shader_str,
                                              particle_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize particle shader program");
        return NULL;
    }

    return program;
}

// GPU particle-sim UPDATE program (spec 5.2). Vertex-only: it captures its
// outputs into a transform-feedback buffer under GL_RASTERIZER_DISCARD and never
// rasterizes, so create_program_from_source (which requires a fragment shader)
// can't build it. Assemble it from the primitives and set the feedback varyings
// between attach and link.
ShaderProgram* create_particle_sim_program() {
    ShaderProgram* program = create_program("particle_sim");
    if (!program) {
        log_error("Failed to create particle sim program");
        return NULL;
    }

    Shader* vs = create_shader(VERTEX_SHADER, particle_sim_vert_shader_str);
    if (!vs || !compile_shader(vs)) {
        log_error("Particle sim vertex shader compilation failed");
        free_program(program);
        return NULL;
    }
    attach_shader_to_program(program, vs);

    // Capture the 5 out vec4s interleaved into one buffer. Order MUST match the
    // ParticleGpuState field layout (particle_sim.h). Must precede linking.
    const char* varyings[] = {"oCenter", "oParams", "oColor", "oVelAge", "oLife"};
    glTransformFeedbackVaryings(program->id, 5, varyings, GL_INTERLEAVED_ATTRIBS);

    if (!link_program(program)) {
        log_error("Particle sim program linking failed");
        free_program(program);
        return NULL;
    }

    setup_program_uniforms(program);
    return program;
}

// The wind bound's instrument (spec 11.54). Vertex-only for the same reason the
// particle sim above is, and assembled the same way -- the feedback varying has
// to be named between attach and link.
//
// Built on demand by wind_bound_probe and freed straight after: it runs once at
// the end of a run behind a flag, so putting it in the engine's program cache
// would cost every session a compile for a diagnostic almost nobody asks for.
ShaderProgram* create_wind_probe_program() {
    ShaderProgram* program = create_program("wind_probe");
    if (!program) {
        log_error("Failed to create wind probe program");
        return NULL;
    }

    Shader* vs = create_shader(VERTEX_SHADER, wind_probe_vert_shader_str);
    if (!vs || !compile_shader(vs)) {
        log_error("Wind probe vertex shader compilation failed");
        free_program(program);
        return NULL;
    }
    attach_shader_to_program(program, vs);

    const char* varyings[] = {"oOffset"};
    glTransformFeedbackVaryings(program->id, 1, varyings, GL_INTERLEAVED_ATTRIBS);

    if (!link_program(program)) {
        log_error("Wind probe program linking failed");
        free_program(program);
        return NULL;
    }

    setup_program_uniforms(program);
    return program;
}

ShaderProgram* create_pbr_skinned_program() {
    return create_pbr_program_variant(PBR_FAMILY_SKINNED, PBR_FEAT_ALL, NULL);
}

ShaderProgram* create_shape_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source(CETRA_PROGRAM_SHAPE, shape_vert_shader_str,
                                              shape_frag_shader_str, shape_geo_shader_str)) ==
        NULL) {
        log_error("Failed to initialize shape shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_xyz_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source(CETRA_PROGRAM_XYZ, xyz_vert_shader_str,
                                              xyz_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize xyz shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_rain_program() {
    ShaderProgram* program = create_program_from_source(CETRA_PROGRAM_RAIN, rain_vert_shader_str,
                                                        rain_frag_shader_str, NULL);
    if (!program)
        log_error("Failed to initialize rain shader program");
    return program;
}

ShaderProgram* create_fire_advect_program() {
    return create_post_program("fire_advect", fire_advect_frag_shader_str);
}

ShaderProgram* create_fire_correct_program() {
    return create_post_program("fire_correct", fire_correct_frag_shader_str);
}

ShaderProgram* create_fire_curl_program() {
    return create_post_program("fire_curl", fire_curl_frag_shader_str);
}

ShaderProgram* create_fire_react_program() {
    return create_post_program("fire_react", fire_react_frag_shader_str);
}

ShaderProgram* create_fire_divergence_program() {
    return create_post_program("fire_divergence", fire_divergence_frag_shader_str);
}

ShaderProgram* create_fire_jacobi_program() {
    return create_post_program("fire_jacobi", fire_jacobi_frag_shader_str);
}

ShaderProgram* create_fire_project_program() {
    return create_post_program("fire_project", fire_project_frag_shader_str);
}

ShaderProgram* create_fire_reduce_program() {
    return create_post_program("fire_reduce", fire_reduce_frag_shader_str);
}

ShaderProgram* create_fire_sum_program() {
    return create_post_program("fire_sum", fire_sum_frag_shader_str);
}

ShaderProgram* create_fire_slice_program() {
    return create_post_program("fire_slice", fire_slice_frag_shader_str);
}

ShaderProgram* create_fire_march_program() {
    ShaderProgram* program = create_program_from_source("fire_march", fire_march_vert_shader_str,
                                                        fire_march_frag_shader_str, NULL);
    if (!program)
        log_error("Failed to initialize fire_march shader program");
    return program;
}

ShaderProgram* create_fire_card_program() {
    ShaderProgram* program = create_program_from_source("fire_card", fire_card_vert_shader_str,
                                                        fire_card_frag_shader_str, NULL);
    if (!program)
        log_error("Failed to initialize fire_card shader program");
    return program;
}

ShaderProgram* create_shadow_depth_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("shadow_depth", shadow_depth_vert_shader_str,
                                              shadow_depth_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize shadow depth shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_shadow_hook_program(const struct ShaderHook* hook, bool absorb, bool cut) {
    if (!hook)
        return NULL;
    // The offset into the vertex stage the two shadow programs share; the surface's alpha into
    // the depth program only, since the absorb program's coverage is a translucency rather than
    // a cut. The surface define reaches the vertex stage as well, which hands the hook its world
    // position: a varying one stage declares and the other does not fails the link.
    const unsigned parts = _hook_parts(hook);
    const unsigned surface = !absorb && cut ? parts & HOOK_PART_SURFACE : 0u;
    char* vert = _hooked_stage(shadow_depth_vert_shader_str, NULL, hook,
                               (parts & HOOK_PART_OFFSET) | surface, parts & HOOK_PART_OFFSET);
    char* frag =
        _hooked_stage(absorb ? shadow_absorb_frag_shader_str : shadow_depth_frag_shader_str, NULL,
                      hook, surface, surface);
    char name[PBR_VARIANT_NAME_MAX];
    snprintf(name, sizeof(name), "%s-h%u", absorb ? "shadow_absorb" : "shadow_depth", hook->id);
    ShaderProgram* program =
        vert && frag ? create_program_from_source(name, vert, frag, NULL) : NULL;
    free(vert);
    free(frag);
    if (!program)
        log_error("shader hook '%s': its %s program%s does not build", hook->name, name,
                  surface ? " with the surface's alpha" : "");
    return program;
}

ShaderProgram* create_depth_prepass_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("depth_prepass", depth_prepass_vert_shader_str,
                                              depth_prepass_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize depth prepass shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_water_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("water", water_vert_shader_str, water_frag_shader_str,
                                              NULL)) == NULL) {
        log_error("Failed to initialize water shader program");
        return NULL;
    }

    return program;
}

// The two spectral passes: fullscreen quads over a 128^2 target with two MRT outputs.
// Through create_post_program like every other fullscreen pass -- an earlier comment here
// claimed that helper was "built for the post chain's single-target passes" and bypassed
// it, which is not what it does. It only picks the vertex shader; MRT lives in the
// fragment shader's `out` declarations and the caller's glDrawBuffers, and
// dof_gather_frag already ships two targets through it.
ShaderProgram* create_water_spectrum_program() {
    return create_post_program("water_spectrum", water_spectrum_frag_shader_str);
}

ShaderProgram* create_water_fft_program() {
    return create_post_program("water_fft", water_fft_frag_shader_str);
}

ShaderProgram* create_water_foam_program() {
    return create_post_program("water_foam", water_foam_frag_shader_str);
}

ShaderProgram* create_water_probe_program() {
    return create_post_program("water_probe", water_probe_frag_shader_str);
}

ShaderProgram* create_water_caustic_land_program() {
    return create_post_program("water_caustic_land", water_caustic_land_frag_shader_str);
}

ShaderProgram* create_water_touch_program() {
    return create_post_program("water_touch", water_touch_frag_shader_str);
}

ShaderProgram* create_water_touch_normals_program() {
    return create_post_program("water_touch_normals", water_touch_normals_frag_shader_str);
}

ShaderProgram* create_water_caustic_program() {
    ShaderProgram* program =
        create_program_from_source("water_caustic", water_caustic_vert_shader_str,
                                   water_caustic_frag_shader_str, water_caustic_geo_shader_str);
    if (!program)
        log_error("Failed to initialize water_caustic shader program");
    return program;
}

ShaderProgram* create_skybox_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("skybox", skybox_vert_shader_str,
                                              skybox_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize skybox shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_ibl_equirect_to_cube_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("ibl_equirect_to_cube", ibl_cubemap_vert_shader_str,
                                              ibl_equirect_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize IBL equirect-to-cube shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_ibl_irradiance_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("ibl_irradiance", ibl_cubemap_vert_shader_str,
                                              ibl_irradiance_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize IBL irradiance shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_ibl_prefilter_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("ibl_prefilter", ibl_cubemap_vert_shader_str,
                                              ibl_prefilter_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize IBL prefilter shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_ibl_charlie_prefilter_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("ibl_charlie_prefilter", ibl_cubemap_vert_shader_str,
                                              ibl_charlie_prefilter_frag_shader_str, NULL)) ==
        NULL) {
        log_error("Failed to initialize IBL Charlie prefilter shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_ibl_brdf_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("ibl_brdf", post_vert_shader_str,
                                              ibl_brdf_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize IBL BRDF shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_sky_transmittance_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("sky_transmittance", post_vert_shader_str,
                                              sky_transmittance_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize sky transmittance shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_sky_multiscatter_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("sky_multiscatter", post_vert_shader_str,
                                              sky_multiscatter_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize sky multiscatter shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_sky_debug_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("sky_debug", post_vert_shader_str,
                                              sky_debug_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize sky debug shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_sky_view_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("sky_view", post_vert_shader_str,
                                              sky_view_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize sky view shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_mask_copy_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("mask_copy", post_vert_shader_str,
                                              mask_copy_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize mask copy shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_layers_vt_bake_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("layers_vt_bake", post_vert_shader_str,
                                              layers_vt_bake_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize layers VT bake shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_layers_vt_feedback_program() {
    ShaderProgram* program = NULL;

    if ((program =
             create_program_from_source("layers_vt_feedback", layers_vt_feedback_vert_shader_str,
                                        layers_vt_feedback_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize layers VT feedback shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_msm_resolve_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("msm_resolve", post_vert_shader_str,
                                              msm_resolve_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize moment shadow resolve shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_shadow_absorb_program() {
    ShaderProgram* program = NULL;

    // shadow_depth's vertex stage, not a copy of it. Skinning and wind must
    // displace an absorbance caster exactly as they displace the same surface
    // in the depth pass, and the only way two files stay identical is to be
    // one file -- the copy this replaced had already drifted in its comments.
    if ((program = create_program_from_source("shadow_absorb", shadow_depth_vert_shader_str,
                                              shadow_absorb_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize translucent shadow absorb shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_tsm_resolve_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("tsm_resolve", post_vert_shader_str,
                                              tsm_resolve_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize translucent shadow resolve shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_sky_env_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("sky_env", ibl_cubemap_vert_shader_str,
                                              sky_env_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize sky env shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_sky_aerial_program() {
    return create_post_program("sky_aerial", aerial_lut_frag_shader_str);
}

ShaderProgram* create_cloud_noise_debug_program() {
    return create_post_program("cloud_noise_debug", cloud_noise_debug_frag_shader_str);
}

ShaderProgram* create_cloud_march_program() {
    return create_post_program("cloud_march", cloud_march_frag_shader_str);
}

ShaderProgram* create_cloud_shadow_program() {
    return create_post_program("cloud_shadow", cloud_shadow_frag_shader_str);
}

ShaderProgram* create_sky_env_clouds_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("sky_env_clouds", ibl_cubemap_vert_shader_str,
                                              sky_env_clouds_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize sky env clouds shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_sky_background_clouds_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("sky_background_clouds", skybox_vert_shader_str,
                                              sky_background_clouds_frag_shader_str, NULL)) ==
        NULL) {
        log_error("Failed to initialize sky background clouds shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_sky_background_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("sky_background", skybox_vert_shader_str,
                                              sky_background_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize sky background shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_text_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("text", text_vert_shader_str, text_frag_shader_str,
                                              NULL)) == NULL) {
        log_error("Failed to initialize text shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_ui_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("ui", ui_vert_shader_str, ui_frag_shader_str,
                                              NULL)) == NULL) {
        log_error("Failed to initialize ui shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_ui_draw_program(const char* name, const char* frag_source) {
    ShaderProgram* program =
        create_program_from_source(name, ui_vert_shader_str, frag_source, NULL);
    if (!program)
        log_error("Failed to initialize ui draw program %s", name ? name : "?");
    return program;
}

ShaderProgram* create_bone_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("bone", bone_vert_shader_str, bone_frag_shader_str,
                                              NULL)) == NULL) {
        log_error("Failed to initialize bone shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_shadow_catcher_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("shadow_catcher", catcher_vert_shader_str,
                                              catcher_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize shadow catcher shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_bloom_bright_program() {
    return create_post_program("bloom_bright", bloom_bright_frag_shader_str);
}

ShaderProgram* create_glare_source_program() {
    return create_post_program("glare_source", glare_source_frag_shader_str);
}

ShaderProgram* create_glare_fft_program() {
    return create_post_program("glare_fft", glare_fft_frag_shader_str);
}

ShaderProgram* create_glare_multiply_program() {
    return create_post_program("glare_multiply", glare_multiply_frag_shader_str);
}

ShaderProgram* create_glare_output_program() {
    return create_post_program("glare_output", glare_output_frag_shader_str);
}

ShaderProgram* create_crt_resample_program() {
    return create_post_program("crt_resample", crt_resample_frag_shader_str);
}

ShaderProgram* create_crt_program() {
    return create_post_program("crt", crt_frag_shader_str);
}

ShaderProgram* create_loading_logo_program() {
    return create_post_program("loading_logo", loading_logo_frag_shader_str);
}

ShaderProgram* create_loading_blur_program() {
    return create_post_program("loading_blur", loading_blur_frag_shader_str);
}

ShaderProgram* create_loading_tape_program() {
    return create_post_program("loading_tape", loading_tape_frag_shader_str);
}

ShaderProgram* create_le_half_program() {
    return create_post_program("le_half", le_half_frag_shader_str);
}

ShaderProgram* create_le_bins_program() {
    return create_post_program("le_bins", le_bins_frag_shader_str);
}

ShaderProgram* create_le_grid_program() {
    return create_post_program("le_grid", le_grid_frag_shader_str);
}

ShaderProgram* create_le_grid_blur_program() {
    return create_post_program("le_grid_blur", le_grid_blur_frag_shader_str);
}

ShaderProgram* create_le_block_sum_program() {
    return create_post_program("le_block_sum", le_block_sum_frag_shader_str);
}

ShaderProgram* create_le_block_program() {
    return create_post_program("le_block", le_block_frag_shader_str);
}

ShaderProgram* create_le_blur_program() {
    return create_post_program("le_blur", le_blur_frag_shader_str);
}

ShaderProgram* create_bloom_down_program() {
    return create_post_program("bloom_downsample", bloom_downsample_frag_shader_str);
}

// Same tent source as the SSR/fog composite, but its own program object:
// bloom re-uploads texelSize per pyramid level, the shared program's is
// set once at init
ShaderProgram* create_bloom_up_program() {
    return create_post_program("bloom_upsample", upsample_tent_frag_shader_str);
}

ShaderProgram* create_lens_flare_program() {
    return create_post_program("lens_flare", lens_flare_frag_shader_str);
}

ShaderProgram* create_tonemap_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("tonemap", post_vert_shader_str,
                                              tonemap_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize tonemap shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_spec_occ_composite_program() {
    return create_post_program("spec_occ_composite", spec_occ_composite_frag_shader_str);
}

ShaderProgram* create_gtao_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("gtao", post_vert_shader_str, gtao_frag_shader_str,
                                              NULL)) == NULL) {
        log_error("Failed to initialize GTAO shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_ssao_blur_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("ssao_blur", post_vert_shader_str,
                                              ssao_blur_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize SSAO blur shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_ssr_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("ssr", post_vert_shader_str, ssr_frag_shader_str,
                                              NULL)) == NULL) {
        log_error("Failed to initialize SSR shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_ssr_hiz_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("ssr_hiz", post_vert_shader_str,
                                              ssr_hiz_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize SSR hi-z shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_upsample_tent_program() {
    return create_post_program("upsample_tent", upsample_tent_frag_shader_str);
}

ShaderProgram* create_ssr_fold_wet_program() {
    return create_post_program("ssr_fold_wet", ssr_fold_wet_frag_shader_str);
}

ShaderProgram* create_taa_resolve_program() {
    ShaderProgram* program = NULL;

    if ((program = create_program_from_source("taa_resolve", post_vert_shader_str,
                                              taa_resolve_frag_shader_str, NULL)) == NULL) {
        log_error("Failed to initialize TAA resolve shader program");
        return NULL;
    }

    return program;
}

ShaderProgram* create_taau_resolve_program() {
    return create_post_program("taau_resolve", taau_resolve_frag_shader_str);
}

ShaderProgram* create_temporal_accum_program() {
    return create_post_program("temporal_accum", temporal_accum_frag_shader_str);
}

ShaderProgram* create_ssgi_composite_program() {
    return create_post_program("ssgi_composite", ssgi_composite_frag_shader_str);
}

// Fullscreen post-pass program: the shared post vertex shader plus a fragment
// source. Every postfx constructor is this call with a different pair.
static ShaderProgram* create_post_program(const char* name, const char* frag_src) {
    ShaderProgram* program = create_program_from_source(name, post_vert_shader_str, frag_src, NULL);
    if (!program)
        log_error("Failed to initialize %s shader program", name ? name : "?");
    return program;
}

ShaderProgram* create_post_pass_program(const char* name, const char* frag_source) {
    return create_post_program(name, frag_source);
}

ShaderProgram* create_present_program() {
    return create_post_program("present", present_frag_shader_str);
}

ShaderProgram* create_late_surface_program(const char* name, const char* frag_source) {
    ShaderProgram* program =
        create_program_from_source(name, late_surface_vert_shader_str, frag_source, NULL);
    if (!program) {
        log_error("Failed to initialize late surface program %s", name ? name : "?");
        return NULL;
    }
    program->late_surface = true;
    return program;
}

ShaderProgram* create_ssgi_accum_program() {
    return create_post_program("ssgi_accum", ssgi_accum_frag_shader_str);
}

ShaderProgram* create_ssgi_atrous_program() {
    return create_post_program("ssgi_atrous", ssgi_atrous_frag_shader_str);
}

ShaderProgram* create_ssr_atrous_program() {
    return create_post_program("ssr_atrous", ssr_atrous_frag_shader_str);
}

ShaderProgram* create_ssr_accum_program() {
    return create_post_program("ssr_accum", ssr_accum_frag_shader_str);
}

// The froxel fog trio (spec 9.5). All three are ordinary fullscreen passes --
// the volume is written one slice per draw, so they need no geometry shader and
// share the standard post vertex shader.
ShaderProgram* create_froxel_inject_program() {
    return create_post_program("froxel_inject", froxel_inject_frag_shader_str);
}

// The same source compiled for the miss probe's count (spec 13.45), whose main is the one under
// FROXEL_HISTORY_MISS.
ShaderProgram* create_froxel_miss_probe_program() {
    char* source = shader_source_with_defines(froxel_inject_frag_shader_str,
                                              "#define FROXEL_HISTORY_MISS 1\n");
    if (!source)
        return NULL;
    ShaderProgram* program = create_post_program("froxel_miss_probe", source);
    free(source);
    return program;
}

ShaderProgram* create_froxel_integrate_program() {
    return create_post_program("froxel_integrate", froxel_integrate_frag_shader_str);
}

ShaderProgram* create_froxel_composite_program() {
    return create_post_program("froxel_composite", froxel_composite_frag_shader_str);
}

ShaderProgram* create_fog_esm_program() {
    return create_post_program("fog_esm", fog_esm_frag_shader_str);
}

// GI probe projection (spec 9.7). Writes a sub-rectangle of the probe atlas
// picked by glViewport rather than a whole target, which is why it is an
// ordinary fullscreen-quad pass despite drawing a 10x10 tile.
ShaderProgram* create_gi_project_program() {
    return create_post_program("gi_project", gi_project_frag_shader_str);
}

// Specular probe projection (spec 11.70). Same sub-rectangle-by-viewport shape
// as the GI tile above, one row of roughness at a time.
ShaderProgram* create_probe_project_program() {
    return create_post_program("probe_project", probe_project_frag_shader_str);
}

ShaderProgram* create_motion_blur_program() {
    return create_post_program("motion_blur", motion_blur_frag_shader_str);
}

ShaderProgram* create_motion_blur_tilemax_program() {
    return create_post_program("motion_blur_tilemax", motion_blur_tilemax_frag_shader_str);
}

ShaderProgram* create_motion_blur_neighbormax_program() {
    return create_post_program("motion_blur_neighbormax", motion_blur_neighbormax_frag_shader_str);
}

ShaderProgram* create_sss_gather_program() {
    return create_post_program("sss_gather", sss_gather_frag_shader_str);
}

ShaderProgram* create_sss_pyr_seed_program() {
    return create_post_program("sss_pyr_seed", sss_pyr_seed_frag_shader_str);
}

ShaderProgram* create_sss_pyr_down_program() {
    return create_post_program("sss_pyr_down", sss_pyr_down_frag_shader_str);
}

ShaderProgram* create_contact_shadow_program() {
    return create_post_program("contact_shadow", contact_shadow_frag_shader_str);
}

ShaderProgram* create_oit_resolve_program() {
    return create_post_program("oit_resolve", oit_resolve_frag_shader_str);
}

ShaderProgram* create_lum_measure_program() {
    return create_post_program("lum_measure", lum_measure_frag_shader_str);
}

ShaderProgram* create_lum_histogram_program() {
    return create_post_program("lum_histogram", lum_histogram_frag_shader_str);
}

ShaderProgram* create_lum_reduce_program() {
    return create_post_program("lum_reduce", lum_reduce_frag_shader_str);
}

ShaderProgram* create_dof_coc_program() {
    return create_post_program("dof_coc", dof_coc_frag_shader_str);
}

ShaderProgram* create_dof_tile_program() {
    return create_post_program("dof_tile", dof_tile_frag_shader_str);
}

ShaderProgram* create_dof_dilate_program() {
    return create_post_program("dof_dilate", dof_dilate_frag_shader_str);
}

ShaderProgram* create_dof_gather_program() {
    return create_post_program("dof_gather", dof_gather_frag_shader_str);
}

ShaderProgram* create_dof_composite_program() {
    return create_post_program("dof_composite", dof_composite_frag_shader_str);
}
