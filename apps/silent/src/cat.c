#include "cat.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cetra/look_at.h"
#include "cetra/probe_set.h"

#include "cat_body.h"

#define CAT_HOME CAT_AT_STUDY_CHAIR

// Turning. A walk's corners are turned into while walking; a turn sharper than TURN_ON_SPOT
// stops it and turns it on the spot, by quarter-turn clips while more than TURN_BY_CLIP is
// left and by easing round at EASE_ROUND after. A jump or a flight is only started squarely,
// within SQUARE, since its clip goes straight ahead.
#define TURN_ON_SPOT (100.0f * GLM_PIf / 180.0f)
#define TURN_BY_CLIP (70.0f * GLM_PIf / 180.0f)
#define SQUARE       (6.0f * GLM_PIf / 180.0f)
#define TURN_IN_AIR  (100.0f * GLM_PIf / 180.0f) // the most a jump turns between its two ends
#define EASE_ROUND   2.5f                        // radians a second
#define WALK_STEER   8.0f  // how fast a walk's facing follows its path, per second
#define GRAVITY      9.81f // what a jump's arc is flown under, for its time of flight

// The player in the way: nearer than this ahead of it, the cat stops and waits.
#define GIVE_WAY 0.55f

// The eyes' glow rises and falls over this.
#define SHINE_EASE 0.08f // seconds

const char* cat_place_list(void) {
    static char list[512];
    if (!list[0]) {
        size_t n = 0;
        for (int i = 0; i < CAT_PLACE_COUNT; i++)
            if (CAT_PLACES[i].rest != CAT_REST_NONE)
                n += (size_t)snprintf(list + n, sizeof(list) - n, "%s%s", n ? ", " : "",
                                      CAT_PLACES[i].name);
    }
    return list;
}

static int find_clip(const char* name) {
    for (int i = 0; i < CAT_CLIP_COUNT; i++)
        if (!strcmp(CAT_CLIPS[i].name, name))
            return i;
    return -1;
}

// ---------------------------------------------------------------------------------------------
// What it plays

// Each posture's held clip, and the transition that goes up a posture from it or down one.
static const int HOLD[] = {[CAT_CURL] = CAT_CLIP_SLEEP,
                           [CAT_LIE] = CAT_CLIP_LIE,
                           [CAT_SIT] = CAT_CLIP_SIT,
                           [CAT_STAND] = CAT_CLIP_IDLE};
static const int UP[] = {
    [CAT_CURL] = CAT_CLIP_UNCURL, [CAT_LIE] = CAT_CLIP_SIT_UP, [CAT_SIT] = CAT_CLIP_STAND_UP};
static const int DOWN[] = {
    [CAT_LIE] = CAT_CLIP_CURL_UP, [CAT_SIT] = CAT_CLIP_LIE_DOWN, [CAT_STAND] = CAT_CLIP_SIT_DOWN};

static CatPosture resting(CatRest rest) {
    switch (rest) {
        case CAT_REST_CURL:
            return CAT_CURL;
        case CAT_REST_LIE:
            return CAT_LIE;
        case CAT_REST_SIT:
            return CAT_SIT;
        default:
            return CAT_STAND;
    }
}

// Play a clip from its start, or leave a looping one that is already playing as it is.
static void play(Cat* cat, int clip, float fade) {
    if (cat->clip == clip && CAT_CLIPS[clip].looping)
        return;
    if (cat->clips[clip])
        animator_play(cat->animator, cat->clips[clip], fade, CAT_CLIPS[clip].looping != 0);
    cat->animator->speed = 1.0f;
    cat->clip = clip;
    cat->clip_seconds = 0.0f;
}

static bool played(const Cat* cat) {
    return cat->clip_seconds >= CAT_CLIPS[cat->clip].seconds;
}

static float event_at(int clip, const char* name) {
    const CatClipSpec* s = &CAT_CLIPS[clip];
    for (int i = 0; i < s->event_count; i++)
        if (!strcmp(s->events[i].name, name))
            return s->events[i].seconds;
    return 0.0f;
}

static bool turning_by_clip(const Cat* cat) {
    return cat->clip == CAT_CLIP_TURN_L90 || cat->clip == CAT_CLIP_TURN_R90;
}

