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
 */

// Below this a projection contributes less than a code and is not worth three
// texture fetches. It is SUBTRACTED rather than compared, so a dropped
// projection has weight exactly zero and the renormalisation below stays exact
// -- compare-and-skip leaves the surviving weights summing to slightly under
// one, which reads as a faint darkening on flat ground.
#define TRIPLANAR_CUTOFF 0.004

// How much each world axis owns this fragment. Normalised and exact: the three
// components sum to 1 whether or not any were dropped.
vec3 triplanarWeights(vec3 n, float sharpness) {
    vec3 w = pow(abs(n), vec3(sharpness));
    w = max(w - TRIPLANAR_CUTOFF, vec3(0.0));
    return w / max(w.x + w.y + w.z, 1e-5);
}

// Which way each axis's projection faces, as +/-1. Without this the two sides of
// a surface sample mirrored copies of the texture, which reads as a seam running
// through every ridge -- the projection is along an axis, so the far side is
// looking at the texture from behind.
vec3 triplanarAxisSign(vec3 n) {
    return vec3(n.x < 0.0 ? -1.0 : 1.0, n.y < 0.0 ? -1.0 : 1.0, n.z < 0.0 ? -1.0 : 1.0);
}

// The three projected coordinates, in texture tiles. `p` is world position
// divided by the tile size before it gets here.
vec2 triplanarUvX(vec3 p, vec3 sgn) { return vec2(sgn.x * p.z, p.y); }
vec2 triplanarUvY(vec3 p, vec3 sgn) { return vec2(sgn.y * p.x, p.z); }
vec2 triplanarUvZ(vec3 p, vec3 sgn) { return vec2(-sgn.z * p.x, p.y); }

// One array layer, blended across whichever projections carry weight.
vec4 triplanarSampleArray(sampler2DArray tex, float layer, vec3 p, vec3 w, vec3 sgn) {
    vec4 c = vec4(0.0);
    if (w.x > 0.0)
        c += w.x * texture(tex, vec3(triplanarUvX(p, sgn), layer));
    if (w.y > 0.0)
        c += w.y * texture(tex, vec3(triplanarUvY(p, sgn), layer));
    if (w.z > 0.0)
        c += w.z * texture(tex, vec3(triplanarUvZ(p, sgn), layer));
    return c;
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
