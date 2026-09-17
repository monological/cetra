#ifndef _ANIM_GRAPH_H_
#define _ANIM_GRAPH_H_

/*
 * What PLAYS, and WHEN (spec 12.20).
 *
 * `animator.h:27` states the non-goal this closes, and closes it from OUTSIDE
 * rather than by amending it: "There is no state machine here. A game decides
 * what plays and when; this fades, blends and reports." That sentence is still
 * true of `animator.c`. This is a game's decision made into DATA -- a table of
 * states and a table of transitions -- and everything it ever does to an
 * Animator goes through that file's existing public calls.
 *
 * WHAT A GRAPH IS: a parameter block, a current state, and how long it has been
 * in it. What is PLAYING is the Animator's -- `animator_source_name`, `fading`,
 * `base.time` -- and is deliberately not mirrored here. Two records of one fact
 * is how a load restores one of them and not the other.
 *
 * WHY THE APP REGISTERS SOURCES RATHER THAN THE TABLE DECLARING THEM: an entry
 * carries a stride this engine MEASURED off the clip's own feet (spec 12.10) or
 * a travel the clip STATES (12.18), and only the caller knows which of its
 * entries were supposed to move. That judgement is ninety lines in the one app
 * that does it and none of it belongs here.
 *
 * THE BASE LAYER ONLY, and that is a decision rather than an omission. The
 * override layer's mask is a BONE NAME, which is rig-specific in exactly the
 * way an arbitrary character breaks; `animator_layer_finished` is a LATCH where
 * `animator_finished` is an EDGE, so one condition cannot mean both; there is
 * exactly ONE layer, so a second machine over it would have no vocabulary for
 * what beats what; and a non-looping layer clip already releases itself. If
 * layers ever become plural that is a change to the ANIMATOR, and this follows
 * it.
 *
 * NO GL, NO PHYSICS, NO CLOCK AND NO GAME TYPE reach this file -- `camera_rig.h`'s
 * rule, for `camera_rig.h`'s reason, so an arm can assert a whole machine with
 * no window. It names one engine type, `Animator`, and that is the one stated
 * departure: a machine over playback that cannot ask what is playing is not a
 * machine. The dt is handed in.
 */

#include <stdbool.h>

#include "animator.h"

// Sixteen and twenty-four are what one readable table holds. Both are refused by
// name rather than truncated, `camera_rig_set_rail`'s rule: a graph silently
// missing its last state is a character that stops doing something nobody chose.
#define ANIM_GRAPH_SOURCE_MAX     16
#define ANIM_GRAPH_STATE_MAX      24
#define ANIM_GRAPH_PARAM_MAX      16
#define ANIM_GRAPH_TRANSITION_MAX 64
// FOUR, and the number is not arbitrary: three is exactly what the hardest row
// in the one app that has a table needs, which is the count that is one short
// for the next app.
#define ANIM_GRAPH_COND_MAX 4

/*
 * ---------------------------------------------------------------- parameters
 */

typedef enum AnimGraphParamKind {
    ANIM_GRAPH_FLOAT = 0,
    ANIM_GRAPH_BOOL,
    /*
     * Set by the app, consumed by the transition it fires, and gone at the end
     * of the update that did not fire one -- `input_action_pressed`'s edge
     * model, and the two line up because input edges are per FRAME and a graph
     * ticks per frame.
     *
     * A trigger is for something that happens once and is acted on immediately.
     * It is the WRONG shape for a fact that has to survive until a condition
     * several frames away reads it; a bool the app clears is right there, and a
     * measured float is better still, because then nothing is remembered.
     */
    ANIM_GRAPH_TRIGGER,
} AnimGraphParamKind;

typedef struct AnimGraphParam {
    const char* name; // borrowed
    AnimGraphParamKind kind;
} AnimGraphParam;

/*
 * ---------------------------------------------------------------- conditions
 */

