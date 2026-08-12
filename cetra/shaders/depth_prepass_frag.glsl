#version 330 core

// Nothing. Depth is written from the interpolated gl_Position, and this stage
// runs only for geometry that cannot discard, so there is no coverage decision
// to make and no colour to compute.
//
// Alpha-masked materials are deliberately NOT prepassed -- see render.c. Their
// coverage depends on the KHR texture transform, the POM march, the vertex
// colour alpha and, under alpha-to-coverage, on finalOpacity, which needs the
// normal map and the TBN. Reproducing that here would mean reproducing most of
// pbr_frag, and getting it wrong by one fragment either drills a hole in the
// canopy or writes depth where no leaf was.
//
// The engine keeps a fragment shader here rather than relying on a
// vertex-only program: GL core requires one whenever rasterization is enabled.
void main()
{
}
