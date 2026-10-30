#include "text_doc.h"

#include <stdlib.h>
#include <string.h>

#include "ext/log.h"
#include "text.h"
#include "text_doc_edit.h"
#include "util.h"

// The most undo steps kept; past it the oldest go.
#define UNDO_MAX 10000
// The most lines holding their colour at once. Past it every line's is let go, and the lines in
// view colour themselves again, which costs microseconds against a scan of every line to choose.
#define SPAN_LINES_MAX 8192

static const char* const ENDINGS[] = {"", "\n", "\r\n"};

static size_t _ending_len(uint8_t ending) {
    return ending == TEXT_ENDING_CRLF ? 2 : ending;
}

static size_t _raw_len(const TextLine* l) {
    return l->len + _ending_len(l->ending);
}

static void _free_spans(TextDoc* doc, TextLine* l) {
    if (l->coloured & TEXT_COLOURED_SPANS)
        doc->span_lines--;
    free(l->spans);
    l->spans = NULL;
    l->span_count = 0;
    l->coloured &= (uint8_t)~TEXT_COLOURED_SPANS;
}

static void _free_line(TextDoc* doc, TextLine* l) {
    if (l->capacity)
        free(l->bytes);
    _free_spans(doc, l);
}

// Splits `len` bytes into lines, each owning its bytes when `own`, else pointing into them. A
// final segment after the last LF is a line of its own, with no ending, only when `trailing`.
// Returns the count, or SIZE_MAX out of memory or for a line too long to hold, with nothing left
// owned.
static size_t _split(const char* bytes, size_t len, bool trailing, bool own, TextLine** out,
                     size_t* capacity) {
    size_t count = 0;
    const char* p = bytes;
    const char* end = bytes + len;
    for (;;) {
        const char* nl = memchr(p, '\n', (size_t)(end - p));
        if (!nl && !trailing)
            break;
        const char* stop = nl ? nl : end;
        uint8_t ending = nl ? TEXT_ENDING_LF : TEXT_ENDING_NONE;
        if (nl && stop > p && stop[-1] == '\r') {
            stop--;
            ending = TEXT_ENDING_CRLF;
        }
        const size_t n = (size_t)(stop - p);
        if (n > UINT32_MAX || !grow_array((void**)out, capacity, count + 1, sizeof(TextLine), 64))
            goto fail;
        TextLine* l = &(*out)[count];
        memset(l, 0, sizeof(*l));
        l->len = (uint32_t)n;
        l->ending = ending;
        if (own) {
            l->bytes = malloc(n ? n : 1);
            if (!l->bytes)
                goto fail;
            memcpy(l->bytes, p, n);
            l->capacity = (uint32_t)(n ? n : 1);
        } else {
            l->bytes = (char*)p;
        }
        count++;
        if (!nl)
            break;
        p = nl + 1;
    }
    return count;
fail:
    for (size_t i = 0; own && i < count; i++)
        free((*out)[i].bytes);
    return SIZE_MAX;
}

TextDoc* create_text_doc(const char* bytes, size_t len) {
    TextDoc* doc = calloc(1, sizeof(TextDoc));
    if (!doc) {
        log_error("create_text_doc: out of memory");
        return NULL;
    }
    doc->block = malloc(len + 1);
    if (!doc->block) {
        log_error("create_text_doc: out of memory for %zu bytes", len);
        free(doc);
        return NULL;
    }
    if (len)
        memcpy(doc->block, bytes, len);
    doc->block[len] = '\0';
    doc->line_count = _split(doc->block, len, true, false, &doc->lines, &doc->line_capacity);
    if (doc->line_count == SIZE_MAX) {
        log_error("create_text_doc: out of memory, or a line past 4 GiB, in %zu bytes", len);
        free(doc->lines);
        free(doc->block);
        free(doc);
        return NULL;
    }
    doc->length = len;
    doc->starts_good = 1;
    doc->newline = doc->lines[0].ending == TEXT_ENDING_CRLF ? TEXT_NEWLINE_CRLF : TEXT_NEWLINE_LF;
    doc->tab_width = 4;
    doc->keep_undo = true;
    doc->undo_sealed = true;
    return doc;
}

static void _free_step(TextUndoStep* s) {
    for (size_t i = 0; i < s->change_count; i++) {
        free((void*)s->changes[i].removed);
        free((void*)s->changes[i].inserted);
    }
    free(s->changes);
    free(s->before);
    free(s->after);
    memset(s, 0, sizeof(*s));
}