bool cat_eyes_shut(const Cat* cat) {
    return cat->entity && (cat->clip == CAT_CLIP_SLEEP ||
                           (cat->clip == CAT_CLIP_CURL_UP &&
                            cat->clip_seconds > event_at(CAT_CLIP_CURL_UP, "lids_shut")) ||
                           (cat->clip == CAT_CLIP_UNCURL &&
                            cat->clip_seconds < event_at(CAT_CLIP_UNCURL, "lids_open")));
}

// ---------------------------------------------------------------------------------------------
// Postures and acts

static void next_leg(Cat* cat);
static void begin_act(Cat* cat, int clip);

// Up or down its postures, a transition at a time, to `want`, then holding it -- or, standing
// with an act waiting for it, playing that. An act is waited for only by a cat standing up.
static void shift_to(Cat* cat, CatPosture want) {
    cat->want = want;
    if (want != CAT_STAND)
        cat->act = -1;
    if (cat->posture == want) {
        if (cat->act >= 0) {
            begin_act(cat, cat->act);
            return;
        }
        play(cat, HOLD[want], 0.25f);
        cat->mode = CAT_RESTING;
        if (want == CAT_STAND && cat->goal >= 0)
            next_leg(cat);
        return;
    }
    cat->mode = CAT_SHIFTING;
    play(cat, want > cat->posture ? UP[cat->posture] : DOWN[cat->posture], 0.15f);
}

// The transition under way is finished, whichever way `want` has turned since it began.
static void step_shift(Cat* cat) {
    if (!played(cat))
        return;
    const bool up = cat->posture < CAT_STAND && cat->clip == UP[cat->posture];
    cat->posture = (CatPosture)((int)cat->posture + (up ? 1 : -1));
    shift_to(cat, cat->want);
}

static bool looping_act(int clip) {
    return clip >= 0 && CAT_CLIPS[clip].looping;
}

// The posture an act leaves the cat in.
static CatPosture act_ends(int clip) {
    return clip == CAT_CLIP_GROOM ? CAT_SIT : CAT_STAND;
}

static void begin_act(Cat* cat, int clip) {
    cat->act = clip;
    cat->want = act_ends(clip);
    cat->mode = CAT_ACTING;
    play(cat, clip, clip == CAT_CLIP_STARTLE ? 0.08f : 0.25f);
}

// Out of an act into the posture it leaves the cat in, and on to what it has been asked for
// since.
static void end_act(Cat* cat) {
    cat->posture = act_ends(cat->act);
    cat->act = -1;
    shift_to(cat, cat->want);
}

static void step_act(Cat* cat) {
    if (!looping_act(cat->act) && played(cat))
        end_act(cat);
}

// Make for `want` from whatever it is doing in place: out of a looping act at once, after a
// transition or a one-shot act has finished; on the move, at the end of the way.
static void head_for(Cat* cat, CatPosture want) {
    if (cat->mode == CAT_ACTING && looping_act(cat->act)) {
        cat->posture = act_ends(cat->act);
        cat->act = -1;
        cat->mode = CAT_RESTING;
    }
    cat->want = want;
    if (cat->mode == CAT_RESTING)
        shift_to(cat, want);
}

bool cat_act(Cat* cat, int clip) {
    if (!cat->entity || !cat->attached || cat->held)
        return false;
    const bool travelling =
        cat->mode == CAT_WALKING || cat->mode == CAT_TURNING || cat->mode == CAT_JUMPING;
    switch (clip) {
        case CAT_CLIP_STARTLE: {
            // Anywhere it has its feet under it on the level: not in the air, on the stair, or
            // part way through a quarter turn.
            NavSample s;
            nav_follower_sample(&cat->follower, &s);
            if (cat->mode == CAT_JUMPING || (cat->mode == CAT_TURNING && turning_by_clip(cat)) ||
                (cat->mode == CAT_WALKING && s.kind != CAT_LINK_WALK))
                return false;
            cat->goal = -1;
            cat->posture = CAT_STAND;
            begin_act(cat, clip);
            return true;
        }
        case CAT_CLIP_GROOM:
            if (cat->posture != CAT_SIT ||
                !(cat->mode == CAT_RESTING || (cat->mode == CAT_ACTING && cat->act == clip)))
                return false;
            begin_act(cat, clip);
            return true;
        case CAT_CLIP_HISS:
            if (travelling || cat->posture != CAT_STAND)
                return false;
            begin_act(cat, clip);
            return true;
        case CAT_CLIP_STRETCH:
            if (travelling || cat->act >= 0)
                return false;
            cat->act = clip;
            head_for(cat, CAT_STAND);
            return true;
        default:
            return false;
    }
}

