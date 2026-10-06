#include "cat_brain.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cetra/game/physics.h"

#include "cat_clips.h"
#include "cat_places.h"
#include "kit.h"
#include "layout.h"
#include "mansion.h"

// The activities, in the order they are registered, which is the brain's index for each.
typedef enum {
    ACT_SLEEP,
    ACT_STRETCH,
    ACT_LOAF,
    ACT_EXPLORE,
    ACT_WATCH_FLAME,
    ACT_WINDOW,
    ACT_AVOID,
    ACT_STARTLE,
    ACT_FOLLOW,
    ACT_RAIL,
    ACT_COUNT
} CatActivity;

static const char* const ACTIVITY_NAMES[ACT_COUNT] = {"sleep",       "stretch", "loaf",  "explore",
                                                      "watch_flame", "window",  "avoid", "startle",
                                                      "follow",      "rail"};

// The aloof cat's temperament: one set of numbers, not presets.
#define WAKE_RATE        (1.0f / 80.0f)  // energy gained a second asleep
#define TIRE_RATE        (1.0f / 400.0f) // and lost a second awake
#define BORED_RATE       (1.0f / 60.0f)  // boredom gained a second sat still awake
#define NOVEL_SECONDS    180.0f          // how long until a place is as good as new again
#define SIGHT_EVERY      0.2f            // seconds between looks for the player
#define SIGHT_RANGE      8.0f            // metres it sees the player within...
#define SIGHT_HALF_ANGLE 120.0f          // ...this many degrees either side of its body's facing
#define HEARS_SPRINT     2.0f            // m/s: a player this fast is heard...
#define HEARS_WITHIN     6.0f            // ...this close, through anything
#define STARTLE_SPEED    2.2f            // a player this fast...
#define STARTLE_WITHIN   2.5f            // ...this close, and closing, startles it
#define PERSONAL_SPACE   1.3f            // nearer than this and coming closer, it moves off
#define TRUSTS           0.7f            // ...unless it trusts the player this much
#define FONDNESS         0.55f // the affinity it purrs at, and sometimes follows the player at
#define SOCIABILITY      0.3f  // the chance, each half minute, that it would follow
#define WATCH_WITHIN     7.0f  // a seen player nearer than this can take the gaze
#define TRILL_AFTER      45.0f // seconds out of sight before seeing the player trills
#define SLOW_BLINK_AFTER 3.0f  // seconds of being looked at, still, before a slow blink

static float rnd(CatMind* m, float lo, float hi) {
    return lo + (hi - lo) * brain_random(m->brain);
}

static float novelty(const CatMind* m, CatPlaceId place) {
    return glm_clamp_zo(m->since[place] / NOVEL_SECONDS);
}

static bool go(CatMind* m, int place, CatGait gait, int settle, bool by_rail, float give_up) {
    m->spot = place;
    m->held = 0.0f;
    m->groomed = false;
    m->give_up = give_up;
    if (place < 0)
        return false;
    return by_rail ? cat_go_by_rail(m->cat, (CatPlaceId)place, gait, settle)
                   : cat_go(m->cat, (CatPlaceId)place, gait, settle);
}

// There, and doing what it does there: holding, sometimes washing if it sits. Done once it has
// held long enough; failed if it never gets there.
static BrainStatus stay(CatMind* m, float dt, float groom) {
    Cat* cat = m->cat;
    if (!cat_here(cat)) {
        m->give_up -= dt;
        return m->give_up <= 0.0f ? BRAIN_FAILED : BRAIN_RUNNING;
    }
    m->held += dt;
    if (groom > 0.0f && !m->groomed && cat->posture == CAT_SIT && m->held > 2.0f &&
        m->held < m->hold - 8.0f) {
        m->groomed = true;
        if (brain_random(m->brain) < groom)
            cat_act(cat, CAT_CLIP_GROOM);
    }
    // A wash lasts a few seconds, then it sits again.
    if (cat->act == CAT_CLIP_GROOM && m->held > 9.0f)
        cat_settle(cat, CAT_SIT);
    return m->held >= m->hold ? BRAIN_DONE : BRAIN_RUNNING;
}

// ---------------------------------------------------------------------------------------------
// Outings: off to somewhere, and a stay there. Loafing, exploring, the candelabra, the window
// and the rail are each one row, run by one begin and one step.

