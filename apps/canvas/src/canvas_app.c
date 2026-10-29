/*
 * The 2D canvas's own app (spec 14.0): test sheets drawn through canvas.h, in a
 * window to pan and zoom, or headless for a picture.
 *
 * A sheet is laid out in millimetres and drawn in nanometres, the units pcb
 * gives the canvas, so every length here goes through MM().
 *
 * Under --canvas-probe a sheet also prints where each of its shapes landed and
 * how big it is, in framebuffer pixels with rows from the top, so the `canvas`
 * gate group can measure the picture against closed forms without restating
 * the sheet's layout.
 *
 * A debug harness for one internal header: the glyphs sheet measures text
 * against stb_truetype's own rasterizer, which only its header reaches.
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
#include "cetra/postfx.h"
#include "cetra/internal/ext/stb_truetype.h"
#include "cetra/program.h"
#include "cetra/scene.h"
#include "cetra/text.h"
#include "cetra/triangulate.h"

#define MM(v) ((int64_t)llround((v) * 1e6))

static Canvas* g_canvas;
static CanvasView g_view;
static bool g_panning;
static float g_last_x, g_last_y; // the cursor, window points
static bool g_probe_target;      // say once which framebuffer the overlay drew into

// What a sheet is built with.
typedef struct SheetContext {
    Canvas* canvas;
    CanvasEdit* edit;
    int64_t offset;         // added to every coordinate
    const CanvasView* view; // the view it will be drawn in
    double fb_scale;        // framebuffer pixels per window point
    bool probe;             // print where its shapes land
    const Font* font;       // what its text is set in
} SheetContext;

// Framebuffer pixels per unit in the sheet's view.
static double ppu_px(const SheetContext* ctx) {
    return ctx->view->points_per_unit * ctx->fb_scale;
}

// A unit's place in the framebuffer, rows from the top.
static void fb_of(const SheetContext* ctx, CanvasPoint p, double* x, double* y) {
    float px, py;
    canvas_view_point_of(ctx->view, p, &px, &py);
    *x = px * ctx->fb_scale;
    *y = py * ctx->fb_scale;
}

// Prints a cell's box, `half` units each way of its centre, then the caller's
// measures; the line is ended by the caller.
static void probe_cell(const SheetContext* ctx, const char* sheet, const char* name,
                       CanvasPoint centre, int64_t half) {
    double x0, y0, x1, y1;
    fb_of(ctx, (CanvasPoint){centre.x - half, centre.y + half}, &x0, &y0);
    fb_of(ctx, (CanvasPoint){centre.x + half, centre.y - half}, &x1, &y1);
    printf("canvas %s %s box %ld %ld %ld %ld", sheet, name, lround(x0), lround(y0), lround(x1),
           lround(y1));
}

/*
 * The shapes sheet: one row of each kind, at sizes from a hair to a pad.
 */