// The bones a vocal moves, so the body under it goes on with what it was doing.
static void vocal_mask(const Cat* cat, int clip, float* mask) {
    static const char* MOUTH[] = {"Jaw", "Ear.L", "Ear.R", NULL};
    static const char* YAWN[] = {"Jaw", "Ear.L", "Ear.R", "Lid.L", "Lid.R", NULL};
    static const char* LIDS[] = {"Lid.L", "Lid.R", NULL};
    static const char* EAR_L[] = {"Ear.L", NULL};
    static const char* EAR_R[] = {"Ear.R", NULL};
    const char** bones = clip == CAT_CLIP_YAWN                                   ? YAWN
                         : clip == CAT_CLIP_BLINK || clip == CAT_CLIP_SLOW_BLINK ? LIDS
                         : clip == CAT_CLIP_EAR_FLICK_L                          ? EAR_L
                         : clip == CAT_CLIP_EAR_FLICK_R                          ? EAR_R
                                                                                 : MOUTH;
    Skeleton* skeleton = cat->animator->state->skeleton;
    memset(mask, 0, sizeof(float) * MAX_BONES);
    for (int i = 0; bones[i]; i++) {
        const int b = get_bone_index_by_name(skeleton, bones[i]);
        if (b >= 0)
            mask[b] = 1.0f;
    }
}

bool cat_vocal(Cat* cat, int clip) {
    if (!cat->entity || !cat->attached || clip < 0 || clip >= CAT_CLIP_COUNT || !cat->clips[clip])
        return false;
    const AnimatorLayerPhase phase = cat->animator->layer.phase;
    if (phase == ANIMATOR_LAYER_IN || phase == ANIMATOR_LAYER_ON)
        return false;
    float mask[MAX_BONES];
    vocal_mask(cat, clip, mask);
    animator_play_layer(cat->animator, cat->clips[clip], mask, 0.06f, 0.1f, false);
    return true;
}

void cat_look(Cat* cat, const vec3 world) {
    cat->look_on = world != NULL;
    if (world)
        glm_vec3_copy((float*)world, cat->look_target);
}

int cat_place(const Cat* cat) {
    return cat->follower.arrived ? cat->follower.at : -1;
}

bool cat_settle(Cat* cat, CatPosture posture) {
    if (!cat->entity || cat->held || cat->goal >= 0)
        return false;
    // Turning to the way it rests, it takes the posture once round.
    cat->arrive_as = posture;
    head_for(cat, posture);
    return true;
}

bool cat_settled(const Cat* cat) {
    return cat->entity && cat->goal < 0 && cat->mode == CAT_RESTING && cat->act < 0;
}

bool cat_here(const Cat* cat) {
    return cat->entity && cat->goal < 0 &&
           (cat->mode == CAT_RESTING || (cat->mode == CAT_ACTING && cat->act >= 0));
}

// ---------------------------------------------------------------------------------------------
// Going from place to place

// A turn on the spot toward `face`: a quarter-turn clip while much is left, easing round once
// little is.
static void turn_to(Cat* cat, float face) {
    cat->face = face;
    cat->mode = CAT_TURNING;
    const float left = wrap_pi(face - cat->yaw);
    play(cat,
         fabsf(left) > TURN_BY_CLIP ? (left > 0.0f ? CAT_CLIP_TURN_L90 : CAT_CLIP_TURN_R90)
                                    : CAT_CLIP_IDLE,
         0.15f);
}

// Arrived where it was going: round to face the way it rests there, and down into its posture.
static void arrive(Cat* cat) {
    cat->goal = -1;
    const CatPlace* p = &CAT_PLACES[cat->follower.at];
    if (p->rest != CAT_REST_NONE && fabsf(wrap_pi(p->yaw - cat->yaw)) > 0.05f) {
        turn_to(cat, p->yaw);
        return;
    }
    shift_to(cat, cat->arrive_as);
}

