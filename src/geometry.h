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
} Circle;

typedef struct {
    vec3 position;
    vec3 size;
    float corner_radius;
} Rectangle;

typedef struct {
    vec3 control_points[4];
} CubicBezierCurve;

/* Bezier functions */
void cubic_bezier_curve_point(const CubicBezierCurve* curve, float t, vec3 result);
CubicBezierCurve* generate_s_shaped_bezier_curve(vec3 start, vec3 end, float intensity);
void free_bezier_curve(CubicBezierCurve* curve);

/* Rasterization to mesh */
void rasterize_point_to_mesh(Mesh* mesh, const Point* point);
void rasterize_circle_to_mesh(Mesh* mesh, const Circle* circle, bool filled);
void rasterize_rectangle_to_mesh(Mesh* mesh, const Rectangle* rectangle, bool filled);
void rasterize_bezier_curves_to_mesh(Mesh* mesh, CubicBezierCurve* curves, 
    size_t num_curves);

#endif // GEOMETRY_H

