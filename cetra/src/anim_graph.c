#include "anim_graph.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ext/log.h"

typedef struct Source {
    const char* name; // borrowed
    AnimatorEntry entries[ANIMATOR_SPACE_MAX];
    int count;
} Source;

struct AnimGraph {
    // ENGINE-OWNED (by the graph): what was registered, and what bind decided.
    Source sources[ANIM_GRAPH_SOURCE_MAX];
    int source_count;

    // BY FUNCTION: anim_graph_set_params / _set_states / _set_transitions.
    // Borrowed const tables; the caller keeps them alive.
    const AnimGraphParam* params;
    int param_count;
    const AnimGraphState* states;
    int state_count;
    const AnimGraphTransition* rows;
    int row_count;

    // BY FUNCTION: anim_graph_bind. Borrowed, and the resolution it settled.
    Animator* animator;
    // Per state: the source index bind resolved, or -1 for a state that plays
    // nothing, or -2 for a state whose source never registered. A row whose
    // `to` resolved to -2 is dead and never evaluated.
    int state_source[ANIM_GRAPH_STATE_MAX];
    bool row_live[ANIM_GRAPH_TRANSITION_MAX];
    bool bound;

    // The parameter block. Parallel to `params`, by index.
    float values[ANIM_GRAPH_PARAM_MAX];

    // Where it is. `state` is an index into `states`.
    int state;
    int previous;
    float seconds;
    // FINISHED, LATCHED on entry rather than read live: the animator's edge is
    // one frame wide and never re-arms, so a row ANDing it with a condition
    // that fails on that frame would lose it for good.
    bool source_finished;
    bool enabled;

    // BY FUNCTION: anim_graph_set_guard_user. Borrowed.
    void* guard_user;
};

// Once per name: a typo is written every frame and the first line says it all.
// input.c carries the same eight-slot table for the same reason -- a shared
// helper would have to live in util.h, which drags GL into a file whose whole
// claim is that it has none.
static void _log_unknown(const char* what, const char* name) {
    static char logged[8][48];
    static int logged_count;
    for (int i = 0; i < logged_count; i++) {
        if (strncmp(logged[i], name, sizeof(logged[i]) - 1) == 0)
            return;
    }
    if (logged_count < 8)
        snprintf(logged[logged_count++], sizeof(logged[0]), "%s", name);
    log_error("anim_graph: no %s named '%s'", what, name);
}

static int _param_index(const AnimGraph* g, const char* name) {
    if (!name)
        return -1;
    for (int i = 0; i < g->param_count; i++) {
        if (g->params[i].name && strcmp(g->params[i].name, name) == 0)
            return i;
    }
    return -1;
}

static int _state_index(const AnimGraph* g, const char* name) {
    if (!name)
        return -1;
    for (int i = 0; i < g->state_count; i++) {
        if (g->states[i].name && strcmp(g->states[i].name, name) == 0)
            return i;
    }
    return -1;
}

static int _source_index(const AnimGraph* g, const char* name) {
    if (!name)
        return -1;
    for (int i = 0; i < g->source_count; i++) {
        if (strcmp(g->sources[i].name, name) == 0)
            return i;
    }
    return -1;
}

AnimGraph* create_anim_graph(void) {
    AnimGraph* g = calloc(1, sizeof(AnimGraph));
    if (!g) {
        log_error("create_anim_graph: out of memory");
        return NULL;
    }
    g->state = -1;
    g->previous = -1;
    g->enabled = true;
    return g;
}

void free_anim_graph(AnimGraph* graph) {
    free(graph);
}