static void sheet_shapes(SheetContext* ctx) {
    Canvas* canvas = ctx->canvas;
    CanvasEdit* edit = ctx->edit;
    const int64_t offset = ctx->offset;
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

// White on a black background, so a pixel's red code over 255 is its coverage.
static CanvasPaint white_paint(Canvas* canvas) {
    return (CanvasPaint){.layer = canvas_add_layer(canvas, "white"),
                         .style = canvas_add_style(canvas, 0xffffffffu)};
}

/*
 * The areas sheet: one shape a cell, each with an area in closed form.
 */
static void sheet_areas(SheetContext* ctx) {
    CanvasEdit* edit = ctx->edit;
    const CanvasPaint paint = white_paint(ctx->canvas);
    const double s = ppu_px(ctx);
    const int64_t half = MM(0.9);
#define CELL(i, j) \
    ((CanvasPoint){MM(-3.0 + 2.0 * (i)) + ctx->offset, MM(1.5 - 3.0 * (j)) + ctx->offset})
#define PROBE(name, centre, fmt, ...)                     \
    do {                                                  \
        if (ctx->probe) {                                 \
            probe_cell(ctx, "areas", name, centre, half); \
            printf(" " fmt "\n", __VA_ARGS__);            \
        }                                                 \
    } while (0)

    CanvasPoint c = CELL(0, 0);
    canvas_edit_circle(edit, &(CanvasCircle){.paint = paint, .centre = c, .radius = MM(0.5)});
    PROBE("circle", c, "r %.6f", MM(0.5) * s);

    c = CELL(1, 0);
    canvas_edit_circle(
        edit, &(CanvasCircle){.paint = paint, .centre = c, .radius = MM(0.5), .stroke = MM(0.1)});
    PROBE("ring", c, "r %.6f s %.6f", MM(0.5) * s, MM(0.1) * s);

    c = CELL(2, 0);
    canvas_edit_box(
        edit,
        &(CanvasBox){
            .paint = paint, .centre = c, .size_x = MM(1.2), .size_y = MM(0.8), .corner = MM(0.2)});
    PROBE("rbox", c, "w %.6f h %.6f c %.6f", MM(1.2) * s, MM(0.8) * s, MM(0.2) * s);

    // A capsule turned 30 degrees: its ends rounded onto units, its length measured from them.
    c = CELL(3, 0);
    const CanvasPoint ca = {c.x - MM(0.5 * 0.8660254037844387), c.y - MM(0.5 * 0.5)};
    const CanvasPoint cb = {c.x + MM(0.5 * 0.8660254037844387), c.y + MM(0.5 * 0.5)};
    canvas_edit_segment(edit, &(CanvasSegment){.paint = paint, .a = ca, .b = cb, .width = MM(0.2)});
    PROBE("capsule", c, "len %.6f w %.6f", hypot((double)(cb.x - ca.x), (double)(cb.y - ca.y)) * s,
          MM(0.2) * s);

    // A quarter of a circle about the cell's centre, counter-clockwise from east to north.
    c = CELL(0, 1);
    canvas_edit_arc(edit, &(CanvasArc){.paint = paint,
                                       .a = {c.x + MM(0.5), c.y},
                                       .b = {c.x, c.y + MM(0.5)},
                                       .radius = MM(0.5),
                                       .width = MM(0.1)});
    PROBE("arc", c, "r %.6f w %.6f sweep %.9f", MM(0.5) * s, MM(0.1) * s, 1.5707963267948966);

    c = CELL(1, 1);
    canvas_edit_box(edit, &(CanvasBox){.paint = paint,
                                       .centre = c,
                                       .size_x = MM(1.0),
                                       .size_y = MM(0.3),
                                       .angle = 0.7853981633974483});
    PROBE("turned", c, "w %.6f h %.6f", MM(1.0) * s, MM(0.3) * s);

    c = CELL(2, 1);
    canvas_edit_box(
        edit,
        &(CanvasBox){
            .paint = paint, .centre = c, .size_x = MM(1.0), .size_y = MM(0.6), .stroke = MM(0.1)});
    PROBE("outline", c, "w %.6f h %.6f s %.6f", MM(1.0) * s, MM(0.6) * s, MM(0.1) * s);

    // A flat-ended bar turned -20 degrees.
    c = CELL(3, 1);
    const CanvasPoint ba = {c.x - MM(0.5 * 0.9396926207859084), c.y + MM(0.5 * 0.3420201433256687)};
    const CanvasPoint bb = {c.x + MM(0.5 * 0.9396926207859084), c.y - MM(0.5 * 0.3420201433256687)};
    canvas_edit_segment(
        edit,
        &(CanvasSegment){.paint = paint, .a = ba, .b = bb, .width = MM(0.2), .flat_ends = true});
    PROBE("bar", c, "len %.6f w %.6f", hypot((double)(bb.x - ba.x), (double)(bb.y - ba.y)) * s,
          MM(0.2) * s);
#undef PROBE
#undef CELL
}

/*
 * The polygons sheet: filled polygons, concave and holed, one a cell. Each
 * prints its area by the shoelace formula over the units it was given, its
 * outline's length, and the area of its own triangulation, which must agree.
 */
typedef struct Poly {
    CanvasPoint pts[160];
    size_t n;
    size_t holes[8];
    size_t hole_count;
} Poly;

// A point at (x, y) millimetres from the centre, turned by `turn` radians.
static void poly_add(Poly* p, CanvasPoint centre, double x, double y, double turn) {
    const double c = cos(turn), s = sin(turn);
    p->pts[p->n++] = (CanvasPoint){centre.x + MM(x * c - y * s), centre.y + MM(x * s + y * c)};
}

// A regular ring of `sides` points `radius` millimetres about (x, y) from the centre.
static void poly_ring(Poly* p, CanvasPoint centre, double x, double y, double radius, int sides,
                      double turn) {
    for (int i = 0; i < sides; i++) {
        const double a = turn + 6.283185307179586 * i / sides;
        poly_add(p, centre, x + radius * cos(a), y + radius * sin(a), 0.0);
    }
}

static void poly_hole(Poly* p) {
    p->holes[p->hole_count++] = p->n;
}

static void probe_polygon(const SheetContext* ctx, const char* name, CanvasPoint centre,
                          const Poly* p) {
    const double s = ppu_px(ctx);
    double area = 0.0, perimeter = 0.0;
    double* xy = malloc(p->n * 2 * sizeof(double));
    if (!xy)
        return;
    for (size_t r = 0; r <= p->hole_count; r++) {
        const size_t start = r ? p->holes[r - 1] : 0;
        const size_t end = r < p->hole_count ? p->holes[r] : p->n;
        double ring = 0.0;
        for (size_t i = start, j = end - 1; i < end; j = i++) {
            const double ax = (double)(p->pts[j].x - p->pts[0].x);
            const double ay = (double)(p->pts[j].y - p->pts[0].y);
            const double bx = (double)(p->pts[i].x - p->pts[0].x);
            const double by = (double)(p->pts[i].y - p->pts[0].y);
            ring += ax * by - bx * ay;
            perimeter += hypot(bx - ax, by - ay);
        }
        area += r == 0 ? fabs(ring) / 2.0 : -fabs(ring) / 2.0;
    }
    for (size_t i = 0; i < p->n; i++) {
        xy[2 * i] = (double)(p->pts[i].x - p->pts[0].x);
        xy[2 * i + 1] = (double)(p->pts[i].y - p->pts[0].y);
    }
    Triangulation t;
    double tri_area = 0.0;
    if (triangulate_polygon(xy, p->n, p->holes, p->hole_count, &t)) {
        for (size_t k = 0; k < t.count; k++) {
            const double* a = xy + 2 * t.indices[3 * k];
            const double* b = xy + 2 * t.indices[3 * k + 1];
            const double* c = xy + 2 * t.indices[3 * k + 2];
            tri_area += fabs((b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])) / 2.0;
        }
    }
    printf("canvas polygons %s", name);
    double x0, y0, x1, y1;
    fb_of(ctx, (CanvasPoint){centre.x - MM(1.2), centre.y + MM(1.2)}, &x0, &y0);
    fb_of(ctx, (CanvasPoint){centre.x + MM(1.2), centre.y - MM(1.2)}, &x1, &y1);
    printf(" box %ld %ld %ld %ld area %.6f perimeter %.6f triangulated %.6f triangles %zu points "
           "%zu holes %zu\n",
           lround(x0), lround(y0), lround(x1), lround(y1), area * s * s, perimeter * s,
           tri_area * s * s, t.count, p->n, p->hole_count);
    free_triangulation(&t);
    free(xy);
}

