#include "cat_brain.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include "cimgui.h"

#include "cetra/camera.h"

#include "cat_clips.h"
#include "cat_places.h"
#include "clock.h"
#include "layout.h"
#include "player.h"

// The activities, in the order they are registered, which is the brain's index for each.
typedef enum {
    ACT_SLEEP,
    ACT_STRETCH,
    ACT_LOAF,
    ACT_EXPLORE,
    ACT_WATCH_CLOCK,
    ACT_WINDOW,
    ACT_AVOID,
    ACT_STARTLE,
    ACT_FOLLOW,
    ACT_RAIL,
    ACT_COUNT
} CatActivity;

static const char* const ACTIVITY_NAMES[ACT_COUNT] = {"sleep",       "stretch", "loaf",  "explore",
                                                      "watch_clock", "window",  "avoid", "startle",
                                                      "follow",      "rail"};

// The aloof cat's temperament: one set of numbers, not presets.
#define WAKE_RATE        (1.0f / 80.0f)  // energy gained a second asleep
#define TIRE_RATE        (1.0f / 400.0f) // and lost a second awake
#define BORED_RATE       (1.0f / 60.0f)  // boredom gained a second sat still awake
#define NOVEL_SECONDS    180.0f          // how long until a place is as good as new again
#define SIGHT_EVERY      0.2f            // seconds between looks for the player
#define HEARS_SPRINT     2.0f            // m/s: a player this fast is heard...
#define HEARS_WITHIN     6.0f            // ...this close, through anything
#define STARTLE_SPEED    2.2f            // a player this fast...
#define STARTLE_WITHIN   2.5f            // ...this close, and closing, startles it
#define PERSONAL_SPACE   1.3f            // nearer than this and coming closer, it moves off
#define TRUSTS           0.7f            // ...unless it trusts the player this much
#define FOLLOWS_ABOVE    0.55f           // the affinity it follows at, sometimes
#define SOCIABILITY      0.3f            // the chance, each half minute, that it would follow
#define WATCH_WITHIN     7.0f            // a seen player nearer than this can take the gaze
#define TRILL_AFTER      45.0f           // seconds out of sight before seeing the player trills
#define SLOW_BLINK_AFTER 3.0f            // seconds of being looked at, still, before a slow blink

static float clamp01(float x) {
    return fminf(1.0f, fmaxf(0.0f, x));
}

static float rnd(CatMind* m, float lo, float hi) {
    return lo + (hi - lo) * brain_random(m->brain);
}

static float flat_distance(const vec3 a, const vec3 b) {
    const float dx = a[0] - b[0], dz = a[2] - b[2];
    return sqrtf(dx * dx + dz * dz);
}

// How far, as the route goes, from where the cat stands to a place; below zero when it cannot
// get there.
static float route_cost(const CatMind* m, int place) {
    NavRoute r;
    const int from =
        m->cat->follower.arrived
            ? m->cat->follower.at
            : m->cat->places->links[m->cat->follower.route.links[m->cat->follower.leg]].to;
    if (from == place)
        return 0.0f;
    return nav_graph_route(m->cat->places, from, place, NULL, &r) ? r.cost : -1.0f;
}

static float novelty(const CatMind* m, int place) {
    return clamp01(m->since[place] / NOVEL_SECONDS);
}

// The best of `places` to go to now: new to it, near, and a little chance; -1 for none.
static int pick(CatMind* m, const char* const* places, const float* weights, int count) {
    int best = -1;
    float best_score = 0.0f;
    for (int i = 0; i < count; i++) {
        const int p = nav_graph_find(m->cat->places, places[i]);
        if (p < 0 || (p == m->cat->at && m->cat->follower.arrived))
            continue;
        const float cost = route_cost(m, p);
        if (cost < 0.0f)
            continue;
        const float w = weights ? weights[i] : 1.0f;
        const float s = (0.15f + novelty(m, p)) * w / (1.0f + cost / 8.0f) * rnd(m, 0.6f, 1.4f);
        if (s > best_score) {
            best_score = s;
            best = p;
        }
    }
    return best;
}

static bool go(CatMind* m, int place, CatGait gait, int settle) {
    m->spot = place;
    m->held = 0.0f;
    m->groomed = false;
    m->give_up = 90.0f;
    return place >= 0 && cat_go(m->cat, CAT_PLACES[place].name, gait, settle);
}

