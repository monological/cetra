#ifndef GEOMETRY_H
#define GEOMETRY_H

#include <cglm/cglm.h>

#include "mesh.h"

#define NUM_CIRCLE_SEGMENTS 64
#define RECTANGLE_RESOLUTION 32

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
    vec3 control_points[4];
    float line_width;
} CubicBezierCurve;

/* Bezier functions */
void cubic_bezier_curve_point(const CubicBezierCurve* curve, float t, vec3 result);
CubicBezierCurve* generate_s_shaped_bezier_curve(vec3 start, vec3 end, float intensity, float line_width);
void free_bezier_curve(CubicBezierCurve* curve);

/* Rasterization to mesh */
void rasterize_point_to_mesh(Mesh* mesh, const Point* point);
void rasterize_circle_to_mesh(Mesh* mesh, const Circle* circle);
void rasterize_rectangle_to_mesh(Mesh* mesh, const Rectangle* rectangle);
void rasterize_bezier_curve_to_mesh(Mesh* mesh, CubicBezierCurve* curves);

#endif // GEOMETRY_H