void free_text_doc(TextDoc* doc) {
    if (!doc)
        return;
    for (size_t i = 0; i < doc->view_count; i++)
        if (doc->views[i].gone)
            doc->views[i].gone(doc->views[i].view);
    for (size_t i = 0; i < doc->line_count; i++)
        _free_line(doc, &doc->lines[i]);
    for (size_t i = 0; i < doc->undo_count + doc->redo_count; i++)
        _free_step(&doc->undo[i]);
    for (int i = 0; i < TEXT_DOC_LAYERS; i++)
        free(doc->ranges[i]);
    free(doc->undo);
    free(doc->views);
    free(doc->lines);
    free(doc->span_scratch);
    free(doc->block);
    free(doc);
}

size_t text_doc_length(const TextDoc* doc) {
    return doc ? doc->length : 0;
}

size_t text_doc_line_count(const TextDoc* doc) {
    return doc ? doc->line_count : 0;
}

const char* text_doc_line(const TextDoc* doc, size_t line, size_t* len) {
    if (!doc || line >= doc->line_count) {
        if (len)
            *len = 0;
        return "";
    }
    if (len)
        *len = doc->lines[line].len;
    return doc->lines[line].bytes;
}

size_t text_doc_line_start(TextDoc* doc, size_t line) {
    if (!doc)
        return 0;
    if (line >= doc->line_count)
        return doc->length;
    TextLine* lines = doc->lines;
    for (size_t i = doc->starts_good; i <= line; i++)
        lines[i].start = lines[i - 1].start + _raw_len(&lines[i - 1]);
    if (doc->starts_good <= line)
        doc->starts_good = line + 1;
    return lines[line].start;
}

size_t text_doc_line_of(TextDoc* doc, size_t offset) {
    if (!doc)
        return 0;
    if (offset >= doc->length)
        return doc->line_count - 1;
    TextLine* lines = doc->lines;
    // Bring the starts up to the line holding the offset, then search the good ones.
    while (doc->starts_good < doc->line_count) {
        const TextLine* last = &lines[doc->starts_good - 1];
        if (last->start + _raw_len(last) > offset)
            break;
        lines[doc->starts_good].start = last->start + _raw_len(last);
        doc->starts_good++;
    }
    size_t lo = 0, hi = doc->starts_good;
    while (hi - lo > 1) {
        const size_t mid = lo + (hi - lo) / 2;
        if (lines[mid].start <= offset)
            lo = mid;
        else
            hi = mid;
    }
    return lo;
}

// The bytes of a line from `from`, its ending included, into out: how many.
static size_t _raw_copy(const TextLine* l, size_t from, size_t to, char* out) {
    size_t n = 0;
    if (from < l->len) {
        const size_t stop = to < l->len ? to : l->len;
        memcpy(out, l->bytes + from, stop - from);
        n = stop - from;
        from = stop;
    }
    const char* ending = ENDINGS[l->ending];
    for (size_t i = from - l->len; from < to && i < _ending_len(l->ending); i++, from++)
        out[n++] = ending[i];
    return n;
}

size_t text_doc_copy(TextDoc* doc, size_t start, size_t end, char* out) {
    if (!doc || start >= end || start >= doc->length)
        return 0;
    if (end > doc->length)
        end = doc->length;
    size_t line = text_doc_line_of(doc, start);
    size_t n = 0;
    while (start < end) {
        const TextLine* l = &doc->lines[line];
        const size_t at = text_doc_line_start(doc, line);
        const size_t to = end - at < _raw_len(l) ? end - at : _raw_len(l);
        n += _raw_copy(l, start - at, to, out + n);
        start = at + to;
        line++;
    }
    return n;
}

char* text_doc_text(const TextDoc* doc, size_t* len) {
    if (!doc)
        return NULL;
    char* out = malloc(doc->length + 1);
    if (!out) {
        log_error("text_doc_text: out of memory for %zu bytes", doc->length);
        return NULL;
    }
    size_t n = 0;
    for (size_t i = 0; i < doc->line_count; i++)
        n += _raw_copy(&doc->lines[i], 0, _raw_len(&doc->lines[i]), out + n);
    out[n] = '\0';
    if (len)
        *len = n;
    return out;
}

// The byte at `offset`, which is inside the text.
static char _byte_at(TextDoc* doc, size_t offset) {
    const size_t line = text_doc_line_of(doc, offset);
    const TextLine* l = &doc->lines[line];
    const size_t at = offset - text_doc_line_start(doc, line);
    return at < l->len ? l->bytes[at] : ENDINGS[l->ending][at - l->len];
}

