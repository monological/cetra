#include "cat_debug.h"

#include <stdio.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include "cimgui.h"

#include "cetra/camera.h"

#include "cat_places.h"

// What the panel's two pickers hold between frames.
static int g_activity;
static int g_place;

typedef struct Screen {
    mat4 view_proj;
    float w, h;
    ImDrawList* list;
} Screen;

static bool to_screen(const Screen* s, const vec3 p, ImVec2* out) {
    vec4 c = {0.0f, 0.0f, 0.0f, 0.0f};
    glm_mat4_mulv((vec4*)s->view_proj, (vec4){p[0], p[1], p[2], 1.0f}, c);
    if (c[3] <= 0.05f)
        return false;
    out->x = (c[0] / c[3] * 0.5f + 0.5f) * s->w;
    out->y = (0.5f - c[1] / c[3] * 0.5f) * s->h;
    return true;
}

static ImU32 rgba(int r, int g, int b, int a) {
    return (ImU32)((a << 24) | (b << 16) | (g << 8) | r);
}

// A link along its profile, in six pieces so a flight and a jump read as their shapes.
static void draw_link(const Screen* s, const NavGraph* g, int link, ImU32 colour, float width) {
    for (int k = 0; k < 6; k++) {
        vec3 a = {0.0f, 0.0f, 0.0f}, b = {0.0f, 0.0f, 0.0f};
        ImVec2 sa, sb;
        nav_link_point(g, link, (float)k / 6.0f, a);
        nav_link_point(g, link, (float)(k + 1) / 6.0f, b);
        if (to_screen(s, a, &sa) && to_screen(s, b, &sb))
            ImDrawList_AddLine(s->list, sa, sb, colour, width);
    }
}

static void overlay(const Cat* cat, const CatMind* mind, Engine* engine) {
    Camera* cam = engine->camera;
    if (!cam)
        return;
    Screen s = {.list = igGetBackgroundDrawList(igGetMainViewport())};
    mat4 view = GLM_MAT4_IDENTITY_INIT, proj = GLM_MAT4_IDENTITY_INIT;
    camera_view_matrix(cam, view);
    camera_projection_matrix(cam, proj);
    glm_mat4_mul(proj, view, s.view_proj);
    const ImGuiIO* io = igGetIO_Nil();
    s.w = io->DisplaySize.x;
    s.h = io->DisplaySize.y;

    const NavGraph* g = cat->places;
    static const int KIND_COLOURS[CAT_LINK_KINDS][4] = {
        [CAT_LINK_WALK] = {120, 200, 255, 140},
        [CAT_LINK_STAIR] = {255, 200, 80, 160},
        [CAT_LINK_JUMP] = {255, 110, 200, 160},
        [CAT_LINK_RAIL] = {160, 255, 140, 160},
    };
    for (int i = 0; i < g->link_count; i++) {
        const int* c = KIND_COLOURS[g->links[i].kind];
        draw_link(&s, g, i, rgba(c[0], c[1], c[2], c[3]), 1.0f);
    }
    // The route ahead, thick.
    const NavFollower* f = &cat->follower;
    for (int r = f->arrived ? f->route.count : f->leg; r < f->route.count; r++)
        draw_link(&s, g, f->route.links[r], rgba(255, 255, 255, 230), 3.0f);
    for (int i = 0; i < g->node_count; i++) {
        ImVec2 at;
        if (to_screen(&s, g->nodes[i].position, &at)) {
            ImDrawList_AddCircle(s.list, at, 3.0f, rgba(255, 255, 255, 160), 8, 1.0f);
            ImDrawList_AddText_Vec2(s.list, (ImVec2){at.x + 4.0f, at.y - 6.0f},
                                    rgba(220, 220, 220, 160), g->nodes[i].name, NULL);
        }
    }
    // What it sees, and what it watches.
    vec3 eye = {0.0f, 0.0f, 0.0f}, forward = {0.0f, 0.0f, 0.0f};
    ImVec2 se, sp;
    if (!mind->brain || !cat_eye(cat, eye, forward) || !to_screen(&s, eye, &se))
        return;
    if (to_screen(&s, mind->player_eye, &sp))
        ImDrawList_AddLine(s.list, se, sp,
                           mind->sees ? rgba(80, 255, 80, 200) : rgba(255, 70, 70, 120), 1.5f);
    if (mind->gaze != CAT_GAZE_NONE && to_screen(&s, mind->gaze_at, &sp))
        ImDrawList_AddCircle(s.list, sp, 8.0f, rgba(255, 220, 60, 230), 12, 2.0f);
}

static void mind_panel(CatMind* mind) {
    Brain* b = mind->brain;
    igText("doing: %s (%.0f s)", brain_current_name(b) ? brain_current_name(b) : "-",
           (double)b->current_seconds);
    const char* names[BRAIN_MAX_ACTIVITIES];
    for (int i = 0; i < b->activity_count; i++) {
        names[i] = b->activities[i].name;
        char label[64];
        snprintf(label, sizeof(label), "%s %.2f%s", names[i], (double)b->scores[i],
                 b->cooldowns[i] > 0.0f ? " (resting)" : "");
        igProgressBar(glm_clamp_zo(b->scores[i]), (ImVec2){-1.0f, 0.0f}, label);
    }
    if (b->activity_count > 0) {
        igCombo_Str_arr("##activity", &g_activity, names, b->activity_count, 10);
        igSameLine(0.0f, -1.0f);
        if (igButton("Start", (ImVec2){0, 0}))
            brain_start(b, g_activity);
    }
    igText("energy %.2f  boredom %.2f", (double)mind->energy, (double)mind->boredom);
    igText("alarm %.2f  affinity %.2f", (double)mind->alarm, (double)mind->affinity);
    igText("player %.1f m, %.1f m/s, closing %.1f", (double)mind->distance,
           (double)mind->player_speed, (double)mind->closing);
    igText("%s%s, out of sight %.0f s", mind->sees ? "sees" : "does not see",
           mind->hears ? ", hears" : "", (double)mind->unseen);
    igCheckbox("blind", &mind->blind);
}

void cat_debug_draw(Cat* cat, CatMind* mind, Engine* engine) {
    if (!cat->entity || !engine->show_gui)
        return;
    overlay(cat, mind, engine);
    igSetNextWindowPos((ImVec2){15, 300}, ImGuiCond_FirstUseEver, (ImVec2){0, 0});
    igSetNextWindowSize((ImVec2){320, 520}, ImGuiCond_FirstUseEver);
    if (igBegin("Cat", NULL, 0)) {
        if (mind->brain)
            mind_panel(mind);
        static const char* places[CAT_PLACE_COUNT];
        if (!places[0])
            for (int i = 0; i < CAT_PLACE_COUNT; i++)
                places[i] = CAT_PLACES[i].name;
        igCombo_Str_arr("##place", &g_place, places, CAT_PLACE_COUNT, 12);
        igSameLine(0.0f, -1.0f);
        if (igButton("Go", (ImVec2){0, 0}))
            cat_go(cat, (CatPlaceId)g_place, CAT_WALK, -1);
        if (cat->fur && igColorEdit3("fur", cat->fur_srgb, 0))
            cat_set_fur(cat, cat->fur_srgb);
        if (cat->eye && igColorEdit3("eyes", cat->eyes_srgb, 0))
            cat_set_eyes(cat, cat->eyes_srgb);
    }
    igEnd();
}