bool anim_graph_add_source(AnimGraph* graph, const char* name, const AnimatorEntry* entries,
                           int count) {
    if (!graph || !name || !entries) {
        log_error("anim_graph_add_source: NULL graph, name or entries");
        return false;
    }
    if (count < 1 || count > ANIMATOR_SPACE_MAX) {
        log_error("anim_graph_add_source: '%s' holds 1..%d entries, not %d", name,
                  ANIMATOR_SPACE_MAX, count);
        return false;
    }
    /*
     * An entry with no clip REFUSES the whole source, by name, and that refusal
     * is the feature rather than a guard against a caller's mistake.
     *
     * An app finds its clips by name on whatever rig it was handed, so a missing
     * one arrives here as a NULL. Refusing the source is what turns "this rig has
     * no stroke" into "this state is unreachable" into "every row into it is
     * pruned" -- one path, decided once, where the alternative is a clip-presence
     * test on every transition that mentions it and an asymmetry nobody notices.
     */
    for (int i = 0; i < count; i++) {
        if (!entries[i].clip) {
            log_info("anim_graph: '%s' needs a clip entry %d does not carry; not registered", name,
                     i);
            return false;
        }
    }
    if (_source_index(graph, name) >= 0) {
        log_error("anim_graph_add_source: '%s' is already registered", name);
        return false;
    }
    if (graph->source_count >= ANIM_GRAPH_SOURCE_MAX) {
        log_error("anim_graph_add_source: '%s' is past the %d-source maximum", name,
                  ANIM_GRAPH_SOURCE_MAX);
        return false;
    }
    Source* s = &graph->sources[graph->source_count++];
    s->name = name;
    s->count = count;
    for (int i = 0; i < count; i++)
        s->entries[i] = entries[i];
    return true;
}

void anim_graph_set_params(AnimGraph* graph, const AnimGraphParam* params, int count) {
    if (!graph)
        return;
    if (count > ANIM_GRAPH_PARAM_MAX) {
        log_error("anim_graph_set_params: %d is past the %d-parameter maximum; refused", count,
                  ANIM_GRAPH_PARAM_MAX);
        return;
    }
    graph->params = params;
    graph->param_count = params ? count : 0;
}

void anim_graph_set_states(AnimGraph* graph, const AnimGraphState* states, int count) {
    if (!graph)
        return;
    if (count > ANIM_GRAPH_STATE_MAX) {
        log_error("anim_graph_set_states: %d is past the %d-state maximum; refused", count,
                  ANIM_GRAPH_STATE_MAX);
        return;
    }
    graph->states = states;
    graph->state_count = states ? count : 0;
}

void anim_graph_set_transitions(AnimGraph* graph, const AnimGraphTransition* rows, int count) {
    if (!graph)
        return;
    if (count > ANIM_GRAPH_TRANSITION_MAX) {
        log_error("anim_graph_set_transitions: %d is past the %d-row maximum; refused", count,
                  ANIM_GRAPH_TRANSITION_MAX);
        return;
    }
    graph->rows = rows;
    graph->row_count = rows ? count : 0;
}

void anim_graph_set_guard_user(AnimGraph* graph, void* user) {
    if (graph)
        graph->guard_user = user;
}

/*
 * ------------------------------------------------------------------ playback
 */

// What a state entry does to the animator. `fade` is the transition's, because
// a fade is between two things and belongs to neither alone.
static void _play(AnimGraph* g, int state, float fade) {
    const AnimGraphState* s = &g->states[state];
    const int src = g->state_source[state];
    if (!g->animator)
        return;
    if (src < 0) {
        // A state that plays nothing STOPS the animator. Leaving the last
        // source running is not the same as not touching it: its clock keeps
        // advancing, its events keep firing and its root motion keeps
        // accumulating with nobody draining it.
        animator_stop(g->animator);
        return;
    }
    const Source* source = &g->sources[src];
    const bool looping = s->kind == ANIM_GRAPH_LOOP;
    if (s->kind == ANIM_GRAPH_RETURN && source->count == 1) {
        animator_play_once(g->animator, source->entries[0].clip, fade);
    } else if (source->count == 1) {
        animator_play(g->animator, source->entries[0].clip, fade, looping);
    } else {
        animator_play_space(g->animator, source->name, source->entries, source->count, fade,
                            looping);
    }
    // One writer of `speed` per frame: a state with no rate parameter pins it
    // on entry and then leaves it, rather than a branch re-asserting it every
    // tick the way a hand-rolled machine ends up doing.
    if (!s->rate)
        g->animator->speed = 1.0f;
}