// Off up a jump's arc: the clip played at the rate that makes the time between its takeoff
// and its landing the arc's own time of flight. It turns in the air to land facing the way it
// goes on, within a quarter turn and a bit of the way it jumped -- onto the hand rail that is
// the only place to turn at all.
static void jump(Cat* cat, const NavSample* s) {
    const NavLink* l = &cat->places->links[s->link];
    const bool up = cat->places->nodes[l->to].position[1] > cat->places->nodes[l->from].position[1];
    const int clip = up ? CAT_CLIP_JUMP_UP : CAT_CLIP_JUMP_DOWN;
    cat->takeoff = event_at(clip, "takeoff");
    cat->land = event_at(clip, "land");
    const float flight = nav_link_flight_time(cat->places, s->link, GRAVITY);
    play(cat, clip, 0.1f);
    cat->animator->speed = flight > 0.0f ? (cat->land - cat->takeoff) / flight : 1.0f;
    cat->mode = CAT_JUMPING;
    cat->landed = false;
    cat->leg = s->link;
    const float heading = nav_link_heading(cat->places, s->link);
    cat->jump_yaw0 = cat->yaw;
    cat->jump_yaw1 = heading + (fabsf(s->turn) <= TURN_IN_AIR ? s->turn : 0.0f);
}

// The next link of the route, from standing at its start: squared up to it first if it needs
// it, then walked, climbed or jumped.
static void next_leg(Cat* cat) {
    NavSample s;
    nav_follower_sample(&cat->follower, &s);
    if (s.arrived) {
        arrive(cat);
        return;
    }
    const NavLink* l = &cat->places->links[s.link];
    const float heading = nav_link_heading(cat->places, s.link);
    const float off = fabsf(wrap_pi(heading - cat->yaw));
    // Nothing turns on the spot on a beam: along it the way is straight ahead, and off it the
    // turn is made in the air.
    const bool beam = (cat->places->nodes[l->from].tags & CAT_TAG_BEAM) != 0;
    const float square = beam ? GLM_PIf : l->kind == CAT_LINK_WALK ? TURN_ON_SPOT : SQUARE;
    if (off > square) {
        turn_to(cat, heading);
        return;
    }
    cat->leg = s.link;
    if (l->kind == CAT_LINK_JUMP) {
        jump(cat, &s);
        return;
    }
    cat->mode = CAT_WALKING;
    const bool up = cat->places->nodes[l->to].position[1] > cat->places->nodes[l->from].position[1];
    static const int GAIT[] = {
        [CAT_WALK] = CAT_CLIP_WALK, [CAT_TROT] = CAT_CLIP_TROT, [CAT_RUN] = CAT_CLIP_RUN};
    play(cat,
         l->kind == CAT_LINK_STAIR  ? (up ? CAT_CLIP_STAIR_UP : CAT_CLIP_STAIR_DOWN)
         : l->kind == CAT_LINK_RAIL ? CAT_CLIP_BEAM_WALK
                                    : GAIT[cat->gait],
         0.2f);
}

static void step_turn(Cat* cat, float turned, float dt) {
    cat->yaw = wrap_pi(cat->yaw + turned);
    if (turning_by_clip(cat)) {
        if (played(cat))
            turn_to(cat, cat->face);
        return;
    }
    const float left = wrap_pi(cat->face - cat->yaw);
    const float most = EASE_ROUND * dt;
    if (fabsf(left) > most) {
        cat->yaw = wrap_pi(cat->yaw + (left > 0.0f ? most : -most));
        return;
    }
    cat->yaw = cat->face;
    if (cat->follower.arrived)
        shift_to(cat, cat->arrive_as);
    else
        next_leg(cat);
}

// Whether the player is standing in the way, close ahead along its path.
static bool in_the_way(const vec3 player, const NavSample* s) {
    const float dx = player[0] - s->position[0], dz = player[2] - s->position[2];
    const float d = sqrtf(dx * dx + dz * dz);
    if (d > GIVE_WAY || fabsf(player[1] - s->position[1]) > 1.0f)
        return false;
    return d < 1e-3f || (dx * s->tangent[0] + dz * s->tangent[2]) / d > 0.3f;
}

static void step_walk(Cat* cat, float moved, const vec3 player, float dt) {
    NavSample s;
    nav_follower_sample(&cat->follower, &s);
    if (s.kind == CAT_LINK_WALK && in_the_way(player, &s)) {
        cat->waiting = true;
        play(cat, CAT_CLIP_IDLE, 0.2f);
        return;
    }
    if (cat->waiting) {
        cat->waiting = false;
        cat->leg = -1; // back to the clip this link wants
    }
    nav_follower_advance(&cat->follower, moved);
    nav_follower_sample(&cat->follower, &s);
    if (s.arrived) {
        arrive(cat);
        return;
    }
    if (s.link != cat->leg) {
        next_leg(cat);
        return;
    }
    // Along the rail it faces exactly the way the rail runs.
    if (s.kind == CAT_LINK_RAIL) {
        cat->yaw = s.heading;
        return;
    }
    cat->yaw = wrap_pi(cat->yaw + wrap_pi(s.heading - cat->yaw) * (1.0f - expf(-dt * WALK_STEER)));
}