typedef struct Outing {
    const CatPlaceId* places; // where it may go, the best picked; NULL for anywhere it rests
    int place_count;
    float stay_min, stay_max; // seconds there
    float groom;              // the chance of a wash while it sits there
    float trot;               // the chance it trots there rather than walks
    float stretch;            // the chance it stretches when done
    float give_up;            // seconds it tries to get there
    int settle;               // the posture it takes there, -1 for the place's own
    bool by_rail;
    bool cures_boredom; // getting there is new enough to leave it unbored
} Outing;

static const CatPlaceId LOAF_PLACES[] = {
    CAT_AT_RUG,    CAT_AT_GALLERY,   CAT_AT_STUDY_BAY_W,  CAT_AT_DINING_S,
    CAT_AT_HEARTH, CAT_AT_STUDY_MID, CAT_AT_STUDY_WINDOW, CAT_AT_SEAT_CUSHION};
static const CatPlaceId FLAME[] = {CAT_AT_HEAD_CHAIR};
static const CatPlaceId WINDOW[] = {CAT_AT_WINDOW_SEAT};
static const CatPlaceId NEWEL[] = {CAT_AT_NEWEL_CAP};

static const Outing OUTINGS[ACT_COUNT] = {
    [ACT_LOAF] = {.places = LOAF_PLACES,
                  .place_count = KIT_COUNT(LOAF_PLACES),
                  .stay_min = 20.0f,
                  .stay_max = 40.0f,
                  .groom = 0.35f,
                  .stretch = 0.4f,
                  .give_up = 90.0f,
                  .settle = -1},
    [ACT_EXPLORE] = {.stay_min = 6.0f,
                     .stay_max = 12.0f,
                     .groom = 0.5f,
                     .trot = 0.2f,
                     .give_up = 90.0f,
                     .settle = -1,
                     .cures_boredom = true},
    [ACT_WATCH_FLAME] = {.places = FLAME,
                         .place_count = 1,
                         .stay_min = 15.0f,
                         .stay_max = 30.0f,
                         .give_up = 90.0f,
                         .settle = CAT_SIT},
    [ACT_WINDOW] = {.places = WINDOW,
                    .place_count = 1,
                    .stay_min = 20.0f,
                    .stay_max = 45.0f,
                    .give_up = 90.0f,
                    .settle = CAT_SIT},
    [ACT_RAIL] = {.places = NEWEL,
                  .place_count = 1,
                  .stay_min = 10.0f,
                  .stay_max = 25.0f,
                  .give_up = 120.0f,
                  .settle = CAT_SIT,
                  .by_rail = true},
};

// Where an outing goes: its one place, or the best of its places to go to now -- new to it,
// near, and a little chance -- never where it already is. -1 for none.
static int pick(CatMind* m, const Outing* o) {
    if (o->place_count == 1)
        return o->places[0];
    const int count = o->places ? o->place_count : CAT_PLACE_COUNT;
    int best = -1;
    float best_score = 0.0f;
    for (int i = 0; i < count; i++) {
        const CatPlaceId p = o->places ? o->places[i] : (CatPlaceId)i;
        NavRoute r;
        if ((!o->places && CAT_PLACES[p].rest == CAT_REST_NONE) ||
            !cat_route(m->cat, p, o->by_rail, &r) || r.count == 0)
            continue;
        const float s = (0.15f + novelty(m, p)) / (1.0f + r.cost / 8.0f) * rnd(m, 0.6f, 1.4f);
        if (s > best_score) {
            best_score = s;
            best = p;
        }
    }
    return best;
}

static bool outing_begin(Brain* b, void* user) {
    CatMind* m = user;
    const Outing* o = &OUTINGS[b->current];
    const int p = pick(m, o);
    m->hold = rnd(m, o->stay_min, o->stay_max);
    const CatGait gait = o->trot > 0.0f && brain_random(b) < o->trot ? CAT_TROT : CAT_WALK;
    return go(m, p, gait, o->settle, o->by_rail, o->give_up);
}

static BrainStatus outing_step(Brain* b, void* user, float dt) {
    CatMind* m = user;
    const Outing* o = &OUTINGS[b->current];
    if (o->cures_boredom && m->held == 0.0f && cat_here(m->cat))
        m->boredom = 0.0f;
    const BrainStatus status = stay(m, dt, o->groom);
    if (status == BRAIN_DONE && o->stretch > 0.0f)
        m->want_stretch = brain_random(b) < o->stretch;
    return status;
}