static void _enter(AnimGraph* g, int state, float fade) {
    g->previous = g->state;
    g->state = state;
    g->seconds = 0.0f;
    g->source_finished = false;
    _play(g, state, fade);
}

/*
 * ---------------------------------------------------------------- conditions
 */

static bool _condition_holds(const AnimGraph* g, const AnimGraphCondition* c) {
    switch (c->op) {
        case ANIM_GRAPH_OP_NONE:
            return true; // an unused row
        case ANIM_GRAPH_FINISHED:
            return g->source_finished;
        case ANIM_GRAPH_ELAPSED:
            return g->seconds >= c->value;
        default:
            break;
    }
    const int p = _param_index(g, c->param);
    if (p < 0)
        return false; // bind refused this already; belt and braces
    const float v = g->values[p];
    switch (c->op) {
        case ANIM_GRAPH_GT:
            return v > c->value;
        case ANIM_GRAPH_GTE:
            return v >= c->value;
        case ANIM_GRAPH_LT:
            return v < c->value;
        case ANIM_GRAPH_LTE:
            return v <= c->value;
        case ANIM_GRAPH_EQ:
            return v == c->value;
        case ANIM_GRAPH_NEQ:
            return v != c->value;
        case ANIM_GRAPH_TRUE:
        case ANIM_GRAPH_FIRED:
            return v != 0.0f;
        case ANIM_GRAPH_FALSE:
            return v == 0.0f;
        default:
            return false;
    }
}

// Whether every condition on a row holds, and whether the row reads a trigger --
// which the caller needs, because a trigger is consumed by the row it FIRES and
// by nothing else.
static bool _row_holds(const AnimGraph* g, const AnimGraphTransition* r) {
    for (int i = 0; i < ANIM_GRAPH_COND_MAX; i++) {
        if (!_condition_holds(g, &r->conditions[i]))
            return false;
    }
    // The guard is ANDed AFTER, and is not reached at all by a row whose
    // conditions already failed, so a guard with a side effect is not a hidden
    // per-frame call.
    if (r->guard && !r->guard(g, g->guard_user))
        return false;
    return true;
}

/*
 * ---------------------------------------------------------------- parameters
 */

static void _write(AnimGraph* g, const char* name, AnimGraphParamKind want, float value) {
    if (!g || !name)
        return;
    const int p = _param_index(g, name);
    if (p < 0) {
        _log_unknown("parameter", name);
        return;
    }
    if (g->params[p].kind != want) {
        _log_unknown("parameter of that kind", name);
        return;
    }
    g->values[p] = value;
}

void anim_graph_set_float(AnimGraph* graph, const char* name, float value) {
    _write(graph, name, ANIM_GRAPH_FLOAT, value);
}

void anim_graph_set_bool(AnimGraph* graph, const char* name, bool value) {
    _write(graph, name, ANIM_GRAPH_BOOL, value ? 1.0f : 0.0f);
}

void anim_graph_fire(AnimGraph* graph, const char* name) {
    _write(graph, name, ANIM_GRAPH_TRIGGER, 1.0f);
}

float anim_graph_float(const AnimGraph* graph, const char* name) {
    if (!graph)
        return 0.0f;
    const int p = _param_index(graph, name);
    return p < 0 ? 0.0f : graph->values[p];
}

bool anim_graph_bool(const AnimGraph* graph, const char* name) {
    return anim_graph_float(graph, name) != 0.0f;
}

const char* anim_graph_state_name(const AnimGraph* graph) {
    if (!graph || graph->state < 0)
        return "";
    return graph->states[graph->state].name;
}

float anim_graph_state_seconds(const AnimGraph* graph) {
    return graph ? graph->seconds : 0.0f;
}

const char* anim_graph_previous_state(const AnimGraph* graph) {
    if (!graph || graph->previous < 0)
        return "";
    return graph->states[graph->previous].name;
}

void anim_graph_set_enabled(AnimGraph* graph, bool enabled) {
    if (graph)
        graph->enabled = enabled;
}

bool anim_graph_enabled(const AnimGraph* graph) {
    return graph ? graph->enabled : false;
}

