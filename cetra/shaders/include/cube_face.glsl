// Which of a point light's six faces covers a direction, by dominant axis. The order is
// +X -X +Y -Y +Z -Z, the GL cubemap face order, and shadow.c renders the faces in it.
//
// Its own chunk because a cached light's faces (spec 13.16) are chosen by the same rule in
// the fog volume, which declares its own punctual sampler and so cannot include the file
// that declares the per-frame lookup's.
int punctualCubeFace(vec3 toFrag) {
    vec3 a = abs(toFrag);
    if (a.x >= a.y && a.x >= a.z)
        return toFrag.x > 0.0 ? 0 : 1;
    if (a.y >= a.z)
        return toFrag.y > 0.0 ? 2 : 3;
    return toFrag.z > 0.0 ? 4 : 5;
}