static float loaf_score(Brain* b, void* user) {
    (void)b;
    const CatMind* m = user;
    return 0.22f + 0.3f * (1.0f - m->energy);
}

static float explore_score(Brain* b, void* user) {
    (void)b;
    const CatMind* m = user;
    return 0.18f + 0.5f * m->boredom;
}

static float watch_flame_score(Brain* b, void* user) {
    (void)b;
    const CatMind* m = user;
    return 0.4f * novelty(m, CAT_AT_HEAD_CHAIR);
}

static float window_score(Brain* b, void* user) {
    (void)b;
    const CatMind* m = user;
    return (0.3f + 0.12f * m->rain) * novelty(m, CAT_AT_WINDOW_SEAT);
}

// The rail, onto the newel at its end over the great hall: only when it has the energy for it.
static float rail_score(Brain* b, void* user) {
    (void)b;
    const CatMind* m = user;
    return 0.34f * novelty(m, CAT_AT_NEWEL_CAP) * (m->energy > 0.4f ? 1.0f : 0.2f);
}

// ---------------------------------------------------------------------------------------------
// Sleep: curled, somewhere it likes, until rested.

static const CatPlaceId SLEEP_PLACES[] = {CAT_AT_STUDY_CHAIR, CAT_AT_SEAT_CUSHION, CAT_AT_DINING_S,
                                          CAT_AT_RUG, CAT_AT_STUDY_BAY_W};
static const float SLEEP_WEIGHTS[] = {3.0f, 1.0f, 1.2f, 0.8f, 0.8f};

static float sleep_score(Brain* b, void* user) {
    const CatMind* m = user;
    // Once asleep it holds out against everything but a fright.
    if (b->current == ACT_SLEEP && m->asleep)
        return 0.95f;
    return 0.9f * powf(1.0f - m->energy, 1.5f);
}

static bool sleep_begin(Brain* b, void* user) {
    (void)b;
    CatMind* m = user;
    m->hold = 240.0f;
    const int here = cat_here(m->cat) ? cat_place(m->cat) : -1;
    // Already curled up somewhere: it sleeps there.
    if (here >= 0 && m->cat->posture == CAT_CURL)
        return go(m, here, CAT_WALK, CAT_CURL, false, 90.0f);
    // Where it is, if it sleeps there; otherwise novelty has no say: home mostly, and the
    // nearer the better.
    int p = -1;
    float best = 0.0f;
    for (int i = 0; i < KIT_COUNT(SLEEP_PLACES); i++) {
        NavRoute r;
        if (!cat_route(m->cat, SLEEP_PLACES[i], false, &r))
            continue;
        if (r.count == 0) {
            p = SLEEP_PLACES[i];
            break;
        }
        const float s = SLEEP_WEIGHTS[i] / (1.0f + r.cost / 6.0f);
        if (s > best) {
            best = s;
            p = SLEEP_PLACES[i];
        }
    }
    return go(m, p, CAT_WALK, CAT_CURL, false, 90.0f);
}

static BrainStatus sleep_step(Brain* b, void* user, float dt) {
    (void)b;
    CatMind* m = user;
    m->asleep = cat_settled(m->cat) && m->cat->posture == CAT_CURL;
    if (!m->asleep) {
        m->give_up -= dt;
        return m->give_up <= 0.0f ? BRAIN_FAILED : BRAIN_RUNNING;
    }
    m->held += dt;
    if (m->energy >= 0.95f || m->held >= m->hold) {
        m->want_stretch = true;
        return BRAIN_DONE;
    }
    return BRAIN_RUNNING;
}

static void sleep_end(Brain* b, void* user, bool interrupted) {
    (void)b;
    (void)interrupted;
    CatMind* m = user;
    m->asleep = false;
}

// ---------------------------------------------------------------------------------------------
// Stretch: up, a bow and a reach, after a sleep or a long loaf.

static float stretch_score(Brain* b, void* user) {
    (void)b;
    const CatMind* m = user;
    return m->want_stretch ? 0.97f : 0.0f;
}

static bool stretch_begin(Brain* b, void* user) {
    CatMind* m = user;
    m->want_stretch = false;
    m->give_up = 10.0f;
    if (!cat_act(m->cat, CAT_CLIP_STRETCH))
        return false;
    if (brain_random(b) < 0.6f)
        cat_vocal(m->cat, CAT_CLIP_YAWN);
    return true;
}