typedef enum AnimGraphOp {
    ANIM_GRAPH_OP_NONE = 0, // an unused row, which is what a zeroed one is
    ANIM_GRAPH_GT,
    ANIM_GRAPH_GTE,
    ANIM_GRAPH_LT,
    ANIM_GRAPH_LTE,
    /*
     * Exact, and deliberately so. A parameter a game assigns from a small
     * integer is exactly representable, so a CATEGORY -- a medium, a stance, a
     * weapon, a gait -- is testable. Every game has at least one, and a
     * vocabulary without this is what makes people write `GT 0.5 && LT 1.5`.
     * Comparing a MEASURED float with it is the caller's mistake to make.
     */
    ANIM_GRAPH_EQ,
    ANIM_GRAPH_NEQ,
    ANIM_GRAPH_TRUE,
    ANIM_GRAPH_FALSE,
    ANIM_GRAPH_FIRED,
    /*
     * The two that read the MACHINE rather than a parameter, so their `param`
     * is NULL.
     *
     * FINISHED is LATCHED on entry to the state and not read live, and that is
     * the whole reason it works: `animator_finished` is a per-frame EDGE that
     * never re-arms once missed, so a row ANDing it with anything else would
     * lose the edge on the first frame the other condition failed and park the
     * graph on a non-looping clip held at its last tick, permanently.
     */
    ANIM_GRAPH_FINISHED,
    ANIM_GRAPH_ELAPSED, // seconds in this state >= value
    ANIM_GRAPH_OP_COUNT,
} AnimGraphOp;

typedef struct AnimGraphCondition {
    const char* param; // borrowed; NULL for FINISHED and ELAPSED
    AnimGraphOp op;
    float value;
} AnimGraphCondition;

// The conditions of a row, as initialisers. They earn their place the way
// INPUT_KEY does: the op sits beside its operand, and a zeroed row reads as
// unused without anybody writing ANIM_GRAPH_OP_NONE.
#define ANIM_GT(p, v)  {p, ANIM_GRAPH_GT, v}
#define ANIM_GTE(p, v) {p, ANIM_GRAPH_GTE, v}
#define ANIM_LT(p, v)  {p, ANIM_GRAPH_LT, v}
#define ANIM_LTE(p, v) {p, ANIM_GRAPH_LTE, v}
#define ANIM_EQ(p, v)  {p, ANIM_GRAPH_EQ, v}
#define ANIM_NEQ(p, v) {p, ANIM_GRAPH_NEQ, v}
#define ANIM_ON(p)     {p, ANIM_GRAPH_TRUE, 0.0f}
#define ANIM_OFF(p)    {p, ANIM_GRAPH_FALSE, 0.0f}
#define ANIM_FIRED(p)  {p, ANIM_GRAPH_FIRED, 0.0f}
#define ANIM_DONE()    {NULL, ANIM_GRAPH_FINISHED, 0.0f}
#define ANIM_AFTER(s)  {NULL, ANIM_GRAPH_ELAPSED, s}

/*
 * -------------------------------------------------------------------- states
 */

typedef enum AnimGraphStateKind {
    // Plays until something else takes the graph away.
    ANIM_GRAPH_LOOP = 0,
    // Plays once and is left by a row of its own -- ANIM_DONE() names the edge.
    ANIM_GRAPH_EXIT,
    /*
     * Plays once and RETURNS to what was playing before, at the clock it left,
     * through `animator_play_once`.
     *
     * The distinction is not decoration and cannot be folded into EXIT.
     * `animator_play_space` builds a fresh space, so a row re-entering a
     * locomotion state restarts its clock from zero -- which teleports the walk
     * phase and drags every foot lock with it. The animator's resume restores
     * the outgoing space INCLUDING its time, and that is the only path that
     * keeps the phase. What this adds over calling it directly is that the
     * return is NAMED in the table instead of implied by a stack of depth one.
     *
     * It needs NO row out and must not be given one: the graph follows the
     * animator back to the state it came from when the clip ends, because the
     * animator resumes inside its own update with the graph standing still, and
     * a graph that did not follow would name the one-shot while the walk was
     * already playing -- undetectable, because there is no second record to
     * disagree with.
     */
    ANIM_GRAPH_RETURN,
} AnimGraphStateKind;