size_t text_doc_map_offset(size_t offset, size_t start, size_t removed, size_t inserted,
                           bool after) {
    const size_t end = start + removed;
    if (offset < start || (offset == start && !after))
        return offset;
    if (offset > end || (offset == end && removed))
        return offset - removed + inserted;
    return start + inserted;
}

// A range moved through a change, the new bytes kept outside it; false once it is empty.
static bool _map_range(TextRange* r, size_t start, size_t removed, size_t inserted) {
    const size_t end = start + removed;
    if (r->start >= end)
        r->start = r->start - removed + inserted;
    else if (r->start >= start)
        r->start = start + inserted;
    if (r->end > end)
        r->end = r->end - removed + inserted;
    else if (r->end > start)
        r->end = start;
    return r->start < r->end;
}

static void _map_ranges(TextDoc* doc, size_t start, size_t removed, size_t inserted) {
    for (int layer = 0; layer < TEXT_DOC_LAYERS; layer++) {
        TextRange* r = doc->ranges[layer];
        size_t kept = 0;
        for (size_t i = 0; i < doc->range_count[layer]; i++)
            if (_map_range(&r[i], start, removed, inserted))
                r[kept++] = r[i];
        doc->range_count[layer] = kept;
    }
}

// Lines [first, first + count) are new: their colour is unknown, and so is every stateful line's
// after them.
static void _lines_changed(TextDoc* doc, size_t first) {
    if (doc->highlight_known > first)
        doc->highlight_known = first;
    if (doc->starts_good > first + 1)
        doc->starts_good = first + 1;
}

// Makes a line's bytes its own, with room for `need`.
static bool _own(TextLine* l, size_t need) {
    if (need > UINT32_MAX)
        return false;
    if (l->capacity >= need && l->capacity)
        return true;
    size_t cap = l->capacity ? l->capacity : 16;
    while (cap < need)
        cap *= 2;
    if (cap > UINT32_MAX)
        cap = UINT32_MAX;
    char* bytes = l->capacity ? realloc(l->bytes, cap) : malloc(cap);
    if (!bytes)
        return false;
    if (!l->capacity && l->len)
        memcpy(bytes, l->bytes, l->len);
    l->bytes = bytes;
    l->capacity = (uint32_t)cap;
    return true;
}

// The one place the text changes: [start, end) becomes `text`, both checked by the caller. Moves
// every view, every range, the history and the version.
static bool _replace(TextDoc* doc, size_t start, size_t end, const char* text, size_t len,
                     const void* actor) {
    if (start == end && !len)
        return true;
    const size_t first = text_doc_line_of(doc, start);
    const size_t first_at = text_doc_line_start(doc, first);
    TextLine* l = &doc->lines[first];
    size_t lines_removed = 1, lines_inserted = 1; // the lines from `first` the change rewrote
    const bool inside = end <= first_at + l->len;
    const bool plain = !memchr(text, '\n', len) && !memchr(text, '\r', len);
    if (inside && plain) {
        // Within one line's content and touching no line break: the line edited in place, unless
        // what is left before an LF would end in a CR, which makes the two one ending.
        const size_t from = start - first_at, to = end - first_at;
        const size_t n = l->len - (to - from) + len;
        const bool cr_joins = l->ending == TEXT_ENDING_LF && to == l->len && !len && from > 0 &&
                              l->bytes[from - 1] == '\r';
        if (!cr_joins) {
            if (!_own(l, n)) {
                log_error("text_doc: out of memory for a line of %zu bytes", n);
                return false;
            }
            memmove(l->bytes + from + len, l->bytes + to, l->len - to);
            memcpy(l->bytes + from, text, len);
            l->len = (uint32_t)n;
            _free_spans(doc, l);
            l->coloured = 0;
            _lines_changed(doc, first);
            goto changed;
        }
    }
    {
        // The general case: the lines the change touches, from the first's start to the last's
        // ending, written out again with the change made and split afresh.
        const size_t last = text_doc_line_of(doc, end);
        const size_t last_at = text_doc_line_start(doc, last);
        const TextLine* ll = &doc->lines[last];
        const size_t head = start - first_at;
        const size_t tail = _raw_len(ll) - (end - last_at);
        char* joined = malloc(head + len + tail + 1);
        if (!joined) {
            log_error("text_doc: out of memory for a change of %zu bytes", len);
            return false;
        }
        _raw_copy(l, 0, head, joined);
        memcpy(joined + head, text, len);
        _raw_copy(ll, end - last_at, _raw_len(ll), joined + head + len);
        TextLine* fresh = NULL;
        size_t fresh_capacity = 0;
        const bool trailing = ll->ending == TEXT_ENDING_NONE;
        const size_t count =
            _split(joined, head + len + tail, trailing, true, &fresh, &fresh_capacity);
        free(joined);
        const size_t old = last - first + 1;
        if (count == SIZE_MAX || !grow_array((void**)&doc->lines, &doc->line_capacity,
                                             doc->line_count - old + count, sizeof(TextLine), 64)) {
            for (size_t i = 0; count != SIZE_MAX && i < count; i++)
                free(fresh[i].bytes);
            free(fresh);
            log_error("text_doc: out of memory for a change of %zu bytes", len);
            return false;
        }
        for (size_t i = first; i <= last; i++)
            _free_line(doc, &doc->lines[i]);
        memmove(&doc->lines[first + count], &doc->lines[last + 1],
                (doc->line_count - last - 1) * sizeof(TextLine));
        memcpy(&doc->lines[first], fresh, count * sizeof(TextLine));
        free(fresh);
        doc->line_count = doc->line_count - old + count;
        doc->lines[first].start = first_at;
        _lines_changed(doc, first);
        lines_removed = old;
        lines_inserted = count;
    }
changed:
    doc->length = doc->length - (end - start) + len;
    doc->history[doc->version % TEXT_DOC_HISTORY].start = start;
    doc->history[doc->version % TEXT_DOC_HISTORY].removed = end - start;
    doc->history[doc->version % TEXT_DOC_HISTORY].inserted = len;
    doc->version++;
    _map_ranges(doc, start, end - start, len);
    for (size_t i = 0; i < doc->view_count; i++) {
        const TextDocView* v = &doc->views[i];
        if (v->lines)
            v->lines(v->view, first, lines_removed, lines_inserted);
        v->map(v->view, start, end - start, len, v->view == actor);
    }
    return true;
}

