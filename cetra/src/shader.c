#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "ext/log.h"
#include "shader.h"
#include "util.h"

// The include/ chunks, unexpanded, by name. The only file that reads them.
#include "shader_includes.h"

char* _read_shader_source(const char* filePath) {
    long length = 0;
    char* buffer = read_entire_file(filePath, &length);
    if (!buffer) {
        log_error("Failed to read shader file: %s", filePath);
        return NULL;
    }
    if (length > 10 * 1024 * 1024) { // Max 10MB
        log_error("Shader file too large: %s", filePath);
        free(buffer);
        return NULL;
    }
    return buffer;
}

// The source with `defines` spliced in after its `#version` line, or a plain
// copy when there are none. Caller owns the result.
//
// A source with no `#version` gets the block at the FRONT. That is not a
// tolerated edge case -- GLSL without a version directive is 1.10, where none of
// this engine's shaders compile, so the only way to reach it is a truncated or
// misidentified string, and putting the defines first keeps the driver's error
// pointing at line 1 of the real problem rather than at a define block.
char* shader_source_with_defines(const char* source, const char* defines) {
    if (!source)
        return NULL;
    if (!defines || !*defines)
        return safe_strdup(source);

    // The directive must START a line. strstr alone finds the token anywhere,
    // and pbr_frag includes a chunk whose comment contains the literal word
    // "#version" -- so on a source whose real directive was missing or malformed
    // it would have spliced the block into the middle of that include.
    const char* line = source;
    const char* version = NULL;
    while (*line) {
        const char* t = line;
        while (*t == ' ' || *t == '\t')
            ++t;
        if (strncmp(t, "#version", 8) == 0) {
            version = t;
            break;
        }
        const char* nl = strchr(line, '\n');
        if (!nl)
            break;
        line = nl + 1;
    }

    const char* body = source;
    size_t head = 0;
    if (version) {
        const char* eol = strchr(version, '\n');
        if (eol) {
            head = (size_t)(eol - source) + 1;
            body = eol + 1;
        }
    }

    // The body's line numbers must not move. gen_shader_header.py preserves them
    // deliberately -- it emits a bare newline for every blank line rather than
    // dropping it, because a GLSL error reports a line number and nothing here
    // dumps the source to compare against. Shifting the body by the height of
    // the define block would put every error in this file off by that much, and
    // pbr_frag now compiles many ways, so a variant-only error is a thing that
    // can exist. The generator's own note that #line "was tried and removed"
    // does not transfer: that was about mapping errors back to N chunk files,
    // where this is one file at a constant offset.
    //
    // The body resumes at the line after #version, which is line 2.
    const char* resume = "#line 2\n";
    size_t defines_len = strlen(defines);
    size_t resume_len = strlen(resume);
    size_t body_len = strlen(body);
    // +1 for a newline after the block, so a defines string without a trailing
    // one cannot weld itself to the directive that follows it.
    size_t out_len = head + defines_len + 1 + resume_len + body_len;
    char* out = malloc(out_len + 1);
    if (!out) {
        log_error("Failed to allocate spliced shader source");
        return NULL;
    }
    char* w = out;
    memcpy(w, source, head);
    w += head;
    memcpy(w, defines, defines_len);
    w += defines_len;
    *w++ = '\n';
    memcpy(w, resume, resume_len);
    w += resume_len;
    memcpy(w, body, body_len);
    w += body_len;
    *w = '\0';
    return out;
}

typedef struct IncludeBuffer {
    char* data;
    size_t len, cap;
} IncludeBuffer;

static bool _include_append(IncludeBuffer* b, const char* s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (b->len + n + 1 > cap)
            cap *= 2;
        char* grown = realloc(b->data, cap);
        if (!grown) {
            log_error("Failed to grow an expanded shader source");
            return false;
        }
        b->data = grown;
        b->cap = cap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return true;
}

static bool _include_appendf(IncludeBuffer* b, const char* fmt, ...) {
    char line[160];
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    return n > 0 && (size_t)n < sizeof(line) && _include_append(b, line, (size_t)n);
}

