// Octahedral mapping between unit directions and the [-1,1]^2 square
// (Cigolle et al. 2014). A whole sphere of directions in a square tile, with no
// seam a bilinear tap can fall into except the border -- which is why the tiles
// this feeds carry a 1px gutter.
//
// Chosen over a cubemap per probe because hundreds of probes need to live in ONE
// texture the scene shader can sample: cube arrays are GL 4.0+ but a cube array
// of hundreds of 8x8 faces is far more machinery than a 2D atlas of tiles, and
// the atlas lets one trilinear-ish gather read eight probes from the same map.

// Direction -> [-1,1]^2. The lower hemisphere folds outward across the diagonals,
// so the square's corners are the -Y pole and its centre is +Y.
vec2 octEncode(vec3 dir) {
    vec3 d = dir / (abs(dir.x) + abs(dir.y) + abs(dir.z));
    if (d.y < 0.0) {
        // Fold: mirror across the diagonal, carrying the sign outward.
        vec2 s = vec2(d.x >= 0.0 ? 1.0 : -1.0, d.z >= 0.0 ? 1.0 : -1.0);
        return (vec2(1.0) - abs(vec2(d.z, d.x))) * s;
    }
    return vec2(d.x, d.z);
}

// [-1,1]^2 -> unit direction. Exact inverse of octEncode.
vec3 octDecode(vec2 uv) {
    vec3 d = vec3(uv.x, 1.0 - abs(uv.x) - abs(uv.y), uv.y);
    if (d.y < 0.0) {
        vec2 s = vec2(d.x >= 0.0 ? 1.0 : -1.0, d.z >= 0.0 ? 1.0 : -1.0);
        d.xz = (vec2(1.0) - abs(vec2(d.z, d.x))) * s;
    }
    return normalize(d);
}

// Texel centre within a tile -> the direction that texel represents.
// `texel` is the integer texel inside the tile's INTERIOR (gutter excluded) and
// `res` the interior edge length. Sampling at texel centres rather than corners
// is what makes the encode/decode round-trip land back on the same texel.
vec3 octDirFromTexel(vec2 texel, float res) {
    return octDecode((texel + 0.5) / res * 2.0 - 1.0);
}