// ---- Undo

static TextDocView* _view(TextDoc* doc, const void* view) {
    for (size_t i = 0; view && i < doc->view_count; i++)
        if (doc->views[i].view == view)
            return &doc->views[i];
    return NULL;
}

static TextSelection* _copy_selections(const TextSelection* s, size_t n) {
    if (!n)
        return NULL;
    TextSelection* out = malloc(n * sizeof(*s));
    if (out)
        memcpy(out, s, n * sizeof(*s));
    return out;
}

static size_t _view_selections(TextDoc* doc, const void* view, TextSelection** out) {
    TextDocView* v = _view(doc, view);
    const TextSelection* s = NULL;
    const size_t n = v && v->selections ? v->selections(v->view, &s) : 0;
    *out = _copy_selections(s, n);
    return *out ? n : 0;
}

static void _drop_redo(TextDoc* doc) {
    for (size_t i = 0; i < doc->redo_count; i++)
        _free_step(&doc->undo[doc->undo_count + i]);
    doc->redo_count = 0;
}

static bool _is_space(char c) {
    return c == ' ' || c == '\t';
}

// Whether typing `changes` continues the last step at each of its carets: each change an insert
// right where that caret's text in the step ends, and not a word's start -- a space after a
// character that is not one, the field's rule.
static bool _joins(TextDoc* doc, const TextDocChange* changes, size_t count, const void* view) {
    if (doc->undo_sealed || !doc->undo_count)
        return false;
    const TextUndoStep* s = &doc->undo[doc->undo_count - 1];
    if (!s->typed || s->view != view || s->change_count != count)
        return false;
    // The step's changes run from the highest start down; after it, change i's text begins at its
    // start moved by every change below it.
    long long below = 0;
    for (size_t k = count; k-- > 0;) {
        const TextDocChange* old = &s->changes[k];
        const TextDocChange* now = &changes[k];
        const size_t ends = (size_t)((long long)old->start + below) + old->inserted_len;
        if (now->removed_len || !now->inserted_len || now->start != ends)
            return false;
        const char prev = old->inserted_len ? old->inserted[old->inserted_len - 1] : ' ';
        if (_is_space(now->inserted[0]) && !_is_space(prev))
            return false;
        below += (long long)old->inserted_len - (long long)old->removed_len;
    }
    return true;
}

static char* _dup_bytes(const char* p, size_t n) {
    char* out = malloc(n ? n : 1);
    if (out && n)
        memcpy(out, p, n);
    return out;
}

