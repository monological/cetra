#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cetra/program.h"

#include "map_screen.h"
#include "silent_shaders.h"

// The paper's most of the window, at the print's own aspect.
#define PAPER_W   0.94f
#define PAPER_H   0.84f
#define HINT_SIZE 14.0f
#define FADE_IN   0.15f
#define BARE      0.01f // a padding of nothing: a zero is the style's

#define ZOOM_MAX   2.0f   // past this a 2048-pixel print magnifies soft on a Retina window
#define ZOOM_RATE  1.5f   // the zoom's factor a second at a full trigger
#define WHEEL_STEP 1.12f  // the zoom's factor a notch
#define PAN_RATE   900.0f // window points a second at a full stick

// The first mark begins a moment after the map is up, and each the next a little after the last.
#define WRITE_DELAY 0.6
#define WRITE_GAP   0.35

#define WHOLE 1e3f // a focus past any mark's writing: the mark whole

// The ink's colours, the marks picture holding only where the ink is and when.
static const vec4 BLUE = {30.0f / 255.0f, 52.0f / 255.0f, 138.0f / 255.0f, 1.0f};
static const vec4 RED = {170.0f / 255.0f, 32.0f / 255.0f, 28.0f / 255.0f, 1.0f};
static const vec4 HINT = {0.93f, 0.87f, 0.74f, 0.75f};
static const vec4 WHITE = {1.0f, 1.0f, 1.0f, 1.0f};

// Where a world point lands on a map's print.
static void to_print(const MapArt* art, float x, float z, float out[2]) {
    out[0] = art->at[0] + (x - art->origin[0]) * art->px_per_m;
    out[1] = art->at[1] + (z - art->origin[1]) * art->px_per_m;
}

// Window points to a print pixel for the paper at `r`.
static float view_scale(const MapScreen* ms, UIRect r) {
    const MapArt* art = &MAP_ART[ms->map];
    return fminf(r.w / art->print_size[0], r.h / art->print_size[1]) * ms->zoom;
}

// The view's middle in print pixels: the centre held so the view never leaves the sheet.
static void view_centre(const MapScreen* ms, UIRect r, float out[2]) {
    const MapArt* art = &MAP_ART[ms->map];
    const float s = view_scale(ms, r);
    for (int k = 0; k < 2; k++) {
        const float half = 0.5f * (k ? r.h : r.w) / s, size = art->print_size[k];
        out[k] = half >= 0.5f * size ? 0.5f * size : glm_clamp(ms->centre[k], half, size - half);
    }
}

// How far through its writing a found place's mark is, or below zero when it is not drawn.
static float mark_focus(const MapScreen* ms, PlaceId place, float seconds) {
    if (ms->shown[place])
        return WHOLE;
    if (ms->writing[place] < 0.0 || ms->clock < ms->writing[place])
        return -1.0f;
    return (float)((ms->clock - ms->writing[place]) / (double)seconds);
}

