#ifndef GEOMETRY_H
#define GEOMETRY_H

#include <cglm/cglm.h>

#include "mesh.h"

#define NUM_CIRCLE_SEGMENTS 64
#define RECT_RESOLUTION     32

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
} Rect;

typedef struct {
    vec3 control_points[4];
    float line_width;
} Curve;

typedef struct {
    vec3 position;
    float base_radius;
    float top_radius;
    float height;
    int segments;
} Cylinder;

/* Bezier functions */
void cubic_bezier_curve_point(const Curve* curve, float t, vec3 result);
Curve* generate_s_shaped_bezier_curve(vec3 start, vec3 end, float intensity, float line_width);
void free_curve(Curve* curve);

/* Generate to mesh */
void generate_point_to_mesh(Mesh* mesh, const Point* point);
void generate_circle_to_mesh(Mesh* mesh, const Circle* circle);
void generate_rect_to_mesh(Mesh* mesh, const Rect* rect);
void generate_curve_to_mesh(Mesh* mesh, const Curve* curve);
void generate_cylinder_to_mesh(Mesh* mesh, const Cylinder* cylinder);

#endif // GEOMETRY_H
