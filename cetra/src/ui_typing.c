#include "ui_typing.h"

#include <ctype.h>
#include <math.h>
#include <string.h>

#include "text.h"

uint32_t ui_typing_next(const char* s, size_t len, size_t* i) {
    if (*i >= len)
        return 0;
    if ((unsigned char)s[*i] < 0x80)
        return (uint32_t)(unsigned char)s[(*i)++];
    char buf[5] = {0};
    memcpy(buf, s + *i, len - *i < 4 ? len - *i : 4);
    const char* p = buf;
    const uint32_t cp = text_utf8_next(&p);
    *i += p > buf ? (size_t)(p - buf) : 1;
    return cp;
}

// The byte past the character at `i`.
static size_t _next(const char* s, size_t len, size_t i) {
    ui_typing_next(s, len, &i);
    return i < len ? i : len;
}

static size_t _prev(const char* s, size_t i) {
    if (i == 0)
        return 0;
    const char* p = s + i;
    text_utf8_prev(&p, s);
    return (size_t)(p - s);
}

int ui_typing_class(char byte) {
    const unsigned char c = (unsigned char)byte;
    if (c == ' ' || c == '\t' || c == '\n')
        return 0;
    if (c >= 0x80 || isalnum(c) || c == '_')
        return 2;
    return 1;
}

size_t ui_typing_word_left(const char* s, size_t len, size_t at) {
    if (at > len)
        at = len;
    if (at == 0)
        return 0;
    size_t i = _prev(s, at);
    while (i > 0 && ui_typing_class(s[i]) == 0)
        i = _prev(s, i);
    if (i == 0)
        return 0;
    const int k = ui_typing_class(s[i]);
    while (i > 0) {
        const size_t p = _prev(s, i);
        if (ui_typing_class(s[p]) != k)
            break;
        i = p;
    }
    return i;
}

size_t ui_typing_word_end(const char* s, size_t len, size_t at) {
    size_t i = at > len ? len : at;
    while (i < len && ui_typing_class(s[i]) == 0)
        i = _next(s, len, i);
    if (i >= len)
        return len;
    const int k = ui_typing_class(s[i]);
    while (i < len && ui_typing_class(s[i]) == k)
        i = _next(s, len, i);
    return i;
}

size_t ui_typing_word_right(const char* s, size_t len, size_t at) {
#ifdef __APPLE__
    return ui_typing_word_end(s, len, at);
#else
    size_t i = at > len ? len : at;
    if (i < len && ui_typing_class(s[i]) != 0) {
        const int k = ui_typing_class(s[i]);
        while (i < len && ui_typing_class(s[i]) == k)
            i = _next(s, len, i);
    }
    while (i < len && ui_typing_class(s[i]) == 0)
        i = _next(s, len, i);
    return i;
#endif
}

void ui_typing_word_round(const char* s, size_t len, size_t at, size_t* from, size_t* to) {
    if (at > len)
        at = len;
    if (len == 0) {
        *from = *to = 0;
        return;
    }
    if (at >= len ||
        (at > 0 && ui_typing_class(s[at]) == 0 && ui_typing_class(s[_prev(s, at)]) != 0))
        at = _prev(s, at);
    const int k = ui_typing_class(s[at]);
    size_t a = at, b = _next(s, len, at);
    while (a > 0) {
        const size_t p = _prev(s, a);
        if (ui_typing_class(s[p]) != k)
            break;
        a = p;
    }
    while (b < len && ui_typing_class(s[b]) == k)
        b = _next(s, len, b);
    *from = a;
    *to = b;
}

size_t ui_typing_clean(const char* in, size_t len, char* out, bool lines, bool tabs) {
    size_t k = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '\r') {
            if (i + 1 < len && in[i + 1] == '\n')
                continue;
            c = '\n';
        }
        if (c == '\n')
            out[k++] = lines ? '\n' : ' ';
        else if (c == '\t')
            out[k++] = tabs ? '\t' : ' ';
        else if (c >= 0x20 && c != 0x7f)
            out[k++] = (char)c;
    }
    return k;
}

int ui_typing_press(UITypingPresses* p, double now, float x, float y, int cycle) {
    const bool again = p->count > 0 && now - p->time < UI_TYPING_PRESS_SECONDS &&
                       fabsf(x - p->x) < UI_TYPING_PRESS_POINTS &&
                       fabsf(y - p->y) < UI_TYPING_PRESS_POINTS;
    p->count = again && (!cycle || p->count < cycle) ? p->count + 1 : 1;
    p->time = now;
    p->x = x;
    p->y = y;
    return p->count;
}

bool ui_typing_caret_lit(UITypingCaret* c, double now, bool blink, double* wake) {
    if (c->moved) {
        c->since = now;
        c->moved = false;
    }
    *wake = INFINITY;
    if (!blink)
        return true;
    const double period = UI_TYPING_CARET_LIT_SECONDS + UI_TYPING_CARET_DARK_SECONDS;
    double phase = fmod(now - c->since, period);
    phase = phase < 0.0 ? 0.0 : phase;
    const bool lit = phase < UI_TYPING_CARET_LIT_SECONDS;
    *wake = (lit ? UI_TYPING_CARET_LIT_SECONDS : period) - phase;
    return lit;
}