// Keeps an action as an undo step, joining typing to the step before it.
static void _keep(TextDoc* doc, const TextDocChange* changes, size_t count,
                  const TextSelection* before, size_t before_count, const TextSelection* after,
                  size_t after_count, void* view, bool typed) {
    if (!doc->keep_undo)
        return;
    _drop_redo(doc);
    if (typed && _joins(doc, changes, count, view)) {
        TextUndoStep* s = &doc->undo[doc->undo_count - 1];
        for (size_t i = 0; i < count; i++) {
            TextDocChange* c = &s->changes[i];
            char* grown = realloc((void*)c->inserted, c->inserted_len + changes[i].inserted_len);
            if (!grown)
                return;
            memcpy(grown + c->inserted_len, changes[i].inserted, changes[i].inserted_len);
            c->inserted = grown;
            c->inserted_len += changes[i].inserted_len;
        }
        free(s->after);
        s->after = _copy_selections(after, after_count);
        s->after_count = s->after ? after_count : 0;
        return;
    }
    if (doc->undo_count == UNDO_MAX) {
        _free_step(&doc->undo[0]);
        memmove(&doc->undo[0], &doc->undo[1], (UNDO_MAX - 1) * sizeof(TextUndoStep));
        doc->undo_count--;
    }
    if (!grow_array((void**)&doc->undo, &doc->undo_capacity, doc->undo_count + 1,
                    sizeof(TextUndoStep), 16))
        return;
    TextUndoStep* s = &doc->undo[doc->undo_count];
    memset(s, 0, sizeof(*s));
    s->changes = calloc(count ? count : 1, sizeof(TextDocChange));
    if (!s->changes)
        return;
    for (size_t i = 0; i < count; i++) {
        s->changes[i] = changes[i];
        s->changes[i].removed = _dup_bytes(changes[i].removed, changes[i].removed_len);
        s->changes[i].inserted = _dup_bytes(changes[i].inserted, changes[i].inserted_len);
    }
    s->change_count = count;
    s->before = _copy_selections(before, before_count);
    s->before_count = s->before ? before_count : 0;
    s->after = _copy_selections(after, after_count);
    s->after_count = s->after ? after_count : 0;
    s->view = view;
    s->typed = typed;
    doc->undo_count++;
    doc->undo_sealed = !typed;
}

void text_doc_seal_undo(TextDoc* doc) {
    if (doc)
        doc->undo_sealed = true;
}

static void _tell(TextDoc* doc, const TextDocChange* changes, size_t count,
                  const TextSelection* before, size_t before_count, const TextSelection* after,
                  size_t after_count, const void* view, uint64_t version) {
    if (!doc->on_action)
        return;
    const TextDocView* v = _view(doc, view);
    const TextDocAction action = {
        .changes = changes,
        .change_count = count,
        .before = before,
        .before_count = before_count,
        .after = after,
        .after_count = after_count,
        .editor = v ? v->element : NULL,
        .version = version,
    };
    doc->on_action(doc, &action, doc->on_action_user);
}

static int _by_start_down(const void* a, const void* b) {
    const TextEdit* x = a;
    const TextEdit* y = b;
    return x->start < y->start ? 1 : x->start > y->start ? -1 : 0;
}

bool text_doc_apply(TextDoc* doc, TextEdit* edits, size_t count, void* view, bool typed) {
    if (!doc || (!edits && count))
        return false;
    qsort(edits, count, sizeof(TextEdit), _by_start_down);
    for (size_t i = 0; i < count; i++) {
        if (edits[i].start > edits[i].end || edits[i].end > doc->length ||
            (i && edits[i].end > edits[i - 1].start)) {
            log_error("text_doc_apply: edit %zu, bytes %zu to %zu, overlaps another or is past "
                      "the text's %zu bytes",
                      i, edits[i].start, edits[i].end, doc->length);
            return false;
        }
    }
    TextSelection* before = NULL;
    const size_t before_count = _view_selections(doc, view, &before);
    TextDocChange* changes = calloc(count ? count : 1, sizeof(TextDocChange));
    char** removed = calloc(count ? count : 1, sizeof(char*));
    bool ok = changes && removed;
    const uint64_t version = doc->version;
    size_t made = 0;
    for (size_t i = 0; ok && i < count; i++) {
        const TextEdit* e = &edits[i];
        if (e->start == e->end && !e->len)
            continue;
        removed[i] = malloc(e->end - e->start + 1);
        ok = removed[i] != NULL;
        if (ok) {
            text_doc_copy(doc, e->start, e->end, removed[i]);
            ok = _replace(doc, e->start, e->end, e->text ? e->text : "", e->len, view);
        }
        if (ok) {
            changes[made++] = (TextDocChange){e->start, removed[i], e->end - e->start,
                                              e->text ? e->text : "", e->len};
        }
    }
    if (!ok && !made)
        log_error("text_doc_apply: out of memory");
    if (made) {
        TextSelection* after = NULL;
        const size_t after_count = _view_selections(doc, view, &after);
        _keep(doc, changes, made, before, before_count, after, after_count, view, typed);
        _tell(doc, changes, made, before, before_count, after, after_count, view, version);
        free(after);
    }
    for (size_t i = 0; removed && i < count; i++)
        free(removed[i]);
    free(removed);
    free(changes);
    free(before);
    return ok;
}

