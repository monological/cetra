#version 330 core
layout(location = 0) in vec3 aPos;

// The only output the pipeline consumes: shape_geo expands each line segment
// into a quad from the two endpoints' world positions, and shape_frag writes a
// flat albedo with no inputs at all. Everything else this shader used to
// export -- normals, TBN, UVs, view/clip positions, the light array -- was
// written every vertex and discarded by the linker.
out vec3 WorldPos_vs;

uniform mat4 model;

void main() {
    vec4 worldPos = model * vec4(aPos, 1.0);
    WorldPos_vs = worldPos.xyz;

    // shape_geo re-projects both endpoints itself from WorldPos_vs and never
    // reads gl_in[].gl_Position, so nothing downstream consumes this -- the
    // input primitive of a geometry stage is not rasterized. Written anyway so
    // the output is defined, but with no view/projection multiply behind it.
    gl_Position = worldPos;
}
