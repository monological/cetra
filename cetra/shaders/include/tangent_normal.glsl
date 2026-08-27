#ifndef TANGENT_NORMAL_GLSL
#define TANGENT_NORMAL_GLSL

/*
 * ONE decode of a tangent-space normal's storage format (spec 11.85).
 *
 * This used to live inside layers.glsl as layerTangentNormal, whose own comment
 * recorded that the arithmetic had been hand-written at seven sites in that file
 * before it was consolidated. 11.85 then wrote an eighth copy in pbr_frag and
 * missed two more sites, which is what promoted it out here: the lesson was
 * already learned and written down, one include away, and re-typing the
 * expression is how it got un-learned.
 *
 * Z IS NOT ALWAYS STORED. A block-compressed normal is BC5, which carries two
 * channels and samples as (r, g, 0, 1), so the third has to be rebuilt. Doing
 * that is safe where it is needed and NOT free where it is not: the mip filter
 * does not renormalise, so a filtered normal is shorter than unit and the
 * rebuilt Z runs systematically larger than the stored one -- measured on
 * texcomp_fixture at 20.6 codes and 3.5 degrees by mip 5. Hence two entry
 * points rather than one that always rebuilds.
 */

// Two channels in, unit normal out. For storage that never carried a Z: BC5, and
// the packed surface maps in the material array.
vec3 tangentNormalFromXY(vec2 xy) {
    vec3 n = vec3(xy * 2.0 - 1.0, 0.0);
    n.z = sqrt(max(1.0 - dot(n.xy, n.xy), 0.0));
    return n;
}

// A sampled RGB normal, decoded. `rebuildZ` is the caller's statement that the
// blue channel is storage padding rather than data -- true exactly when the
// texture went to the GPU as BC5. Passing it wrongly is the failure this
// function exists to make visible: false on a BC5 map gives z = -1, a normal
// pointing INTO the surface, and true on an uncompressed one silently
// renormalises every mip level.
vec3 tangentNormalDecode(vec3 sampled, bool rebuildZ) {
    if (rebuildZ)
        return tangentNormalFromXY(sampled.xy);
    return sampled * 2.0 - 1.0;
}

#endif // TANGENT_NORMAL_GLSL
