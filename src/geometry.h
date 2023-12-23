#ifndef GEOMETRY_H
#define GEOMETRY_H

#include <cglm/cglm.h>

#include "mesh.h"

#define NUM_CIRCLE_SEGMENTS 64
#define RECTANGLE_RESOLUTION 32

/*
 * Primitives
 */
typedef struct {
    vec3 position;
} Point;

typedef struct {
    vec3 position;
    float radius;
    bool filled;
    float line_width;
} Circle;

typedef struct {
    vec3 position;
    vec3 size;
    float corner_radius;
    bool filled;
    float line_width;
} Rectangle;

typedef struct {
    float x;      // X-coordinate of the rectangle's origin
    float y;      // Y-coordinate of the rectangle's origin
    float width;  // Width of the rectangle
    float height; // Height of the rectangle
} Rect;

/*
 * Curves
 */
typedef struct {
    vec3 control_points[4];
    float line_width;
} CubicBezierCurve;

/*
 * Surfaces
 */

typedef float (*SurfaceFunction)(float x, float y, void* params);

typedef struct {
    int octaves;
    float persistence;
} fbmParams;

float fbm_2d(float x, float y, void* params);

/* Bezier functions */
void cubic_bezier_curve_point(const CubicBezierCurve* curve, float t, vec3 result);
CubicBezierCurve* generate_s_shaped_bezier_curve(vec3 start, vec3 end, float intensity, float line_width);
void free_bezier_curve(CubicBezierCurve* curve);

/* Rasterization to mesh */
void rasterize_point_to_mesh(Mesh* mesh, const Point* point);
void rasterize_circle_to_mesh(Mesh* mesh, const Circle* circle);
void rasterize_rectangle_to_mesh(Mesh* mesh, const Rectangle* rectangle);
void rasterize_bezier_curve_to_mesh(Mesh* mesh, CubicBezierCurve* curves);

void rasterize_contours_to_mesh(Mesh* mesh, SurfaceFunction surface, void* params, 
        const Rect* bounds, int resolution,
        int levels);

#endif // GEOMETRY_H

