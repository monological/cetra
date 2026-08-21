// CDLOD vertex morphing, for the terrain quadtree (spec 11.63).
//
// A quadtree patch and its parent cover the same ground at half the density,
// and the parent's lattice restricted to this patch is EXACTLY this patch's
// even-indexed subset -- same world XZ, same height function, so the shared
// vertices are the same vertices. The odd ones sit on the parent's own
// triangulation of them. So the target differs from the vertex in Y ALONE, and
// the whole displacement is one float per vertex.
//
// Morphing to it as the camera retreats is what removes both the pop and the
// crack. The pop, because a patch has already become its parent by the time the
// parent replaces it. The crack, because a fine patch abutting a coarse one is
// at factor 1 there while the coarse one is still at 0, so both sides are
// evaluating the SAME coarse surface at the seam.
//
// The window rides the attribute rather than a per-level uniform array indexed
// by a baked level, which costs eight bytes a vertex of per-patch constant and
// buys the property that matters: an unbound attribute reads (0,0,0), so the
// span reciprocal is 0, the factor is 0, and every mesh in every other app is an
// exact identity with nothing to switch off.
layout(location = 12) in vec3 aMorph;       // x parent Y, y window start, z 1/(end - start)
layout(location = 13) in vec3 aMorphNormal; // the parent surface's normal at this vertex

// Where the camera is, in the space vertex positions are STORED in -- so after
// an origin shift it is the shifted camera, like everything else here.
//
// A uniform rather than something derived from `view`, because the shadow depth
// pass has no view matrix and has to morph identically anyway: it rasterizes the
// same triangles, and a caster displaced differently from its surface detaches
// from it.
uniform vec3 uMorphEye;
// Last frame's, for the previous-frame position the motion vectors need. The
// morph is a function of the camera, so a static surface really does move when
// the camera does, and reporting zero velocity for it smears under TAA.
uniform vec3 uMorphEyePrev;

// The factor for a vertex, from ITS OWN distance to the camera rather than the
// patch's. Per vertex because the seam is where this has to be right, and the
// two sides of a seam agree on nothing except the shared vertex itself.
float cetraMorphFactor(vec3 world, vec3 eye) {
    return clamp((distance(world, eye) - aMorph.y) * aMorph.z, 0.0, 1.0);
}

// Object-space displacement toward the parent surface.
//
// The early-out is not an optimisation for the terrain, it is what keeps this
// free for everything else: without it every vertex in the engine would pay a
// mat4 multiply to be told it does not morph. The branch is coherent by
// construction -- the attribute is constant across a mesh.
vec3 cetraMorphOffset(vec3 rest, mat4 model, vec3 eye) {
    if (aMorph.z <= 0.0)
        return vec3(0.0);
    return vec3(0.0, cetraMorphFactor((model * vec4(rest, 1.0)).xyz, eye) * (aMorph.x - rest.y),
                0.0);
}

// The shading normal, morphed alongside the position.
//
// Without this the surface slides under its own shading as it morphs, and two
// patches meeting at a seam agree on where the ground is while disagreeing about
// which way it faces -- a crease that moves with the camera. Recomputing the
// factor rather than taking it as a parameter is deliberate: two callers passing
// it separately is two chances to pass a different one, which is the drift this
// whole chunk exists to prevent.
//
// mix of two normals cannot cancel here: both are terrain normals and both point
// broadly up, so the sum is never near zero.
vec3 cetraMorphNormal(vec3 n, vec3 rest, mat4 model, vec3 eye) {
    if (aMorph.z <= 0.0)
        return n;
    return normalize(mix(n, aMorphNormal, cetraMorphFactor((model * vec4(rest, 1.0)).xyz, eye)));
}
