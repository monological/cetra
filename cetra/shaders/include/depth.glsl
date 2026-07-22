// View-space depth reconstruction, shared by the post passes that need it.
// Both helpers encode the cglm right-handed perspective convention (view Z is
// NEGATIVE in front of the camera), so a change to how projections are built
// has to land in exactly one place instead of four.

// View Z from an NDC depth, for callers that hold the projection matrix.
// Requires a `projection` uniform in the including shader -- every current
// caller already has one, and passing the two matrix elements instead would
// be more error-prone than requiring the name.
float viewZFromNdcZ(float ndcZ)
{
    return -projection[3][2] / (projection[2][2] + ndcZ);
}

// View-space position from screen UV plus the aux G-buffer's stored LINEAR
// view Z. invFocal is (1/focalX, 1/focalY): Xv = ndc.x * (-z) / focalX, and
// likewise for Y.
vec3 viewPosFromLinZ(vec2 uv, float linZ, vec2 invFocal)
{
    vec2 ndc = uv * 2.0 - 1.0;
    return vec3(ndc * (-linZ) * invFocal, linZ);
}

// Define DOF_COC before including for the circle-of-confusion helper. It is
// gated because it reads focusDistance / focusRange / maxCoC, which only the
// two DoF passes declare -- GLSL compiles the whole unit, so an ungated
// version would fail to compile in ssr/gtao/fog, which include this chunk for
// the reconstruction helpers above and have no focus uniforms.
#ifdef DOF_COC

// Signed circle of confusion at an NDC depth, in half-res texels. The blur
// pass and the composite MUST agree on this: the composite exists to blend by
// the same CoC the blur was built for, so a drift between them blends at a
// radius the blur never produced.
float cocAtNdcZ(float ndcZ, bool isSky)
{
    if (isSky)
        return maxCoC; // sky/background: fully defocused far
    float dist = -viewZFromNdcZ(ndcZ); // positive view distance
    return clamp((dist - focusDistance) / focusRange, -1.0, 1.0) * maxCoC;
}

#endif // DOF_COC
