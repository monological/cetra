#include "text_doc.h"

#include <stdlib.h>
#include <string.h>

#include "ext/log.h"
#include "text.h"
#include "text_doc_edit.h"
#include "util.h"

// The most undo steps kept; past it the oldest go.
#define UNDO_MAX 10000
// Spans the ring holds: the lines coloured last keep theirs, and a line whose spans were written
// over colours itself again when it is next drawn. Many screens of lines, a few hundred KB.
#define SPAN_RING 65536

static const char* const ENDINGS[] = {"", "\n", "\r\n"};

static size_t _ending_len(uint8_t ending) {
    return ending == TEXT_ENDING_CRLF ? 2 : ending;
}

static size_t _raw_len(const TextLine* l) {
    return l->len + _ending_len(l->ending);
}

// A line whose bytes changed: its colour, and the state it leaves, are unknown.
static void _uncolour(TextLine* l) {
    l->state_known = false;
    l->spans_known = false;
}

static void _free_line(TextLine* l) {
    if (l->capacity)
        free(l->bytes);
}

static void* _dup(const void* p, size_t bytes) {
    void* out = malloc(bytes ? bytes : 1);
    if (out && bytes)
        memcpy(out, p, bytes);
    return out;
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
    for (size_t p = 0; p < s->part_count; p++) {
        const TextUndoPart* part = &s->parts[p];
        for (size_t i = 0; i < part->change_count; i++) {
            free((void*)part->changes[i].removed);
            free((void*)part->changes[i].inserted);
        }
        free(part->changes);
    }
    free(s->parts);
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
        _free_line(&doc->lines[i]);
    for (size_t i = 0; i < doc->undo_count + doc->redo_count; i++)
        _free_step(&doc->undo[i]);
    for (int i = 0; i < TEXT_DOC_LAYERS; i++)
        free(doc->ranges[i]);
    free(doc->typing.ends);
    free(doc->typing.last);
    free(doc->undo);
    free(doc->views);
    free(doc->lines);
    free(doc->span_ring);
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

char text_doc_byte(TextDoc* doc, size_t offset) {
    if (!doc || offset >= doc->length)
        return 0;
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

void text_doc_map_selection(TextSelection* s, const TextViewChange* c) {
    s->anchor = text_doc_map_offset(s->anchor, c->start, c->removed, c->inserted, c->actor);
    s->head = text_doc_map_offset(s->head, c->start, c->removed, c->inserted, c->actor);
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

// Lines from `first` changed: the starts after it, and every stateful line's state from it, are
// unknown.
static void _lines_changed(TextDoc* doc, size_t first) {
    if (doc->highlight_known > first)
        doc->highlight_known = first;
    if (doc->starts_good > first + 1)
        doc->starts_good = first + 1;
}

// Makes a line's bytes its own, with room for `need`. A line still in the block is copied whole
// before it is edited, so its room is for the longer of what it holds and what it will.
static bool _own(TextLine* l, size_t need) {
    if (!l->capacity && need < l->len)
        need = l->len;
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

// The change made inside line `first`'s content, the line edited in place: false when it touches a
// line break, or would leave a CR just before the line's LF, which makes the two one ending.
static bool _replace_in_line(TextDoc* doc, size_t first, size_t first_at, size_t start, size_t end,
                             const char* text, size_t len, bool* failed) {
    TextLine* l = &doc->lines[first];
    if (end > first_at + l->len || memchr(text, '\n', len) || memchr(text, '\r', len))
        return false;
    const size_t from = start - first_at, to = end - first_at;
    if (l->ending == TEXT_ENDING_LF && to == l->len && !len && from > 0 &&
        l->bytes[from - 1] == '\r')
        return false;
    const size_t n = l->len - (to - from) + len;
    if (!_own(l, n)) {
        log_error("text_doc: out of memory for a line of %zu bytes", n);
        *failed = true;
        return false;
    }
    memmove(l->bytes + from + len, l->bytes + to, l->len - to);
    memcpy(l->bytes + from, text, len);
    l->len = (uint32_t)n;
    _uncolour(l);
    return true;
}

// The change made by writing out the lines it touches, from the first's start to the last's
// ending, with the change made and split afresh: how many lines replaced the old ones, SIZE_MAX
// out of memory.
static size_t _replace_lines(TextDoc* doc, size_t first, size_t first_at, size_t start, size_t end,
                             const char* text, size_t len, size_t* old) {
    const size_t last = text_doc_line_of(doc, end);
    const size_t last_at = text_doc_line_start(doc, last);
    const TextLine* l = &doc->lines[first];
    const TextLine* ll = &doc->lines[last];
    const size_t head = start - first_at;
    const size_t tail = _raw_len(ll) - (end - last_at);
    char* joined = malloc(head + len + tail + 1);
    if (!joined) {
        log_error("text_doc: out of memory for a change of %zu bytes", len);
        return SIZE_MAX;
    }
    _raw_copy(l, 0, head, joined);
    memcpy(joined + head, text, len);
    _raw_copy(ll, end - last_at, _raw_len(ll), joined + head + len);
    TextLine* fresh = NULL;
    size_t fresh_capacity = 0;
    const bool trailing = ll->ending == TEXT_ENDING_NONE;
    const size_t count = _split(joined, head + len + tail, trailing, true, &fresh, &fresh_capacity);
    free(joined);
    *old = last - first + 1;
    if (count == SIZE_MAX || !grow_array((void**)&doc->lines, &doc->line_capacity,
                                         doc->line_count - *old + count, sizeof(TextLine), 64)) {
        for (size_t i = 0; count != SIZE_MAX && i < count; i++)
            free(fresh[i].bytes);
        free(fresh);
        log_error("text_doc: out of memory for a change of %zu bytes", len);
        return SIZE_MAX;
    }
    for (size_t i = first; i <= last; i++)
        _free_line(&doc->lines[i]);
    memmove(&doc->lines[first + count], &doc->lines[last + 1],
            (doc->line_count - last - 1) * sizeof(TextLine));
    memcpy(&doc->lines[first], fresh, count * sizeof(TextLine));
    free(fresh);
    doc->line_count = doc->line_count - *old + count;
    doc->lines[first].start = first_at;
    return count;
}

// The one place the text changes: [start, end) becomes `text`, both checked by the caller. Moves
// every range, the history, the version and every view but `held`, `actor`'s as an actor's.
static bool _replace(TextDoc* doc, size_t start, size_t end, const char* text, size_t len,
                     const void* actor, const void* held) {
    if (start == end && !len)
        return true;
    const size_t first = text_doc_line_of(doc, start);
    const size_t first_at = text_doc_line_start(doc, first);
    TextViewChange c = {first, 1, 1, start, end - start, len, false, false};
    bool failed = false;
    if (!_replace_in_line(doc, first, first_at, start, end, text, len, &failed)) {
        if (failed)
            return false;
        c.lines_inserted =
            _replace_lines(doc, first, first_at, start, end, text, len, &c.lines_removed);
        if (c.lines_inserted == SIZE_MAX)
            return false;
    }
    _lines_changed(doc, first);
    doc->length = doc->length - (end - start) + len;
    doc->history[doc->version % TEXT_DOC_HISTORY].start = start;
    doc->history[doc->version % TEXT_DOC_HISTORY].removed = end - start;
    doc->history[doc->version % TEXT_DOC_HISTORY].inserted = len;
    doc->version++;
    _map_ranges(doc, start, end - start, len);
    for (size_t i = 0; i < doc->view_count; i++) {
        const TextDocView* v = &doc->views[i];
        c.actor = v->view == actor;
        c.held = v->view == held;
        v->changed(v->view, &c);
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

// A copy of selections, which the caller frees; NULL for none, or out of memory.
static TextSelection* _copy_selections(const TextSelection* s, size_t n, size_t* count) {
    TextSelection* out = n ? _dup(s, n * sizeof(*s)) : NULL;
    *count = out ? n : 0;
    return out;
}

static TextSelection* _view_selections(TextDoc* doc, const void* view, size_t* count) {
    const TextDocView* v = _view(doc, view);
    const TextSelection* s = NULL;
    const size_t n = v && v->selections ? v->selections(v->view, &s) : 0;
    return _copy_selections(s, n, count);
}

static void _drop_redo(TextDoc* doc) {
    for (size_t i = 0; i < doc->redo_count; i++)
        _free_step(&doc->undo[doc->undo_count + i]);
    doc->redo_count = 0;
}

static void _forget_undo(TextDoc* doc) {
    _drop_redo(doc);
    for (size_t i = 0; i < doc->undo_count; i++)
        _free_step(&doc->undo[i]);
    doc->undo_count = 0;
    doc->undo_sealed = true;
}

static bool _is_space(char c) {
    return c == ' ' || c == '\t';
}

// Whether typing `changes` continues the typing before it at each of its carets: each change an
// insert right where that caret's text ends, and not a word's start -- a blank after a character
// that is not one. Kept apart from the undo history, so a host keeping its own joins the same.
static bool _joins(const TextDoc* doc, const TextDocChange* changes, size_t count,
                   const void* view) {
    if (doc->undo_sealed || !doc->typing.count || doc->typing.view != view ||
        doc->typing.count != count)
        return false;
    for (size_t k = 0; k < count; k++) {
        const TextDocChange* now = &changes[k];
        if (now->removed_len || !now->inserted_len || now->start != doc->typing.ends[k])
            return false;
        if (_is_space(now->inserted[0]) && !_is_space(doc->typing.last[k]))
            return false;
    }
    return true;
}

// What the next typing is joined against: where each caret's text ends after `changes`, which run
// from the highest start down, so change k's text stands moved by every change below it. Nothing
// unless the action was typing.
static void _note_typing(TextDoc* doc, const TextDocChange* changes, size_t count, const void* view,
                         bool typed) {
    doc->typing.count = 0;
    size_t cap = doc->typing.capacity;
    if (!typed || !count ||
        !grow_array((void**)&doc->typing.ends, &doc->typing.capacity, count, sizeof(size_t), 4) ||
        !grow_array((void**)&doc->typing.last, &cap, count, sizeof(char), 4))
        return;
    long long below = 0;
    for (size_t k = count; k-- > 0;) {
        const TextDocChange* c = &changes[k];
        doc->typing.ends[k] = (size_t)((long long)c->start + below) + c->inserted_len;
        doc->typing.last[k] = c->inserted_len ? c->inserted[c->inserted_len - 1] : ' ';
        below += (long long)c->inserted_len - (long long)c->removed_len;
    }
    doc->typing.view = view;
    doc->typing.count = count;
}

// How an action is kept: a step of its own, another action of the last step, or its typing added
// to the last action's.
typedef enum { KEEP_STEP, KEEP_PART, KEEP_MERGE } Keep;

// The changes, their bytes copied, into a part; false out of memory.
static bool _part(TextUndoPart* part, const TextDocChange* changes, size_t count) {
    part->changes = calloc(count ? count : 1, sizeof(TextDocChange));
    if (!part->changes)
        return false;
    for (size_t i = 0; i < count; i++) {
        part->changes[i] = changes[i];
        part->changes[i].removed = _dup(changes[i].removed, changes[i].removed_len);
        part->changes[i].inserted = _dup(changes[i].inserted, changes[i].inserted_len);
    }
    part->change_count = count;
    return true;
}

// Keeps an action for undo, as `how` says; with no last step to join, a step of its own.
static void _keep(TextDoc* doc, Keep how, const TextDocChange* changes, size_t count,
                  const TextSelection* before, size_t before_count, const TextSelection* after,
                  size_t after_count, void* view) {
    if (!doc->keep_undo)
        return;
    _drop_redo(doc);
    TextUndoStep* s = doc->undo_count ? &doc->undo[doc->undo_count - 1] : NULL;
    if (how != KEEP_STEP && s && s->part_count) {
        // Typing joins the last action's text caret by caret, which kept it with as many changes;
        // one kept before keep_undo was turned on may not have.
        if (how == KEEP_MERGE && s->parts[s->part_count - 1].change_count == count) {
            TextUndoPart* part = &s->parts[s->part_count - 1];
            for (size_t i = 0; i < count; i++) {
                TextDocChange* c = &part->changes[i];
                char* grown =
                    realloc((void*)c->inserted, c->inserted_len + changes[i].inserted_len);
                if (!grown)
                    return;
                memcpy(grown + c->inserted_len, changes[i].inserted, changes[i].inserted_len);
                c->inserted = grown;
                c->inserted_len += changes[i].inserted_len;
            }
        } else {
            TextUndoPart* parts = realloc(s->parts, (s->part_count + 1) * sizeof(TextUndoPart));
            if (!parts)
                return;
            s->parts = parts;
            if (!_part(&s->parts[s->part_count], changes, count))
                return;
            s->part_count++;
        }
        free(s->after);
        s->after = _copy_selections(after, after_count, &s->after_count);
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
    s = &doc->undo[doc->undo_count];
    memset(s, 0, sizeof(*s));
    s->parts = calloc(1, sizeof(TextUndoPart));
    if (!s->parts || !_part(&s->parts[0], changes, count)) {
        free(s->parts);
        s->parts = NULL;
        return;
    }
    s->part_count = 1;
    s->before = _copy_selections(before, before_count, &s->before_count);
    s->after = _copy_selections(after, after_count, &s->after_count);
    s->view = view;
    doc->undo_count++;
}

// How the action `changes` is kept, and its step: typing that joins the typing before it is the
// same step, as is any action inside a group past its first; anything else starts a step.
static Keep _joining(TextDoc* doc, const TextDocChange* changes, size_t count, const void* view,
                     bool typed) {
    const bool words = typed && _joins(doc, changes, count, view);
    const bool grouped = doc->grouping && !doc->group_fresh;
    doc->group_fresh = false;
    if (!words && !grouped)
        doc->step++;
    return words ? KEEP_MERGE : grouped ? KEEP_PART : KEEP_STEP;
}

void text_doc_seal_undo(TextDoc* doc) {
    if (doc)
        doc->undo_sealed = true;
}

void text_doc_group(TextDoc* doc, bool open) {
    if (!doc)
        return;
    doc->grouping = open;
    doc->group_fresh = open;
    doc->undo_sealed = true;
}

void text_doc_empty_step(TextDoc* doc, void* view) {
    if (!doc || (doc->grouping && !doc->group_fresh))
        return;
    doc->group_fresh = false;
    doc->step++;
    doc->typing.count = 0;
    doc->undo_sealed = true;
    if (!doc->keep_undo)
        return;
    _drop_redo(doc);
    if (doc->undo_count == UNDO_MAX) {
        _free_step(&doc->undo[0]);
        memmove(&doc->undo[0], &doc->undo[1], (UNDO_MAX - 1) * sizeof(TextUndoStep));
        doc->undo_count--;
    }
    if (!grow_array((void**)&doc->undo, &doc->undo_capacity, doc->undo_count + 1,
                    sizeof(TextUndoStep), 16))
        return;
    TextUndoStep* s = &doc->undo[doc->undo_count++];
    memset(s, 0, sizeof(*s));
    size_t n = 0;
    s->before = _view_selections(doc, view, &n);
    s->before_count = n;
    s->after = _copy_selections(s->before, n, &s->after_count);
    s->view = view;
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
        .step = doc->step,
    };
    doc->telling = true;
    doc->on_action(doc, &action, doc->on_action_user);
    doc->telling = false;
}

// Whether the text may change now, saying by name why not: not from inside on_action.
static bool _may_change(const TextDoc* doc, const char* who) {
    if (doc->telling)
        log_error("%s: the text is not changed from inside on_action", who);
    return !doc->telling;
}

// The one way changes are made: `changes`, from the highest start down, each in the text as the
// first found it. Every view but `held` has its carets moved through each, `actor`'s as an
// actor's. Out of memory part way, the changes made stand, and the undo history, which no longer
// matches the text, is let go. Returns how many were made.
static size_t _run(TextDoc* doc, const TextDocChange* changes, size_t count, const void* actor,
                   const void* held) {
    size_t made = 0;
    while (made < count &&
           _replace(doc, changes[made].start, changes[made].start + changes[made].removed_len,
                    changes[made].inserted, changes[made].inserted_len, actor, held))
        made++;
    if (made < count)
        _forget_undo(doc);
    return made;
}

// The changes `edits` make, from the highest start down, with the bytes each removes copied into
// one block for the caller to free; NULL out of memory.
static TextDocChange* _changes_of(TextDoc* doc, const TextEdit* edits, size_t count,
                                  char** removed) {
    size_t total = 1;
    for (size_t i = 0; i < count; i++)
        total += edits[i].end - edits[i].start;
    TextDocChange* changes = malloc((count ? count : 1) * sizeof(TextDocChange));
    *removed = malloc(total);
    if (!changes || !*removed) {
        free(changes);
        free(*removed);
        *removed = NULL;
        return NULL;
    }
    size_t at = 0;
    for (size_t i = 0; i < count; i++) {
        const TextEdit* e = &edits[count - 1 - i];
        const size_t n = text_doc_copy(doc, e->start, e->end, *removed + at);
        changes[i] = (TextDocChange){e->start, *removed + at, n, e->text ? e->text : "", e->len};
        at += n;
    }
    return changes;
}

bool text_doc_apply(TextDoc* doc, const TextEdit* edits, size_t count, void* view,
                    const TextSelection* after, size_t after_count, bool typed) {
    if (!doc || (!edits && count) || !_may_change(doc, "text_doc_apply"))
        return false;
    for (size_t i = 0; i < count; i++) {
        if (edits[i].start > edits[i].end || edits[i].end > doc->length ||
            (i && edits[i].start < edits[i - 1].end)) {
            log_error("text_doc_apply: edit %zu, bytes %zu to %zu, is out of order, overlaps "
                      "another or is past the text's %zu bytes",
                      i, edits[i].start, edits[i].end, doc->length);
            return false;
        }
    }
    char* removed = NULL;
    TextDocChange* changes = _changes_of(doc, edits, count, &removed);
    if (!changes) {
        log_error("text_doc_apply: out of memory");
        return false;
    }
    size_t before_count = 0;
    TextSelection* before = _view_selections(doc, view, &before_count);
    const uint64_t version = doc->version;
    const size_t made = _run(doc, changes, count, view, after ? view : NULL);
    if (made) {
        size_t now_count = after_count;
        TextSelection* now = after ? NULL : _view_selections(doc, view, &now_count);
        const TextSelection* left = after ? after : now;
        const Keep how = _joining(doc, changes, made, view, typed);
        if (made == count)
            _keep(doc, how, changes, made, before, before_count, left, now_count, view);
        _note_typing(doc, changes, made, view, typed && made == count);
        doc->undo_sealed = !typed;
        _tell(doc, changes, made, before, before_count, left, now_count, view, version);
        free(now);
    }
    free(before);
    free(changes);
    free(removed);
    return made == count;
}

// A step taken back (`undo`) or made again: its actions taken back last first, or made again
// first first, each one's changes, or their inverses, applied from the highest start down; the
// editor given the step's selections, and each action told as one of a new step.
static bool _step(TextDoc* doc, void* view, bool undo) {
    if (!doc || !doc->keep_undo || (undo ? !doc->undo_count : !doc->redo_count) ||
        !_may_change(doc, undo ? "text_doc_undo" : "text_doc_redo"))
        return false;
    TextUndoStep* s = &doc->undo[undo ? doc->undo_count - 1 : doc->undo_count];
    // An empty step takes nothing back and puts the selections back as it found them.
    size_t most = 1;
    for (size_t p = 0; p < s->part_count; p++)
        most = s->parts[p].change_count > most ? s->parts[p].change_count : most;
    TextDocChange* made = calloc(most, sizeof(TextDocChange));
    if (!made) {
        log_error("text_doc_undo: out of memory");
        return false;
    }
    const void* target = view ? view : s->view;
    const TextSelection* sel = undo ? s->before : s->after;
    const size_t sel_count = undo ? s->before_count : s->after_count;
    const TextSelection* from = undo ? s->after : s->before;
    const size_t from_count = undo ? s->after_count : s->before_count;
    // The editor given the step's selections is not moved through its changes.
    const TextDocView* v = sel_count ? _view(doc, target) : NULL;
    if (v && !v->set_selections)
        v = NULL;
    doc->grouping = false;
    doc->typing.count = 0;
    doc->step++;
    for (size_t p = 0; p < s->part_count; p++) {
        const size_t part = undo ? s->part_count - 1 - p : p;
        const TextUndoPart* a = &s->parts[part];
        const size_t n = a->change_count;
        // An action's changes are in the text as it found it. Taken back, each one's new bytes
        // stand where its start has been moved by every change below it.
        long long below = 0;
        for (size_t k = n; k-- > 0;) {
            const TextDocChange* c = &a->changes[k];
            made[k].start = undo ? (size_t)((long long)c->start + below) : c->start;
            made[k].removed = undo ? c->inserted : c->removed;
            made[k].removed_len = undo ? c->inserted_len : c->removed_len;
            made[k].inserted = undo ? c->removed : c->inserted;
            made[k].inserted_len = undo ? c->removed_len : c->inserted_len;
            below += (long long)c->inserted_len - (long long)c->removed_len;
        }
        const uint64_t version = doc->version;
        // Out of memory part way the history is let go, and the step with it.
        if (_run(doc, made, n, target, v ? target : NULL) < n) {
            free(made);
            return false;
        }
        const bool first = p == 0, last = p + 1 == s->part_count;
        _tell(doc, made, n, first ? from : NULL, first ? from_count : 0, last ? sel : NULL,
              last ? sel_count : 0, target, version);
    }
    if (v)
        v->set_selections(v->view, sel, sel_count);
    if (undo) {
        doc->undo_count--;
        doc->redo_count++;
    } else {
        doc->undo_count++;
        doc->redo_count--;
    }
    doc->undo_sealed = true;
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
    return text_utf8_is_continuation(text_doc_byte(doc, offset));
}

bool text_doc_replace(TextDoc* doc, size_t start, size_t end, const char* text, size_t len) {
    if (!doc || (!text && len) || !_may_change(doc, "text_doc_replace"))
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
    if (start == end && !len)
        return true;
    const TextEdit edit = {start, end, text, len};
    char* removed = NULL;
    TextDocChange* change = _changes_of(doc, &edit, 1, &removed);
    if (!change) {
        log_error("text_doc_replace: out of memory");
        return false;
    }
    const bool ok = _run(doc, change, 1, NULL, NULL) == 1;
    // A step of its own, the typing before it sealed and a group's next action a new step.
    doc->step++;
    doc->typing.count = 0;
    doc->undo_sealed = true;
    doc->group_fresh = doc->grouping;
    if (ok)
        _keep(doc, KEEP_STEP, change, 1, NULL, 0, NULL, 0, NULL);
    free(change);
    free(removed);
    return ok;
}

bool text_doc_set_text(TextDoc* doc, const char* text, size_t len) {
    if (!doc || (!text && len) || !_may_change(doc, "text_doc_set_text"))
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

// Whether every change since `version` is still in the history, saying by name when not.
static bool _history_reaches(const TextDoc* doc, uint64_t version, const char* who) {
    if (version <= doc->version && doc->version - version <= TEXT_DOC_HISTORY)
        return true;
    log_error("%s: version %llu is not within the last %d changes of the text's %llu", who,
              (unsigned long long)version, TEXT_DOC_HISTORY, (unsigned long long)doc->version);
    return false;
}

bool text_doc_map(const TextDoc* doc, uint64_t version, size_t* offset) {
    if (!doc || !offset || !_history_reaches(doc, version, "text_doc_map"))
        return false;
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
    if (!_history_reaches(doc, version, "text_doc_set_ranges"))
        return false;
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

void text_doc_set_highlighter(TextDoc* doc, TextHighlightFn fn, void* user, bool stateful) {
    if (!doc)
        return;
    for (size_t i = 0; i < doc->line_count; i++)
        _uncolour(&doc->lines[i]);
    doc->highlight_known = 0;
    // Room for a line's spans, so only a line with more than this asks its highlighter twice.
    if (fn && (!grow_array((void**)&doc->span_scratch, &doc->span_scratch_capacity, 64,
                           sizeof(TextSpan), 64) ||
               (!doc->span_ring && !(doc->span_ring = malloc(SPAN_RING * sizeof(TextSpan)))))) {
        log_error("text_doc_set_highlighter: out of memory");
        fn = NULL;
    }
    doc->highlight = fn;
    doc->highlight_user = user;
    doc->highlight_stateful = stateful;
}

// Whether a line's spans are still in the ring: written no further back than its length.
static bool _spans_kept(const TextDoc* doc, const TextLine* l) {
    return l->spans_known && doc->span_written - l->span_at <= SPAN_RING;
}

// A line coloured from `state_in`, unless it already is: its state out always, and its spans,
// written into the ring, when `spans`. Whether it now is.
static bool _coloured(TextDoc* doc, size_t line, uint32_t state_in, bool spans) {
    TextLine* l = &doc->lines[line];
    if (l->state_known && l->state_in == state_in && (!spans || _spans_kept(doc, l)))
        return true;
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
    l->state_in = state_in;
    l->state_out = state;
    l->state_known = true;
    l->spans_known = spans;
    if (!spans)
        return true;
    if (n > SPAN_RING)
        n = SPAN_RING;
    // A line's spans are one piece: ones that would run past the ring's end start it again.
    uint64_t at = doc->span_written;
    if (at % SPAN_RING + n > SPAN_RING)
        at += SPAN_RING - at % SPAN_RING;
    memcpy(&doc->span_ring[at % SPAN_RING], doc->span_scratch, n * sizeof(TextSpan));
    doc->span_written = at + n;
    l->span_at = at;
    l->span_count = (uint32_t)n;
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
    if (!_coloured(doc, line, state_in, true))
        return false;
    if (doc->highlight_stateful && line == doc->highlight_known)
        doc->highlight_known = line + 1;
    const TextLine* l = &doc->lines[line];
    *spans = &doc->span_ring[l->span_at % SPAN_RING];
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
        if (!l->state_known || l->state_in != state_in) {
            if (!_coloured(doc, line, state_in, false))
                return false;
            coloured++;
        }
        doc->highlight_known = line + 1;
    }
    return doc->highlight_known < up_to;
}
