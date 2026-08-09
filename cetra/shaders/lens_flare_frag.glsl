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
// construction, so the reuse is correct rather than a compromise. It does mean
// this pass is only meaningful when bloom is on.
uniform sampler2D bloomTex;
uniform float ghostSpacing; // Fraction of the centre vector between ghosts
uniform float haloWidth;    // Radius of the halo ring, in UV units
uniform float chroma;       // Per-channel radial offset; 0 = achromatic ghosts
uniform int ghostCount;

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
    float t = clamp(r, 0.0, 0.999) * 4.0;
    int i = int(t);
    return mix(LENS_TINT[i], LENS_TINT[i + 1], fract(t));
}

// One sample with the three channels pulled apart along the radius. This is
// what separates a ghost from a blurred copy of the source: a real internal
// reflection is dispersed by the glass, so its edges fringe.
vec3 sampleDispersed(vec2 uv, float amount) {
    vec2 toCentre = vec2(0.5) - uv;
    return vec3(texture(bloomTex, uv + toCentre * amount).r,
                texture(bloomTex, uv).g,
                texture(bloomTex, uv - toCentre * amount).b);
}

void main()
{
    // Mirror through frame centre: every ghost lies along this vector, which is
    // why the source has to be off-centre for any of this to be visible.
    vec2 flipped = vec2(1.0) - TexCoords;
    vec2 toCentre = vec2(0.5) - flipped;

    vec3 flare = vec3(0.0);
    for (int i = 0; i < ghostCount; i++) {
        // Successive reflections land at successive fractions along the centre
        // vector. i + 1 so no ghost sits exactly on the mirrored source, which
        // would read as a second light rather than an artifact.
        vec2 uv = flipped + toCentre * (float(i + 1) * ghostSpacing);
        // Distance from centre drives both the tint and the falloff: a ghost
        // thrown far off-axis is dimmer because less of the cone makes it back.
        float r = length(vec2(0.5) - uv) * 2.0;
        float weight = pow(clamp(1.0 - r, 0.0, 1.0), 2.0);
        flare += sampleDispersed(uv, chroma) * lensTint(r) * weight;
    }

    // The halo: not a discrete reflection but the aperture ring itself, so it
    // samples at a FIXED radius from centre rather than a fraction of the
    // source's offset -- it stays put while the ghosts slide.
    vec2 haloDir = normalize(toCentre + vec2(1e-6));
    vec2 haloUV = flipped + haloDir * haloWidth;
    float haloR = length(vec2(0.5) - haloUV) * 2.0;
    float haloWeight = pow(clamp(1.0 - haloR, 0.0, 1.0), 2.0);
    flare += sampleDispersed(haloUV, chroma) * lensTint(haloR) * haloWeight;

    FragColor = vec4(flare, 1.0);
}