static void sheet_polygons(SheetContext* ctx) {
    const CanvasPaint paint = white_paint(ctx->canvas);
#define CELL(i, j) \
    ((CanvasPoint){MM(-2.6 + 2.6 * (i)) + ctx->offset, MM(1.5 - 3.0 * (j)) + ctx->offset})
    static Poly p;
    const struct {
        const char* name;
        int i, j;
    } cells[6] = {{"holed", 0, 0},  {"star", 1, 0},  {"ell", 2, 0},
                  {"holes3", 0, 1}, {"round", 1, 1}, {"comb", 2, 1}};
    for (int k = 0; k < 6; k++) {
        const CanvasPoint c = CELL(cells[k].i, cells[k].j);
        memset(&p, 0, sizeof(p));
        switch (k) {
            case 0: // a square with a square hole, its edges off the pixel grid
                poly_add(&p, c, -1.003, -1.003, 0.0), poly_add(&p, c, 1.003, -1.003, 0.0);
                poly_add(&p, c, 1.003, 1.003, 0.0), poly_add(&p, c, -1.003, 1.003, 0.0);
                poly_hole(&p);
                poly_add(&p, c, -0.497, -0.497, 0.0), poly_add(&p, c, 0.497, -0.497, 0.0);
                poly_add(&p, c, 0.497, 0.497, 0.0), poly_add(&p, c, -0.497, 0.497, 0.0);
                break;
            case 1: // a five-pointed star
                for (int s = 0; s < 10; s++) {
                    const double r = s % 2 ? 0.4 : 1.0;
                    const double a = 1.5707963267948966 + 6.283185307179586 * s / 10.0;
                    poly_add(&p, c, r * cos(a), r * sin(a), 0.0);
                }
                break;
            case 2: // an L turned 15 degrees
                poly_add(&p, c, -0.8, -0.8, 0.2618), poly_add(&p, c, 0.8, -0.8, 0.2618);
                poly_add(&p, c, 0.8, -0.3, 0.2618), poly_add(&p, c, -0.3, -0.3, 0.2618);
                poly_add(&p, c, -0.3, 0.8, 0.2618), poly_add(&p, c, -0.8, 0.8, 0.2618);
                break;
            case 3: // a plate with three octagonal holes, the holes wound either way
                poly_add(&p, c, -1.0, -0.8, 0.0), poly_add(&p, c, 1.0, -0.8, 0.0);
                poly_add(&p, c, 1.0, 0.8, 0.0), poly_add(&p, c, -1.0, 0.8, 0.0);
                poly_hole(&p);
                poly_ring(&p, c, -0.55, 0.3, 0.25, 8, 0.1);
                poly_hole(&p);
                poly_ring(&p, c, 0.55, 0.3, 0.25, 8, 0.2);
                poly_hole(&p);
                for (int s = 7; s >= 0; s--) // clockwise
                    poly_add(&p, c, 0.25 * cos(0.3 + 0.7853981633974483 * s),
                             -0.35 + 0.25 * sin(0.3 + 0.7853981633974483 * s), 0.0);
                break;
            case 4: // a circle of 64 sides
                poly_ring(&p, c, 0.0, 0.0, 1.0, 64, 0.05);
                break;
            default: // a comb: six teeth 0.2 wide and 0.16 apart, every gap's corners reflex
                poly_add(&p, c, -1.0, -0.9, 0.0), poly_add(&p, c, 1.0, -0.9, 0.0);
                for (int t = 0; t < 6; t++) {
                    const double right = 1.0 - 0.36 * t, left = right - 0.2;
                    poly_add(&p, c, right, 0.9, 0.0);
                    poly_add(&p, c, left, 0.9, 0.0);
                    if (t < 5) {
                        poly_add(&p, c, left, -0.4, 0.0);
                        poly_add(&p, c, left - 0.16, -0.4, 0.0);
                    }
                }
                break;
        }
        canvas_edit_polygon(ctx->edit, &(CanvasPolygon){.paint = paint,
                                                        .points = p.pts,
                                                        .count = p.n,
                                                        .holes = p.holes,
                                                        .hole_count = p.hole_count});
        if (ctx->probe)
            probe_polygon(ctx, cells[k].name, c, &p);
    }
#undef CELL
}