// The sheet as the view sees it, then each mark written so far and the arrow, all through one
// transform and clipped to the paper.
static void paper_draw(UIElement* el, UIDrawList* dl, void* user) {
    MapScreen* ms = user;
    if (ms->map == MAP_NONE || !ms->print[ms->map])
        return;
    const MapArt* art = &MAP_ART[ms->map];
    const UIRect r = el->rect;
    const float s = view_scale(ms, r);
    float c[2];
    view_centre(ms, r, c);
    const float x0 = c[0] - 0.5f * r.w / s, y0 = c[1] - 0.5f * r.h / s;
    const float pw = art->print_size[0], ph = art->print_size[1];
    ui_push_clip(dl, r);
    ui_draw_textured_quad_uv(
        dl, r, ms->print[ms->map],
        (const float[4]){x0 / pw, y0 / ph, (x0 + r.w / s) / pw, (y0 + r.h / s) / ph},
        (float*)WHITE);
    Texture* marks = ms->marks[ms->map];
    if (marks && ms->ink) {
        for (int i = 0; i < art->mark_count; i++) {
            const MapMark* m = &art->marks[i];
            const float focus = mark_focus(ms, m->place, m->seconds);
            if (focus < 0.0f)
                continue;
            float p[2];
            to_print(art, PLACES[m->place].mark[0], PLACES[m->place].mark[1], p);
            const UIRect q = {r.x + (p[0] - m->anchor[0] - x0) * s,
                              r.y + (p[1] - m->anchor[1] - y0) * s, m->size[0] * s, m->size[1] * s};
            ui_set_draw_program(dl, ms->ink, q, focus);
            ui_draw_textured_quad_uv(dl, q, marks, m->uv, (float*)BLUE);
        }
        // The arrow's frame nearest the player's facing, its middle on their feet.
        const int k = (int)lroundf(ms->heading / (2.0f * GLM_PIf) * (float)art->arrow_frames);
        const int frame = ((k % art->arrow_frames) + art->arrow_frames) % art->arrow_frames;
        const float cell = (float)art->arrow_cell;
        const int col = frame % art->arrow_cols, row = frame / art->arrow_cols;
        const float u = cell * (float)col, v = cell * (float)row;
        const float mw = art->marks_size[0], mh = art->marks_size[1];
        float p[2];
        to_print(art, ms->feet[0], ms->feet[1], p);
        const UIRect q = {r.x + (p[0] - 0.5f * cell - x0) * s, r.y + (p[1] - 0.5f * cell - y0) * s,
                          cell * s, cell * s};
        ui_set_draw_program(dl, ms->ink, q, WHOLE);
        ui_draw_textured_quad_uv(dl, q, marks,
                                 (const float[4]){u / mw, v / mh, (u + cell) / mw, (v + cell) / mh},
                                 (float*)RED);
        ui_set_draw_program(dl, NULL, r, 0.0f);
    }
    ui_pop_clip(dl);
}

bool map_screen_start(MapScreen* ms, UISystem* ui, Engine* engine, TexturePool* pool) {
    memset(ms, 0, sizeof(*ms));
    ms->map = MAP_NONE;
    ms->zoom = 1.0f;
    for (int i = 0; i < PLACE_COUNT; i++)
        ms->writing[i] = -1.0;
    if (!ui || !engine || !pool)
        return false;
    // UI pictures: top row first, and in display values, as the UI draws.
    for (int i = 0; i < MAP_COUNT; i++) {
        ms->print[i] = texture_load_file(pool, MAP_ART[i].print_file, texture_desc(false));
        ms->marks[i] = texture_load_file(pool, MAP_ART[i].marks_file, texture_desc(false));
    }
    ms->ink = create_program_from_source("map_ink", map_ink_vert_shader_str,
                                         map_ink_frag_shader_str, NULL);
    if (ms->ink)
        engine_add_program(engine, ms->ink);
    else
        fprintf(stderr, "silent: the map's ink program did not build; the map shows no marks\n");
    ms->ui = ui;

    ms->screen = ui_screen(ui, "map");
    ui_screen_set_modal(ms->screen, true);
    ui_screen_transition(ms->screen, UI_TRANSITION_FADE, FADE_IN);
    UIElement* root = ui_screen_root(ms->screen);
    root->align_main = UI_ALIGN_CENTER;
    root->align_cross = UI_ALIGN_CENTER;
    root->spacing = 10.0f;
    // The world dimmed behind it, out of the flow.
    UIElement* backdrop = ui_panel(root);
    backdrop->fill = true;
    const UIStyle dim = {.bg = {0.0f, 0.0f, 0.0f, 0.6f}, .corner_radius = BARE};
    ui_set_style(backdrop, &dim);

    ms->paper = ui_panel(root);
    for (int i = 0; i < 4; i++)
        ms->paper->padding[i] = BARE;
    ui_set_draw(ms->paper, paper_draw, ms);
    UIElement* hint = ui_label(root, "M  CLOSE        DRAG  MOVE        WHEEL  ZOOM");
    const UIStyle h = {
        .fg = {HINT[0], HINT[1], HINT[2], HINT[3]}, .font_size = HINT_SIZE, .tracking = 2.5f};
    ui_set_style(hint, &h);
    return true;
}