static BrainStatus stretch_step(Brain* b, void* user, float dt) {
    (void)b;
    CatMind* m = user;
    m->give_up -= dt;
    if (cat_settled(m->cat))
        return BRAIN_DONE;
    return m->give_up <= 0.0f ? BRAIN_FAILED : BRAIN_RUNNING;
}

// ---------------------------------------------------------------------------------------------
// Avoid: away from a player who rushes it or crowds it, to somewhere the way there does not
// pass them; cornered, it hisses instead.

static bool crowded(const CatMind* m) {
    return (m->sees || m->hears) && m->distance < PERSONAL_SPACE && m->closing > 0.2f &&
           m->affinity < TRUSTS;
}

static float avoid_score(Brain* b, void* user) {
    const CatMind* m = user;
    const float now = m->alarm > 0.5f ? 0.9f + 0.05f * m->alarm : crowded(m) ? 0.75f : 0.0f;
    // Once away, it keeps going until it is somewhere.
    return b->current == ACT_AVOID ? fmaxf(now, 0.8f) : now;
}

// Whether a route keeps a metre from the player all the way.
static bool clear_of_player(const CatMind* m, const NavRoute* r, const vec3 player) {
    for (int i = 0; i < r->count; i++) {
        for (int k = 1; k <= 2; k++) {
            vec3 at = {0.0f, 0.0f, 0.0f};
            nav_link_point(m->cat->places, r->links[i], 0.5f * (float)k, at);
            if (glm_vec3_distance(at, (float*)player) < 1.0f)
                return false;
        }
    }
    return true;
}

static bool avoid_begin(Brain* b, void* user) {
    (void)b;
    CatMind* m = user;
    vec3 player = {0.0f, 0.0f, 0.0f};
    player_feet(m->player, player);
    int best = -1;
    float best_score = -1e9f;
    for (int p = 0; p < CAT_PLACE_COUNT; p++) {
        vec3 feet = {0.0f, 0.0f, 0.0f};
        cat_place_feet(p, feet);
        const float away = glm_vec3_distance(feet, player);
        NavRoute r;
        if (away < 3.0f || !cat_route(m->cat, (CatPlaceId)p, false, &r) ||
            !clear_of_player(m, &r, player))
            continue;
        const float s = fminf(away, 6.0f) - 0.3f * r.cost;
        if (s > best_score) {
            best_score = s;
            best = p;
        }
    }
    if (best < 0) {
        m->hold = rnd(m, 2.0f, 3.5f);
        m->held = 0.0f;
        m->spot = -1;
        return cat_act(m->cat, CAT_CLIP_HISS);
    }
    return go(m, best, m->alarm > 0.5f ? CAT_RUN : CAT_TROT, CAT_SIT, false, 30.0f);
}

static BrainStatus avoid_step(Brain* b, void* user, float dt) {
    (void)b;
    CatMind* m = user;
    m->give_up -= dt;
    // Cornered: a hiss, held a moment.
    if (m->spot < 0) {
        m->held += dt;
        return m->held >= m->hold ? BRAIN_DONE : BRAIN_RUNNING;
    }
    if (cat_settled(m->cat)) {
        m->alarm *= 0.5f;
        return BRAIN_DONE;
    }
    return m->give_up <= 0.0f ? BRAIN_FAILED : BRAIN_RUNNING;
}

static void avoid_end(Brain* b, void* user, bool interrupted) {
    (void)b;
    (void)interrupted;
    CatMind* m = user;
    if (m->cat->act == CAT_CLIP_HISS)
        cat_settle(m->cat, CAT_STAND);
}

// ---------------------------------------------------------------------------------------------
// Startle: the flinch and the arch, then whatever the fright makes of it.

static float startle_score(Brain* b, void* user) {
    (void)b;
    const CatMind* m = user;
    return m->startled ? 1.0f : 0.0f;
}

static bool startle_begin(Brain* b, void* user) {
    (void)b;
    CatMind* m = user;
    m->startled = false;
    if (!cat_act(m->cat, CAT_CLIP_STARTLE))
        return false;
    m->alarm = 1.0f;
    m->affinity = glm_clamp_zo(m->affinity - 0.15f);
    m->asleep = false;
    return true;
}

static BrainStatus startle_step(Brain* b, void* user, float dt) {
    (void)b;
    (void)dt;
    const CatMind* m = user;
    return m->cat->act == CAT_CLIP_STARTLE ? BRAIN_RUNNING : BRAIN_DONE;
}