/*
 * The ramp sheet: boxes whose left edge falls a known fraction across a pixel.
 * The pixel the edge crosses holds 1 - f, the one before it 0 and the one
 * after 1: coverage is clamp(0.5 - d) with d the distance past the edge.
 */
static void sheet_ramp(SheetContext* ctx) {
    const CanvasPaint paint = white_paint(ctx->canvas);
    const double s = ppu_px(ctx);
    const double fb_w = ctx->view->width * ctx->fb_scale;
    const double fb_h = ctx->view->height * ctx->fb_scale;
    const double fractions[] = {0.1, 0.3, 0.5, 0.7, 0.9};
    for (int i = 0; i < 5; i++) {
        // The edge at pixel column k plus f, from the view's centre; with this
        // view a pixel is a whole number of units, so the edge lands exactly.
        const int k = (int)(fb_w * 0.15) + i * (int)(fb_w * 0.16);
        const int64_t edge = llround((k + fractions[i] - fb_w * 0.5) / s);
        const int64_t top = llround((fb_h * 0.25) / s), bottom = -top;
        canvas_edit_box(ctx->edit,
                        &(CanvasBox){.paint = paint,
                                     .centre = {edge + MM(0.25) + ctx->offset, ctx->offset},
                                     .size_x = MM(0.5),
                                     .size_y = top - bottom});
        if (ctx->probe)
            printf("canvas ramp f%.1f col %d rows %ld %ld want %.6f\n", fractions[i], k,
                   lround(fb_h * 0.5 - fb_h * 0.2), lround(fb_h * 0.5 + fb_h * 0.2),
                   1.0 - fractions[i]);
    }
}

/*
 * The thin sheet: lines a tenth of a pixel wide, one across and one down, each
 * off the pixel grid. Widened to a pixel, each column across the first (and
 * each row across the second) sums to exactly one pixel of coverage.
 */
