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
    if (!grow_array((void**)&b->data, &b->cap, b->len + n + 1, 1, 4096))
        return false;
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

// The text of a line past its leading whitespace.
static const char* _line_text(const char* line) {
    while (*line == ' ' || *line == '\t')
        line++;
    return line;
}

// The number the driver gives the line after the one whose text is `t` and whose number is
// `number`: a #line directive sets it, any other line counts one.
static int _next_line_number(const char* t, int number) {
    int reset = 0;
    return strncmp(t, "#line", 5) == 0 && sscanf(t, "#line %d", &reset) == 1 ? reset : number + 1;
}

// The chunk `len` bytes of `name` call for, or -1.
static int _include_chunk(const char* name, size_t len) {
    for (int i = 0; i < shader_include_chunk_count; i++) {
        const char* candidate = shader_include_chunks[i].name;
        if (strlen(candidate) == len && strncmp(candidate, name, len) == 0)
            return i;
    }
    return -1;
}

typedef enum { INCLUDE_NONE, INCLUDE_CHUNK, INCLUDE_BAD } IncludeLine;

// Whether the line `line` (`len` bytes, its newline included) is an #include, and of which chunk.
// The grammar is gen_shader_header.py's INCLUDE_RE: `#include "name"`, then at most a // comment.
// A line that starts #include and is anything else is INCLUDE_BAD, logged as line `number` of
// `where`.
static IncludeLine _include_line(const char* line, size_t len, int number, const char* where,
                                 int* chunk) {
    const char* t = _line_text(line);
    if (strncmp(t, "#include", 8) != 0)
        return INCLUDE_NONE;
    const char* end = line + len;
    const char* open = _line_text(t + 8);
    const char* close = open < end && *open == '"' && open > t + 8
                            ? memchr(open + 1, '"', (size_t)(end - open - 1))
                            : NULL;
    const char* tail = close ? _line_text(close + 1) : NULL;
    if (!tail || !(tail >= end || *tail == '\n' || *tail == '\r' || strncmp(tail, "//", 2) == 0)) {
        log_error("Malformed #include on line %d of %s: %.*s", number, where, (int)len, line);
        return INCLUDE_BAD;
    }
    *chunk = _include_chunk(open + 1, (size_t)(close - open - 1));
    if (*chunk < 0) {
        log_error("#include \"%.*s\" on line %d of %s names no engine chunk",
                  (int)(close - open - 1), open + 1, number, where);
        return INCLUDE_BAD;
    }
    return INCLUDE_CHUNK;
}

// Chunk `chunk` into `out` between its begin and end lines, its own includes with it -- or one line
// saying so, when `seen` says this shader already holds it. `seen` is what makes a chunk included
// twice a single copy, since GLSL has no include guards, and a chunk is marked before its own lines
// are read, which is the cycle guard. A chunk the shader's build-time expansion defines further
// down, in `rest`, is refused, as `where` includes it: here it would be defined twice. No #line
// inside a chunk: a number there could not say WHICH chunk, which is why the build-time expansion
// carries none either (gen_shader_header.py).
static bool _include_chunk_into(IncludeBuffer* out, int chunk, bool* seen, const char* rest,
                                const char* where) {
    const char* name = shader_include_chunks[chunk].name;
    if (seen[chunk])
        return _include_appendf(out, "// #include \"%s\" (already expanded)\n", name);
    char marker[96];
    snprintf(marker, sizeof(marker), "// ---- begin %s ----", name);
    if (strstr(rest, marker)) {
        log_error("#include \"%s\" in %s: the shader around it defines that chunk further down, "
                  "so it cannot be included here",
                  name, where);
        return false;
    }
    seen[chunk] = true;
    if (!_include_appendf(out, "// ---- begin %s ----\n", name))
        return false;
    int number = 0;
    for (const char* line = shader_include_chunks[chunk].source; *line;) {
        const char* eol = strchr(line, '\n');
        const size_t len = eol ? (size_t)(eol - line) + 1 : strlen(line);
        int inner = -1;
        const IncludeLine kind = _include_line(line, len, ++number, name, &inner);
        if (kind == INCLUDE_BAD ||
            !(kind == INCLUDE_CHUNK ? _include_chunk_into(out, inner, seen, rest, name)
                                    : _include_append(out, line, len)))
            return false;
        line += len;
    }
    return _include_appendf(out, "// ---- end %s ----\n", name);
}