static void step_jump(Cat* cat) {
    if (!cat->landed) {
        const float u = (cat->clip_seconds - cat->takeoff) / (cat->land - cat->takeoff);
        nav_follower_set_progress(&cat->follower, u);
        cat->landed = u >= 1.0f;
        const float turned = glm_smoothstep(0.0f, 1.0f, glm_clamp(u, 0.0f, 1.0f));
        cat->yaw = wrap_pi(cat->jump_yaw0 + turned * wrap_pi(cat->jump_yaw1 - cat->jump_yaw0));
    }
    if (played(cat))
        next_leg(cat);
}

static bool go_with(Cat* cat, CatPlaceId place, CatGait gait, int settle, NavQuery query) {
    if (!cat->entity || cat->held || place < 0 || place >= CAT_PLACE_COUNT)
        return false;
    if (!nav_follower_replan(&cat->follower, place, &query)) {
        fprintf(stderr, "silent: the cat has no way from %s to %s\n",
                CAT_PLACES[cat->follower.at].name, CAT_PLACES[place].name);
        return false;
    }
    cat->arrive_as = settle >= 0 ? (CatPosture)settle : resting(CAT_PLACES[place].rest);
    cat->gait = gait;
    if (cat->follower.arrived) {
        // Already there: only the posture changes.
        cat->goal = -1;
        head_for(cat, cat->arrive_as);
        return true;
    }
    cat->goal = place;
    cat->leg = -1; // whatever plays, the next link's clip is the gait's
    head_for(cat, CAT_STAND);
    return true;
}

bool cat_go(Cat* cat, CatPlaceId place, CatGait gait, int settle) {
    return go_with(cat, place, gait, settle, cat_places_query(false));
}

bool cat_go_by_rail(Cat* cat, CatPlaceId place, CatGait gait, int settle) {
    return go_with(cat, place, gait, settle, cat_places_query(true));
}

bool cat_route(const Cat* cat, CatPlaceId place, bool by_rail, NavRoute* out) {
    const NavQuery q = cat_places_query(by_rail);
    return cat->entity && nav_follower_plan(&cat->follower, place, &q, out);
}

void cat_step(Cat* cat, const vec3 player, float dt) {
    if (!cat->entity || !cat->attached || cat->held)
        return;
    cat->clip_seconds += dt * cat->animator->speed;
    vec3 travel = {0.0f, 0.0f, 0.0f};
    float turned = 0.0f;
    animator_take_root_motion(cat->animator, travel, &turned);
    const float moved = sqrtf(travel[0] * travel[0] + travel[2] * travel[2]);
    switch (cat->mode) {
        case CAT_RESTING:
            break;
        case CAT_SHIFTING:
            step_shift(cat);
            break;
        case CAT_TURNING:
            step_turn(cat, turned, dt);
            break;
        case CAT_WALKING:
            step_walk(cat, moved, player, dt);
            break;
        case CAT_JUMPING:
            step_jump(cat);
            break;
        case CAT_ACTING:
            step_act(cat);
            break;
    }
    NavSample s;
    nav_follower_sample(&cat->follower, &s);
    glm_vec3_copy(s.position, cat->entity->position);
    cat->entity->position[1] += CAT_HALF_HEIGHT;
    glm_quatv(cat->entity->rotation, cat->yaw, (vec3){0.0f, 1.0f, 0.0f});
}

void cat_trace(const Cat* cat, int step) {
    if (!cat->entity)
        return;
    static const char* MODES[] = {"resting", "shifting", "turning", "walking", "jumping", "acting"};
    const float* p = cat->entity->position;
    char link[80] = "-";
    NavSample s;
    nav_follower_sample(&cat->follower, &s);
    if (s.link >= 0) {
        const NavLink* l = &cat->places->links[s.link];
        snprintf(link, sizeof(link), "%s>%s %.2f", cat->places->nodes[l->from].name,
                 cat->places->nodes[l->to].name, (double)s.progress);
    }
    printf("cat step %5d pos %7.3f %6.3f %7.3f yaw %6.1f %-8s %-10s %s%s%s\n", step, (double)p[0],
           (double)(p[1] - CAT_HALF_HEIGHT), (double)p[2], (double)glm_deg(cat->yaw),
           MODES[cat->mode], CAT_CLIPS[cat->clip].name, link, cat->goal >= 0 ? " to " : "",
           cat->goal >= 0 ? CAT_PLACES[cat->goal].name : "");
}