// A step taken back (`undo`) or made again: its changes, or their inverses, applied from the
// highest start down, the editor given the step's selections, and the action told.
static bool _step(TextDoc* doc, void* view, bool undo) {
    if (!doc || !doc->keep_undo || (undo ? !doc->undo_count : !doc->redo_count))
        return false;
    TextUndoStep* s = &doc->undo[undo ? doc->undo_count - 1 : doc->undo_count];
    const size_t n = s->change_count;
    TextDocChange* made = calloc(n ? n : 1, sizeof(TextDocChange));
    if (!made) {
        log_error("text_doc_undo: out of memory");
        return false;
    }
    const uint64_t version = doc->version;
    // A step's changes are in the text as it found it. Taken back, each one's new bytes stand
    // where its start has been moved by every change below it.
    long long below = 0;
    for (size_t k = n; k-- > 0;) {
        const TextDocChange* c = &s->changes[k];
        made[k].start = undo ? (size_t)((long long)c->start + below) : c->start;
        made[k].removed = undo ? c->inserted : c->removed;
        made[k].removed_len = undo ? c->inserted_len : c->removed_len;
        made[k].inserted = undo ? c->removed : c->inserted;
        made[k].inserted_len = undo ? c->removed_len : c->inserted_len;
        below += (long long)c->inserted_len - (long long)c->removed_len;
    }
    const void* target = view ? view : s->view;
    for (size_t i = 0; i < n; i++)
        _replace(doc, made[i].start, made[i].start + made[i].removed_len, made[i].inserted,
                 made[i].inserted_len, target);
    TextDocView* v = _view(doc, target);
    const TextSelection* sel = undo ? s->before : s->after;
    const size_t sel_count = undo ? s->before_count : s->after_count;
    if (v && v->set_selections && sel_count)
        v->set_selections(v->view, sel, sel_count);
    if (undo) {
        doc->undo_count--;
        doc->redo_count++;
    } else {
        doc->undo_count++;
        doc->redo_count--;
    }
    doc->undo_sealed = true;
    _tell(doc, made, n, undo ? s->after : s->before, undo ? s->after_count : s->before_count, sel,
          sel_count, target, version);
    free(made);
    return true;
}

bool text_doc_undo_in(TextDoc* doc, void* view) {
    return _step(doc, view, true);
}

bool text_doc_redo_in(TextDoc* doc, void* view) {
    return _step(doc, view, false);
}

bool text_doc_undo(TextDoc* doc) {
    return _step(doc, NULL, true);
}

bool text_doc_redo(TextDoc* doc) {
    return _step(doc, NULL, false);
}

// ---- The host's edits

static bool _splits_char(TextDoc* doc, size_t offset) {
    return offset < doc->length && text_utf8_is_continuation(_byte_at(doc, offset));
}

bool text_doc_replace(TextDoc* doc, size_t start, size_t end, const char* text, size_t len) {
    if (!doc || (!text && len))
        return false;
    if (start > end || end > doc->length) {
        log_error("text_doc_replace: bytes %zu to %zu are not inside the text's %zu", start, end,
                  doc->length);
        return false;
    }
    if (_splits_char(doc, start) || _splits_char(doc, end)) {
        log_error("text_doc_replace: bytes %zu to %zu start or end inside a character", start, end);
        return false;
    }
    char* removed = malloc(end - start + 1);
    if (!removed) {
        log_error("text_doc_replace: out of memory");
        return false;
    }
    text_doc_copy(doc, start, end, removed);
    const bool ok = _replace(doc, start, end, text ? text : "", len, NULL);
    if (ok && (start != end || len)) {
        const TextDocChange change = {start, removed, end - start, text ? text : "", len};
        _keep(doc, &change, 1, NULL, 0, NULL, 0, NULL, false);
    }
    free(removed);
    return ok;
}