bool map_screen_open(const MapScreen* ms) {
    return ms->screen && ui_top(ms->ui) == ms->screen;
}

void map_screen_show(MapScreen* ms, MapId map, bool over_bag, const bool found[PLACE_COUNT]) {
    if (!ms->screen || map <= MAP_NONE || map >= MAP_COUNT || !ms->print[map] ||
        map_screen_open(ms))
        return;
    ms->map = map;
    ms->over_bag = over_bag;
    ms->zoom = 1.0f;
    to_print(&MAP_ART[map], ms->feet[0], ms->feet[1], ms->centre);
    ms->dragging = false;
    ms->clock = 0.0;
    double at = WRITE_DELAY;
    for (int i = 0; i < MAP_ART[map].mark_count; i++) {
        const MapMark* m = &MAP_ART[map].marks[i];
        const bool due = found && found[m->place] && !ms->shown[m->place];
        ms->writing[m->place] = due ? at : -1.0;
        at += due ? (double)m->seconds + WRITE_GAP : 0.0;
    }
    ui_push(ms->ui, ms->screen);
}

void map_screen_layout(MapScreen* ms, float width, float height) {
    if (!ms->screen || ms->map == MAP_NONE)
        return;
    const MapArt* art = &MAP_ART[ms->map];
    const float k =
        fminf(PAPER_W * width / art->print_size[0], PAPER_H * height / art->print_size[1]);
    ui_set_size(ms->paper, UI_FIXED, k * art->print_size[0], UI_FIXED, k * art->print_size[1]);
}

void map_screen_input(MapScreen* ms, const UIInput* in, const MapControls* controls, float dt) {
    if (!map_screen_open(ms))
        return;
    const UIRect r = ms->paper->rect;
    // Held to the sheet first, so a pan from an edge moves at once rather than first unwinding
    // how far past the edge the centre had been asked to go.
    view_centre(ms, r, ms->centre);
    const float zoom0 = ms->zoom;
    float factor = powf(ZOOM_RATE, controls->zoom * dt);
    if (controls->wheel != 0.0f)
        factor *= powf(WHEEL_STEP, controls->wheel);
    ms->zoom = glm_clamp(ms->zoom * factor, 1.0f, ZOOM_MAX);
    // Zooming from the whole sheet goes in toward the arrow.
    if (zoom0 <= 1.0f && ms->zoom > 1.0f)
        to_print(&MAP_ART[ms->map], ms->feet[0], ms->feet[1], ms->centre);
    const float s = view_scale(ms, r);
    ms->centre[0] += controls->pan[0] * PAN_RATE * dt / s;
    ms->centre[1] += controls->pan[1] * PAN_RATE * dt / s;
    if (in->pointer_pressed && ui_rect_hit(r, in->pointer_x, in->pointer_y)) {
        ms->dragging = true;
        ms->drag_from[0] = in->pointer_x;
        ms->drag_from[1] = in->pointer_y;
    }
    if (ms->dragging && in->pointer_down) {
        ms->centre[0] -= (in->pointer_x - ms->drag_from[0]) / s;
        ms->centre[1] -= (in->pointer_y - ms->drag_from[1]) / s;
        ms->drag_from[0] = in->pointer_x;
        ms->drag_from[1] = in->pointer_y;
    } else {
        ms->dragging = false;
    }
}

void map_screen_update(MapScreen* ms, float dt, const vec3 feet, const vec3 forward) {
    ms->feet[0] = feet[0];
    ms->feet[1] = feet[2];
    ms->heading = atan2f(forward[0], -forward[2]);
    if (!map_screen_open(ms))
        return;
    ms->clock += dt;
    // A mark once written is whole from then on.
    for (int i = 0; i < MAP_ART[ms->map].mark_count; i++) {
        const MapMark* m = &MAP_ART[ms->map].marks[i];
        if (ms->writing[m->place] >= 0.0 &&
            ms->clock >= ms->writing[m->place] + (double)m->seconds) {
            ms->shown[m->place] = true;
            ms->writing[m->place] = -1.0;
        }
    }
}

void map_screen_free(MapScreen* ms) {
    memset(ms, 0, sizeof(*ms));
}
