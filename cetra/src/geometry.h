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

typedef struct {
    vec3 position;
    vec3 size; // width (x), height (y), depth (z)
} Box;

typedef struct {
    vec3 position;
    float width;
    float depth;
    int segments_w; // UV tiling segments
    int segments_d;
} Plane;

typedef struct {
    vec3 position;
    float radius;
    int segments_lon; // longitude divisions (around Y); <=2 falls back to a default
    int segments_lat; // latitude divisions (pole to pole); <=1 falls back to a default
} Sphere;

/* Bezier functions */
void curve_point_at(const Curve* curve, float t, vec3 result);
Curve* create_s_bezier_curve(vec3 start, vec3 end, float intensity, float line_width);
void free_curve(Curve* curve);

/* Generate to mesh */
void mesh_generate_point(Mesh* mesh, const Point* point);
void mesh_generate_circle(Mesh* mesh, const Circle* circle);
void mesh_generate_rect(Mesh* mesh, const Rect* rect);
void mesh_generate_curve(Mesh* mesh, const Curve* curve);
void mesh_generate_cylinder(Mesh* mesh, const Cylinder* cylinder);
void mesh_generate_box(Mesh* mesh, const Box* box);
void mesh_generate_plane(Mesh* mesh, const Plane* plane);
void mesh_generate_sphere(Mesh* mesh, const Sphere* sphere);

#endif // GEOMETRY_H
