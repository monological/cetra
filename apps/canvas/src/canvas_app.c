/*
 * The 2D canvas's own app (spec 14.0): test sheets drawn through canvas.h, in a
 * window to pan and zoom, or headless for a picture.
 *
 * A sheet is laid out in millimetres and drawn in nanometres, the units pcb
 * gives the canvas, so every length here goes through MM().
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include "cetra/camera.h"
#include "cetra/canvas.h"
#include "cetra/engine.h"
#include "cetra/scene.h"

#define MM(v) ((int64_t)llround((v) * 1e6))

static Canvas* g_canvas;
static CanvasView g_view;
static bool g_panning;
static float g_last_x, g_last_y; // the cursor, window points

/*
 * The shapes sheet: one row of each kind, at sizes from a hair to a pad.
 */
static void sheet_shapes(Canvas* canvas, CanvasEdit* edit, int64_t offset) {
    CanvasLayer* copper = canvas_add_layer(canvas, "copper");
    CanvasLayer* silk = canvas_add_layer(canvas, "silk");
    const uint16_t orange = canvas_add_style(canvas, 0xc8873affu);
    const uint16_t white = canvas_add_style(canvas, 0xe6e6e6ffu);
    const uint16_t blue = canvas_add_style(canvas, 0x4f8fd8ffu);
    const uint16_t green = canvas_add_style(canvas, 0x58b368ffu);
    const CanvasPaint cu = {.layer = copper, .style = orange};
    const CanvasPaint sk = {.layer = silk, .style = white};
#define AT(x, y) ((CanvasPoint){MM(x) + offset, MM(y) + offset})

    // Segments: widths from 0.02 mm to 1 mm, round ends, then flat.
    const double widths[] = {0.02, 0.05, 0.1, 0.25, 0.5, 1.0};
    for (int i = 0; i < 6; i++) {
        const double x = -10.0 + i * 2.2;
        canvas_edit_segment(
            edit, &(CanvasSegment){
                      .paint = cu, .a = AT(x, 5.0), .b = AT(x + 1.4, 7.0), .width = MM(widths[i])});
        canvas_edit_segment(edit, &(CanvasSegment){.paint = cu,
                                                   .a = AT(x + 0.8, 5.0),
                                                   .b = AT(x + 1.6, 5.6),
                                                   .width = MM(widths[i]),
                                                   .flat_ends = true});
    }

    // Arcs: a quarter, a major arc, a clockwise one, flat ends, a half turn.
    const CanvasPaint bl = {.layer = copper, .style = blue};
    canvas_edit_arc(edit, &(CanvasArc){.paint = bl,
                                       .a = AT(-9.0, 1.5),
                                       .b = AT(-10.0, 2.5),
                                       .radius = MM(1.0),
                                       .width = MM(0.25)});
    canvas_edit_arc(edit, &(CanvasArc){.paint = bl,
                                       .a = AT(-6.5, 1.5),
                                       .b = AT(-7.5, 2.5),
                                       .radius = MM(1.0),
                                       .width = MM(0.25),
                                       .major = true});
    canvas_edit_arc(edit, &(CanvasArc){.paint = bl,
                                       .a = AT(-3.5, 1.5),
                                       .b = AT(-4.5, 2.5),
                                       .radius = MM(1.0),
                                       .width = MM(0.25),
                                       .clockwise = true});
    canvas_edit_arc(edit, &(CanvasArc){.paint = bl,
                                       .a = AT(-1.0, 1.5),
                                       .b = AT(-2.0, 2.5),
                                       .radius = MM(1.0),
                                       .width = MM(0.4),
                                       .flat_ends = true});
    canvas_edit_arc(edit, &(CanvasArc){.paint = bl,
                                       .a = AT(1.0, 2.0),
                                       .b = AT(3.0, 2.0),
                                       .radius = MM(1.0),
                                       .width = MM(0.1)});
    canvas_edit_arc(edit, &(CanvasArc){.paint = bl,
                                       .a = AT(4.0, 2.0),
                                       .b = AT(6.0, 2.0),
                                       .radius = MM(1.0),
                                       .width = MM(0.1),
                                       .clockwise = true});

    // Circles: filled, rings, and one a hair across.
    const CanvasPaint gr = {.layer = copper, .style = green};
    canvas_edit_circle(edit,
                       &(CanvasCircle){.paint = gr, .centre = AT(-9.5, -1.0), .radius = MM(0.5)});
    canvas_edit_circle(edit,
                       &(CanvasCircle){.paint = gr, .centre = AT(-7.0, -1.0), .radius = MM(1.0)});
    canvas_edit_circle(
        edit, &(CanvasCircle){
                  .paint = gr, .centre = AT(-4.0, -1.0), .radius = MM(1.0), .stroke = MM(0.1)});
    canvas_edit_circle(
        edit, &(CanvasCircle){
                  .paint = gr, .centre = AT(-1.0, -1.0), .radius = MM(1.0), .stroke = MM(0.4)});
    canvas_edit_circle(edit,
                       &(CanvasCircle){.paint = gr, .centre = AT(1.0, -1.0), .radius = MM(0.02)});

    // Boxes: a pad, a turned one, an outline, a stadium, a thin one at 45 degrees.
    canvas_edit_box(edit, &(CanvasBox){.paint = cu,
                                       .centre = AT(-9.5, -4.5),
                                       .size_x = MM(1.0),
                                       .size_y = MM(0.6),
                                       .corner = MM(0.15)});
    canvas_edit_box(edit, &(CanvasBox){.paint = cu,
                                       .centre = AT(-7.0, -4.5),
                                       .size_x = MM(1.6),
                                       .size_y = MM(0.8),
                                       .corner = MM(0.1),
                                       .angle = 0.5235987755982988});
    canvas_edit_box(edit, &(CanvasBox){.paint = cu,
                                       .centre = AT(-4.0, -4.5),
                                       .size_x = MM(2.0),
                                       .size_y = MM(1.2),
                                       .corner = MM(0.3),
                                       .stroke = MM(0.1)});
    canvas_edit_box(edit, &(CanvasBox){.paint = cu,
                                       .centre = AT(-1.0, -4.5),
                                       .size_x = MM(2.0),
                                       .size_y = MM(0.8),
                                       .corner = MM(0.4)});
    canvas_edit_box(edit, &(CanvasBox){.paint = cu,
                                       .centre = AT(2.0, -4.5),
                                       .size_x = MM(2.4),
                                       .size_y = MM(0.3),
                                       .angle = 0.7853981633974483});

    // A resistor's zigzag and a closed square, in silk over the copper.
    const CanvasPoint zig[] = {AT(4.0, -1.0), AT(4.5, -0.4), AT(5.0, -1.6), AT(5.5, -0.4),
                               AT(6.0, -1.6), AT(6.5, -0.4), AT(7.0, -1.0)};
    canvas_edit_polyline(
        edit, &(CanvasPolyline){.paint = sk, .points = zig, .count = 7, .width = MM(0.15)});
    const CanvasPoint square[] = {AT(4.5, -5.5), AT(6.5, -5.5), AT(6.5, -3.5), AT(4.5, -3.5)};
    canvas_edit_polyline(
        edit, &(CanvasPolyline){
                  .paint = sk, .points = square, .count = 4, .width = MM(0.1), .closed = true});
    // Silk across a copper pad, to show the order layers draw in.
    canvas_edit_segment(
        edit, &(CanvasSegment){
                  .paint = sk, .a = AT(-10.5, -4.5), .b = AT(-8.5, -4.5), .width = MM(0.12)});
#undef AT
}