void anim_graph_enter(AnimGraph* graph, const char* state, float fade) {
    if (!graph || !graph->bound)
        return;
    const int s = _state_index(graph, state);
    if (s < 0) {
        _log_unknown("state", state ? state : "(null)");
        return;
    }
    if (graph->state_source[s] == -2) {
        log_error("anim_graph_enter: '%s' plays a source this rig does not carry", state);
        return;
    }
    _enter(graph, s, fade);
}

/*
 * -------------------------------------------------------------------- update
 */

void anim_graph_update(AnimGraph* graph, float dt) {
    if (!graph || !graph->bound || !graph->enabled || graph->state < 0)
        return;

    graph->seconds += dt;
    if (graph->animator && animator_finished(graph->animator))
        graph->source_finished = true;

    int fired = -1;
    for (int i = 0; i < graph->row_count; i++) {
        if (!graph->row_live[i])
            continue;
        const AnimGraphTransition* r = &graph->rows[i];
        // A row out of a state we are not in. `from` NULL is ANY, and is for
        // interrupts: a fallback names its `from`, because an ANY row's
        // condition is a superset of the specific rows' preconditions.
        if (r->from && strcmp(r->from, graph->states[graph->state].name) != 0)
            continue;
        // Never into the state we are already in, or an interrupt that stays
        // true re-issues its own play every tick and the clip never advances
        // past its first frame.
        if (_state_index(graph, r->to) == graph->state)
            continue;
        if (!_row_holds(graph, r))
            continue;
        fired = i;
        break; // AT MOST ONE, and no chaining: see the header.
    }

    if (fired >= 0) {
        const AnimGraphTransition* r = &graph->rows[fired];
        // A trigger is consumed by the row it FIRED and by no other.
        for (int c = 0; c < ANIM_GRAPH_COND_MAX; c++) {
            if (r->conditions[c].op != ANIM_GRAPH_FIRED)
                continue;
            const int p = _param_index(graph, r->conditions[c].param);
            if (p >= 0)
                graph->values[p] = 0.0f;
        }
        _enter(graph, _state_index(graph, r->to), r->fade);
    }

    // Every trigger the tick did not consume is gone at the end of it: an edge
    // nobody was listening for is an edge, not a fact.
    for (int i = 0; i < graph->param_count; i++) {
        if (graph->params[i].kind == ANIM_GRAPH_TRIGGER)
            graph->values[i] = 0.0f;
    }

    // The state's own writes, after any transition, so the frame a state is
    // entered is already carrying its parameters.
    const AnimGraphState* s = &graph->states[graph->state];
    if (graph->animator) {
        if (s->param)
            graph->animator->param = anim_graph_float(graph, s->param);
        if (s->rate)
            graph->animator->speed = anim_graph_float(graph, s->rate);
    }
}

/*
 * ---------------------------------------------------------------------- bind
 */

// Which ops a parameter of each kind can answer. A float's truthiness is a trap
// and a trigger is not a quantity, so neither is allowed to be tested as the
// other -- refused at bind, where a table is read once, rather than read as
// false every frame.
static bool _op_fits_kind(AnimGraphOp op, AnimGraphParamKind kind) {
    switch (kind) {
        case ANIM_GRAPH_FLOAT:
            return op == ANIM_GRAPH_GT || op == ANIM_GRAPH_GTE || op == ANIM_GRAPH_LT ||
                   op == ANIM_GRAPH_LTE || op == ANIM_GRAPH_EQ || op == ANIM_GRAPH_NEQ;
        case ANIM_GRAPH_BOOL:
            return op == ANIM_GRAPH_TRUE || op == ANIM_GRAPH_FALSE;
        case ANIM_GRAPH_TRIGGER:
            return op == ANIM_GRAPH_FIRED;
        default:
            return false;
    }
}

