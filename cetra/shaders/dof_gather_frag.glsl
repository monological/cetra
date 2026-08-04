#version 330 core
in vec2 TexCoords;
// Near and far fields, gathered in one pass. FAR: normalized colour with a
// fallback to the sharp centre colour where the field is empty, so half-res
// bilinear at the sharp-to-far transition never averages toward black (the
// classic dark-halo source); .a is accumulated weight, confidence/debug only.
// NEAR: PREMULTIPLIED colour + geometric coverage -- empty texels are exactly
// vec4(0), so upsampling can only fade coverage, never smear un-owned colour.
layout(location = 0) out vec4 FarOut;
layout(location = 1) out vec4 NearOut;

// Depth-of-field gather at half resolution, scatter-as-gather with
// area-normalized weights. Each tap represents its own blur disc: `reach`
// admits it only where that disc covers this pixel (the original
// scatter-as-gather term), and the 1/area weight makes a big disc dim --
// conserving a point source's energy, ordering occlusion by CoC (nearer,
// smaller-CoC content dominates where both reach), and turning the near
// field's weight sum into GEOMETRIC coverage: the fraction of this pixel
// covered by foreground blur, ramping smoothly past the silhouette. The
// weight is also invariant to the tile radius (tap count ~ 1/R^2, area
// weight ~ R^2), which makes tile-radius jumps second-order -- sampling
// density changes, brightness and coverage do not.
//
// The kernel arrives pre-warped to the aperture's N-gon and pre-rotated --
// deliberately NOT rotated per pixel, which would grind the polygon shape
// into grainy noise.
uniform sampler2D cocColorTex; // Half-res scene colour (rgb) + signed CoC (a)
uniform sampler2D tileTex; // Dilated per-tile (maxFarCoC, maxNearCoC)
uniform vec2 texelSize;    // Half-res texel size
uniform int tileSize;      // DOF_TILE (must match postfx.c)

const int TAPS = 64;         // mirror of postfx.c DOF_TAPS
uniform vec2 kernel[TAPS];   // Unit-radius aperture points (disk or N-gon)
// |CoC| below this is in-focus: excluded from both fields, so neither ever
// carries sharp-scene colour. Safely under the composite's 0.5 farBlend
// start, so every pixel the composite blends is classified by its own tap.
const float CLASS_EPS = 0.25;

void main()
{
    vec4 center = texture(cocColorTex, TexCoords);

    // Kernel radius from the DILATED tile, not this pixel's own CoC: a sharp
    // pixel beside a defocused neighbour must still reach out far enough to
    // collect that neighbour's spill -- its own CoC says zero exactly where
    // the spill matters most. texelFetch, never bilinear: interpolating two
    // maxima can dip below either plateau at a seam and cut a needed reach.
    vec2 tile = texelFetch(tileTex, ivec2(gl_FragCoord.xy) / tileSize, 0).rg;
    float radius = max(tile.r, tile.g);

    // Whole tile in focus: nothing here or nearby spreads anywhere. The
    // composite's passthrough reproduces the sharp scene from these writes.
    if (radius < 0.5) {
        FarOut = vec4(center.rgb, 0.0);
        NearOut = vec4(0.0);
        return;
    }

    float spread = (radius * radius) / float(TAPS); // per-tap area / pi

    vec3 farAcc = vec3(0.0);
    float farW = 0.0;
    vec3 nearAcc = vec3(0.0);
    float nearW = 0.0;

    // i == -1 is the centre tap: v = 0 makes d = 0 and reach exactly 1, so
    // one loop body carries the whole weight formula and classification.
    for (int i = -1; i < TAPS; i++) {
        vec2 v = i < 0 ? vec2(0.0) : kernel[i];
        vec4 tap = texture(cocColorTex, TexCoords + v * radius * texelSize);
        float d = length(v) * radius; // half-res texels from centre
        float ac = abs(tap.a);
        float reach = clamp(ac - d + 1.0, 0.0, 1.0);          // tap's disc covers us
        float aw = min(1.0, spread / max(ac * ac, 0.25));     // 1/area energy norm
        float w = reach * aw;
        if (tap.a > CLASS_EPS) {
            farAcc += w * tap.rgb;
            farW += w;
        } else if (tap.a < -CLASS_EPS) {
            nearAcc += w * tap.rgb;
            nearW += w;
        }
    }

    vec3 farC = farW > 1e-4 ? farAcc / farW : center.rgb;
    FarOut = vec4(farC, clamp(farW, 0.0, 1.0));
    // The near weight sum IS the geometric coverage (the 1/area weights make
    // each contribution the fraction of this pixel the tap's disc owns).
    float coverage = clamp(nearW, 0.0, 1.0);
    NearOut = vec4((nearAcc / max(nearW, 1e-4)) * coverage, coverage);
}