// ---------------------------------------------------------------------------------------------

bool cat_create(Cat* cat, const CatDesc* desc, Game* game, PhysicsWorld* physics) {
    memset(cat, 0, sizeof(*cat));
    cat->eye_bones[0] = cat->eye_bones[1] = -1;
    cat->goal = -1;
    cat->leg = -1;
    cat->act = -1;
    cat->go = -1;

    cat->places = cat_places_build();
    if (!cat->places)
        return false;
    cat_places_check(cat->places, physics);
    int at = desc->at ? cat_place_find(desc->at) : CAT_HOME;
    if (at < 0) {
        fprintf(stderr, "silent: no place called '%s' for the cat (%s); home instead\n", desc->at,
                cat_place_list());
        at = CAT_HOME;
    }
    const CatPlace* place = &CAT_PLACES[at];
    nav_follower_start(&cat->follower, cat->places, at, at, NULL);
    cat->posture = cat->want = cat->arrive_as = resting(place->rest);
    cat->yaw = cat->face = place->yaw;
    int clip = HOLD[cat->posture];
    if (desc->clip) {
        const int named = find_clip(desc->clip);
        if (named < 0) {
            fprintf(stderr, "silent: the cat has no clip '%s'; resting\n", desc->clip);
        } else {
            clip = named;
            cat->held = true;
        }
    }
    if (desc->go) {
        cat->go = cat_place_find(desc->go);
        if (cat->go < 0)
            fprintf(stderr, "silent: the cat knows no place called '%s'\n", desc->go);
    }
    cat->go_gait = desc->gait;

    vec3 feet = {0.0f, 0.0f, 0.0f};
    cat_place_feet(at, feet);
    if (!cat_body_load(cat, desc, game, physics, feet, place->yaw))
        return false;
    cat->clip = -1;
    play(cat, clip, 0.0f);
    if (cat->held && desc->clip_seconds >= 0.0f) {
        animator_update(cat->animator, desc->clip_seconds);
        cat->animator->speed = 0.0f;
    }
    printf("silent: the cat at %s, %s\n", place->name, CAT_CLIPS[clip].name);
    return true;
}

void cat_free(Cat* cat) {
    free_nav_graph(cat->places);
    cat->places = NULL;
}

// Whether the head is its own to turn: not asleep, in the air, through a quarter turn, or
// busy in a stretch, a startle or grooming.
static bool head_free(const Cat* cat) {
    if (cat_eyes_shut(cat) || cat->mode == CAT_JUMPING ||
        (cat->mode == CAT_TURNING && turning_by_clip(cat)))
        return false;
    if (cat->mode == CAT_ACTING && cat->act != CAT_CLIP_HISS)
        return false;
    return cat->posture != CAT_CURL;
}

void cat_update(Cat* cat, Game* game, Scene* scene, const Lights* lights, const vec3 viewer,
                float dt) {
    if (!cat->entity)
        return;
    // Into the scene at once, and out of every capture: a reflection probe captured while the
    // cat is in its room would otherwise keep it there for good, asleep on a chair it has long
    // since left. Skinned meshes are left out already; this takes whatever rides on the body.
    if (!cat->attached) {
        cat->entity->node->capture_hidden = true;
        node_add_child(scene->root_node, cat->entity->node);
        cat->attached = true;
        // Sent somewhere from the command line: it sets off once it can be seen to, by the rail
        // if that is the way.
        if (cat->go >= 0)
            cat_go_by_rail(cat, (CatPlaceId)cat->go, cat->go_gait, -1);
    }
    const float target = cat_body_shine(cat, game, lights, viewer);
    cat->shine += (target - cat->shine) * (1.0f - expf(-dt / SHINE_EASE));
    if (cat->eye)
        cat->eye->emissive_strength = cat->shine;

    LookAtSystem* look = cat->animator->state->look_at;
    if (look) {
        mat4 world = GLM_MAT4_IDENTITY_INIT;
        cat_body_world(cat, world);
        look_at_set_world(look, world);
        look->has_target = cat->look_on && head_free(cat);
        glm_vec3_copy(cat->look_target, look->target);
    }
}