typedef struct Sheet {
    const char* name;
    void (*build)(Canvas* canvas, CanvasEdit* edit, int64_t offset);
    double centre_mm[2]; // where a view of it starts
    double width_mm;     // how much of it the window's width shows
} Sheet;

static const Sheet SHEETS[] = {
    {"shapes", sheet_shapes, {-2.0, 0.5}, 24.0},
};

static const Sheet* find_sheet(const char* name) {
    for (size_t i = 0; i < sizeof(SHEETS) / sizeof(SHEETS[0]); i++)
        if (strcmp(SHEETS[i].name, name) == 0)
            return &SHEETS[i];
    return NULL;
}

/*
 * The window.
 */

static void draw_view(Engine* engine, void* user) {
    (void)user;
    g_view.width = (float)engine->win_width;
    g_view.height = (float)engine->win_height;
    canvas_draw(g_canvas, &g_view);
}

static void to_points(const Engine* engine, double fb_x, double fb_y, float* x, float* y) {
    *x = (float)(fb_x * engine->win_width / engine->fb_width);
    *y = (float)((engine->fb_height - fb_y) * engine->win_height / engine->fb_height);
}

static void on_cursor(Engine* engine, double fb_x, double fb_y) {
    float x, y;
    to_points(engine, fb_x, fb_y, &x, &y);
    if (g_panning)
        canvas_view_pan(&g_view, x - g_last_x, y - g_last_y);
    g_last_x = x, g_last_y = y;
}