// ---------------------------------------------------------------------------------------------
// Follow: now and then, a player it has come to like, trailed at a distance.

static float follow_score(Brain* b, void* user) {
    (void)b;
    const CatMind* m = user;
    const bool off = m->sees && m->distance > 2.0f && m->distance < 6.0f && m->player_speed > 0.4f;
    return m->follow_on && m->affinity >= FONDNESS && off ? 0.42f : 0.0f;
}

// The place nearest the player that leaves a metre and a half between them, and that it can
// get to.
static int near_player(const CatMind* m) {
    vec3 player = {0.0f, 0.0f, 0.0f};
    player_feet(m->player, player);
    int best = -1;
    float best_d = 1e9f;
    for (int p = 0; p < CAT_PLACE_COUNT; p++) {
        vec3 feet = {0.0f, 0.0f, 0.0f};
        cat_place_feet(p, feet);
        const float d = glm_vec3_distance(feet, player);
        NavRoute r;
        if (d >= 1.5f && d < best_d && cat_route(m->cat, (CatPlaceId)p, false, &r)) {
            best_d = d;
            best = p;
        }
    }
    return best;
}

static bool follow_begin(Brain* b, void* user) {
    (void)b;
    CatMind* m = user;
    m->hold = 40.0f;
    m->held = 0.0f;
    m->retarget_in = 0.0f;
    m->spot = -1;
    return near_player(m) >= 0;
}

static BrainStatus follow_step(Brain* b, void* user, float dt) {
    (void)b;
    CatMind* m = user;
    m->held += dt;
    m->retarget_in -= dt;
    if (m->held >= m->hold || m->unseen > 6.0f)
        return BRAIN_DONE;
    if (m->retarget_in <= 0.0f) {
        m->retarget_in = 2.5f;
        const int p = near_player(m);
        if (p >= 0 && p != m->spot && cat_go(m->cat, (CatPlaceId)p, CAT_WALK, CAT_SIT))
            m->spot = p;
    }
    return BRAIN_RUNNING;
}

static void follow_end(Brain* b, void* user, bool interrupted) {
    (void)b;
    (void)interrupted;
    CatMind* m = user;
    m->follow_on = false;
}

// ---------------------------------------------------------------------------------------------

static const BrainActivity ACTIVITIES[ACT_COUNT] = {
    [ACT_SLEEP] = {.score = sleep_score,
                   .begin = sleep_begin,
                   .step = sleep_step,
                   .end = sleep_end,
                   .cooldown = 120.0f,
                   .fail_cooldown = 20.0f,
                   .min_seconds = 20.0f},
    [ACT_STRETCH] = {.score = stretch_score,
                     .begin = stretch_begin,
                     .step = stretch_step,
                     .cooldown = 30.0f,
                     .fail_cooldown = 5.0f},
    [ACT_LOAF] = {.score = loaf_score,
                  .begin = outing_begin,
                  .step = outing_step,
                  .cooldown = 25.0f,
                  .fail_cooldown = 10.0f,
                  .min_seconds = 10.0f},
    [ACT_EXPLORE] = {.score = explore_score,
                     .begin = outing_begin,
                     .step = outing_step,
                     .cooldown = 15.0f,
                     .fail_cooldown = 10.0f,
                     .min_seconds = 6.0f},
    [ACT_WATCH_FLAME] = {.score = watch_flame_score,
                         .begin = outing_begin,
                         .step = outing_step,
                         .cooldown = 150.0f,
                         .fail_cooldown = 30.0f,
                         .min_seconds = 10.0f},
    [ACT_WINDOW] = {.score = window_score,
                    .begin = outing_begin,
                    .step = outing_step,
                    .cooldown = 180.0f,
                    .fail_cooldown = 30.0f,
                    .min_seconds = 10.0f},
    [ACT_AVOID] = {.score = avoid_score,
                   .begin = avoid_begin,
                   .step = avoid_step,
                   .end = avoid_end,
                   .cooldown = 4.0f,
                   .fail_cooldown = 4.0f,
                   .min_seconds = 1.5f},
    [ACT_STARTLE] = {.score = startle_score,
                     .begin = startle_begin,
                     .step = startle_step,
                     .cooldown = 6.0f,
                     .fail_cooldown = 1.0f},
    [ACT_FOLLOW] = {.score = follow_score,
                    .begin = follow_begin,
                    .step = follow_step,
                    .end = follow_end,
                    .cooldown = 90.0f,
                    .fail_cooldown = 20.0f,
                    .min_seconds = 5.0f},
    [ACT_RAIL] = {.score = rail_score,
                  .begin = outing_begin,
                  .step = outing_step,
                  .cooldown = 240.0f,
                  .fail_cooldown = 60.0f,
                  .min_seconds = 15.0f},
};