static void sheet_thin(SheetContext* ctx) {
    const CanvasPaint paint = white_paint(ctx->canvas);
    const double s = ppu_px(ctx);
    const int64_t o = ctx->offset;
    const int64_t width = llround(0.1 / s);
    const int64_t across = llround(0.37 / s), down = llround(-0.63 / s);
    canvas_edit_segment(ctx->edit, &(CanvasSegment){.paint = paint,
                                                    .a = {o - MM(3.0), o + across},
                                                    .b = {o + MM(3.0), o + across},
                                                    .width = width});
    canvas_edit_segment(ctx->edit, &(CanvasSegment){.paint = paint,
                                                    .a = {o + down, o - MM(2.0)},
                                                    .b = {o + down, o + MM(2.0)},
                                                    .width = width});
    if (ctx->probe) {
        // Away from the ends and from where the two cross.
        double x0, y0, x1, y1;
        fb_of(ctx, (CanvasPoint){o + MM(1.0), o + across}, &x0, &y0);
        fb_of(ctx, (CanvasPoint){o + MM(2.5), o + across}, &x1, &y1);
        printf("canvas thin across cols %ld %ld rows %ld %ld\n", lround(x0), lround(x1),
               lround(y0) - 4, lround(y0) + 4);
        fb_of(ctx, (CanvasPoint){o + down, o + MM(1.5)}, &x0, &y0);
        fb_of(ctx, (CanvasPoint){o + down, o + MM(0.5)}, &x1, &y1);
        printf("canvas thin down rows %ld %ld cols %ld %ld\n", lround(y0), lround(y1),
               lround(x0) - 4, lround(x0) + 4);
    }
}

/*
 * One line at a slope of 1 in 7, through the origin and through the view's
 * centre at (7, 1) * 2^18: long, reaching about 1.9e9 units each way from the
 * origin and so cut into hundreds of pieces, or short, reaching just past the
 * view either side of its centre. Seen deep in the middle of one of the long
 * line's pieces the two should draw the same pixels.
 */
static void line_sheet(SheetContext* ctx, int64_t centre, int64_t k) {
    const CanvasPaint paint = white_paint(ctx->canvas);
    const int64_t x = ctx->offset + 7 * centre, y = ctx->offset + centre;
    canvas_edit_segment(ctx->edit, &(CanvasSegment){.paint = paint,
                                                    .a = {x - 7 * k, y - k},
                                                    .b = {x + 7 * k, y + k},
                                                    .width = MM(0.002)});
}

static void sheet_long(SheetContext* ctx) {
    line_sheet(ctx, 0, (int64_t)1 << 28);
}

static void sheet_long_short(SheetContext* ctx) {
    line_sheet(ctx, (int64_t)1 << 18, (int64_t)1 << 14);
}

/*
 * The symbol sheet: an integrated circuit as a schematic draws one, its pin
 * names inside the body, the top row's turned to read down it, its pin numbers
 * along the pins, and a row of sizes down to where text stops being drawn.
 */
