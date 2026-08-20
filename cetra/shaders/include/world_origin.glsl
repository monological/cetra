// The world origin the engine has shifted to, and the one conversion that goes
// with it (spec 11.62).
//
// There is exactly ONE coordinate change in this engine:
//
//     storage = authoring - uWorldOrigin
//
// Storage is what you SHADE with. Differences, lighting, depth, motion vectors --
// everything that reads a position as a location -- must use it, because keeping
// it small near the camera is the entire point of shifting the origin.
//
// Authoring is what you HASH or TILE with. Those read a position as an IDENTITY,
// and an identity may not move: a hash is discontinuous, so a shifted object gets
// a phase unrelated to its old one rather than a slightly different one, and a
// tiling lattice slides against the world it is supposed to be locked to.
//
// A consumer declares which space it wants by calling authoredPos() or not. That
// is the whole of the spec's "Rule 2" -- it is not a second mechanism, it is the
// half of one conversion that some consumers need.
//
// This lives in its own file rather than beside its first consumer because it is
// needed in BOTH stages: the wind phase hashes in the vertex shader, the layered
// ground tiles in the fragment shader. A vertex-only home is why the rollout
// stopped at one of four consumers.
//
// NOT a varying, deliberately. Interpolating a coordinate at world scale hands
// back exactly the fp32 quantisation the origin shift exists to remove, so the
// addition has to happen per-fragment.

uniform vec3 uWorldOrigin; // accumulated shift since the world was authored; 0 = never moved

vec3 authoredPos(vec3 storagePos) {
    return storagePos + uWorldOrigin;
}
