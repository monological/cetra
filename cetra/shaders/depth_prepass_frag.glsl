#version 330 core

// Nothing. Depth is written from the interpolated gl_Position, and this stage
// draws only geometry that cannot discard, so there is no coverage decision to
// make and no colour to compute. Anything that CAN discard is prepassed by its
// own program in pbr_frag's depthOnly mode instead, so the decision is made by
// the shader that will make it again when shading.
//
// The engine keeps a fragment shader here rather than relying on a
// vertex-only program: GL core requires one whenever rasterization is enabled.
void main()
{
}