static void sheet_symbol(SheetContext* ctx) {
    Canvas* canvas = ctx->canvas;
    CanvasEdit* edit = ctx->edit;
    const int64_t offset = ctx->offset;
    CanvasLayer* body = canvas_add_layer(canvas, "body");
    CanvasLayer* graphics = canvas_add_layer(canvas, "graphics");
    CanvasLayer* text = canvas_add_layer(canvas, "text");
    const CanvasPaint fill = {.layer = body, .style = canvas_add_style(canvas, 0xffffc2ffu)};
    const CanvasPaint line = {.layer = graphics, .style = canvas_add_style(canvas, 0x840000ffu)};
    const CanvasPaint name = {.layer = text, .style = canvas_add_style(canvas, 0x008484ffu)};
    const CanvasPaint number = {.layer = text, .style = canvas_add_style(canvas, 0xa90000ffu)};
    const CanvasPaint label = {.layer = text, .style = canvas_add_style(canvas, 0x404040ffu)};
#define AT(x, y) ((CanvasPoint){MM(x) + offset, MM(y) + offset})
    const double PI = 3.141592653589793, PITCH = 2.54, LEAD = 2.54;
    const double hx = 7.62, hy = 11.43; // the body's half size

    canvas_edit_box(edit, &(CanvasBox){.paint = fill,
                                       .centre = AT(0.0, 0.0),
                                       .size_x = MM(2.0 * hx),
                                       .size_y = MM(2.0 * hy)});
    canvas_edit_box(edit, &(CanvasBox){.paint = line,
                                       .centre = AT(0.0, 0.0),
                                       .size_x = MM(2.0 * hx),
                                       .size_y = MM(2.0 * hy),
                                       .stroke = MM(0.254)});

    static const char* LEFT[] = {"VDD", "PA0", "PA1", "PA2", "NRST", "GND"};
    static const char* RIGHT[] = {"PB0", "PB1/SCL", "PB2/SDA", "PB3", "SWDIO", "SWCLK"};
    static const char* TOP[] = {"VBAT", "VDDA", "VREF+"};
    char digits[8];
    int pin = 1;
    for (int i = 0; i < 6; i++, pin++) {
        const double y = 3.81 - i * PITCH;
        canvas_edit_segment(
            edit, &(CanvasSegment){
                      .paint = line, .a = AT(-hx - LEAD, y), .b = AT(-hx, y), .width = MM(0.152)});
        canvas_edit_text(edit, &(CanvasText){.paint = name,
                                             .font = ctx->font,
                                             .string = LEFT[i],
                                             .anchor = AT(-hx + 0.6, y),
                                             .size = MM(1.27),
                                             .align_y = CANVAS_ALIGN_MIDDLE});
        snprintf(digits, sizeof(digits), "%d", pin);
        canvas_edit_text(edit, &(CanvasText){.paint = number,
                                             .font = ctx->font,
                                             .string = digits,
                                             .anchor = AT(-hx - LEAD * 0.5, y + 0.25),
                                             .size = MM(1.0),
                                             .align_x = CANVAS_ALIGN_CENTRE});
    }
    for (int i = 0; i < 6; i++, pin++) {
        const double y = -8.89 + i * PITCH;
        canvas_edit_segment(
            edit, &(CanvasSegment){
                      .paint = line, .a = AT(hx, y), .b = AT(hx + LEAD, y), .width = MM(0.152)});
        canvas_edit_text(edit, &(CanvasText){.paint = name,
                                             .font = ctx->font,
                                             .string = RIGHT[i],
                                             .anchor = AT(hx - 0.6, y),
                                             .size = MM(1.27),
                                             .align_x = CANVAS_ALIGN_RIGHT,
                                             .align_y = CANVAS_ALIGN_MIDDLE});
        snprintf(digits, sizeof(digits), "%d", pin);
        canvas_edit_text(edit, &(CanvasText){.paint = number,
                                             .font = ctx->font,
                                             .string = digits,
                                             .anchor = AT(hx + LEAD * 0.5, y + 0.25),
                                             .size = MM(1.0),
                                             .align_x = CANVAS_ALIGN_CENTRE});
    }
    // The top row: names turned a quarter clockwise to read down into the body,
    // numbers a quarter the other way along the pin's left side.
    for (int i = 0; i < 3; i++, pin++) {
        const double x = -PITCH + i * PITCH;
        canvas_edit_segment(
            edit, &(CanvasSegment){
                      .paint = line, .a = AT(x, hy), .b = AT(x, hy + LEAD), .width = MM(0.152)});
        canvas_edit_text(edit, &(CanvasText){.paint = name,
                                             .font = ctx->font,
                                             .string = TOP[i],
                                             .anchor = AT(x, hy - 0.6),
                                             .size = MM(1.27),
                                             .angle = -PI * 0.5,
                                             .align_y = CANVAS_ALIGN_MIDDLE});
        snprintf(digits, sizeof(digits), "%d", pin);
        canvas_edit_text(edit, &(CanvasText){.paint = number,
                                             .font = ctx->font,
                                             .string = digits,
                                             .anchor = AT(x - 0.25, hy + LEAD * 0.5),
                                             .size = MM(1.0),
                                             .angle = PI * 0.5,
                                             .align_x = CANVAS_ALIGN_CENTRE});
    }
    canvas_edit_text(edit, &(CanvasText){.paint = label,
                                         .font = ctx->font,
                                         .string = "U1",
                                         .anchor = AT(-hx, hy + 0.6),
                                         .size = MM(1.27),
                                         .align_y = CANVAS_ALIGN_BOTTOM});
    canvas_edit_text(edit, &(CanvasText){.paint = label,
                                         .font = ctx->font,
                                         .string = "STM32G431K",
                                         .anchor = AT(0.0, -hy - 0.6),
                                         .size = MM(1.27),
                                         .align_x = CANVAS_ALIGN_CENTRE,
                                         .align_y = CANVAS_ALIGN_TOP});

    // Beside it: the same words mirrored, as a board's underside reads, and a
    // row of sizes halving down past the canvas's smallest.
    canvas_edit_text(edit, &(CanvasText){.paint = label,
                                         .font = ctx->font,
                                         .string = "Bottom silk",
                                         .anchor = AT(14.0, 9.5),
                                         .size = MM(1.5)});
    canvas_edit_text(edit, &(CanvasText){.paint = label,
                                         .font = ctx->font,
                                         .string = "Bottom silk",
                                         .anchor = AT(14.0, 7.0),
                                         .size = MM(1.5),
                                         .align_x = CANVAS_ALIGN_RIGHT,
                                         .mirrored = true});
    double y = 4.0;
    for (double size = 2.0; size > 0.02; size *= 0.5) {
        char words[32];
        snprintf(words, sizeof(words), "%.3g mm text", size);
        canvas_edit_text(edit, &(CanvasText){.paint = label,
                                             .font = ctx->font,
                                             .string = words,
                                             .anchor = AT(14.0, y),
                                             .size = MM(size)});
        y -= size * 1.2 + 0.3;
    }
#undef AT
}

