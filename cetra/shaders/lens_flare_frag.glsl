#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Chapman-style lens ghosts, at quarter post resolution.
//
// A real lens is a stack of elements, and a bright source reflects between
// their surfaces before reaching the sensor. Each internal reflection lands as
// a defocused copy of the source, mirrored through the optical axis and scaled
// by the element spacing -- so the ghosts always sit on the LINE from the
// source through frame centre, on the far side, which is the property the gate
// asserts and the reason this is more than a blur.
//
// SOURCE IS THE FINISHED BLOOM PYRAMID, not a private bright pass. Bloom's
// upsample writes back onto level 0, so after it runs there is no thresholded
// buffer left to read; taking one here would duplicate the threshold, knee and
// firefly clamp, and a second copy of that arithmetic drifts from the first.
// The pyramid is already thresholded and blurred, and ghosts are defocused by
// construction, so the reuse is correct rather than a compromise. The pyramid
// is built whenever a consumer wants one, so this does not require bloom to be
// switched on -- only that something asked for a thresholded bright image.
uniform sampler2D bloomTex;
uniform float ghostSpacing; // Fraction of the centre vector between ghosts
// Halo radius as a fraction of the HALF-DIAGONAL, matching how the tonemap's
// vignette and aberration measure radius. A plain UV radius would make the ring
// elliptical: UV is not isotropic, so the same offset covers 1.78x the pixel
// distance horizontally as vertically on a 16:9 frame -- and the aperture ring
// is the one part of a flare that is physically a circle.
uniform float haloWidth;
uniform float chroma;   // Per-channel separation in PIXELS at the frame edge
uniform vec2 texelSize; // Display-pixel size, so chroma can be denominated in pixels
uniform int ghostCount;
// Which pyramid level to read. Explicitly a MID mip, not level 0: an internal
// reflection is badly out of focus, so a ghost wants soft edges.
//
// It buys the EDGE, not the shape. Blurring a square yields a soft square until
// the blur exceeds the source, so a square emitter gives square ghosts at every
// level -- visible in flare_fixture_golden.png, whose emitter is a quad. Real
// sources are small and round and this does not come up; do not read a square
// ghost there as this level being wrong.
uniform float sourceLod;

// Ghosts are tinted by how far off-axis they land: light that reflects at a
// steep angle takes a longer path through the glass and comes back warmer.
// A const ramp rather than a texture -- the same call spec 11.13 made for the
// pre-integrated skin fit, and it costs no sampler.
const vec3 LENS_TINT[5] = vec3[5](vec3(1.00, 0.95, 0.85),  // on-axis, near white
                                  vec3(1.00, 0.85, 0.60),  // warm
                                  vec3(0.85, 0.90, 1.00),  // cool mid
                                  vec3(0.70, 0.85, 1.00),  // blue
                                  vec3(0.55, 0.60, 1.00)); // far off-axis

vec3 lensTint(float r) {
    float t = clamp(r, 0.0, 1.0) * 4.0;
    int i = min(int(t), 3); // keeps i + 1 in range at exactly r == 1
    return mix(LENS_TINT[i], LENS_TINT[i + 1], t - float(i));
}

// One reflection: the source read at `uv`, tinted and faded by how far
// off-axis it lands, with the channels pulled apart along the radius. The
// dispersion is what separates a ghost from a blurred copy of the source -- a
// real internal reflection is spread by the glass, so its edges fringe.
//
// Shared by the ghosts and the halo. They differ only in where they sample,
// and keeping the tint/falloff policy in one place stops a change to the
// falloff applying to one and not the other.
vec3 flareTap(vec2 uv) {
    vec2 toCentre = vec2(0.5) - uv;
    // Radius normalised so 1 is a CORNER, the same convention the tonemap uses.
    float r = length(toCentre) / 0.7071;
    float w = clamp(1.0 - r, 0.0, 1.0);
    vec2 shift = toCentre * chroma * texelSize;
    return vec3(textureLod(bloomTex, uv + shift, sourceLod).r,
                textureLod(bloomTex, uv, sourceLod).g,
                textureLod(bloomTex, uv - shift, sourceLod).b) *
           lensTint(r) * (w * w);
}

void main()
{
    // Mirror through frame centre: every ghost lies along this vector, which is
    // why the source has to be off-centre for any of this to be visible.
    vec2 flipped = vec2(1.0) - TexCoords;
    vec2 toCentre = TexCoords - 0.5; // == 0.5 - flipped
    float centreDist = length(toCentre);

    vec3 flare = vec3(0.0);
    for (int i = 0; i < ghostCount; i++) {
        // Successive reflections land at successive fractions along the centre
        // vector. i + 1 so no ghost sits exactly on the mirrored source, which
        // would read as a second light rather than an artifact.
        flare += flareTap(flipped + toCentre * (float(i + 1) * ghostSpacing));
    }

    // The halo is the aperture ring rather than a discrete reflection, so it
    // sits at a FIXED radius from centre instead of a fraction of the source's
    // offset -- it stays put while the ghosts slide. Skipped at width 0, which
    // would otherwise sample the mirrored source itself at full weight and make
    // the slider's low end its brightest setting.
    if (haloWidth > 0.0) {
        vec2 haloDir = toCentre / max(centreDist, 1e-6);
        flare += flareTap(flipped + haloDir * (haloWidth * 0.7071));
    }

    // Normalised by the tap count so brightness is a property of strength
    // alone. Without this, dragging the ghost count doubles the total energy
    // and every other control in the group cross-talks into exposure.
    FragColor = vec4(flare / float(ghostCount + 1), 1.0);
}