typedef struct AnimGraphState {
    const char* name; // borrowed; what anim_graph_state_name reports
    /*
     * A registered source, or NULL for a state that plays NOTHING -- which
     * STOPS the animator rather than leaving the last source running.
     *
     * Leaving it running is not the same as not touching it: the clock keeps
     * advancing, the clip's own events keep firing at whatever weights were
     * frozen, and undrained root motion accumulates with nobody taking it. A
     * body physics has taken over is exactly that case.
     */
    const char* source;
    // The parameter driving the blend axis, written to `animator->param` every
    // tick. NULL leaves it alone, which is what a single-clip state wants.
    const char* param;
    // The parameter driving PLAYBACK RATE, written to `animator->speed` every
    // tick. NULL pins `speed` to 1 on ENTRY and then leaves it alone, so there
    // is one writer of that field per frame rather than two.
    const char* rate;
    AnimGraphStateKind kind;
} AnimGraphState;

/*
 * --------------------------------------------------------------- transitions
 */

typedef struct AnimGraph AnimGraph;

/*
 * The single escape hatch: an app predicate ANDed AFTER the conditions, and not
 * evaluated at all for a row whose conditions already failed -- so a guard with
 * a side effect is not a hidden per-frame call.
 *
 * `ui.h`'s shape, a closed vocabulary with a way out under it, and
 * `CameraRigProbeFn`'s: one `user` on the graph rather than one per row,
 * because the rows are const data in a binary and a pointer cannot be written
 * into one.
 */
typedef bool (*AnimGraphGuardFn)(const AnimGraph* graph, void* user);

typedef struct AnimGraphTransition {
    /*
     * NULL means ANY state -- and it is for INTERRUPTS ONLY.
     *
     * A row that beats whatever is playing (drowning, death, a hit) is an ANY
     * row. A row that is a FALLBACK must name its `from`, because an ANY row's
     * condition is a superset of the specific rows' preconditions and eats them
     * on their first frame: an any-state "not grounded, so fall" cuts a jump's
     * rise one frame after it starts, at any position in the table.
     */
    const char* from;
    const char* to; // borrowed; a state name
    /*
     * ANDed. Two rows are an OR, and the table stays readable.
     *
     * A row whose `to` is the CURRENT state is never taken, so an interrupt
     * that stays true does not re-issue its own play every tick -- "re-issuing
     * a play every step restarts the clip continuously and it never advances
     * past its first tick" is a failure this tree has already met. A row where
     * `from` equals `to` is refused at bind as the spelling error it is.
     */
    AnimGraphCondition conditions[ANIM_GRAPH_COND_MAX];
    // Seconds, handed to the animator. Zero is a CUT, which resets the spring
    // bones where a fade, being continuous, must not.
    float fade;
    /*
     * LAST in the struct deliberately: every table of this shape is a
     * positional initialiser of the form
     * {"ground", "lunge", {ANIM_FIRED("lunge")}, 0.08f}, and a field inserted
     * before `conditions` would silently rewire all of them.
     */
    AnimGraphGuardFn guard;
} AnimGraphTransition;

/*
 * --------------------------------------------------------------------- graph
 */

AnimGraph* create_anim_graph(void);
void free_anim_graph(AnimGraph* graph);

/*
 * What can play. Entries are COPIED under a BORROWED name: an entry array is
 * measured at load and the caller's may not outlive the graph, while a name is
 * a string literal in the same table the states are.
 *
 * A single clip is a one-entry source. Refused by name past the maximum, or
 * outside the entry count `animator_play_space` accepts.
 *
 * AN ENTRY WITH NO CLIP REFUSES THE SOURCE, and that is the feature rather than
 * a guard: an app finds its clips by name on whatever rig it was handed, so a
 * missing one arrives here as a NULL, and refusing is what turns "this rig has
 * no stroke" into "this state is unreachable" into "every row into it is
 * pruned". Decided once, where the alternative is a clip-presence test on every
 * row that mentions it and an asymmetry between two of them that nobody
 * notices. The caller need not check the return for that reason -- it is told
 * at bind, by name, along with everything else the rig cannot do.
 */
bool anim_graph_add_source(AnimGraph* graph, const char* name, const AnimatorEntry* entries,
                           int count);

// Three BORROWED const tables, `input_bind`'s shape: each outlives the binding,
// and each is validated whole at bind rather than here.
void anim_graph_set_params(AnimGraph* graph, const AnimGraphParam* params, int count);
void anim_graph_set_states(AnimGraph* graph, const AnimGraphState* states, int count);
void anim_graph_set_transitions(AnimGraph* graph, const AnimGraphTransition* rows, int count);

