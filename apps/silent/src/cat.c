#include "cat.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cetra/animation.h"
#include "cetra/engine.h"
#include "cetra/import.h"
#include "cetra/probe_set.h"
#include "cetra/program.h"

#include "cetra/game/animator_component.h"

#include "cat_clips.h"
#include "cat_places.h"

#define CAT_MODEL "assets/models/cat.glb"
#define CAT_HOME  "study_chair"

// The body the player bumps into: half extents across, up and along, standing. Its top, at
// twice the half height, is above the player's step, so the player cannot climb onto the cat.
static const vec3 CAT_BOX = {0.07f, 0.16f, 0.20f};

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

// The player in the way: nearer than this ahead of it, the cat stops and waits, and after
// WAIT_REPLAN it looks for another way round.
#define GIVE_WAY    0.55f
#define WAIT_REPLAN 3.0f

static float wrap_angle(float a) {
    while (a > GLM_PIf)
        a -= 2.0f * GLM_PIf;
    while (a < -GLM_PIf)
        a += 2.0f * GLM_PIf;
    return a;
}

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

// The coat, the values assets/scenes/cat.cscn gives the render app: twenty shells over the
// body, combed back and down and lying close; a shorter fringe standing up inside the ears.
typedef struct Coat {
    int layers;
    float length, density, thickness, root_shade, clump, lie;
    vec3 comb;
} Coat;
static const Coat BODY_COAT = {20,    0.011f, 900.0f, 0.75f,
                               0.28f, 0.85f,  0.75f,  {0.0f, -0.5f, -1.0f}};
static const Coat EAR_COAT = {10, 0.007f, 1100.0f, 0.4f, 0.7f, 0.6f, 0.3f, {0.0f, 1.0f, 0.0f}};

// The eyeshine. What the flashlight puts on the eye, in lux, times this, is how bright the
// eye glows back, and no brighter than the cap; it rises and falls over EASE. The eye throws
// the light back the way it came, so only a viewer near the beam sees it, and the falloff
// with the angle between the cat's gaze and the viewer is this power of its cosine.
#define SHINE_GAIN  2.5f
#define SHINE_MAX   600.0f // nits
#define SHINE_EASE  0.08f  // seconds
#define SHINE_POWER 6.0f

// A fur this light or lighter, by luminance, has a pink nose and pink inside its ears.
#define PINK_ABOVE 0.15f