// There and settled: holds, sometimes grooming if it sits. True once it has held long enough.
static bool stay(CatMind* m, float dt, float groom_chance) {
    Cat* cat = m->cat;
    m->give_up -= dt;
    if (cat->goal >= 0 || cat->mode == CAT_WALKING || cat->mode == CAT_JUMPING ||
        cat->mode == CAT_TURNING || cat->mode == CAT_SHIFTING)
        return m->give_up <= 0.0f;
    m->held += dt;
    if (!m->groomed && cat->posture == CAT_SIT && m->held > 2.0f && m->held < m->hold - 8.0f) {
        m->groomed = true;
        if (brain_random(m->brain) < groom_chance)
            cat_act(cat, CAT_CLIP_GROOM);
    }
    // A groom lasts a few seconds, then it sits again.
    if (cat->act == CAT_CLIP_GROOM && m->held > 9.0f)
        cat_go(cat, CAT_PLACES[cat->at].name, CAT_WALK, CAT_SIT);
    return m->held >= m->hold;
}

static bool is_running(const Brain* b, int act) {
    return b->current == act;
}

static void took(CatMind* m, Brain* b) {
    m->running = b->current >= 0 ? b->scores[b->current] : 0.0f;
}

// ---------------------------------------------------------------------------------------------
// Sleep: curled, somewhere it likes, until rested.

static const char* const SLEEP_SPOTS[] = {"study_chair", "kitchen_chair", "stove_mat", "rug",
                                          "study_bay_w"};
static const float SLEEP_WEIGHTS[] = {3.0f, 1.0f, 1.2f, 0.8f, 0.8f};

static float sleep_score(Brain* b, void* user) {
    CatMind* m = user;
    if (is_running(b, ACT_SLEEP))
        return m->asleep ? 0.95f : m->running;
    return 0.9f * powf(1.0f - m->energy, 1.5f);
}

static bool sleep_begin(Brain* b, void* user) {
    CatMind* m = user;
    took(m, b);
    m->hold = 240.0f;
    // Already curled up somewhere it may sleep: sleep there.
    if (m->cat->posture == CAT_CURL && m->cat->follower.arrived) {
        m->spot = m->cat->at;
        m->held = 0.0f;
        m->give_up = 90.0f;
        return true;
    }
    const int n = (int)(sizeof(SLEEP_SPOTS) / sizeof(SLEEP_SPOTS[0]));
    int p = -1;
    for (int i = 0; i < n && p < 0; i++)
        if (m->cat->follower.arrived && !strcmp(CAT_PLACES[m->cat->at].name, SLEEP_SPOTS[i]))
            p = m->cat->at;
    if (p < 0) {
        // Novelty has no say in where it sleeps: home, mostly, and the nearer the better.
        float best = 0.0f;
        for (int i = 0; i < n; i++) {
            const int q = nav_graph_find(m->cat->places, SLEEP_SPOTS[i]);
            const float cost = q >= 0 ? route_cost(m, q) : -1.0f;
            const float s = cost < 0.0f ? 0.0f : SLEEP_WEIGHTS[i] / (1.0f + cost / 6.0f);
            if (s > best) {
                best = s;
                p = q;
            }
        }
    }
    return go(m, p, CAT_WALK, CAT_CURL);
}

static BrainStatus sleep_step(Brain* b, void* user, float dt) {
    (void)b;
    CatMind* m = user;
    m->asleep = m->cat->posture == CAT_CURL && m->cat->mode == CAT_RESTING;
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
    CatMind* m = user;
    return is_running(b, ACT_STRETCH) ? m->running : m->want_stretch ? 0.97f : 0.0f;
}