static const char* _op_name(AnimGraphOp op) {
    switch (op) {
        case ANIM_GRAPH_GT:
            return ">";
        case ANIM_GRAPH_GTE:
            return ">=";
        case ANIM_GRAPH_LT:
            return "<";
        case ANIM_GRAPH_LTE:
            return "<=";
        case ANIM_GRAPH_EQ:
            return "==";
        case ANIM_GRAPH_NEQ:
            return "!=";
        case ANIM_GRAPH_TRUE:
            return "is true";
        case ANIM_GRAPH_FALSE:
            return "is false";
        case ANIM_GRAPH_FIRED:
            return "fired";
        case ANIM_GRAPH_FINISHED:
            return "finished";
        case ANIM_GRAPH_ELAPSED:
            return "elapsed";
        default:
            return "?";
    }
}

// Every authoring error in one row, reported by name. Returns false on the
// first, because a table with a typo in it is refused WHOLE -- input_bind's
// rule, and the reason is that a graph half-built is a character that behaves
// almost right.
static bool _check_row(const AnimGraph* g, int i) {
    const AnimGraphTransition* r = &g->rows[i];
    if (!r->to) {
        log_error("anim_graph_bind: row %d names no destination", i);
        return false;
    }
    const int to = _state_index(g, r->to);
    if (to < 0) {
        log_error("anim_graph_bind: row %d goes to '%s', which is not a state", i, r->to);
        return false;
    }
    if (r->from) {
        const int from = _state_index(g, r->from);
        if (from < 0) {
            log_error("anim_graph_bind: row %d comes from '%s', which is not a state", i, r->from);
            return false;
        }
        if (from == to) {
            log_error("anim_graph_bind: row %d is '%s' to itself, which is a spelling error", i,
                      r->from);
            return false;
        }
        // The animator resumes a RETURN state on the very frame its edge fires,
        // so a row authored on that edge would be a second writer in the same
        // frame doing a different thing.
        if (g->states[from].kind == ANIM_GRAPH_RETURN) {
            for (int c = 0; c < ANIM_GRAPH_COND_MAX; c++) {
                if (r->conditions[c].op == ANIM_GRAPH_FINISHED) {
                    log_error("anim_graph_bind: row %d leaves '%s' on finished, but that state "
                              "returns by itself",
                              i, r->from);
                    return false;
                }
            }
        }
    }
    for (int c = 0; c < ANIM_GRAPH_COND_MAX; c++) {
        const AnimGraphCondition* cond = &r->conditions[c];
        if (cond->op == ANIM_GRAPH_OP_NONE)
            continue;
        if (cond->op == ANIM_GRAPH_FINISHED || cond->op == ANIM_GRAPH_ELAPSED) {
            if (cond->param) {
                log_error("anim_graph_bind: row %d reads the machine with '%s' but names a "
                          "parameter",
                          i, _op_name(cond->op));
                return false;
            }
            continue;
        }
        const int p = _param_index(g, cond->param);
        if (p < 0) {
            log_error("anim_graph_bind: row %d reads '%s', which is not a parameter", i,
                      cond->param ? cond->param : "(null)");
            return false;
        }
        if (!_op_fits_kind(cond->op, g->params[p].kind)) {
            log_error("anim_graph_bind: row %d asks '%s' %s, which its kind cannot answer", i,
                      cond->param, _op_name(cond->op));
            return false;
        }
    }
    return true;
}

