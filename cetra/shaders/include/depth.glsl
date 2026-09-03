// View-space reconstruction, shared by every pass that turns a screen position
// back into a position, a ray, or a size. All of it encodes the cglm
// right-handed convention (view Z is NEGATIVE in front of the camera), and
// since spec 11.104 all of it is correct under BOTH projections cglm builds:
// perspective, where clip.w is -z_eye and every screen quantity carries a
// divide by depth, and orthographic, where clip.w is 1 and none of them do.
//
// THE PROJECTION IS READ OFF THE MATRIX, NOT OFF A FLAG. projection[2][3] is a
// literal -1 under glm_perspective and a literal 0 under glm_ortho, and the TAA
// jitter lands in [2][0..1] or [3][0..1] and never here -- so the test is exact,
// and it is the same test render.c uses to choose the jitter's element. It is
// uniform across a draw, so the branch costs nothing (spec 11.93's rule).
//
// The perspective side of each branch below is the expression that was here
// before, and that is NOT a byte-identity guarantee, which 11.104 learned by
// measurement: the shader compiler lowers a DIVIDE differently in some programs
// once the source around it changes at all -- if, ternary, or a select-free
// form alike -- so the unchanged expression came back a last bit different in
// some programs and identical in the rest. The depth inverse below is one
// coefficient form for that reason, and two goldens moved by last-bit rounding
// on scattered single pixels under it. Read that as a property of the compiler,
// not of the arithmetic, before spending a spec cycle chasing it.
//
// Requires a `projection` uniform in the including shader -- every current
// caller already has one, and passing matrix elements instead would be more
// error-prone than requiring the name.

bool projectionIsOrtho()
{
    return projection[2][3] == 0.0;
}

// View Z from an NDC depth, as ONE formula read off the matrix. Solving
// ndcZ = clip.z / clip.w for Z gives (P33*ndcZ - P32) / (P22 - P23*ndcZ),
// which is the perspective rational when P23 = -1, P33 = 0 and the
// orthographic affine inverse when P23 = 0, P33 = 1 -- with no branch and no
// mode. The multiply-by-zero and multiply-by-one are exact, so the operands of
// the divide are the same as the old perspective form's; only the compiler's
// lowering of the divide differs (see the header).
float viewZFromNdcZ(float ndcZ)
{
    float num = projection[3][3] * ndcZ - projection[3][2];
    float den = projection[2][2] - projection[2][3] * ndcZ;
    return num / den;
}

// The near plane as a positive distance -- ONE recovery, where five passes
// used to hand-roll the perspective form and none of them would have known
// the orthographic one differs.
float nearPlaneDist()
{
    return -viewZFromNdcZ(-1.0);
}

// View-space position from screen UV plus the aux G-buffer's stored LINEAR
// view Z. Under perspective Xv = ndc.x * (-z) / focalX; under orthographic the
// depth term is gone and the translation column is subtracted, because an
// off-centre glm_ortho volume carries a real offset there and the TAA jitter
// for this projection lands there too. The perspective arm ignores its own
// jitter element, as it always has: that asymmetry is deliberate.
vec3 viewPosFromLinZ(vec2 uv, float linZ)
{
    vec2 ndc = uv * 2.0 - 1.0;
    vec2 invFocal = 1.0 / vec2(projection[0][0], projection[1][1]);
    if (projectionIsOrtho())
        return vec3((ndc - vec2(projection[3][0], projection[3][1])) * invFocal, linZ);
    return vec3(ndc * (-linZ) * invFocal, linZ);
}

// The homogeneous w a view-space depth projects to: -z under perspective, 1
// under orthographic. Every "pixels per world unit at this depth" quantity is
// a constant divided by this, so a caller that divides by clipWAt(z) instead
// of by -z is right under both without knowing which it is under.
float clipWAt(float viewZ)
{
    if (projectionIsOrtho())
        return 1.0;
    return -viewZ;
}

// A world-space length r at view depth viewZ, as a screen length in UV units.
// Only the +r offset along X matters, so under perspective this is
// 0.5 * focalX * r / (-z); under orthographic the same without the divide.
float screenLengthAt(float r, float viewZ)
{
    if (projectionIsOrtho())
        return 0.5 * projection[0][0] * r;
    return 0.5 * projection[0][0] * r / (-viewZ);
}

// The view-space ray through a pixel, UNnormalised with z == -1 exactly, so a
// caller that wants the per-slice path length takes length() of it and a
// caller that wants a direction normalizes it. Perspective fans the direction
// from the eye; orthographic holds it at -Z for every pixel and varies the
// ORIGIN instead, which is why this returns a direction and leaves the origin
// to viewPosFromLinZ.
vec3 viewRayVecAt(vec2 ndc)
{
    if (projectionIsOrtho())
        return vec3(0.0, 0.0, -1.0);
    // Reciprocal THEN multiply, not a divide: the callers this replaced took an
    // uploaded 1/focal and multiplied, and a single division rounds differently.
    vec2 invFocal = 1.0 / vec2(projection[0][0], projection[1][1]);
    return vec3(ndc * invFocal, -1.0);
}

// Surface-to-camera direction for a VIEW-space position: the eye is at the
// origin under perspective, and at infinity along +Z under orthographic.
vec3 viewDirToCamera(vec3 viewPos)
{
    if (projectionIsOrtho())
        return vec3(0.0, 0.0, 1.0);
    return normalize(-viewPos);
}

// The same for a WORLD-space position, given the eye and the view matrix. The
// orthographic direction is the view matrix's third rotation row: the
// view-space +Z axis in world coordinates, i.e. the camera's backward, which
// is exactly the surface-to-camera direction every fragment shares under a
// parallel projection. NOT negated -- shadow.c negates the same row to get the
// camera's FORWARD. Takes the matrix as a parameter because GLSL compiles the
// whole unit and only some includers declare `view`.
vec3 worldDirToCamera(vec3 worldPos, vec3 camPos, mat4 viewM)
{
    if (projectionIsOrtho())
        return vec3(viewM[0][2], viewM[1][2], viewM[2][2]);
    return normalize(camPos - worldPos);
}

// The same test for a matrix that is not the bound `projection`, such as the
// previous frame's.
bool projectionIsOrthoM(mat4 P)
{
    return P[2][3] == 0.0;
}

// The length of the view ray from the eye plane to a view-space point: the
// sight line under perspective, the planar depth under orthographic, where
// every ray runs along -Z and a line from the eye POINT measures nothing.
float viewPathLength(vec3 viewPos)
{
    if (projectionIsOrtho())
        return -viewPos.z;
    return length(viewPos);
}

// Direction from the eye toward a world-space point, given the vector to it
// and the inverse view matrix: fans from the eye under perspective, and is the
// camera's forward for every point under orthographic. invView's third column
// is the backward axis, hence the negation.
vec3 worldRayDirFromEye(vec3 toPoint, mat4 invViewM)
{
    if (projectionIsOrtho())
        return -invViewM[2].xyz;
    return normalize(toPoint);
}

// Screen UV from a view-space XY and its positive depth under a given
// projection: the inverse of viewPosFromLinZ, for reprojecting through a
// previous frame's matrix. Orthographic adds the translation column the
// forward reconstruction subtracts.
vec2 uvFromViewXY(vec2 viewXY, float depth, mat4 P)
{
    vec2 focal = vec2(P[0][0], P[1][1]);
    if (projectionIsOrthoM(P))
        return (viewXY * focal + vec2(P[3][0], P[3][1])) * 0.5 + 0.5;
    return (viewXY * focal / depth) * 0.5 + 0.5;
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