// The chunk a build-time expansion marker names, `// ---- begin X ----`, or -1 for any other line.
static int _build_expanded_chunk(const char* t) {
    static const char head[] = "// ---- begin ";
    if (strncmp(t, head, sizeof(head) - 1) != 0)
        return -1;
    const char* name = t + sizeof(head) - 1;
    const char* end = strstr(name, " ----");
    const char* eol = strchr(name, '\n');
    return end && (!eol || end < eol) ? _include_chunk(name, (size_t)(end - name)) : -1;
}

// `source` with every `#include "x.glsl"` line expanded from the engine's shared chunks. A chunk
// the build already expanded is counted as held from its marker line on, in order: an app's GLSL
// spliced into an engine shader (spec 13.29) shares the copies above the splice point, and a chunk
// the shader defines only BELOW it -- included directly or by another chunk -- is refused by name,
// since including it there would define it twice. After each expansion the numbering goes back to
// the shader's own, so an error names its line rather than that line plus every chunk above it;
// within a spliced chunk that is still the chunk's numbering, because a #line without a
// source-string number keeps the current one.
static char* _source_with_includes(const char* source) {
    bool* seen = calloc((size_t)shader_include_chunk_count, sizeof(bool));
    IncludeBuffer out = {0};
    bool ok = seen && _include_append(&out, "", 0);
    int number = 1;
    for (const char* line = source; ok && *line;) {
        const char* eol = strchr(line, '\n');
        const size_t len = eol ? (size_t)(eol - line) + 1 : strlen(line);
        const char* t = _line_text(line);
        int chunk = -1;
        const IncludeLine kind = _include_line(line, len, number, "the shader", &chunk);
        if (kind == INCLUDE_BAD) {
            ok = false;
        } else if (kind == INCLUDE_NONE) {
            const int built = _build_expanded_chunk(t);
            if (built >= 0)
                seen[built] = true;
            ok = _include_append(&out, line, len);
        } else {
            // A chunk already held stands as one line for one, so only an expansion needs the
            // numbering put back.
            char where[48];
            snprintf(where, sizeof(where), "line %d of the shader", number);
            const bool held = seen[chunk];
            ok = _include_chunk_into(&out, chunk, seen, line + len, where) &&
                 (held || _include_appendf(&out, "#line %d\n", number + 1));
        }
        number = _next_line_number(t, number);
        line += len;
    }
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
        const char* t = _line_text(line);
        if ((size_t)(t - line) + marker_len <= len && strncmp(t, marker, marker_len) == 0) {
            at = line;
            break;
        }
        number = _next_line_number(t, number);
        if (!eol)
            break;
        line = eol + 1;
    }
    if (!at) {
        log_error("shader_source_splice: no line '%s' to splice at", marker);
        return NULL;
    }

    // Source string 1 for the chunk, so a compile error in it reads "1:<its own line>", then back
    // to string 0 at the host's next line. Its #includes stay for create_shader, which expands
    // them against what the host holds above this point.
    const char* after = strchr(at, '\n');
    IncludeBuffer out = {0};
    const bool ok = _include_append(&out, host, (size_t)(at - host)) &&
                    _include_appendf(&out, "#line 1 1\n") &&
                    _include_append(&out, chunk, strlen(chunk)) &&
                    _include_appendf(&out, "\n#line %d 0\n", number + 1) &&
                    (!after || _include_append(&out, after + 1, strlen(after + 1)));
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
    shader->source = _source_with_includes(source);
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