static float srgb_to_linear(float c) {
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static void linear(const vec3 srgb, vec3 out) {
    for (int i = 0; i < 3; i++)
        out[i] = srgb_to_linear(srgb[i]);
}

static void dress(Material* m, const Coat* coat) {
    m->fur_layers = coat->layers;
    m->fur_length = coat->length;
    m->fur_density = coat->density;
    m->fur_thickness = coat->thickness;
    m->fur_root_shade = coat->root_shade;
    m->fur_clump = coat->clump;
    m->fur_lie = coat->lie;
    glm_vec3_copy((float*)coat->comb, m->fur_comb);
}

// A material of silent's own standing in for an imported one: every value the import gave it,
// then the cat's coat; its colours are cat_set_fur's and cat_set_eyes'. Its own because a
// material belongs to the scene that first registers it, and the import's belong to the
// model's scene, which is never drawn.
static Material* own_material(const Material* imported) {
    Material* m = create_material();
    if (!m)
        return NULL;
    for (size_t i = 0; i < MATERIAL_PARAM_COUNT; i++) {
        const MaterialParam* p = &MATERIAL_PARAMS[i];
        if (p->type == MATERIAL_PARAM_TEXTURE)
            continue;
        float v[3];
        material_param_get(imported, p, v);
        material_param_set(m, p, v);
    }
    m->alpha_mode = imported->alpha_mode;
    m->alphaCutoff = imported->alphaCutoff;
    m->doubleSided = imported->doubleSided;
    const char* name = imported->name ? imported->name : "";
    m->name = strdup(name);
    if (!strcmp(name, "cat_fur")) {
        dress(m, &BODY_COAT);
    } else if (!strcmp(name, "cat_eye")) {
        // Its glow is the eyeshine, which lights nothing: no panel is derived from it.
        m->emissive_strength = 0.0f;
        m->emissive_light = 1;
    } else if (!strcmp(name, "cat_ear")) {
        dress(m, &EAR_COAT);
    }
    return m;
}

static void own_materials(SceneNode* node, Cat* cat) {
    for (size_t i = 0; i < node->mesh_count; i++) {
        Mesh* mesh = node->meshes[i];
        Material* m = mesh->material ? own_material(mesh->material) : NULL;
        if (!m)
            continue;
        mesh->material = m;
        const char* name = m->name ? m->name : "";
        if (!strcmp(name, "cat_eye")) {
            cat->eye = m;
        } else if (!strcmp(name, "cat_fur")) {
            cat->fur = m;
        } else if (!strcmp(name, "cat_ear")) {
            cat->ear = m;
            glm_vec3_copy(m->albedo, cat->ear_dark);
        } else if (!strcmp(name, "cat_nose")) {
            cat->nose = m;
            glm_vec3_copy(m->albedo, cat->nose_dark);
        }
        if (mesh->is_skinned && !cat->skin)
            cat->skin = node;
    }
    for (size_t i = 0; i < node->children_count; i++)
        own_materials(node->children[i], cat);
}

void cat_set_fur(Cat* cat, const vec3 srgb) {
    glm_vec3_copy((float*)srgb, cat->fur_srgb);
    vec3 fur = {0.0f, 0.0f, 0.0f};
    linear(srgb, fur);
    if (cat->fur)
        glm_vec3_copy(fur, cat->fur->albedo);
    const bool pink = 0.2126f * fur[0] + 0.7152f * fur[1] + 0.0722f * fur[2] > PINK_ABOVE;
    if (cat->ear) {
        if (pink)
            linear((vec3){0.82f, 0.6f, 0.6f}, cat->ear->albedo);
        else
            glm_vec3_copy(cat->ear_dark, cat->ear->albedo);
    }
    if (cat->nose) {
        if (pink)
            linear((vec3){0.79f, 0.55f, 0.55f}, cat->nose->albedo);
        else
            glm_vec3_copy(cat->nose_dark, cat->nose->albedo);
    }
}

void cat_set_eyes(Cat* cat, const vec3 srgb) {
    glm_vec3_copy((float*)srgb, cat->eyes_srgb);
    if (!cat->eye)
        return;
    linear(srgb, cat->eye->albedo);
    glm_vec3_copy(cat->eye->albedo, cat->eye->emissive);
}

// The model as a node that is not its scene's root, which cannot be moved under one of its
// own descendants: the root's children go into a wrapper carrying the root's own transform.
static SceneNode* take_model(const Scene* library) {
    SceneNode* root = library->root_node;
    SceneNode* wrapper = create_node();
    if (!wrapper)
        return NULL;
    node_set_name(wrapper, "cat_model");
    glm_mat4_copy(root->original_transform, wrapper->original_transform);
    // Bounded rather than drained: node_add_child refuses a cycle, and a while on a refusal
    // would spin forever.
    for (size_t guard = root->children_count; guard > 0 && root->children_count > 0; guard--)
        node_add_child(wrapper, root->children[0]);
    return wrapper;
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

// Whether the lids are down: asleep, and the moment either side of it.
static bool eyes_shut(const Cat* cat) {
    return cat->clip == CAT_CLIP_SLEEP ||
           (cat->clip == CAT_CLIP_CURL_UP && cat->clip_seconds > 1.2f) ||
           (cat->clip == CAT_CLIP_UNCURL && cat->clip_seconds < 0.4f);
}

bool cat_eyes_shut(const Cat* cat) {
    return cat->entity && eyes_shut(cat);
}

// ---------------------------------------------------------------------------------------------
// Going from place to place

static void next_leg(Cat* cat);
static void begin_act(Cat* cat, int clip);

// Up or down its postures, a transition at a time, to `want`, then holding it -- or, standing
// with an act waiting for it, playing that.
static void shift_to(Cat* cat, CatPosture want) {
    cat->want = want;
    if (cat->posture == want) {
        if (want == CAT_STAND && cat->act_pending >= 0) {
            begin_act(cat, cat->act_pending);
            return;
        }
        play(cat, HOLD[want], 0.25f);
        cat->mode = CAT_RESTING;
        if (want == CAT_STAND && cat->goal >= 0)
            next_leg(cat);
        return;
    }
    cat->mode = CAT_SHIFTING;
    cat->shift_dir = want > cat->posture ? 1 : -1;
    play(cat, cat->shift_dir > 0 ? UP[cat->posture] : DOWN[cat->posture], 0.15f);
}

// The transition under way is finished, whichever way `want` has turned since it began.
static void step_shift(Cat* cat) {
    if (!played(cat))
        return;
    cat->posture = (CatPosture)((int)cat->posture + cat->shift_dir);
    shift_to(cat, cat->want);
}

// ---------------------------------------------------------------------------------------------
// In place

static bool looping_act(int clip) {
    return clip >= 0 && CAT_CLIPS[clip].looping;
}

static void begin_act(Cat* cat, int clip) {
    cat->act_pending = -1;
    cat->act = clip;
    cat->act_ends = clip == CAT_CLIP_GROOM ? CAT_SIT : CAT_STAND;
    cat->mode = CAT_ACTING;
    play(cat, clip, clip == CAT_CLIP_STARTLE ? 0.08f : 0.25f);
}

// Out of an act into the posture it leaves the cat in, and on if it has been sent somewhere.
static void end_act(Cat* cat) {
    cat->act = -1;
    cat->posture = cat->want = cat->act_ends;
    shift_to(cat, cat->goal >= 0 ? CAT_STAND : cat->posture);
}

bool cat_act(Cat* cat, int clip) {
    if (!cat->entity || !cat->attached || cat->held || clip < 0 || clip >= CAT_CLIP_COUNT)
        return false;
    const bool travelling =
        cat->mode == CAT_WALKING || cat->mode == CAT_TURNING || cat->mode == CAT_JUMPING;
    if (clip == CAT_CLIP_STARTLE) {
        // Anywhere it has its feet under it on the level: not in the air, on the stair, or
        // part way through a quarter turn.
        NavSample s;
        nav_follower_sample(&cat->follower, &s);
        if (cat->mode == CAT_JUMPING || (cat->mode == CAT_TURNING && cat->turn_clip) ||
            (cat->mode == CAT_WALKING && s.kind != CAT_LINK_WALK))
            return false;
        cat->goal = -1;
        cat->posture = cat->want = CAT_STAND;
        begin_act(cat, clip);
        return true;
    }
    if (travelling || cat->act_pending >= 0)
        return false;
    if (clip == CAT_CLIP_GROOM) {
        if (cat->posture != CAT_SIT || (cat->mode != CAT_RESTING && cat->act != CAT_CLIP_GROOM))
            return false;
        begin_act(cat, clip);
        return true;
    }
    if (clip == CAT_CLIP_HISS) {
        if (cat->posture != CAT_STAND && cat->act != CAT_CLIP_STARTLE)
            return false;
        cat->posture = cat->want = CAT_STAND;
        begin_act(cat, clip);
        return true;
    }
    if (clip == CAT_CLIP_STRETCH) {
        if (cat->mode == CAT_ACTING)
            return false;
        cat->act_pending = clip;
        shift_to(cat, CAT_STAND);
        return true;
    }
    return false;
}

static void step_act(Cat* cat) {
    if (!looping_act(cat->act) && played(cat))
        end_act(cat);
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
    if (cat->vocal >= 0 && cat->vocal_seconds < CAT_CLIPS[cat->vocal].seconds)
        return false;
    float mask[MAX_BONES];
    vocal_mask(cat, clip, mask);
    animator_play_layer(cat->animator, cat->clips[clip], mask, 0.06f, 0.1f, false);
    cat->vocal = clip;
    cat->vocal_seconds = 0.0f;
    return true;
}

void cat_look(Cat* cat, const vec3 world) {
    cat->look_on = world != NULL;
    if (world)
        glm_vec3_copy((float*)world, cat->look_target);
}

bool cat_settled(const Cat* cat) {
    return cat->entity && cat->goal < 0 && cat->mode == CAT_RESTING && cat->act_pending < 0;
}

// A turn on the spot toward `face`: a quarter-turn clip while much is left, easing round once
// little is.
static void turn_to(Cat* cat, float face) {
    cat->face = face;
    cat->mode = CAT_TURNING;
    const float left = wrap_angle(face - cat->yaw);
    cat->turn_clip = fabsf(left) > TURN_BY_CLIP;
    play(cat,
         cat->turn_clip ? (left > 0.0f ? CAT_CLIP_TURN_L90 : CAT_CLIP_TURN_R90) : CAT_CLIP_IDLE,
         0.15f);
}

// What it takes up on arriving: what it was told to, or what the place says.
static CatPosture arrival_posture(const Cat* cat) {
    return cat->settle >= 0 ? (CatPosture)cat->settle : resting(CAT_PLACES[cat->at].rest);
}

// Arrived where it was going: round to face the way it rests there, and down into its posture.
static void arrive(Cat* cat) {
    cat->at = cat->follower.at;
    cat->goal = -1;
    const CatPlace* p = &CAT_PLACES[cat->at];
    if (p->rest != CAT_REST_NONE && fabsf(wrap_angle(p->yaw - cat->yaw)) > 0.05f) {
        turn_to(cat, p->yaw);
        return;
    }
    shift_to(cat, arrival_posture(cat));
}

static float link_heading(const Cat* cat, int link) {
    const NavLink* l = &cat->places->links[link];
    const float* a = cat->places->nodes[l->from].position;
    const float* b = cat->places->nodes[l->to].position;
    return atan2f(b[0] - a[0], b[2] - a[2]);
}

// Off up a jump's arc: the clip played at the rate that makes the time between its takeoff
// and its landing the arc's own time of flight.
static void jump(Cat* cat, int link) {
    const NavLink* l = &cat->places->links[link];
    const float y0 = cat->places->nodes[l->from].position[1];
    const float y1 = cat->places->nodes[l->to].position[1];
    const float top = fmaxf(y0, y1) + l->shape.apex;
    const float flight = sqrtf(2.0f * (top - y0) / GRAVITY) + sqrtf(2.0f * (top - y1) / GRAVITY);
    const int clip = y1 > y0 ? CAT_CLIP_JUMP_UP : CAT_CLIP_JUMP_DOWN;
    const float air = event_at(clip, "land") - event_at(clip, "takeoff");
    play(cat, clip, 0.1f);
    cat->animator->speed = flight > 0.0f ? air / flight : 1.0f;
    cat->mode = CAT_JUMPING;
    cat->landed = false;
    cat->leg = link;
    // It turns in the air to land facing the way it goes on, within a quarter turn and a bit
    // of the way it jumped -- onto the hand rail that is the only place to turn at all.
    const float heading = link_heading(cat, link);
    const NavFollower* f = &cat->follower;
    cat->jump_yaw0 = cat->yaw;
    cat->jump_yaw1 = heading;
    if (f->leg + 1 < f->route.count) {
        const float next = link_heading(cat, f->route.links[f->leg + 1]);
        if (fabsf(wrap_angle(next - heading)) <= TURN_IN_AIR)
            cat->jump_yaw1 = next;
    }
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
    const int kind = l->kind;
    const float heading = link_heading(cat, s.link);
    const float off = fabsf(wrap_angle(heading - cat->yaw));
    // Nothing turns on the spot on a beam: along it the way is straight ahead, and off it the
    // turn is made in the air.
    const bool beam = (cat->places->nodes[l->from].tags & CAT_TAG_BEAM) != 0;
    const float square = beam ? GLM_PIf : kind == CAT_LINK_WALK ? TURN_ON_SPOT : SQUARE;
    if (off > square) {
        turn_to(cat, heading);
        return;
    }
    cat->leg = s.link;
    if (kind == CAT_LINK_JUMP) {
        jump(cat, s.link);
        return;
    }
    cat->mode = CAT_WALKING;
    const bool up = cat->places->nodes[l->to].position[1] > cat->places->nodes[l->from].position[1];
    static const int GAIT[] = {
        [CAT_WALK] = CAT_CLIP_WALK, [CAT_TROT] = CAT_CLIP_TROT, [CAT_RUN] = CAT_CLIP_RUN};
    play(cat,
         kind == CAT_LINK_STAIR  ? (up ? CAT_CLIP_STAIR_UP : CAT_CLIP_STAIR_DOWN)
         : kind == CAT_LINK_RAIL ? CAT_CLIP_BEAM_WALK
                                 : GAIT[cat->gait],
         0.2f);
}

static void step_turn(Cat* cat, float turned, float dt) {
    cat->yaw = wrap_angle(cat->yaw + turned);
    if (cat->turn_clip) {
        if (played(cat))
            turn_to(cat, cat->face);
        return;
    }
    const float left = wrap_angle(cat->face - cat->yaw);
    const float most = EASE_ROUND * dt;
    if (fabsf(left) > most) {
        cat->yaw = wrap_angle(cat->yaw + (left > 0.0f ? most : -most));
        return;
    }
    cat->yaw = cat->face;
    if (cat->follower.arrived)
        shift_to(cat, arrival_posture(cat));
    else
        next_leg(cat);
}

// Whether the player is standing in the way, close ahead along its path.
static bool in_the_way(const Cat* cat, const vec3 player, const NavSample* s) {
    const float dx = player[0] - s->position[0], dz = player[2] - s->position[2];
    const float d = sqrtf(dx * dx + dz * dz);
    if (d > GIVE_WAY || fabsf(player[1] - s->position[1]) > 1.0f)
        return false;
    return d < 1e-3f || (dx * s->tangent[0] + dz * s->tangent[2]) / d > 0.3f;
}

static void step_walk(Cat* cat, float moved, const vec3 player, float dt) {
    NavSample s;
    nav_follower_sample(&cat->follower, &s);
    if (s.kind == CAT_LINK_WALK && in_the_way(cat, player, &s)) {
        cat->blocked += dt;
        play(cat, CAT_CLIP_IDLE, 0.2f);
        if (cat->blocked > WAIT_REPLAN) {
            cat->blocked = 0.0f;
            nav_follower_replan(&cat->follower, cat->follower.goal);
        }
        return;
    }
    if (cat->blocked > 0.0f) {
        cat->blocked = 0.0f;
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
    cat->yaw =
        wrap_angle(cat->yaw + wrap_angle(s.heading - cat->yaw) * (1.0f - expf(-dt * WALK_STEER)));
}

static void step_jump(Cat* cat) {
    const float takeoff = event_at(cat->clip, "takeoff"), land = event_at(cat->clip, "land");
    if (!cat->landed) {
        const float u = (cat->clip_seconds - takeoff) / (land - takeoff);
        if (u >= 1.0f) {
            nav_follower_set_progress(&cat->follower, 1.0f);
            cat->landed = true;
        } else {
            nav_follower_set_progress(&cat->follower, fmaxf(u, 0.0f));
        }
        const float turned = glm_smoothstep(0.0f, 1.0f, glm_clamp(u, 0.0f, 1.0f));
        cat->yaw =
            wrap_angle(cat->jump_yaw0 + turned * wrap_angle(cat->jump_yaw1 - cat->jump_yaw0));
    }
    if (played(cat))
        next_leg(cat);
}

static bool go_with(Cat* cat, const char* place, CatGait gait, int settle, NavQuery query);

bool cat_go(Cat* cat, const char* place, CatGait gait, int settle) {
    return go_with(cat, place, gait, settle, cat_places_query(false));
}

bool cat_go_by_rail(Cat* cat, const char* place, int settle) {
    return go_with(cat, place, CAT_WALK, settle, cat_places_query(true));
}

static bool go_with(Cat* cat, const char* place, CatGait gait, int settle, NavQuery query) {
    if (!cat->entity || cat->held)
        return false;
    const int goal = nav_graph_find(cat->places, place);
    if (goal < 0) {
        fprintf(stderr, "silent: the cat knows no place called '%s'\n", place);
        return false;
    }
    cat->settle = settle;
    // Already there: only the posture changes.
    if (cat->follower.arrived && goal == cat->at && cat->mode != CAT_TURNING) {
        cat->goal = -1;
        if (cat->mode == CAT_ACTING && looping_act(cat->act)) {
            cat->act = -1;
            cat->posture = cat->want = cat->act_ends;
            cat->mode = CAT_RESTING;
        }
        if (cat->mode == CAT_RESTING)
            shift_to(cat, arrival_posture(cat));
        else if (cat->mode == CAT_SHIFTING)
            cat->want = arrival_posture(cat);
        return true;
    }
    // From a place it starts afresh; from part way along a link it replans from there, under
    // the new query, keeping the old one if there is no way.
    const NavQuery was = cat->follower.query;
    if (!cat->follower.arrived)
        cat->follower.query = query;
    if (cat->follower.arrived
            ? !nav_follower_start(&cat->follower, cat->places, cat->at, goal, &query)
            : !nav_follower_replan(&cat->follower, goal)) {
        cat->follower.query = was;
        fprintf(stderr, "silent: the cat has no way from %s to %s\n", CAT_PLACES[cat->at].name,
                place);
        return false;
    }
    cat->goal = goal;
    cat->gait = gait;
    cat->leg = -1; // whatever plays, the next link's clip is the gait's
    if (cat->mode == CAT_ACTING && looping_act(cat->act)) {
        cat->act = -1;
        cat->posture = cat->want = cat->act_ends;
        cat->mode = CAT_RESTING;
    }
    if (cat->mode == CAT_RESTING)
        shift_to(cat, CAT_STAND);
    else if (cat->mode == CAT_SHIFTING)
        cat->want = CAT_STAND;
    return true;
}

void cat_step(Cat* cat, const vec3 player, float dt) {
    if (!cat->entity || !cat->attached || cat->held)
        return;
    cat->clip_seconds += dt * cat->animator->speed;
    cat->vocal_seconds += dt;
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
    cat->entity->position[1] += CAT_BOX[1];
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
           (double)(p[1] - CAT_BOX[1]), (double)p[2], (double)glm_deg(cat->yaw), MODES[cat->mode],
           CAT_CLIPS[cat->clip].name, link, cat->goal >= 0 ? " to " : "",
           cat->goal >= 0 ? CAT_PLACES[cat->goal].name : "");
}

// ---------------------------------------------------------------------------------------------

bool cat_create(Cat* cat, const CatDesc* desc, Game* game, Scene* scene, PhysicsWorld* physics) {
    memset(cat, 0, sizeof(*cat));
    cat->eye_bones[0] = cat->eye_bones[1] = -1;
    cat->goal = -1;
    cat->leg = -1;
    cat->settle = -1;
    cat->act = cat->act_pending = cat->vocal = -1;
    Engine* engine = game->engine;
    (void)scene;

    cat->places = cat_places_build();
    if (!cat->places)
        return false;
    cat_places_check(cat->places, physics);
    cat->at = nav_graph_find(cat->places, desc->at ? desc->at : CAT_HOME);
    if (cat->at < 0) {
        fprintf(stderr, "silent: no place called '%s' for the cat (%s); home instead\n", desc->at,
                cat_place_list());
        cat->at = nav_graph_find(cat->places, CAT_HOME);
    }
    const CatPlace* place = &CAT_PLACES[cat->at];
    nav_follower_start(&cat->follower, cat->places, cat->at, cat->at, NULL);
    cat->posture = cat->want = resting(place->rest);
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

    // The model's own scene, owned by the engine from here and never drawn: the game draws
    // only its own. The model carries no textures, so nothing is left in flight on the loader.
    Scene* library = create_scene_from_model_path(CAT_MODEL, NULL, engine->async_loader);
    if (!library || library->skeleton_count == 0) {
        fprintf(stderr, "silent: cannot load the cat from %s\n", CAT_MODEL);
        if (library)
            free_scene(library);
        return false;
    }
    engine_add_scene(engine, library);
    SceneNode* model = take_model(library);
    if (!model)
        return false;
    own_materials(model, cat);
    cat_set_fur(cat, desc->fur);
    cat_set_eyes(cat, desc->eyes);
    ShaderProgram* skinned = engine_find_program(engine, CETRA_PROGRAM_PBR_SKINNED);
    if (!skinned) {
        skinned = create_pbr_skinned_program();
        if (skinned)
            engine_add_program(engine, skinned);
    }
    node_set_programs(model, engine_get_program(engine, CETRA_PROGRAM_PBR), skinned);

    // The entity stands at its body's middle, so the model hangs below it to the feet.
    cat->entity = create_entity(game->entity_manager, "cat");
    if (!cat->entity)
        return false;
    glm_vec3_copy((float*)place->feet, cat->entity->position);
    cat->entity->position[1] += CAT_BOX[1];
    glm_quatv(cat->entity->rotation, place->yaw, (vec3){0.0f, 1.0f, 0.0f});
    cat->holder = create_node();
    node_set_name(cat->holder, "cat");
    SceneNode* inner = create_node();
    node_set_name(inner, "cat_rig");
    node_add_child(cat->holder, inner);
    node_add_child(inner, model);
    glm_translate_make(inner->original_transform, (vec3){0.0f, -CAT_BOX[1], 0.0f});
    cat->entity->node = cat->holder;

    PhysicsShapeDesc box = {.type = SHAPE_BOX,
                            .box.half_extents = {CAT_BOX[0], CAT_BOX[1], CAT_BOX[2]},
                            .density = 0.0f};
    entity_add_rigid_body(cat->entity, physics, &box, MOTION_KINEMATIC, OBJ_LAYER_KINEMATIC);

    Skeleton* skeleton = library->skeletons[0];
    cat->animator = entity_add_animator(cat->entity, create_animator(skeleton));
    if (!cat->animator) {
        cat->entity = NULL;
        return false;
    }
    // The walks, the flights and the turns state where they take the body, and it goes there.
    cat->animator->root_motion = true;
    for (int i = 0; i < CAT_CLIP_COUNT; i++) {
        cat->clips[i] = scene_find_animation(library, CAT_CLIPS[i].name);
        if (!cat->clips[i])
            fprintf(stderr, "silent: the cat's model has no clip '%s'\n", CAT_CLIPS[i].name);
    }
    cat->clip = -1;
    play(cat, clip, 0.0f);
    if (cat->held && desc->clip_seconds >= 0.0f) {
        animator_update(cat->animator, desc->clip_seconds);
        cat->animator->speed = 0.0f;
    }
    cat->eye_bones[0] = get_bone_index_by_name(skeleton, "Eye.L");
    cat->eye_bones[1] = get_bone_index_by_name(skeleton, "Eye.R");
    cat->eyeshine = desc->eyeshine && cat->eye && cat->skin;

    // The head turns on the neck: the upper neck takes a share and the head the rest, so a
    // glance over the shoulder bends the neck rather than screwing the head round on it.
    LookAtSystem* look = create_look_at_system(skeleton);
    if (look && look_at_add_bone(look, "Neck2", 0.4f) && look_at_add_bone(look, "Head", 0.6f)) {
        glm_vec3_copy((vec3){0.0f, CAT_EYE_Y, CAT_EYE_Z}, look->eye);
        look->blend_rate = 3.0f;
        cat->animator->state->look_at = look;
        cat->look = look;
    } else {
        free_look_at_system(look);
    }

    printf("silent: the cat at %s, %s\n", place->name, CAT_CLIPS[clip].name);
    cat->go = desc->go;
    cat->go_gait = desc->gait;
    return true;
}

// Where one eye is in the world, and which way it looks, from the pose being drawn.
static bool eye_world(const Cat* cat, int side, vec3 at, vec3 gaze) {
    const int bone = cat->eye_bones[side];
    if (bone < 0 || !cat->animator)
        return false;
    mat4 m;
    glm_mat4_mul(cat->skin->global_transform, cat->animator->state->bone_matrices[bone], m);
    const float x = side == 0 ? CAT_EYE_X : -CAT_EYE_X;
    vec3 ahead;
    glm_mat4_mulv3(m, (vec3){x, CAT_EYE_Y, CAT_EYE_Z}, 1.0f, at);
    glm_mat4_mulv3(m, (vec3){x, CAT_EYE_Y, CAT_EYE_Z + 0.02f}, 1.0f, ahead);
    glm_vec3_sub(ahead, at, gaze);
    glm_vec3_normalize(gaze);
    return true;
}

// How bright the eyes glow now, in nits: what the flashlight throws on them, seen back along
// the beam, unless a wall stands between them or the lids are shut.
static float shine_now(const Cat* cat, Game* game, const Lights* lights, const vec3 viewer) {
    if (!cat->eyeshine || !cat->attached || eyes_shut(cat) || !lights || !lights->flashlight ||
        !lights->flashlight_on)
        return 0.0f;
    vec3 at = {0.0f, 0.0f, 0.0f}, gaze = {0.0f, 0.0f, 0.0f};
    for (int side = 0; side < 2; side++) {
        vec3 a = {0.0f, 0.0f, 0.0f}, g = {0.0f, 0.0f, 0.0f};
        if (!eye_world(cat, side, a, g))
            return 0.0f;
        glm_vec3_muladds(a, 0.5f, at);
        glm_vec3_muladds(g, 0.5f, gaze);
    }
    glm_vec3_normalize(gaze);
    const Light* f = lights->flashlight;
    vec3 beam;
    glm_vec3_sub(at, (float*)f->global_position, beam);
    const float d = glm_vec3_norm(beam);
    if (d < 1e-3f)
        return 0.0f;
    glm_vec3_scale(beam, 1.0f / d, beam);
    vec3 aim;
    glm_vec3_copy((float*)f->direction, aim);
    const float cone = glm_smoothstep(f->outerCutOff, f->cutOff, glm_vec3_dot(beam, aim));
    if (cone <= 0.0f)
        return 0.0f;
    RaycastHit hit;
    if (game->physics_world &&
        physics_world_raycast_filtered(game->physics_world, (float*)f->global_position, beam,
                                       d - 0.05f, 1u << OBJ_LAYER_STATIC, &hit) &&
        hit.hit)
        return 0.0f;
    vec3 back;
    glm_vec3_sub((float*)viewer, at, back);
    glm_vec3_normalize(back);
    const float facing = powf(fmaxf(0.0f, glm_vec3_dot(gaze, back)), SHINE_POWER);
    const float lux = f->intensity * cone / (d * d);
    return fminf(SHINE_MAX, SHINE_GAIN * lux * facing);
}

bool cat_eye(const Cat* cat, vec3 at, vec3 forward) {
    if (!cat->entity || !cat->skin)
        return false;
    glm_vec3_zero(at);
    glm_vec3_zero(forward);
    for (int side = 0; side < 2; side++) {
        vec3 a = {0.0f, 0.0f, 0.0f}, g = {0.0f, 0.0f, 0.0f};
        if (!eye_world(cat, side, a, g))
            return false;
        glm_vec3_muladds(a, 0.5f, at);
        glm_vec3_muladds(g, 0.5f, forward);
    }
    glm_vec3_normalize(forward);
    return true;
}

void cat_feet(const Cat* cat, vec3 out) {
    glm_vec3_copy((float*)cat->entity->position, out);
    out[1] -= CAT_BOX[1];
}

// Where the rig's model space is in the world now: the entity's pose this step, carried down
// the fixed chain of nodes between it and the skin -- not the skin's global, which is the last
// frame's until the walk after this hook.
static void rig_to_world(const Cat* cat, mat4 out) {
    mat4 chain;
    glm_mat4_identity(chain);
    for (const SceneNode* n = cat->skin; n && n != cat->holder; n = n->parent)
        glm_mat4_mul((vec4*)n->original_transform, chain, chain);
    mat4 entity;
    entity_get_transform_matrix(cat->entity, entity);
    glm_mat4_mul(entity, chain, out);
}

// Whether the head is its own to turn: not asleep, in the air, through a quarter turn, or
// busy in a stretch, a startle or grooming.
static bool head_free(const Cat* cat) {
    if (eyes_shut(cat) || cat->mode == CAT_JUMPING || (cat->mode == CAT_TURNING && cat->turn_clip))
        return false;
    if (cat->mode == CAT_ACTING && cat->act != CAT_CLIP_HISS)
        return false;
    return cat->posture != CAT_CURL;
}

void cat_update(Cat* cat, Game* game, Scene* scene, const Lights* lights, const vec3 viewer,
                float dt) {
    if (!cat->entity)
        return;
    // Into the scene once the reflection probes have their pictures. They capture once, and a
    // cat in a room then would be in that room's reflections for good, asleep on a chair it
    // has long since left.
    if (!cat->attached && game->engine->total_frames > 2 &&
        (!scene->probe_set || scene->probe_set->ready)) {
        node_add_child(scene->root_node, cat->holder);
        cat->attached = true;
        // Sent somewhere from the command line: it sets off once it can be seen to.
        if (cat->go)
            go_with(cat, cat->go, cat->go_gait, -1, cat_places_query(true));
    }
    const float target = shine_now(cat, game, lights, viewer);
    cat->shine += (target - cat->shine) * (1.0f - expf(-dt / SHINE_EASE));
    if (cat->eye)
        cat->eye->emissive_strength = cat->shine;

    if (cat->look) {
        mat4 world = GLM_MAT4_IDENTITY_INIT;
        rig_to_world(cat, world);
        look_at_set_world(cat->look, world);
        if (cat->look_on && head_free(cat))
            look_at_set_target(cat->look, cat->look_target);
        else
            look_at_clear_target(cat->look);
    }
}
