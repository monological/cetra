#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cetra/program.h"

#include "hud.h"
#include "map_screen.h"
#include "silent_shaders.h"

// The paper's most of the window, at the print's own aspect.
#define PAPER_W   0.94f
#define PAPER_H   0.84f
#define DIM       0.6f // the world behind it
#define HINT_SIZE 14.0f

#define ZOOM_MAX   2.0f   // past this a 2048-pixel print magnifies soft on a Retina window
#define ZOOM_RATE  1.5f   // the zoom's factor a second at a full trigger
#define WHEEL_STEP 1.12f  // the zoom's factor a notch
#define PAN_RATE   900.0f // window points a second at a full stick

// The first mark begins a moment after the map is up, and each the next a little after the last.
#define WRITE_DELAY 0.6
#define WRITE_GAP   0.35

#define WHOLE 1e3f // a focus past any mark's writing: the mark whole

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

// The print as the paper at `r` shows it: print pixel p lands at r's corner + (p - corner) * s.
typedef struct View {
    UIRect r;
    float s;
    float corner[2]; // the print pixel at r's top left
} View;

static View view_of(const MapScreen* ms, UIRect r) {
    View v = {.r = r, .s = view_scale(ms, r)};
    float c[2];
    view_centre(ms, r, c);
    v.corner[0] = c[0] - 0.5f * r.w / v.s;
    v.corner[1] = c[1] - 0.5f * r.h / v.s;
    return v;
}

// The window rect of the print-pixel box w x h whose top left is at print pixel (x, y).
static UIRect on_paper(const View* v, float x, float y, float w, float h) {
    return (UIRect){v->r.x + (x - v->corner[0]) * v->s, v->r.y + (y - v->corner[1]) * v->s,
                    w * v->s, h * v->s};
}

// The sheet as the view sees it, then each mark written so far and the arrow, all through one
// view and clipped to the paper.
static void paper_draw(UIElement* el, UIDrawList* dl, void* user) {
    MapScreen* ms = user;
    const MapArt* art = &MAP_ART[ms->map];
    const View v = view_of(ms, el->rect);
    const float pw = art->print_size[0], ph = art->print_size[1];
    const float x1 = v.corner[0] + v.r.w / v.s, y1 = v.corner[1] + v.r.h / v.s;
    ui_push_clip(dl, v.r);
    ui_draw_textured_quad_uv(dl, v.r, ms->print[ms->map],
                             (const float[4]){v.corner[0] / pw, v.corner[1] / ph, x1 / pw, y1 / ph},
                             (float*)WHITE);
    // The marks picture holds only where the ink is and when; its colours are the map's.
    Texture* marks = ms->marks[ms->map];
    vec4 blue = {art->ink[0], art->ink[1], art->ink[2], 1.0f};
    vec4 red = {art->arrow_ink[0], art->arrow_ink[1], art->arrow_ink[2], 1.0f};
    if (marks && ms->ink) {
        for (int i = 0; i < art->mark_count; i++) {
            const MapMark* m = &art->marks[i];
            const double written = ms->clock - ms->began[m->place];
            if (written < 0.0)
                continue;
            float p[2];
            to_print(art, PLACES[m->place].mark[0], PLACES[m->place].mark[1], p);
            const UIRect q =
                on_paper(&v, p[0] - m->anchor[0], p[1] - m->anchor[1], m->size[0], m->size[1]);
            ui_set_draw_program(dl, ms->ink, v.r, fminf((float)(written / m->seconds), WHOLE));
            ui_draw_textured_quad_uv(dl, q, marks, m->uv, blue);
        }
        // The arrow's frame nearest the player's facing, its middle on their feet.
        const int k = (int)lroundf(ms->heading / (2.0f * GLM_PIf) * (float)art->arrow_frames);
        const int frame = ((k % art->arrow_frames) + art->arrow_frames) % art->arrow_frames;
        const int col = frame % art->arrow_cols, row = frame / art->arrow_cols;
        const float cell = (float)art->arrow_cell;
        const float u = cell * (float)col, t = cell * (float)row;
        const float mw = art->marks_size[0], mh = art->marks_size[1];
        float p[2];
        to_print(art, ms->feet[0], ms->feet[1], p);
        const UIRect q = on_paper(&v, p[0] - 0.5f * cell, p[1] - 0.5f * cell, cell, cell);
        ui_set_draw_program(dl, ms->ink, v.r, WHOLE);
        ui_draw_textured_quad_uv(
            dl, q, marks, (const float[4]){u / mw, t / mh, (u + cell) / mw, (t + cell) / mh}, red);
        ui_set_draw_program(dl, NULL, v.r, 0.0f);
    }
    ui_pop_clip(dl);
}

