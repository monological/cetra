#ifndef TRIPLANAR_GLSL
#define TRIPLANAR_GLSL

/*
 * World-aligned (triplanar) projection, spec 11.60.
 *
 * A UV-mapped ground plane is a TOP-DOWN projection, so it stretches by
 * 1/cos(slope) on anything steep -- 5.8x on an 80-degree face, which is exactly
 * where an eroded terrain is most worth looking at. Triplanar projects the same
 * texture down all three world axes and blends by how much the surface faces
 * each, so a cliff is textured by its cliff face rather than by its shadow on
 * the ground.
 *
 * Everything here is a pure function of world position and world normal. That is
 * what makes it usable from a program that has no tangent frame it trusts: the
 * blend returns a WORLD-space normal directly rather than something that needs a
 * TBN to interpret.
 *
 * Every tap takes EXPLICIT gradients. The callers skip projections whose weight
 * is zero, which is fragment-varying control flow, and an implicit-LOD fetch
 * there is undefined in GLSL 3.30. The projected coordinates are affine in world
 * position, so a caller hands in dFdx/dFdy of that -- taken once, in uniform
 * flow -- and every gradient below follows from it with no further derivative.
 */

// Below this a projection contributes less than a code and is not worth three
// texture fetches. It is SUBTRACTED rather than compared, so a dropped
// projection has weight exactly zero and the renormalisation below stays exact
// -- compare-and-skip leaves the surviving weights summing to slightly under
// one, which reads as a faint darkening on flat ground.
#define TRIPLANAR_CUTOFF 0.004

/*
 * How much each world axis owns this fragment. Normalised and exact: the three
 * components sum to 1 whether or not any were dropped.
 *
 * The cutoff is floored against the LARGEST component, and that is a correctness
 * requirement rather than a refinement. `pow` is taken before normalisation, so
 * its scale collapses with sharpness: the biggest component of a unit normal is
 * at worst 1/sqrt(3), which falls under a fixed 0.004 once sharpness passes
 * 10.05. All three weights then round to zero, the sum clamps to 1e-5, and the
 * caller ends at normalize(vec3(0)) -- a NaN normal, into the HDR target, where
 * TAA history and the bloom pyramid both spread it. The material slider reaches
 * 16, and the worst case is a 45-degree face pointing between two world axes,
 * which is most of an eroded terrain.
 *
 * The floor is inert for every sharpness at or below 8, so it moves no frame
 * anything currently authors.
 */
vec3 triplanarWeights(vec3 n, float sharpness) {
    vec3 w = pow(abs(n), vec3(sharpness));
    float peak = max(w.x, max(w.y, w.z));
    w = max(w - min(TRIPLANAR_CUTOFF, 0.25 * peak), vec3(0.0));
    return w / max(w.x + w.y + w.z, 1e-5);
}

// Which way each axis's projection faces, as +/-1. Without this the two sides of
// a surface sample mirrored copies of the texture, which reads as a seam running
// through every ridge -- the projection is along an axis, so the far side is
// looking at the texture from behind.
//
// Exactly zero maps to +1, where sign() would return 0 and collapse the
// coordinate.
vec3 triplanarAxisSign(vec3 n) {
    return vec3(n.x < 0.0 ? -1.0 : 1.0, n.y < 0.0 ? -1.0 : 1.0, n.z < 0.0 ? -1.0 : 1.0);
}

// The three projected coordinates, in texture tiles. `p` is world position
// already divided by the tile size. Each is a swizzle of p, so the same swizzle
// of a world-position derivative is that coordinate's gradient -- which is what
// lets the taps below be explicit-LOD for the cost of a few swizzles.
vec2 triplanarUvX(vec3 p, vec3 sgn) { return vec2(sgn.x * p.z, p.y); }
vec2 triplanarUvY(vec3 p, vec3 sgn) { return vec2(sgn.y * p.x, p.z); }
vec2 triplanarUvZ(vec3 p, vec3 sgn) { return vec2(-sgn.z * p.x, p.y); }

/*
 * The three taps, kept APART.
 *
 * Apart because the normal blend needs each projection's tangent normal in its
 * own frame and cannot use a pre-summed value -- and because one skip policy in
 * one place is the only way the two consumers cannot disagree about what a
 * dropped projection returns. A ternary would not do: a back-end if-converts it
 * and issues every fetch anyway, which on flat ground is two wasted array reads
 * per layer out of the three taken.
 */
void triplanarTapsArray(sampler2DArray tex, float layer, vec3 p, vec3 w, vec3 sgn, vec3 dpx,
                        vec3 dpy, out vec4 sx, out vec4 sy, out vec4 sz) {
    // A dropped projection returns a flat tangent normal at mid roughness and
    // full AO -- neutral in every channel, so a zero weight multiplies neutral
    // rather than multiplying black.
    sx = vec4(0.5, 0.5, 1.0, 1.0);
    sy = sx;
    sz = sx;
    if (w.x > 0.0)
        sx = textureGrad(tex, vec3(triplanarUvX(p, sgn), layer), triplanarUvX(dpx, sgn),
                         triplanarUvX(dpy, sgn));
    if (w.y > 0.0)
        sy = textureGrad(tex, vec3(triplanarUvY(p, sgn), layer), triplanarUvY(dpx, sgn),
                         triplanarUvY(dpy, sgn));
    if (w.z > 0.0)
        sz = textureGrad(tex, vec3(triplanarUvZ(p, sgn), layer), triplanarUvZ(dpx, sgn),
                         triplanarUvZ(dpy, sgn));
}

// One array layer, blended across whichever projections carry weight.
vec4 triplanarSampleArray(sampler2DArray tex, float layer, vec3 p, vec3 w, vec3 sgn, vec3 dpx,
                          vec3 dpy) {
    vec4 sx, sy, sz;
    triplanarTapsArray(tex, layer, p, w, sgn, dpx, dpy, sx, sy, sz);
    return w.x * sx + w.y * sy + w.z * sz;
}

/*
 * The same three taps, reinterpreted as tangent-space normals and blended into
 * one world-space normal -- the "whiteout" blend (Golus).
 *
 * The naive thing is to blend the three tangent normals and then rotate, which
 * is wrong twice over: the three live in different tangent frames, so averaging
 * them mixes unrelated bases, and the blend flattens detail toward the geometric
 * normal wherever two projections are equally weighted. Whiteout instead adds
 * each projection's tangent perturbation to the GEOMETRIC normal's components in
 * that projection's own frame, so detail survives the seam.
 */
vec3 triplanarBlendNormal(vec3 tnx, vec3 tny, vec3 tnz, vec3 n, vec3 w, vec3 sgn) {
    // Undo the coordinate flip above, or the relief runs backwards on exactly
    // the faces the flip was there to fix.
    tnx.x *= sgn.x;
    tny.x *= sgn.y;
    tnz.x *= -sgn.z;

    tnx = vec3(tnx.xy + n.zy, abs(tnx.z) * n.x);
    tny = vec3(tny.xy + n.xz, abs(tny.z) * n.y);
    tnz = vec3(tnz.xy + n.xy, abs(tnz.z) * n.z);

    return normalize(tnx.zyx * w.x + tny.xzy * w.y + tnz.xyz * w.z);
}

#endif // TRIPLANAR_GLSL