/*
 * The glyphs sheet: one character a cell, white on black, each the size of a
 * large heading, so the gate can sum its coverage against stb_truetype's own
 * rasterizer at the same em; and one kerned string, whose width it checks
 * against the font's advances and kerning summed here.
 */
static const char GLYPHS[] = "Bg@We";

static void sheet_glyphs(SheetContext* ctx) {
    const CanvasPaint paint = white_paint(ctx->canvas);
    const stbtt_fontinfo* info = (const stbtt_fontinfo*)ctx->font->stb_font_info;
    const int64_t size = MM(1.0), half = MM(0.75);
    const double em_px = (double)size * ppu_px(ctx);
    for (int i = 0; GLYPHS[i]; i++) {
        const CanvasPoint centre = {MM(-3.0 + 1.5 * i) + ctx->offset, MM(1.0) + ctx->offset};
        const char one[2] = {GLYPHS[i], 0};
        canvas_edit_text(ctx->edit, &(CanvasText){.paint = paint,
                                                  .font = ctx->font,
                                                  .string = one,
                                                  .anchor = centre,
                                                  .size = size,
                                                  .align_x = CANVAS_ALIGN_CENTRE,
                                                  .align_y = CANVAS_ALIGN_MIDDLE});
        if (!ctx->probe)
            continue;
        // stb's own coverage of the glyph at the same em: an exact-area
        // rasterizer, quantised to a byte a pixel.
        const float scale = stbtt_ScaleForMappingEmToPixels(info, (float)em_px);
        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBox(info, GLYPHS[i], scale, scale, &x0, &y0, &x1, &y1);
        const int w = x1 - x0, h = y1 - y0;
        unsigned char* pixels = calloc((size_t)w * (size_t)h, 1);
        double area = 0.0;
        if (pixels) {
            stbtt_MakeCodepointBitmap(info, pixels, w, h, w, scale, scale, GLYPHS[i]);
            for (int k = 0; k < w * h; k++)
                area += pixels[k] / 255.0;
            free(pixels);
        }
        char cell[8];
        snprintf(cell, sizeof(cell), "%c", GLYPHS[i]);
        probe_cell(ctx, "glyphs", cell, centre, half);
        printf(" em %.4f stb %.4f\n", em_px, area);
    }
    const CanvasText kerned = {.paint = paint,
                               .font = ctx->font,
                               .string = "AVATAR To Wave",
                               .anchor = {MM(-3.9) + ctx->offset, MM(-1.5) + ctx->offset},
                               .size = size};
    canvas_edit_text(ctx->edit, &kerned);
    if (ctx->probe) {
        // The font's advances and pair adjustments in its own units, then
        // scaled to the em, all in double.
        const double per_unit = (double)size * stbtt_ScaleForMappingEmToPixels(info, 1.0f);
        double units = 0.0;
        int prev = 0;
        for (const char* p = kerned.string; *p; p++) {
            int advance, bearing;
            stbtt_GetCodepointHMetrics(info, *p, &advance, &bearing);
            units += advance + (prev ? stbtt_GetCodepointKernAdvance(info, prev, *p) : 0);
            prev = *p;
        }
        printf("canvas glyphs width %lld stb %.1f\n", (long long)canvas_text_width(&kerned),
               units * per_unit);
    }
}

typedef struct Sheet {
    const char* name;
    void (*build)(SheetContext* ctx);
    double centre_mm[2]; // where a view of it starts
    double width_mm;     // how much of it the window's width shows
    uint32_t background;
} Sheet;