static bool stretch_begin(Brain* b, void* user) {
    CatMind* m = user;
    took(m, b);
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
// Loaf: lie about somewhere it has not been for a while.

static const char* const LOAF_SPOTS[] = {"rug",    "gallery",      "study_bay_w", "stove_mat",
                                         "hearth", "study_window", "study_mid",   "kitchen_chair"};

static float loaf_score(Brain* b, void* user) {
    CatMind* m = user;
    return is_running(b, ACT_LOAF) ? m->running : 0.22f + 0.3f * (1.0f - m->energy);
}

static bool loaf_begin(Brain* b, void* user) {
    CatMind* m = user;
    took(m, b);
    const int p = pick(m, LOAF_SPOTS, NULL, (int)(sizeof(LOAF_SPOTS) / sizeof(LOAF_SPOTS[0])));
    m->hold = rnd(m, 20.0f, 40.0f);
    // Where the place has no posture of its own it lies down.
    const int settle = p >= 0 && CAT_PLACES[p].rest == CAT_REST_NONE ? CAT_LIE : -1;
    return go(m, p, CAT_WALK, settle);
}

static BrainStatus loaf_step(Brain* b, void* user, float dt) {
    CatMind* m = user;
    if (!stay(m, dt, 0.35f))
        return m->give_up <= 0.0f ? BRAIN_FAILED : BRAIN_RUNNING;
    m->want_stretch = brain_random(b) < 0.4f;
    return BRAIN_DONE;
}

// ---------------------------------------------------------------------------------------------
// Explore: off somewhere new, a look round, a wash.

static float explore_score(Brain* b, void* user) {
    CatMind* m = user;
    return is_running(b, ACT_EXPLORE) ? m->running : 0.18f + 0.5f * m->boredom;
}

static bool explore_begin(Brain* b, void* user) {
    CatMind* m = user;
    took(m, b);
    const char* names[CAT_MIND_PLACES] = {NULL};
    int n = 0;
    for (int i = 0; i < CAT_PLACE_COUNT && n < CAT_MIND_PLACES; i++)
        if (CAT_PLACES[i].rest != CAT_REST_NONE)
            names[n++] = CAT_PLACES[i].name;
    const int p = pick(m, names, NULL, n);
    m->hold = rnd(m, 6.0f, 12.0f);
    return go(m, p, brain_random(b) < 0.2f ? CAT_TROT : CAT_WALK, -1);
}

static BrainStatus explore_step(Brain* b, void* user, float dt) {
    (void)b;
    CatMind* m = user;
    if (m->held == 0.0f && cat_settled(m->cat))
        m->boredom = 0.0f;
    if (!stay(m, dt, 0.5f))
        return m->give_up <= 0.0f ? BRAIN_FAILED : BRAIN_RUNNING;
    return BRAIN_DONE;
}

// ---------------------------------------------------------------------------------------------
// The clock and the window: sitting and watching something move.

static int place_named(const CatMind* m, const char* name) {
    return nav_graph_find(m->cat->places, name);
}

static float watch_clock_score(Brain* b, void* user) {
    const CatMind* m = user;
    if (is_running(b, ACT_WATCH_CLOCK))
        return m->running;
    const int p = place_named(m, "hall_clock");
    return p < 0 ? 0.0f : 0.4f * novelty(m, p);
}

static bool watch_clock_begin(Brain* b, void* user) {
    CatMind* m = user;
    took(m, b);
    m->hold = rnd(m, 15.0f, 30.0f);
    return go(m, place_named(m, "hall_clock"), CAT_WALK, CAT_SIT);
}

static BrainStatus watch_step(Brain* b, void* user, float dt) {
    (void)b;
    CatMind* m = user;
    if (!stay(m, dt, 0.0f))
        return m->give_up <= 0.0f ? BRAIN_FAILED : BRAIN_RUNNING;
    return BRAIN_DONE;
}

static float window_score(Brain* b, void* user) {
    const CatMind* m = user;
    if (is_running(b, ACT_WINDOW))
        return m->running;
    const int p = place_named(m, "kitchen_window");
    return p < 0 ? 0.0f : (0.3f + 0.12f * m->rain) * novelty(m, p);
}

static bool window_begin(Brain* b, void* user) {
    CatMind* m = user;
    took(m, b);
    m->hold = rnd(m, 20.0f, 45.0f);
    return go(m, place_named(m, "kitchen_window"), CAT_WALK, CAT_SIT);
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
    if (is_running(b, ACT_AVOID))
        return fmaxf(m->running, 0.8f);
    if (m->alarm > 0.5f)
        return 0.9f + 0.05f * m->alarm;
    return crowded(m) ? 0.75f : 0.0f;
}

// Whether a route from where the cat is keeps a metre from the player all the way.
static bool clear_of_player(const CatMind* m, int place) {
    NavRoute r;
    const NavFollower* f = &m->cat->follower;
    const int from = f->arrived ? f->at : m->cat->places->links[f->route.links[f->leg]].to;
    if (from != place && !nav_graph_route(m->cat->places, from, place, NULL, &r))
        return false;
    vec3 here = {0.0f, 0.0f, 0.0f};
    cat_feet(m->cat, here);
    const int count = from == place ? 0 : r.count;
    for (int i = -1; i < count; i++) {
        vec3 a, mid;
        if (i < 0) {
            glm_vec3_copy(m->cat->places->nodes[from].position, a);
            glm_vec3_lerp(here, a, 0.5f, mid);
        } else {
            nav_link_point(m->cat->places, r.links[i], 1.0f, a);
            nav_link_point(m->cat->places, r.links[i], 0.5f, mid);
        }
        if (glm_vec3_distance(a, (float*)m->player_feet) < 1.0f ||
            glm_vec3_distance(mid, (float*)m->player_feet) < 1.0f)
            return false;
    }
    return true;
}

static bool avoid_begin(Brain* b, void* user) {
    CatMind* m = user;
    took(m, b);
    int best = -1;
    float best_score = -1e9f;
    for (int p = 0; p < CAT_PLACE_COUNT; p++) {
        const float away = glm_vec3_distance(m->cat->places->nodes[p].position, m->player_feet);
        if (away < 3.0f || !clear_of_player(m, p))
            continue;
        const float cost = route_cost(m, p);
        const float s = fminf(away, 6.0f) - 0.3f * cost;
        if (cost >= 0.0f && s > best_score) {
            best_score = s;
            best = p;
        }
    }
    m->give_up = 30.0f;
    if (best < 0) {
        m->hold = rnd(m, 2.0f, 3.5f);
        m->held = 0.0f;
        m->spot = -1;
        return cat_act(m->cat, CAT_CLIP_HISS);
    }
    return go(m, best, m->alarm > 0.5f ? CAT_RUN : CAT_TROT, CAT_SIT);
}

static BrainStatus avoid_step(Brain* b, void* user, float dt) {
    (void)b;
    CatMind* m = user;
    m->give_up -= dt;
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
    // Out of a hiss back to standing.
    if (m->cat->act == CAT_CLIP_HISS && m->cat->follower.arrived)
        cat_go(m->cat, CAT_PLACES[m->cat->at].name, CAT_WALK, CAT_STAND);
}

// ---------------------------------------------------------------------------------------------
// Startle: the flinch and the arch, then whatever the fright makes of it.

static float startle_score(Brain* b, void* user) {
    CatMind* m = user;
    return is_running(b, ACT_STARTLE) ? 1.0f : m->startled ? 1.0f : 0.0f;
}

static bool startle_begin(Brain* b, void* user) {
    CatMind* m = user;
    (void)b;
    m->startled = false;
    if (!cat_act(m->cat, CAT_CLIP_STARTLE))
        return false;
    m->alarm = 1.0f;
    m->affinity = clamp01(m->affinity - 0.15f);
    m->asleep = false;
    return true;
}

static BrainStatus startle_step(Brain* b, void* user, float dt) {
    (void)dt;
    const CatMind* m = user;
    if (m->cat->act == CAT_CLIP_STARTLE)
        return BRAIN_RUNNING;
    brain_interrupt(b);
    return BRAIN_DONE;
}

// ---------------------------------------------------------------------------------------------
// Follow: now and then, a player it has come to like, trailed at a distance.

static float follow_score(Brain* b, void* user) {
    const CatMind* m = user;
    if (is_running(b, ACT_FOLLOW))
        return m->running;
    const bool off = m->sees && m->distance > 2.0f && m->distance < 6.0f && m->player_speed > 0.4f;
    return m->follow_on && m->affinity >= FOLLOWS_ABOVE && off ? 0.42f : 0.0f;
}

// The place nearest the player that leaves a metre and a half between them.
static int near_player(const CatMind* m) {
    int best = -1;
    float best_d = 1e9f;
    for (int p = 0; p < CAT_PLACE_COUNT; p++) {
        const float d =
            glm_vec3_distance(m->cat->places->nodes[p].position, (float*)m->player_feet);
        if (d >= 1.5f && d < best_d && route_cost(m, p) >= 0.0f) {
            best_d = d;
            best = p;
        }
    }
    return best;
}

static bool follow_begin(Brain* b, void* user) {
    CatMind* m = user;
    took(m, b);
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
        if (p >= 0 && p != m->spot) {
            m->spot = p;
            cat_go(m->cat, CAT_PLACES[p].name, CAT_WALK, CAT_SIT);
        }
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
// The rail: up onto the gallery's hand rail, along it over the drop, onto the newel at its end,
// and a sit there looking out over the great hall. Only when it has the energy for it.

static float rail_score(Brain* b, void* user) {
    const CatMind* m = user;
    if (is_running(b, ACT_RAIL))
        return m->running;
    const int p = place_named(m, "newel_cap");
    return p < 0 ? 0.0f : 0.34f * novelty(m, p) * (m->energy > 0.4f ? 1.0f : 0.2f);
}

static bool rail_begin(Brain* b, void* user) {
    CatMind* m = user;
    took(m, b);
    m->hold = rnd(m, 10.0f, 25.0f);
    const int p = place_named(m, "newel_cap");
    m->spot = p;
    m->held = 0.0f;
    m->groomed = true;
    m->give_up = 120.0f;
    return p >= 0 && cat_go_by_rail(m->cat, CAT_PLACES[p].name, CAT_SIT);
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
                  .begin = loaf_begin,
                  .step = loaf_step,
                  .cooldown = 25.0f,
                  .fail_cooldown = 10.0f,
                  .min_seconds = 10.0f},
    [ACT_EXPLORE] = {.score = explore_score,
                     .begin = explore_begin,
                     .step = explore_step,
                     .cooldown = 15.0f,
                     .fail_cooldown = 10.0f,
                     .min_seconds = 6.0f},
    [ACT_WATCH_CLOCK] = {.score = watch_clock_score,
                         .begin = watch_clock_begin,
                         .step = watch_step,
                         .cooldown = 150.0f,
                         .fail_cooldown = 30.0f,
                         .min_seconds = 10.0f},
    [ACT_WINDOW] = {.score = window_score,
                    .begin = window_begin,
                    .step = watch_step,
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
                  .begin = rail_begin,
                  .step = watch_step,
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

bool cat_mind_create(CatMind* m, Cat* cat, Game* game, const Entity* player, uint32_t seed,
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
    m->unseen = 0.0f;
    for (int i = 0; i < CAT_MIND_PLACES; i++)
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
    m->follow_roll_in = 0.0f;
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

static void sense_player(CatMind* m, const vec3 eye, float dt) {
    Cat* cat = m->cat;
    vec3 feet = {eye[0], eye[1] - PLAYER_EYE_HEIGHT, eye[2]};
    if (m->player_eye[0] != 0.0f || m->player_eye[2] != 0.0f) {
        vec3 v;
        glm_vec3_sub(feet, m->player_feet, v);
        glm_vec3_scale(v, dt > 0.0f ? 1.0f / dt : 0.0f, v);
        glm_vec3_lerp(m->player_vel, v, 0.3f, m->player_vel);
    }
    glm_vec3_copy((float*)eye, m->player_eye);
    glm_vec3_copy(feet, m->player_feet);
    m->player_speed =
        sqrtf(m->player_vel[0] * m->player_vel[0] + m->player_vel[2] * m->player_vel[2]);

    vec3 here = {0.0f, 0.0f, 0.0f};
    cat_feet(cat, here);
    const float d = flat_distance(here, feet) + 0.5f * fabsf(here[1] - feet[1]);
    // How fast the PLAYER comes on, not how fast the gap closes: a cat walking past someone
    // standing still is not being crowded.
    const float flat = flat_distance(here, feet);
    m->closing =
        flat > 1e-3f
            ? (m->player_vel[0] * (here[0] - feet[0]) + m->player_vel[2] * (here[2] - feet[2])) /
                  flat
            : 0.0f;
    m->distance = d;

    m->sense_in -= dt;
    if (m->sense_in <= 0.0f) {
        m->sense_in = SIGHT_EVERY;
        vec3 at = {0.0f, 0.0f, 0.0f}, forward = {0.0f, 0.0f, 0.0f};
        // Looking at the chest rather than the eyes, which the capsule's top may not reach.
        vec3 chest = {eye[0], eye[1] - 0.4f, eye[2]};
        m->sees =
            !m->blind && cat->attached && !cat_eyes_shut(cat) && cat_eye(cat, at, forward) &&
            brain_in_view(m->brain, at, (vec3){sinf(cat->yaw), 0.0f, cosf(cat->yaw)}, chest) &&
            brain_can_see(m->brain, m->game->physics_world, at, chest, m->player);
        m->hears = !m->blind && cat->attached && m->player_speed > HEARS_SPRINT &&
                   d < HEARS_WITHIN && fabsf(here[1] - feet[1]) < 2.5f;
        if (m->sees || m->hears)
            brain_notice(m->brain, m->player->id, feet);
    }

    if (m->sees) {
        if (m->unseen > TRILL_AFTER && !m->asleep)
            cat_vocal(cat, CAT_CLIP_TRILL);
        m->unseen = 0.0f;
    } else {
        m->unseen += dt;
    }

    // Rushed: fast, near and coming on, or bumped into.
    const bool noticed = m->sees || m->hears;
    if (noticed && m->brain->current != ACT_STARTLE && m->brain->cooldowns[ACT_STARTLE] <= 0.0f &&
        ((m->player_speed > STARTLE_SPEED && d < STARTLE_WITHIN && m->closing > 0.8f) ||
         (d < 0.5f && m->player_speed > 0.8f))) {
        m->startled = true;
        brain_interrupt(m->brain);
    }
    if (crowded(m))
        brain_interrupt(m->brain);
}

void cat_mind_sense(CatMind* m, const vec3 player_eye, const vec3 player_forward, float rain,
                    float dt) {
    if (!m->brain || !m->cat->attached)
        return;
    if (m->brain->activity_count == 0)
        wake_up(m);
    Cat* cat = m->cat;
    m->rain = clamp01(rain);
    sense_player(m, player_eye, dt);

    const bool asleep = m->asleep;
    m->energy = clamp01(m->energy + (asleep ? WAKE_RATE : -TIRE_RATE) * dt);
    m->alarm = fmaxf(0.0f, m->alarm - 0.08f * dt);
    const bool still = cat_settled(cat) || cat->mode == CAT_ACTING;
    m->boredom = clamp01(m->boredom + (still && !asleep ? BORED_RATE : -BORED_RATE) * dt);
    if (m->sees && m->distance < 3.0f && m->player_speed < 0.6f)
        m->affinity = clamp01(m->affinity + 0.003f * dt);
    for (int i = 0; i < CAT_PLACE_COUNT && i < CAT_MIND_PLACES; i++)
        m->since[i] += dt;
    if (cat_settled(cat) && cat->follower.arrived)
        m->since[cat->at] = 0.0f;

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
    vec3 to_cat = {0.0f, 0.0f, 0.0f}, here = {0.0f, 0.0f, 0.0f};
    cat_feet(cat, here);
    glm_vec3_sub(here, (float*)player_eye, to_cat);
    glm_vec3_normalize(to_cat);
    const bool looked_at = m->sees && m->distance < 4.0f && m->player_speed < 0.1f &&
                           glm_vec3_dot(to_cat, (float*)player_forward) > 0.95f;
    m->still_look = looked_at ? m->still_look + dt : fminf(m->still_look, 0.0f);
    if (m->still_look > SLOW_BLINK_AFTER && cat_vocal(cat, CAT_CLIP_SLOW_BLINK)) {
        m->still_look = -10.0f;
        m->affinity = clamp01(m->affinity + 0.05f);
    }
}

// ---------------------------------------------------------------------------------------------
// The head

void cat_mind_frame(CatMind* m, double time) {
    if (!m->brain)
        return;
    Cat* cat = m->cat;
    const int act = m->brain->current;
    const bool there = cat_settled(cat) || cat->act == CAT_CLIP_GROOM;
    CatGaze gaze = CAT_GAZE_NONE;
    if (act == ACT_WATCH_CLOCK && there) {
        gaze = CAT_GAZE_CLOCK;
        clock_bob(time, m->gaze_at);
    } else if (act == ACT_WINDOW && there) {
        gaze = CAT_GAZE_WINDOW;
        // The drips off the gutter past the glass, and between them the street.
        const float t = (float)time;
        const float drips = 0.5f + 0.5f * sinf(t * 0.21f);
        const float street_x = 1.95f + 1.5f * sinf(t * 0.13f);
        vec3 drip = {0.5f * (KITCHEN_WIN_X0 + KITCHEN_WIN_X1),
                     KITCHEN_WIN_SILL + 0.3f + 0.4f * fmodf(t * 0.7f, 1.0f), HOUSE_FRONT_Z - 0.45f};
        vec3 street = {street_x, 1.2f, HOUSE_FRONT_Z - 7.0f};
        glm_vec3_lerp(street, drip, drips > 0.6f ? 1.0f : 0.0f, m->gaze_at);
    }

    // The player takes the gaze when near, fast, or the reason for what it is doing; otherwise
    // only for a glance now and then.
    const float dt = (float)m->game->sim_clock.delta;
    if (m->sees && m->distance < WATCH_WITHIN) {
        const bool held = m->distance < 3.0f || m->player_speed > 1.0f || act == ACT_AVOID ||
                          act == ACT_FOLLOW || act == ACT_STARTLE;
        m->glance_in -= dt;
        if (m->glance_in <= 0.0f) {
            m->glance_in = rnd(m, 4.0f, 8.0f);
            m->glance_left = 1.5f;
        }
        m->glance_left -= dt;
        if (held || m->glance_left > 0.0f) {
            gaze = CAT_GAZE_PLAYER;
            glm_vec3_copy(m->player_eye, m->gaze_at);
        }
    }
    m->gaze = gaze;
    cat_look(cat, gaze == CAT_GAZE_NONE ? NULL : m->gaze_at);
}

// ---------------------------------------------------------------------------------------------
// Seeing it think

static bool to_screen(const mat4 view_proj, const vec3 p, float w, float h, ImVec2* out) {
    vec4 c = {0.0f, 0.0f, 0.0f, 0.0f};
    glm_mat4_mulv((vec4*)view_proj, (vec4){p[0], p[1], p[2], 1.0f}, c);
    if (c[3] <= 0.05f)
        return false;
    out->x = (c[0] / c[3] * 0.5f + 0.5f) * w;
    out->y = (0.5f - c[1] / c[3] * 0.5f) * h;
    return true;
}

static ImU32 rgba(int r, int g, int b, int a) {
    return (ImU32)((a << 24) | (b << 16) | (g << 8) | r);
}

static void overlay(CatMind* m, Engine* engine) {
    Camera* cam = engine->camera;
    if (!cam)
        return;
    mat4 view, proj, vp;
    camera_view_matrix(cam, view);
    camera_projection_matrix(cam, proj);
    glm_mat4_mul(proj, view, vp);
    const ImGuiIO* io = igGetIO_Nil();
    const float w = io->DisplaySize.x, h = io->DisplaySize.y;
    ImDrawList* dl = igGetBackgroundDrawList(igGetMainViewport());
    const NavGraph* g = m->cat->places;
    // By kind: walks, flights, jumps, and the rail.
    const ImU32 colours[] = {rgba(120, 200, 255, 140), rgba(255, 200, 80, 160),
                             rgba(255, 110, 200, 160), rgba(160, 255, 140, 160)};
    for (int i = 0; i < g->link_count; i++) {
        const NavLink* l = &g->links[i];
        const ImU32 c = colours[l->kind < 4 ? l->kind : 0];
        vec3 a, b;
        ImVec2 sa, sb;
        for (int k = 0; k < 6; k++) {
            nav_link_point(g, i, (float)k / 6.0f, a);
            nav_link_point(g, i, (float)(k + 1) / 6.0f, b);
            if (to_screen(vp, a, w, h, &sa) && to_screen(vp, b, w, h, &sb))
                ImDrawList_AddLine(dl, sa, sb, c, 1.0f);
        }
    }
    // The route ahead, thick.
    const NavFollower* f = &m->cat->follower;
    if (!f->arrived) {
        for (int r = f->leg; r < f->route.count; r++) {
            vec3 a, b;
            ImVec2 sa, sb;
            for (int k = 0; k < 6; k++) {
                nav_link_point(g, f->route.links[r], (float)k / 6.0f, a);
                nav_link_point(g, f->route.links[r], (float)(k + 1) / 6.0f, b);
                if (to_screen(vp, a, w, h, &sa) && to_screen(vp, b, w, h, &sb))
                    ImDrawList_AddLine(dl, sa, sb, rgba(255, 255, 255, 230), 3.0f);
            }
        }
    }
    for (int i = 0; i < g->node_count; i++) {
        ImVec2 s;
        if (to_screen(vp, g->nodes[i].position, w, h, &s)) {
            ImDrawList_AddCircle(dl, s, 3.0f, rgba(255, 255, 255, 160), 8, 1.0f);
            ImDrawList_AddText_Vec2(dl, (ImVec2){s.x + 4.0f, s.y - 6.0f}, rgba(220, 220, 220, 160),
                                    g->nodes[i].name, NULL);
        }
    }
    // What it sees, and what it watches.
    vec3 eye = {0.0f, 0.0f, 0.0f}, fwd = {0.0f, 0.0f, 0.0f};
    ImVec2 se, sp;
    if (cat_eye(m->cat, eye, fwd) && to_screen(vp, eye, w, h, &se)) {
        if (to_screen(vp, m->player_eye, w, h, &sp))
            ImDrawList_AddLine(dl, se, sp,
                               m->sees ? rgba(80, 255, 80, 200) : rgba(255, 70, 70, 120), 1.5f);
        if (m->gaze != CAT_GAZE_NONE && to_screen(vp, m->gaze_at, w, h, &sp))
            ImDrawList_AddCircle(dl, sp, 8.0f, rgba(255, 220, 60, 230), 12, 2.0f);
    }
}

void cat_mind_panel(CatMind* m, Engine* engine) {
    if (!m->brain || !engine->show_gui)
        return;
    overlay(m, engine);
    igSetNextWindowPos((ImVec2){15, 300}, ImGuiCond_FirstUseEver, (ImVec2){0, 0});
    igSetNextWindowSize((ImVec2){320, 520}, ImGuiCond_FirstUseEver);
    if (igBegin("Cat", NULL, 0)) {
        Brain* b = m->brain;
        igText("doing: %s (%.0f s)", brain_current_name(b) ? brain_current_name(b) : "-",
               (double)b->current_seconds);
        for (int i = 0; i < b->activity_count; i++) {
            char label[64];
            snprintf(label, sizeof(label), "%s %.2f%s", b->activities[i].name, (double)b->scores[i],
                     b->cooldowns[i] > 0.0f ? " (resting)" : "");
            igProgressBar(clamp01(b->scores[i]), (ImVec2){-1.0f, 0.0f}, label);
        }
        static int force = 0;
        igCombo_Str_arr("##activity", &force, ACTIVITY_NAMES, ACT_COUNT, 10);
        igSameLine(0.0f, -1.0f);
        if (igButton("Start", (ImVec2){0, 0}))
            brain_start(b, force);
        igText("energy %.2f  boredom %.2f", (double)m->energy, (double)m->boredom);
        igText("alarm %.2f  affinity %.2f", (double)m->alarm, (double)m->affinity);
        igText("player %.1f m, %.1f m/s, closing %.1f", (double)m->distance,
               (double)m->player_speed, (double)m->closing);
        igText("%s%s, out of sight %.0f s", m->sees ? "sees" : "does not see",
               m->hears ? ", hears" : "", (double)m->unseen);
        static int place = 0;
        static const char* names[CAT_MIND_PLACES];
        for (int i = 0; i < CAT_PLACE_COUNT && i < CAT_MIND_PLACES; i++)
            names[i] = CAT_PLACES[i].name;
        igCombo_Str_arr("##place", &place, names, CAT_PLACE_COUNT, 12);
        igSameLine(0.0f, -1.0f);
        if (igButton("Go", (ImVec2){0, 0}))
            cat_go(m->cat, CAT_PLACES[place].name, CAT_WALK, -1);
        igCheckbox("blind", &m->blind);
        if (m->cat->fur && igColorEdit3("fur", m->cat->fur_srgb, 0))
            cat_set_fur(m->cat, m->cat->fur_srgb);
        if (m->cat->eye && igColorEdit3("eyes", m->cat->eyes_srgb, 0))
            cat_set_eyes(m->cat, m->cat->eyes_srgb);
    }
    igEnd();
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