bool anim_graph_bind(AnimGraph* graph, Animator* animator, const char* start) {
    // A NULL animator binds: the graph decides and plays nothing, which is what
    // asserting the table itself wants. A NULL start does not -- a machine has
    // to begin somewhere and there is no sensible default to pick for a caller.
    if (!graph || !start) {
        log_error("anim_graph_bind: NULL graph or start state");
        return false;
    }
    if (graph->state_count < 1 || graph->row_count < 1) {
        log_error("anim_graph_bind: a graph needs at least one state and one row");
        return false;
    }
    for (int i = 0; i < graph->state_count; i++) {
        if (!graph->states[i].name) {
            log_error("anim_graph_bind: state %d has no name", i);
            return false;
        }
        for (int j = 0; j < i; j++) {
            if (strcmp(graph->states[i].name, graph->states[j].name) == 0) {
                log_error("anim_graph_bind: two states are named '%s'", graph->states[i].name);
                return false;
            }
        }
    }
    for (int i = 0; i < graph->row_count; i++) {
        if (!_check_row(graph, i))
            return false;
    }

    // Resolve each state against what the rig actually carries. This is the
    // clip-presence guard every transition used to carry, applied once.
    int missing = 0;
    char names[256] = {0};
    for (int i = 0; i < graph->state_count; i++) {
        const char* source = graph->states[i].source;
        if (!source) {
            graph->state_source[i] = -1; // plays nothing, deliberately
            continue;
        }
        const int s = _source_index(graph, source);
        graph->state_source[i] = s >= 0 ? s : -2;
        if (s < 0) {
            const size_t used = strlen(names);
            snprintf(names + used, sizeof(names) - used, "%s%s", missing ? ", " : "",
                     graph->states[i].name);
            missing++;
        }
    }
    if (missing > 0) {
        log_info("anim_graph: %d state(s) this rig cannot reach: %s", missing, names);
    }

    for (int i = 0; i < graph->row_count; i++) {
        const int to = _state_index(graph, graph->rows[i].to);
        graph->row_live[i] = graph->state_source[to] != -2;
    }

    // A state with no live way out. Forward reachability is half the analysis
    // and this is the silent half: a graph that can enter something it can
    // never leave reads as a character that stops responding.
    for (int i = 0; i < graph->state_count; i++) {
        if (graph->state_source[i] == -2)
            continue;
        bool exit_exists = false;
        for (int r = 0; r < graph->row_count && !exit_exists; r++) {
            if (!graph->row_live[r])
                continue;
            if (!graph->rows[r].from || strcmp(graph->rows[r].from, graph->states[i].name) == 0)
                exit_exists = _state_index(graph, graph->rows[r].to) != i;
        }
        if (!exit_exists)
            log_warn("anim_graph: '%s' has no way out on this rig", graph->states[i].name);
    }

    const int s = _state_index(graph, start);
    if (s < 0 || graph->state_source[s] == -2) {
        log_error("anim_graph_bind: cannot start in '%s'", start);
        return false;
    }

    if (animator && animator->graph && animator->graph != graph) {
        log_warn("anim_graph_bind: an animator drives one graph; the previous one stands down");
        animator->graph->animator = NULL;
        animator->graph->bound = false;
    }
    graph->animator = animator;
    if (animator)
        animator->graph = graph;
    graph->bound = true;
    graph->state = -1;
    graph->previous = -1;
    // A hard cut into the initial state: there is nothing to fade FROM, and a
    // fade from nothing is a fade from the bind pose.
    _enter(graph, s, 0.0f);
    return true;
}

void anim_graph_print(const AnimGraph* graph) {
    if (!graph) {
        printf("anim_graph: (none)\n");
        return;
    }
    printf("anim_graph: %d state(s), %d row(s), %d parameter(s), %d source(s)\n",
           graph->state_count, graph->row_count, graph->param_count, graph->source_count);
    for (int i = 0; i < graph->state_count; i++) {
        const AnimGraphState* s = &graph->states[i];
        const char* mark = !graph->bound                  ? "?"
                           : graph->state_source[i] == -2 ? "x"
                           : i == graph->state            ? "*"
                                                          : " ";
        printf("  %s %-12s source %-14s param %-10s rate %-10s\n", mark, s->name,
               s->source ? s->source : "(none)", s->param ? s->param : "-",
               s->rate ? s->rate : "-");
    }
    for (int i = 0; i < graph->row_count; i++) {
        const AnimGraphTransition* r = &graph->rows[i];
        printf("  %s %-12s -> %-12s fade %.2f", graph->bound && !graph->row_live[i] ? "x" : " ",
               r->from ? r->from : "(any)", r->to, (double)r->fade);
        for (int c = 0; c < ANIM_GRAPH_COND_MAX; c++) {
            const AnimGraphCondition* cond = &r->conditions[c];
            if (cond->op == ANIM_GRAPH_OP_NONE)
                continue;
            printf("  [%s %s %g]", cond->param ? cond->param : "", _op_name(cond->op),
                   (double)cond->value);
        }
        printf("%s\n", r->guard ? "  +guard" : "");
    }
}