static const Sheet SHEETS[] = {
    {"shapes", sheet_shapes, {-2.0, 0.5}, 24.0, 0x14181effu},
    {"areas", sheet_areas, {0.0, 0.0}, 8.0, 0x000000ffu},
    {"polygons", sheet_polygons, {0.0, 0.0}, 8.0, 0x000000ffu},
    {"ramp", sheet_ramp, {0.0, 0.0}, 8.0, 0x000000ffu},
    {"thin", sheet_thin, {0.0, 0.0}, 8.0, 0x000000ffu},
    // A view 10 micrometres across, centred on the line half a piece from a cut.
    {"long", sheet_long, {1.835008, 0.262144}, 0.01, 0x000000ffu},
    {"long-short", sheet_long_short, {1.835008, 0.262144}, 0.01, 0x000000ffu},
    {"symbol", sheet_symbol, {8.0, 1.0}, 48.0, 0xf5f4efffu},
    {"glyphs", sheet_glyphs, {0.0, 0.0}, 8.0, 0x000000ffu},
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
    if (g_probe_target) {
        printf("canvas frame target %s\n",
               postfx_picture_fbo(engine->postfx) ? "picture" : "window");
        fflush(stdout);
        g_probe_target = false;
    }
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
            "          [--tile-shift N] [--canvas-probe] [--picture]\n",
            argv0);
}

// An after-tonemap pass that changes nothing, so the frame is drawn into the
// picture rather than the window, as a CRT or a grade puts it.
static const char* IDENTITY_PASS = "#version 330 core\n"
                                   "in vec2 TexCoords;\n"
                                   "out vec4 FragColor;\n"
                                   "uniform sampler2D sceneColor;\n"
                                   "void main() {\n"
                                   "    FragColor = vec4(texelFetch(sceneColor, "
                                   "ivec2(gl_FragCoord.xy), 0).rgb, 1.0);\n"
                                   "}\n";

int main(int argc, char** argv) {
    bool headless = false, mirror = false, probe = false, picture = false;
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
        } else if (strcmp(argv[i], "--canvas-probe") == 0) {
            probe = true;
        } else if (strcmp(argv[i], "--picture") == 0) {
            picture = true;
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
    if (picture) {
        ShaderProgram* pass = create_post_pass_program("canvas_identity", IDENTITY_PASS);
        if (pass) {
            engine_add_program(engine, pass);
            postfx_add_pass(engine->postfx, POSTFX_AT_AFTER_TONEMAP, pass);
        }
    }

    g_canvas = create_canvas(engine, &(CanvasDesc){.tile_shift = tile_shift});
    if (!g_canvas) {
        free_engine(engine);
        return 1;
    }

    const double cx = isnan(view_x) ? sheet->centre_mm[0] : view_x;
    const double cy = isnan(view_y) ? sheet->centre_mm[1] : view_y;
    const double cw = isnan(view_w) ? sheet->width_mm : view_w;
    g_view = (CanvasView){.width = (float)width,
                          .height = (float)height,
                          .points_per_unit = (double)width / (cw * 1e6),
                          .mirror_x = mirror,
                          .background = sheet->background};
    // The centre in whole units plus its fraction, from millimetres and the offset.
    const double ux = cx * 1e6, uy = cy * 1e6;
    g_view.centre = (CanvasPoint){(int64_t)floor(ux) + offset, (int64_t)floor(uy) + offset};
    g_view.fraction[0] = ux - floor(ux);
    g_view.fraction[1] = uy - floor(uy);

    const Font* font = load_font(engine->text_renderer->font_pool,
                                 "apps/splash/assets/Roboto-Bold.ttf", 64.0f, true);
    if (!font) {
        free_canvas(g_canvas);
        free_engine(engine);
        return 1;
    }
    SheetContext ctx = {.canvas = g_canvas,
                        .edit = create_canvas_edit(g_canvas),
                        .offset = (int64_t)offset,
                        .view = &g_view,
                        .fb_scale = (double)engine->fb_width / (double)engine->win_width,
                        .probe = probe,
                        .font = font};
    sheet->build(&ctx);
    canvas_apply(g_canvas, ctx.edit);
    free_canvas_edit(ctx.edit);
    fflush(stdout);
    g_probe_target = probe;

    engine_add_overlay(engine, draw_view, NULL, 0);
    engine_set_cursor_position_callback(engine, on_cursor);
    engine_set_mouse_button_callback(engine, on_button);
    engine_set_scroll_callback(engine, on_scroll);

    engine_run(engine, NULL, NULL, NULL);
    free_canvas(g_canvas);
    free_engine(engine);
    return 0;
}