const char* cat_mind_activities(void) {
    static char list[256];
    if (!list[0]) {
        size_t n = 0;
        for (int i = 0; i < ACT_COUNT; i++)
            n += (size_t)snprintf(list + n, sizeof(list) - n, "%s%s", i ? ", " : "",
                                  ACTIVITY_NAMES[i]);
    }
    return list;
}

bool cat_mind_create(CatMind* m, Cat* cat, Game* game, const Player* player, uint32_t seed,
                     bool blind, const char* first) {
    memset(m, 0, sizeof(*m));
    if (!cat->entity)
        return false;
    m->cat = cat;
    m->game = game;
    m->player = player;
    m->blind = blind;
    m->energy = 0.55f;
    m->affinity = 0.5f;
    m->spot = -1;
    for (int i = 0; i < CAT_PLACE_COUNT; i++)
        m->since[i] = NOVEL_SECONDS;
    Brain* b = create_brain(seed);
    if (!b)
        return false;
    m->brain = entity_add_brain(cat->entity, b);
    if (!m->brain) {
        free_brain(b);
        return false;
    }
    m->blink_in = rnd(m, 8.0f, 20.0f);
    m->flick_in = rnd(m, 10.0f, 25.0f);
    m->meow_in = rnd(m, 60.0f, 150.0f);
    m->glance_in = rnd(m, 4.0f, 8.0f);
    m->first = -1;
    for (int i = 0; first && i < ACT_COUNT; i++)
        if (!strcmp(first, ACTIVITY_NAMES[i]))
            m->first = i;
    if (first && m->first < 0)
        fprintf(stderr, "silent: the cat has no activity '%s' (%s)\n", first,
                cat_mind_activities());
    // Curled up where it starts, it is already asleep.
    if (!first && cat->posture == CAT_CURL)
        m->first = ACT_SLEEP;
    return true;
}

// The activities go in once the cat is in the house: until then it can neither move nor be
// seen, and a brain with something to choose would begin it, fail, and hold that against it.
static void wake_up(CatMind* m) {
    for (int i = 0; i < ACT_COUNT; i++) {
        BrainActivity a = ACTIVITIES[i];
        a.name = ACTIVITY_NAMES[i];
        a.user = m;
        brain_add_activity(m->brain, &a);
    }
    if (m->first >= 0)
        brain_start(m->brain, m->first);
}

// ---------------------------------------------------------------------------------------------
// Senses and needs

// Whether a point is where the cat can see it. The cone is the body's and not the head's: the
// head turns to what it watches, and a cone that turned with it would keep the player in view by
// looking at them.
static bool in_view(const Cat* cat, const vec3 eye, const vec3 point) {
    vec3 to;
    glm_vec3_sub((float*)point, (float*)eye, to);
    const float d = glm_vec3_norm(to);
    if (d > SIGHT_RANGE)
        return false;
    const vec3 facing = {sinf(cat->yaw), 0.0f, cosf(cat->yaw)};
    return d < 1e-4f || glm_vec3_dot(to, (float*)facing) / d >= cosf(glm_rad(SIGHT_HALF_ANGLE));
}

