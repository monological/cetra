// Which of a point light's six layers covers a direction, by dominant axis.
// The order is +X -X +Y -Y +Z -Z, the GL cubemap face order, and it is the ONE
// thing this file and shadow.c must agree on -- everything else about a face
// travels in its matrix. shadow.c renders the faces in this order.
//
// A boundary is exactly a 45-degree plane, so a PCF tap taken near one lands
// past the face's edge and reads the array's border -- "lit", since a 2D array
// has no neighbouring face to sample. That is a real mechanism and it measures
// below the noise floor at this map size; shadow.c records the measurement
// beside the 90-degree fov it decided on.
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
