#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// SSR's temporal accumulator (spec 10.7.2): the shared plain-RGBA shape
// (temporal_accum_frag) with three signal-specific policies -- the house
// inverse-luma blend on the radiance, a motion-adaptive slack on the
// neighborhood clamp, and a motion-adaptive history window.
// SSR's input is premultiplied (radiance * coverage, coverage), re-jittered
// every frame -- a bright reflected rim is hit by some rays and missed by
// others, and a plain mix passes each swing straight through (the 3x3
// neighborhood clamp cannot reject a sample that IS the local extreme).
// Weighting both blend arms by 1 / (1 + luma) makes bright outliers count
// less than the settled mean, so the accumulation converges instead of
// flickering. Alpha is COVERAGE, not radiance: it keeps the plain mix --
// luma-warping it would breathe the composite's lerp at reflection edges.
// The 1.0 pivot is diffuse white in working space, deliberately absolute --
// same reasoning as taa_resolve_frag / ssgi_accum_frag.
uniform sampler2D currentTex;  // This frame's raw SSR trace (premultiplied)
uniform sampler2D velocityTex; // Screen-space motion .xy (UV units)
uniform sampler2D historyTex;  // Last frame's accumulation
uniform vec2 texelSize;        // 1 / effect resolution
uniform int reset;             // 1 on the first frame -> no history yet
uniform float feedback;        // History weight

float lumaOf(vec3 c) {
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

// History weight while the pixel is IN MOTION (the uniform carries the
// at-rest weight): a ~6-frame window, short enough that the parallax-wrong
// reprojection cannot accumulate into a visible ghost.
const float SSR_FEEDBACK_MOVING = 0.85;

void main()
{
    vec4 current = texture(currentTex, TexCoords);

    if (reset != 0) {
        FragColor = current;
        return;
    }

    vec2 velocity = texture(velocityTex, TexCoords).xy;
    vec2 histUv = TexCoords - velocity;
    if (histUv.x < 0.0 || histUv.x > 1.0 || histUv.y < 0.0 || histUv.y > 1.0) {
        // Reprojected off-screen (disocclusion at the frame edge): current only.
        FragColor = current;
        return;
    }

    // Bound the history to the current 3x3 neighborhood -- always, but with
    // motion-adaptive SLACK. A tight clamp at rest is what made the
    // reflection boil: the re-jittered rays swing the whole box every frame
    // and clamping drags converged history along with it. A clamp gated OFF
    // at rest is what made it ghost: the frames banked during a pan sit in
    // the history when motion stops, and nothing flushes them. Widening the
    // box by its own extent at rest resolves both -- the settled mean sits
    // mid-box and is never touched, while a stranded ghost lands far
    // outside even the widened box and is snapped back within a frame. In
    // motion the slack drops to zero: reprojection is parallax-wrong for
    // reflections, so history earns no slack there.
    vec4 nmin = current;
    vec4 nmax = current;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            if (x == 0 && y == 0)
                continue;
            vec4 n = texture(currentTex, TexCoords + vec2(float(x), float(y)) * texelSize);
            nmin = min(nmin, n);
            nmax = max(nmax, n);
        }
    }
    // velPx is pixels-per-frame at effect resolution: below 0.1 px is the
    // dead-band for subpixel reprojection noise (still "at rest"), 1 px and
    // up is full motion.
    float velPx = length(velocity / texelSize);
    float motion = smoothstep(0.1, 1.0, velPx);
    vec4 slack = (nmax - nmin) * (1.0 - motion);
    vec4 history = clamp(texture(historyTex, histUv), nmin - slack, nmax + slack);

    // Motion splits the estimator's two failure modes. At rest the
    // re-jittered rays need a LONG window (the residual flicker scales with
    // 1 - feedback). In motion the history was reprojected by SURFACE
    // velocity, which is wrong for a reflection -- it moves with parallax,
    // not with its floor pixel -- so trusting it long smears ghosts across
    // the floor. The same motion measure that zeroes the clamp's slack
    // shortens the window; motion itself masks the extra noise, and the
    // a-trous cleans the rest.
    float fb = mix(feedback, SSR_FEEDBACK_MOVING, motion);
    // max(): TAA's YCoCg clamp can emit mildly negative RGB, and a negative
    // luma would INFLATE a blend weight (same guard as the sibling blends).
    float wCurrent = (1.0 - fb) / (1.0 + max(lumaOf(current.rgb), 0.0));
    float wHistory = fb / (1.0 + max(lumaOf(history.rgb), 0.0));
    FragColor = vec4((current.rgb * wCurrent + history.rgb * wHistory) / (wCurrent + wHistory),
                     mix(current.a, history.a, fb));
}