static void on_button(Engine* engine, int button, int action, int mods) {
    (void)engine;
    (void)mods;
    if (button == GLFW_MOUSE_BUTTON_LEFT)
        g_panning = action == GLFW_PRESS;
}

static void on_scroll(Engine* engine, double xoffset, double yoffset) {
    (void)engine;
    (void)xoffset;
    canvas_view_zoom_at(&g_view, g_last_x, g_last_y, pow(1.15, yoffset));
}

static void usage(const char* argv0) {
    fprintf(stderr,
            "usage: %s [-x] [-f frames] [-S out.ppm] [-W points] [-H points]\n"
            "          [--sheet NAME] [--view X,Y,WIDTH (mm)] [--offset UNITS] [--mirror]\n"
            "          [--tile-shift N]\n",
            argv0);
}

int main(int argc, char** argv) {
    bool headless = false, mirror = false;
    int frames = 0, width = 960, height = 600, tile_shift = 0;
    const char* screenshot = NULL;
    const char* sheet_name = "shapes";
    double view_x = NAN, view_y = NAN, view_w = NAN;
    long long offset = 0;
    for (int i = 1; i < argc; i++) {
        const bool more = i + 1 < argc;
        if (strcmp(argv[i], "-x") == 0) {
            headless = true;
        } else if (strcmp(argv[i], "-f") == 0 && more) {
            frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-S") == 0 && more) {
            screenshot = argv[++i];
        } else if (strcmp(argv[i], "-W") == 0 && more) {
            width = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-H") == 0 && more) {
            height = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--sheet") == 0 && more) {
            sheet_name = argv[++i];
        } else if (strcmp(argv[i], "--view") == 0 && more) {
            if (sscanf(argv[++i], "%lf,%lf,%lf", &view_x, &view_y, &view_w) != 3) {
                usage(argv[0]);
                return 2;
            }
        } else if (strcmp(argv[i], "--offset") == 0 && more) {
            offset = atoll(argv[++i]);
        } else if (strcmp(argv[i], "--mirror") == 0) {
            mirror = true;
        } else if (strcmp(argv[i], "--tile-shift") == 0 && more) {
            tile_shift = atoi(argv[++i]);
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    const Sheet* sheet = find_sheet(sheet_name);
    if (!sheet) {
        fprintf(stderr, "canvas: no sheet named '%s'\n", sheet_name);
        return 2;
    }

    EngineConfig cfg = {.title = "canvas", .width = width, .height = height, .headless = headless};
    Engine* engine = create_engine(&cfg);
    if (!engine)
        return 1;
    engine->exit_after_frames = frames;
    engine_set_screenshot_path(engine, screenshot);
    // An empty scene and a camera on it: the canvas draws over the frame, and
    // the frame wants a scene to be the frame of.
    Scene* scene = create_scene();
    engine_add_scene(engine, scene);
    engine_set_camera(engine, create_camera(&(CameraDesc){.position = {0.0f, 0.0f, 1.0f}}));
    engine_set_2d_preset(engine, scene);
    glm_vec3_zero(engine->clear_color);

    g_canvas = create_canvas(engine, &(CanvasDesc){.tile_shift = tile_shift});
    if (!g_canvas) {
        free_engine(engine);
        return 1;
    }
    CanvasEdit* edit = create_canvas_edit(g_canvas);
    sheet->build(g_canvas, edit, (int64_t)offset);
    canvas_apply(g_canvas, edit);
    free_canvas_edit(edit);

    const double cx = isnan(view_x) ? sheet->centre_mm[0] : view_x;
    const double cy = isnan(view_y) ? sheet->centre_mm[1] : view_y;
    const double cw = isnan(view_w) ? sheet->width_mm : view_w;
    g_view = (CanvasView){.width = (float)width,
                          .height = (float)height,
                          .points_per_unit = (double)width / (cw * 1e6),
                          .mirror_x = mirror,
                          .background = 0x14181eff};
    // The centre in whole units plus its fraction, from millimetres and the offset.
    const double ux = cx * 1e6, uy = cy * 1e6;
    g_view.centre = (CanvasPoint){(int64_t)floor(ux) + offset, (int64_t)floor(uy) + offset};
    g_view.fraction[0] = ux - floor(ux);
    g_view.fraction[1] = uy - floor(uy);

    engine_add_overlay(engine, draw_view, NULL, 0);
    engine_set_cursor_position_callback(engine, on_cursor);
    engine_set_mouse_button_callback(engine, on_button);
    engine_set_scroll_callback(engine, on_scroll);

    engine_run(engine, NULL, NULL, NULL);
    free_canvas(g_canvas);
    free_engine(engine);
    return 0;
}