bool text_doc_set_text(TextDoc* doc, const char* text, size_t len) {
    if (!doc || (!text && len))
        return false;
    size_t old_len = 0;
    char* old = text_doc_text(doc, &old_len);
    if (!old)
        return false;
    // What the two share at the start and at the end, each ending on a character's edge.
    size_t head = 0;
    while (head < old_len && head < len && old[head] == text[head])
        head++;
    while (head > 0 && ((head < old_len && text_utf8_is_continuation(old[head])) ||
                        (head < len && text_utf8_is_continuation(text[head]))))
        head--;
    size_t tail = 0;
    while (tail < old_len - head && tail < len - head &&
           old[old_len - 1 - tail] == text[len - 1 - tail])
        tail++;
    while (tail > 0 && (text_utf8_is_continuation(old[old_len - tail]) ||
                        text_utf8_is_continuation(text[len - tail])))
        tail--;
    free(old);
    if (head == old_len && head == len)
        return true;
    return text_doc_replace(doc, head, old_len - tail, text + head, len - tail - head);
}

bool text_doc_map(const TextDoc* doc, uint64_t version, size_t* offset) {
    if (!doc || !offset)
        return false;
    if (version > doc->version || doc->version - version > TEXT_DOC_HISTORY) {
        log_error("text_doc_map: version %llu is not within the last %d changes of the text's %llu",
                  (unsigned long long)version, TEXT_DOC_HISTORY, (unsigned long long)doc->version);
        return false;
    }
    size_t at = *offset;
    for (uint64_t v = version; v < doc->version; v++) {
        const size_t i = v % TEXT_DOC_HISTORY;
        at = text_doc_map_offset(at, doc->history[i].start, doc->history[i].removed,
                                 doc->history[i].inserted, false);
    }
    *offset = at;
    return true;
}

// ---- Views

void text_doc_add_view(TextDoc* doc, const TextDocView* view) {
    if (!doc || !view || _view(doc, view->view))
        return;
    if (!grow_array((void**)&doc->views, &doc->view_capacity, doc->view_count + 1,
                    sizeof(TextDocView), 4))
        return;
    doc->views[doc->view_count++] = *view;
}

void text_doc_remove_view(TextDoc* doc, const void* view) {
    if (!doc)
        return;
    for (size_t i = 0; i < doc->view_count; i++) {
        if (doc->views[i].view == view) {
            doc->views[i] = doc->views[--doc->view_count];
            break;
        }
    }
    for (size_t i = 0; i < doc->undo_count + doc->redo_count; i++)
        if (doc->undo[i].view == view)
            doc->undo[i].view = NULL;
}

void text_doc_set_on_action(TextDoc* doc, TextDocActionFn fn, void* user) {
    if (!doc)
        return;
    doc->on_action = fn;
    doc->on_action_user = user;
}

// ---- Ranges

static int _by_range_start(const void* a, const void* b) {
    const TextRange* x = a;
    const TextRange* y = b;
    return x->start < y->start ? -1 : x->start > y->start ? 1 : 0;
}

bool text_doc_set_ranges(TextDoc* doc, int layer, uint64_t version, const TextRange* ranges,
                         size_t count) {
    if (!doc || (!ranges && count))
        return false;
    if (layer < 0 || layer >= TEXT_DOC_LAYERS) {
        log_error("text_doc_set_ranges: layer %d is not one of the %d", layer, TEXT_DOC_LAYERS);
        return false;
    }
    if (version > doc->version || doc->version - version > TEXT_DOC_HISTORY) {
        log_error("text_doc_set_ranges: version %llu is not within the last %d changes of the "
                  "text's %llu",
                  (unsigned long long)version, TEXT_DOC_HISTORY, (unsigned long long)doc->version);
        return false;
    }
    if (!grow_array((void**)&doc->ranges[layer], &doc->range_capacity[layer], count ? count : 1,
                    sizeof(TextRange), 16))
        return false;
    TextRange* out = doc->ranges[layer];
    size_t kept = 0;
    for (size_t i = 0; i < count; i++) {
        TextRange r = ranges[i];
        bool live = r.start < r.end;
        for (uint64_t v = version; live && v < doc->version; v++) {
            const size_t h = v % TEXT_DOC_HISTORY;
            live = _map_range(&r, doc->history[h].start, doc->history[h].removed,
                              doc->history[h].inserted);
        }
        if (live && r.end <= doc->length)
            out[kept++] = r;
    }
    qsort(out, kept, sizeof(TextRange), _by_range_start);
    doc->range_count[layer] = kept;
    return true;
}