// What the guards are handed. Borrowed.
void anim_graph_set_guard_user(AnimGraph* graph, void* user);

/*
 * Resolve everything against what was registered, and enter `start`.
 *
 * A state whose source never registered is UNREACHABLE: every transition into
 * it is pruned and the whole set is reported ONCE, by name. That is what
 * replaces a clip-presence guard on every transition -- the shape that lets one
 * medium be guarded on its clip existing and the next not be, with nothing to
 * notice the asymmetry.
 *
 * Also reported: a state left with NO live exit. Forward reachability is half
 * the analysis and a trap state is the silent half.
 *
 * False, logged by name, for an unresolvable `start`, a row where `from` equals
 * `to`, a row naming a state or a parameter that does not exist, an op a
 * parameter's KIND cannot answer, or a FINISHED row out of a RETURN state --
 * that last because the animator already resumes on the very frame the edge
 * fires, and the two would be two writers in one frame doing different things.
 * A graph is refused WHOLE, `input_bind`'s rule.
 *
 * The animator is borrowed and must outlive the graph. Binding a second graph
 * to one animator stands the first down and logs it by name.
 *
 * A NULL animator is LEGAL and is not a degenerate case: the graph decides,
 * reports and refuses exactly as it would otherwise, and plays nothing. That is
 * what lets the table itself be asserted with no rig, no clip and no GL -- and
 * a state with no source is resolvable for the same reason, so a whole machine
 * can be exercised as the pure function over named values it is.
 */
bool anim_graph_bind(AnimGraph* graph, Animator* animator, const char* start);

/*
 * Parameters, by name on every write -- `input_action_value`'s shape, so there
 * is no id to keep, no cache and no ordering to get wrong. A name the table
 * does not declare, or one whose kind cannot take the write, is logged ONCE by
 * name and does nothing.
 */
void anim_graph_set_float(AnimGraph* graph, const char* name, float value);
void anim_graph_set_bool(AnimGraph* graph, const char* name, bool value);
void anim_graph_fire(AnimGraph* graph, const char* name);
// Reads are NOT kind-checked where writes are: a parameter is one float either
// way, and the kind is re-erected at bind, where an op that cannot answer it is
// refused. Ask for a bool by comparing against zero.
float anim_graph_float(const AnimGraph* graph, const char* name);

// What it is doing. The name is borrowed from the table; "" when unbound.
const char* anim_graph_state_name(const AnimGraph* graph);
float anim_graph_state_seconds(const AnimGraph* graph);

/*
 * Stand the graph down, or bring it back.
 *
 * A stood-down graph FREEZES: no row is evaluated, the time in state does not
 * advance, and no trigger is consumed -- so an exit-time transition does not
 * fire under a body somebody else is posing, and coming back resumes the state
 * it was in rather than the initial one. A ragdoll is that case.
 */
void anim_graph_set_enabled(AnimGraph* graph, bool enabled);

/*
 * Evaluate once, take AT MOST ONE transition, and write the state's param and
 * rate.
 *
 * One and not a chain: chaining reaches a state whose entry conditions were
 * never true, and costs one play per link in a single frame, every one after
 * the first freezing what was on screen.
 *
 * Exactly once per `animator_update`, IMMEDIATELY BEFORE it. The game framework
 * does that in `update_all_animators`; this stays public for an app with no
 * framework. The position is load-bearing three ways: it is the one cadence the
 * animator's clocks agree with, it reads the FINISHED edge on the frame that
 * produced it, and it sits after a fixed step has drained root motion and
 * before the next accumulation -- so a switch costs at most one rendered frame
 * of travel, where a switch from an app's own fixed step costs however many
 * steps that frame happened to run.
 *
 * A dt of 0 holds: nothing advances and nothing finishes, but rows ARE
 * evaluated and triggers ARE consumed, because a paused game that swallows a
 * button press is a button press that did nothing.
 */
void anim_graph_update(AnimGraph* graph, float dt);

// The tables as text: the states, the rows in evaluation order, and what bind
// pruned and why. `input_print_actions`' shape, and the first thing anybody
// debugging a graph wants.
void anim_graph_print(const AnimGraph* graph);

#endif // _ANIM_GRAPH_H_