static void got_picture(Texture* tex, void* user) {
    *(Texture**)user = tex;
}

void map_screen_start(MapScreen* ms, UISystem* ui, Engine* engine, TexturePool* pool) {
    memset(ms, 0, sizeof(*ms));
    ms->map = MAP_NONE;
    ms->zoom = 1.0f;
    for (int i = 0; i < PLACE_COUNT; i++)
        ms->began[i] = INFINITY;
    if (!ui || !engine || !pool)
        return;
    ms->engine = engine;
    ms->ui = ui;
    // UI pictures: top row first, and in display values, as the UI draws. Decoded on the loader's
    // threads, which have them long before anyone can open the map.
    for (int i = 0; i < MAP_COUNT; i++) {
        engine_load_texture(engine, pool, MAP_ART[i].print_file, texture_desc(false), got_picture,
                            &ms->print[i]);
        engine_load_texture(engine, pool, MAP_ART[i].marks_file, texture_desc(false), got_picture,
                            &ms->marks[i]);
    }
    ms->ink = create_ui_draw_program("map_ink", map_ink_frag_shader_str);
    if (ms->ink)
        engine_add_program(engine, ms->ink);
    else
        fprintf(stderr, "silent: the map's ink program did not build; the map shows no marks\n");

    UIElement* root = NULL;
    ms->screen = hud_modal_screen(ui, "map", DIM, &root);
    root->spacing = 10.0f;
    ms->paper = ui_panel(root);
    for (int i = 0; i < 4; i++)
        ms->paper->padding[i] = HUD_BARE;
    ui_set_draw(ms->paper, paper_draw, ms);
    UIElement* hint = ui_label(root, "M  CLOSE        DRAG  MOVE        WHEEL  ZOOM");
    const UIStyle h = {
        .fg = {HINT[0], HINT[1], HINT[2], HINT[3]}, .font_size = HINT_SIZE, .tracking = 2.5f};
    ui_set_style(hint, &h);
}

bool map_screen_open(const MapScreen* ms) {
    return ms->screen && ui_top(ms->ui) == ms->screen;
}

void map_screen_show(MapScreen* ms, MapId map, const bool found[PLACE_COUNT]) {
    if (!ms->screen || map <= MAP_NONE || map >= MAP_COUNT || map_screen_open(ms))
        return;
    if (!ms->print[map])
        engine_finish_texture_loads(ms->engine);
    if (!ms->print[map])
        return;
    // A mark whose writing finished while the map was last open is whole from now on; one the
    // closing cut off is written again.
    if (ms->map != MAP_NONE) {
        const MapArt* last = &MAP_ART[ms->map];
        for (int i = 0; i < last->mark_count; i++) {
            double* began = &ms->began[last->marks[i].place];
            if (isfinite(*began) && ms->clock >= *began + (double)last->marks[i].seconds)
                *began = -INFINITY;
        }
    }
    ms->map = map;
    ms->zoom = 1.0f;
    to_print(&MAP_ART[map], ms->feet[0], ms->feet[1], ms->centre);
    ms->dragging = false;
    ms->clock = 0.0;
    double at = WRITE_DELAY;
    for (int i = 0; i < MAP_ART[map].mark_count; i++) {
        const MapMark* m = &MAP_ART[map].marks[i];
        double* began = &ms->began[m->place];
        if (*began == -INFINITY)
            continue;
        const bool due = PLACES[m->place].known || (found && found[m->place]);
        *began = due ? at : INFINITY;
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
    if (map_screen_open(ms))
        ms->clock += dt;
}

void map_screen_free(MapScreen* ms) {
    memset(ms, 0, sizeof(*ms));
}