const TextRange* text_doc_ranges(const TextDoc* doc, int layer, size_t* count) {
    if (!doc || layer < 0 || layer >= TEXT_DOC_LAYERS) {
        if (count)
            *count = 0;
        return NULL;
    }
    if (count)
        *count = doc->range_count[layer];
    return doc->ranges[layer];
}

// ---- Colour

static void _forget_colour(TextDoc* doc) {
    for (size_t i = 0; i < doc->line_count; i++) {
        _free_spans(doc, &doc->lines[i]);
        doc->lines[i].coloured = 0;
    }
    doc->highlight_known = 0;
}

void text_doc_set_highlighter(TextDoc* doc, TextHighlightFn fn, void* user, bool stateful) {
    if (!doc)
        return;
    _forget_colour(doc);
    // Room for a line's spans, so only a line with more than this asks its highlighter twice.
    if (fn)
        grow_array((void**)&doc->span_scratch, &doc->span_scratch_capacity, 64, sizeof(TextSpan),
                   64);
    doc->highlight = fn;
    doc->highlight_user = user;
    doc->highlight_stateful = stateful;
}

// Colours a line from `state_in`: its state out always, its spans kept when `keep`.
static bool _colour(TextDoc* doc, size_t line, uint32_t state_in, bool keep) {
    TextLine* l = &doc->lines[line];
    uint32_t state = state_in;
    size_t n = doc->highlight(l->bytes, l->len, &state, doc->span_scratch,
                              doc->span_scratch_capacity, doc->highlight_user);
    if (n > doc->span_scratch_capacity) {
        if (!grow_array((void**)&doc->span_scratch, &doc->span_scratch_capacity, n,
                        sizeof(TextSpan), 64))
            return false;
        state = state_in;
        n = doc->highlight(l->bytes, l->len, &state, doc->span_scratch, doc->span_scratch_capacity,
                           doc->highlight_user);
        if (n > doc->span_scratch_capacity)
            n = doc->span_scratch_capacity;
    }
    _free_spans(doc, l);
    l->state_in = state_in;
    l->state_out = state;
    l->coloured = TEXT_COLOURED_STATE;
    if (!keep)
        return true;
    if (doc->span_lines >= SPAN_LINES_MAX) {
        for (size_t i = 0; i < doc->line_count; i++)
            _free_spans(doc, &doc->lines[i]);
    }
    l->spans = malloc((n ? n : 1) * sizeof(TextSpan));
    if (!l->spans)
        return false;
    memcpy(l->spans, doc->span_scratch, n * sizeof(TextSpan));
    l->span_count = (uint32_t)n;
    l->coloured |= TEXT_COLOURED_SPANS;
    doc->span_lines++;
    return true;
}

bool text_doc_line_spans(TextDoc* doc, size_t line, const TextSpan** spans, size_t* count) {
    *spans = NULL;
    *count = 0;
    if (!doc || !doc->highlight || line >= doc->line_count)
        return false;
    uint32_t state_in = 0;
    if (doc->highlight_stateful && line > 0) {
        if (line > doc->highlight_known)
            return false;
        state_in = doc->lines[line - 1].state_out;
    }
    const TextLine* l = &doc->lines[line];
    if (!(l->coloured & TEXT_COLOURED_SPANS) || l->state_in != state_in) {
        if (!_colour(doc, line, state_in, true))
            return false;
    }
    if (doc->highlight_stateful && line == doc->highlight_known)
        doc->highlight_known = line + 1;
    *spans = l->spans;
    *count = l->span_count;
    return true;
}

bool text_doc_highlight_step(TextDoc* doc, size_t up_to, size_t max_lines) {
    if (!doc || !doc->highlight || !doc->highlight_stateful)
        return false;
    if (up_to > doc->line_count)
        up_to = doc->line_count;
    size_t coloured = 0;
    while (doc->highlight_known < up_to && coloured < max_lines) {
        const size_t line = doc->highlight_known;
        const uint32_t state_in = line ? doc->lines[line - 1].state_out : 0;
        const TextLine* l = &doc->lines[line];
        // A line already worked out from the same state leaves what it left before.
        if (!(l->coloured & TEXT_COLOURED_STATE) || l->state_in != state_in) {
            if (!_colour(doc, line, state_in, false))
                return false;
            coloured++;
        }
        doc->highlight_known = line + 1;
    }
    return doc->highlight_known < up_to;
}
