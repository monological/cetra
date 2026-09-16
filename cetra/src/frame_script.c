#include "frame_script.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "ext/log.h"
#include "util.h"

// The next whitespace-delimited token of a line, NUL-terminated in place; NULL
// at the end. A hand lexer, as lut.c's is: strtok's reentrant form is strtok_r
// on two of the three platforms and strtok_s on the third.
static char* _next_token(char** cursor) {
    char* p = *cursor;
    while (*p == ' ' || *p == '\t' || *p == '\r')
        p++;
    if (*p == '\0')
        return NULL;
    char* start = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r')
        p++;
    if (*p)
        *p++ = '\0';
    *cursor = p;
    return start;
}

bool frame_script_parse(char* text, const char* path, size_t stride, const void* idle,
                        FrameScriptTokenFn on_token, void** out_ranges, size_t* out_count) {
    if (!text || !path || !idle || !on_token || !out_ranges || !out_count ||
        stride < sizeof(FrameScriptRange)) {
        log_error("frame_script_parse: bad argument");
        return false;
    }
    *out_ranges = NULL;
    *out_count = 0;

    char* ranges = NULL;
    size_t count = 0, cap = 0;
    int line = 0;
    char* ln = text;
    while (ln && *ln) {
        char* next = strchr(ln, '\n');
        if (next)
            *next++ = '\0';
        line++;
        char* hash = strchr(ln, '#');
        if (hash)
            *hash = '\0';
        char* p = ln;
        while (isspace((unsigned char)*p))
            p++;
        if (*p == '\0') {
            ln = next;
            continue;
        }

        if (!grow_array((void**)&ranges, &cap, count + 1, stride, 8)) {
            free(ranges);
            return false;
        }
        char* slot = ranges + count * stride;
        memcpy(slot, idle, stride);
        FrameScriptRange* r = (FrameScriptRange*)slot;

        char* end = NULL;
        long from = strtol(p, &end, 10);
        long to = from;
        if (end == p || from < 0) {
            log_error("%s:%d: a line starts with a frame or a range", path, line);
            free(ranges);
            return false;
        }
        p = end;
        if (*p == '-') {
            to = strtol(p + 1, &end, 10);
            if (end == p + 1 || to < from) {
                log_error("%s:%d: bad range", path, line);
                free(ranges);
                return false;
            }
            p = end;
        }
        r->from = (int)from;
        r->to = (int)to;

        for (const char* tok = _next_token(&p); tok; tok = _next_token(&p)) {
            if (!on_token(slot, tok, path, line)) {
                free(ranges);
                return false;
            }
        }

        count++;
        ln = next;
    }

    *out_ranges = ranges;
    *out_count = count;
    return true;
}

const void* frame_script_at(const void* ranges, size_t count, size_t stride, const void* idle,
                            int frame) {
    const void* found = idle;
    const char* base = ranges;
    for (size_t i = 0; i < count; i++) {
        const FrameScriptRange* r = (const FrameScriptRange*)(base + i * stride);
        if (frame >= r->from && frame <= r->to)
            found = r;
    }
    return found;
}