// `seen` is per top-level shader and is what makes a chunk included twice -- directly, or once
// directly and once through another chunk -- a single copy, since GLSL has no include guards.
// It is also the cycle guard: a chunk is marked before its own lines are read.
static bool _include_expand(IncludeBuffer* out, const char* source, bool* seen, bool top) {
    int number = 0;
    for (const char* line = source; *line;) {
        const char* eol = strchr(line, '\n');
        const size_t len = eol ? (size_t)(eol - line) + 1 : strlen(line);
        number++;
        const char* t = line;
        while (*t == ' ' || *t == '\t')
            t++;
        if (strncmp(t, "#include", 8) != 0) {
            if (!_include_append(out, line, len))
                return false;
            line += len;
            continue;
        }
        const char* open = strchr(t, '"');
        const char* close = open && open < line + len ? strchr(open + 1, '"') : NULL;
        if (!close || close >= line + len) {
            log_error("Malformed #include on line %d: %.*s", number, (int)len, line);
            return false;
        }
        const size_t name_len = (size_t)(close - open - 1);
        int chunk = -1;
        for (int i = 0; i < shader_include_chunk_count; i++) {
            const char* candidate = shader_include_chunks[i].name;
            if (strlen(candidate) == name_len && strncmp(candidate, open + 1, name_len) == 0) {
                chunk = i;
                break;
            }
        }
        if (chunk < 0) {
            log_error("#include \"%.*s\" on line %d names no engine chunk", (int)name_len, open + 1,
                      number);
            return false;
        }
        const char* name = shader_include_chunks[chunk].name;
        if (seen[chunk]) {
            if (!_include_appendf(out, "// #include \"%s\" (already expanded)\n", name))
                return false;
        } else {
            seen[chunk] = true;
            if (!_include_appendf(out, "// ---- begin %s ----\n", name) ||
                !_include_expand(out, shader_include_chunks[chunk].source, seen, false) ||
                !_include_appendf(out, "// ---- end %s ----\n", name))
                return false;
            // Back to the including file's own numbering, so an error in an app's shader names
            // its line rather than that line plus every chunk above it. Only at the top: a
            // number inside a chunk could not say WHICH chunk anyway, which is why the build-
            // time expansion carries none (gen_shader_header.py).
            if (top && !_include_appendf(out, "#line %d\n", number + 1))
                return false;
        }
        line += len;
    }
    return true;
}

// Every chunk the build already expanded into `source`, by the marker gen_shader_header.py leaves
// at its head, counted as included. An app's chunk spliced into an engine shader (spec 13.29)
// can then include noise.glsl when the shader around it already has, and get one copy.
static void _include_mark_expanded(const char* source, bool* seen) {
    static const char head[] = "// ---- begin ";
    for (const char* at = strstr(source, head); at; at = strstr(at + 1, head)) {
        const char* name = at + sizeof(head) - 1;
        const char* end = strstr(name, " ----");
        const char* eol = strchr(name, '\n');
        if (!end || (eol && eol < end))
            continue;
        for (int i = 0; i < shader_include_chunk_count; i++) {
            const char* candidate = shader_include_chunks[i].name;
            if (strlen(candidate) == (size_t)(end - name) &&
                strncmp(candidate, name, (size_t)(end - name)) == 0)
                seen[i] = true;
        }
    }
}

char* shader_source_with_includes(const char* source) {
    if (!source)
        return NULL;
    bool* seen = calloc((size_t)shader_include_chunk_count, sizeof(bool));
    if (seen)
        _include_mark_expanded(source, seen);
    IncludeBuffer out = {0};
    const bool ok =
        seen && _include_append(&out, "", 0) && _include_expand(&out, source, seen, true);
    free(seen);
    if (!ok) {
        free(out.data);
        return NULL;
    }
    return out.data;
}