// The player as the body that moves, step by step, and not the camera, which is posed once a
// frame and may be pinned somewhere else entirely.
static void sense_player(CatMind* m, float dt) {
    Cat* cat = m->cat;
    vec3 feet = {0.0f, 0.0f, 0.0f}, vel = {0.0f, 0.0f, 0.0f}, here = {0.0f, 0.0f, 0.0f};
    player_feet(m->player, feet);
    player_velocity(m->player, vel);
    cat_feet(cat, here);
    glm_vec3_copy(feet, m->player_eye);
    m->player_eye[1] += PLAYER_EYE_HEIGHT;
    m->player_speed = sqrtf(vel[0] * vel[0] + vel[2] * vel[2]);
    const float dx = here[0] - feet[0], dz = here[2] - feet[2];
    const float flat = sqrtf(dx * dx + dz * dz);
    m->distance = flat + 0.5f * fabsf(here[1] - feet[1]);
    // How fast the PLAYER comes on, not how fast the gap closes: a cat walking past someone
    // standing still is not being crowded.
    m->closing = flat > 1e-3f ? (vel[0] * dx + vel[2] * dz) / flat : 0.0f;

    m->sense_in -= dt;
    if (m->sense_in <= 0.0f) {
        m->sense_in = SIGHT_EVERY;
        vec3 at = {0.0f, 0.0f, 0.0f}, gaze = {0.0f, 0.0f, 0.0f};
        // Looking at the chest rather than the eyes, which the capsule's top may not reach.
        const vec3 chest = {feet[0], feet[1] + PLAYER_EYE_HEIGHT - 0.4f, feet[2]};
        m->sees =
            !m->blind && cat->attached && !cat_eyes_shut(cat) && cat_eye(cat, at, gaze) &&
            in_view(cat, at, chest) && m->game->physics_world &&
            physics_world_line_of_sight(m->game->physics_world, at, chest,
                                        entity_get_rigid_body(cat->entity), m->player->entity);
        m->hears = !m->blind && cat->attached && m->player_speed > HEARS_SPRINT &&
                   m->distance < HEARS_WITHIN && fabsf(here[1] - feet[1]) < 2.5f;
    }

    if (m->sees) {
        if (m->unseen > TRILL_AFTER && !m->asleep)
            cat_vocal(cat, CAT_CLIP_TRILL);
        m->unseen = 0.0f;
    } else {
        m->unseen += dt;
    }

    // A glance at a player in sight now and then, timed here at the step's cadence, since the
    // draws it takes are the brain's.
    if (m->sees && m->distance < WATCH_WITHIN) {
        m->glance_in -= dt;
        if (m->glance_in <= 0.0f) {
            m->glance_in = rnd(m, 4.0f, 8.0f);
            m->glance_left = 1.5f;
        }
        m->glance_left -= dt;
    }

    // Rushed: fast, near and coming on, or bumped into.
    const bool noticed = m->sees || m->hears;
    if (noticed && m->brain->current != ACT_STARTLE && m->brain->cooldowns[ACT_STARTLE] <= 0.0f &&
        ((m->player_speed > STARTLE_SPEED && m->distance < STARTLE_WITHIN && m->closing > 0.8f) ||
         (m->distance < 0.5f && m->player_speed > 0.8f))) {
        m->startled = true;
        brain_interrupt(m->brain);
    }
    if (crowded(m))
        brain_interrupt(m->brain);
}

void cat_mind_sense(CatMind* m, float rain, float dt) {
    if (!m->brain || !m->cat->attached)
        return;
    if (m->brain->activity_count == 0)
        wake_up(m);
    Cat* cat = m->cat;
    m->rain = glm_clamp_zo(rain);
    sense_player(m, dt);

    const bool asleep = m->asleep;
    m->energy = glm_clamp_zo(m->energy + (asleep ? WAKE_RATE : -TIRE_RATE) * dt);
    m->alarm = fmaxf(0.0f, m->alarm - 0.08f * dt);
    m->boredom =
        glm_clamp_zo(m->boredom + (cat_here(cat) && !asleep ? BORED_RATE : -BORED_RATE) * dt);
    if (m->sees && m->distance < 3.0f && m->player_speed < 0.6f)
        m->affinity = glm_clamp_zo(m->affinity + 0.003f * dt);
    for (int i = 0; i < CAT_PLACE_COUNT; i++)
        m->since[i] += dt;
    const int here = cat_here(cat) ? cat_place(cat) : -1;
    if (here >= 0)
        m->since[here] = 0.0f;

    m->follow_roll_in -= dt;
    if (m->follow_roll_in <= 0.0f) {
        m->follow_roll_in = 30.0f;
        m->follow_on = brain_random(m->brain) < SOCIABILITY;
    }

    // The face: blinks, flicks of the ears, a meow now and then at someone near.
    if (asleep || cat_eyes_shut(cat))
        return;
    m->blink_in -= dt;
    if (m->blink_in <= 0.0f && cat_vocal(cat, CAT_CLIP_BLINK))
        m->blink_in = rnd(m, 8.0f, 20.0f);
    if (cat_settled(cat)) {
        m->flick_in -= dt;
        if (m->flick_in <= 0.0f &&
            cat_vocal(cat,
                      brain_random(m->brain) < 0.5f ? CAT_CLIP_EAR_FLICK_L : CAT_CLIP_EAR_FLICK_R))
            m->flick_in = rnd(m, 10.0f, 25.0f);
        if (m->sees && m->distance < 3.0f) {
            m->meow_in -= dt;
            if (m->meow_in <= 0.0f &&
                cat_vocal(cat,
                          brain_random(m->brain) < 0.6f ? CAT_CLIP_MEOW_SHORT : CAT_CLIP_MEOW_LONG))
                m->meow_in = rnd(m, 60.0f, 150.0f);
        }
    }
    // Held still and looked at, it blinks slowly back.
    vec3 camera = {0.0f, 0.0f, 0.0f}, forward = {0.0f, 0.0f, -1.0f};
    player_eye(m->player, camera, forward);
    vec3 to_cat = {0.0f, 0.0f, 0.0f}, here_feet = {0.0f, 0.0f, 0.0f};
    cat_feet(cat, here_feet);
    glm_vec3_sub(here_feet, m->player_eye, to_cat);
    glm_vec3_normalize(to_cat);
    const bool looked_at = m->sees && m->distance < 4.0f && m->player_speed < 0.1f &&
                           glm_vec3_dot(to_cat, forward) > 0.95f;
    m->still_look = looked_at ? m->still_look + dt : fminf(m->still_look, 0.0f);
    if (m->still_look > SLOW_BLINK_AFTER && cat_vocal(cat, CAT_CLIP_SLOW_BLINK)) {
        m->still_look = -10.0f;
        m->affinity = glm_clamp_zo(m->affinity + 0.05f);
    }
}

