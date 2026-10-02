#version 330 core

// The box a fire is marched through (spec 13.14): 36 vertices of a cube from gl_VertexID, so
// the draw reads no attributes. Drawn with FRONT faces culled -- the back faces cover the box's
// whole footprint whether the eye is outside it or in it, and the fragment stage finds the
// entry point itself.

uniform mat4 viewProj;
uniform vec3 boxMin;
uniform vec3 boxMax;

// Six faces wound counter-clockwise seen from outside, corner i at (i & 1, i >> 1 & 1, i >> 2 & 1).
const int CUBE[36] = int[36](0, 2, 3, 0, 3, 1, 4, 5, 7, 4, 7, 6, 0, 4, 6, 0, 6, 2, 1, 3, 7, 1, 7, 5,
                             0, 1, 5, 0, 5, 4, 2, 6, 7, 2, 7, 3);

void main() {
    int i = CUBE[gl_VertexID];
    vec3 corner = vec3(float(i & 1), float((i >> 1) & 1), float((i >> 2) & 1));
    gl_Position = viewProj * vec4(mix(boxMin, boxMax, corner), 1.0);
}
