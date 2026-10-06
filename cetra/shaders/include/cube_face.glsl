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

// Which of a panel's faces (spec 13.27) covers a direction from its centre: the same rule in
// the panel's own frame, its width axis cross(up, axis), its up and its normal standing for x, y
// and z, the frame shadow.c draws its faces in. 5 is the face behind the panel, never drawn.
int panelCubeFace(vec3 toFrag, vec3 axis, vec3 up) {
    vec3 right = cross(up, axis);
    return punctualCubeFace(vec3(dot(toFrag, right), dot(toFrag, up), dot(toFrag, axis)));
}