char* shader_source_splice(const char* host, const char* marker, const char* chunk) {
    if (!host || !marker || !chunk)
        return NULL;
    // The marker's line, and the number the driver gives it: counted from the last #line, since
    // a variant's defines block resets the count with one.
    const size_t marker_len = strlen(marker);
    const char* at = NULL;
    int number = 1;
    for (const char* line = host; *line;) {
        const char* eol = strchr(line, '\n');
        const size_t len = eol ? (size_t)(eol - line) : strlen(line);
        const char* t = line;
        while (*t == ' ' || *t == '\t')
            t++;
        if ((size_t)(t - line) + marker_len <= len && strncmp(t, marker, marker_len) == 0) {
            at = line;
            break;
        }
        int reset = 0;
        number = sscanf(t, "#line %d", &reset) == 1 ? reset : number + 1;
        if (!eol)
            break;
        line = eol + 1;
    }
    if (!at) {
        log_error("shader_source_splice: no line '%s' to splice at", marker);
        return NULL;
    }

    // The chunk's own #includes, expanded against what the host already holds, so a chunk the
    // host expanded at build time is not defined twice.
    bool* seen = calloc((size_t)shader_include_chunk_count, sizeof(bool));
    IncludeBuffer expanded = {0};
    bool ok = seen && _include_append(&expanded, "", 0);
    if (ok) {
        _include_mark_expanded(host, seen);
        ok = _include_expand(&expanded, chunk, seen, false);
    }
    free(seen);
    if (!ok) {
        free(expanded.data);
        return NULL;
    }

    // Source string 1 for the chunk, so a compile error in it reads "1:<its own line>", then back
    // to string 0 at the host's next line.
    const char* after = strchr(at, '\n');
    IncludeBuffer out = {0};
    ok = _include_append(&out, host, (size_t)(at - host)) &&
         _include_appendf(&out, "#line 1 1\n") &&
         _include_append(&out, expanded.data, expanded.len) &&
         _include_appendf(&out, "\n#line %d 0\n", number + 1) &&
         (!after || _include_append(&out, after + 1, strlen(after + 1)));
    free(expanded.data);
    if (!ok) {
        free(out.data);
        return NULL;
    }
    return out.data;
}

Shader* create_shader_from_path(ShaderType type, const char* file_path) {
    char* source = _read_shader_source(file_path);
    if (!source)
        return NULL;

    Shader* shader = create_shader(type, source);
    free(source);

    return shader;
}

Shader* create_shader(ShaderType type, const char* source) {
    if (!source) {
        log_error("Shader source is NULL");
        return NULL;
    }

    Shader* shader = malloc(sizeof(Shader));
    if (!shader) {
        log_error("Failed to allocate memory for shader");
        return NULL;
    }

    shader->type = type;
    // Every source, whoever built it: an engine shader was expanded at build time and has no
    // #include line left, so for it this is a copy, and an app's or a scene file's resolves here.
    shader->source = shader_source_with_includes(source);
    if (!shader->source) {
        free(shader);
        return NULL;
    }

    GLenum glType;
    switch (shader->type) {
        case VERTEX_SHADER:
            glType = GL_VERTEX_SHADER;
            break;
        case GEOMETRY_SHADER:
            glType = GL_GEOMETRY_SHADER;
            break;
        case FRAGMENT_SHADER:
            glType = GL_FRAGMENT_SHADER;
            break;
        default:
            log_error("Unknown shader type");
            free(shader->source);
            free(shader);
            return NULL;
    }

    shader->shaderID = glCreateShader(glType);

    if (shader->shaderID == 0) {
        log_error("Failed to create shader object.");
        free(shader->source);
        free(shader);
        return NULL;
    }

    return shader;
}

GLboolean compile_shader(Shader* shader) {
    if (!shader || !shader->source) {
        log_error("Invalid shader or shader source.");
        return GL_FALSE;
    }

    // Set shader source and compile
    const GLchar* source = shader->source;
    glShaderSource(shader->shaderID, 1, &source, NULL);
    check_gl_error("glShaderSource");

    glCompileShader(shader->shaderID);
    check_gl_error("glCompileShader");

    // Check compilation status
    int success;
    glGetShaderiv(shader->shaderID, GL_COMPILE_STATUS, &success);
    check_gl_error("glGetShaderiv");

    if (!success) {
        GLint logLength = 0;
        glGetShaderiv(shader->shaderID, GL_INFO_LOG_LENGTH, &logLength);
        check_gl_error("glGetShaderiv log length");

        if (logLength > 0) {
            char* log = (char*)malloc(logLength);
            if (log) {
                glGetShaderInfoLog(shader->shaderID, logLength, &logLength, log);
                check_gl_error("glGetShaderInfoLog");
                log_error("Shader compilation failed: %s", log);
                free(log);
            } else {
                log_error("Failed to allocate memory for shader log.");
            }
        } else {
            log_error("Shader compilation failed with no additional information.");
        }

        return GL_FALSE;
    }

    return GL_TRUE;
}

void free_shader(Shader* shader) {
    if (shader) {
        if (shader->shaderID != 0) {
            glDeleteShader(shader->shaderID);
        }
        if (shader->source) {
            free(shader->source);
        }
        free(shader);
    }
}