bool cat_mind_at_ease(const CatMind* m) {
    return !m->brain || (m->affinity >= FONDNESS && m->player_speed < 1.0f);
}

// ---------------------------------------------------------------------------------------------
// The head

void cat_mind_frame(CatMind* m, double time) {
    if (!m->brain)
        return;
    Cat* cat = m->cat;
    const int act = m->brain->current;
    const bool there = cat_here(cat);
    CatGaze gaze = CAT_GAZE_NONE;
    if (act == ACT_WATCH_FLAME && there) {
        // The middle candle's flame, with the small wander of an eye that follows one.
        gaze = CAT_GAZE_FLAME;
        mansion_candelabra_flame(m->gaze_at);
        m->gaze_at[0] += 0.01f * sinf((float)time * 2.3f);
        m->gaze_at[1] += 0.01f * sinf((float)time * 3.1f);
    } else if (act == ACT_WINDOW && there) {
        gaze = CAT_GAZE_WINDOW;
        // The drips off the gutter past the glass, and between them the grounds.
        const float t = (float)time;
        const float drips = 0.5f + 0.5f * sinf(t * 0.21f);
        const float out_x = 2.6f + 1.5f * sinf(t * 0.13f);
        const float drip[3] = {0.5f * (KITCHEN_WIN_X0 + KITCHEN_WIN_X1),
                               DINING_WIN_SILL + 0.6f + 0.4f * fmodf(t * 0.7f, 1.0f),
                               HOUSE_FRONT_Z - 0.45f};
        const float grounds[3] = {out_x, 1.2f, HOUSE_FRONT_Z - 7.0f};
        mansion_at(drips > 0.6f ? drip : grounds, m->gaze_at);
    }

    // The player takes the gaze when near, fast, or the reason for what it is doing; otherwise
    // only for a glance now and then.
    if (m->sees && m->distance < WATCH_WITHIN) {
        const bool held = m->distance < 3.0f || m->player_speed > 1.0f || act == ACT_AVOID ||
                          act == ACT_FOLLOW || act == ACT_STARTLE;
        if (held || m->glance_left > 0.0f) {
            gaze = CAT_GAZE_PLAYER;
            glm_vec3_copy(m->player_eye, m->gaze_at);
        }
    }
    m->gaze = gaze;
    cat_look(cat, gaze == CAT_GAZE_NONE ? NULL : m->gaze_at);
}

void cat_mind_trace(const CatMind* m) {
    if (!m->brain)
        return;
    printf("cat mind %-11s E %.2f B %.2f A %.2f F %.2f %s %.1fm gaze %d\n",
           brain_current_name(m->brain) ? brain_current_name(m->brain) : "-", (double)m->energy,
           (double)m->boredom, (double)m->alarm, (double)m->affinity,
           m->sees    ? "sees"
           : m->hears ? "hears"
                      : "-",
           (double)m->distance, (int)m->gaze);
}
