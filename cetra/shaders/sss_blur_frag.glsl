#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Separable screen-space subsurface scattering blur, one axis per pass.
// Blurs the skin diffuse buffer (0 off-skin) with a depth-aware Gaussian whose
// screen-space width comes from a fixed world radius / view depth, per-channel-
// scaled by the scatter color so red bleeds widest. A view-Z bilateral weight
// rejects taps across the skin silhouette so the scatter never pulls in the
// background (and off-skin taps don't dilute the edge). The vertical pass also
// folds the composite: it outputs blur - originalDiffuse, which the C side
// additive-blends into the HDR scene (hdr + blur - D), softening the diffuse
// while FragColor's specular stays sharp. The profile is a per-channel sum of
// Gaussians (profileWeight below) approximating the skin diffusion falloff.
uniform sampler2D srcTex;   // buffer being blurred (diffuse D in H, H-blur in V)
uniform sampler2D origTex;  // original diffuse D (for the composite subtract; V pass)
uniform sampler2D auxTex;   // .z = linear view-space Z (negative in front; 0 = sky/off-skin)
uniform vec2 texelSize;     // 1 / render resolution
uniform vec2 dir;           // (1,0) horizontal pass, (0,1) vertical pass
uniform float projScale;    // 0.5 * projection[1][1] * renderHeight (world radius -> px)
#define MAX_SSS_PROFILES 8 // mirror of postfx.h MAX_SSS_PROFILES
// Per-material scatter profiles: rgb = per-channel weight (skin ~(1,0.3,0.2), red
// widest), w = world scatter radius. The center pixel's profile index arrives in
// the skin-diffuse alpha (from pbr_frag), selecting its kernel width + color.
uniform vec4 sssProfiles[MAX_SSS_PROFILES];
uniform int mode; // 0 = H blur, 1 = V/composite (blur - origDiffuse),
                  // 2 = passthrough copy of srcTex (TAA delta fold)

const int HALF_TAPS = 12;   // samples per side (cover the broad tail Gaussian)
const float TAIL = 2.2;     // how far (x the base radius) the widest Gaussian reaches

// Ceiling on the base scatter radius, in WORLD units per unit of view depth --
// i.e. a scatter half-angle, 2.86 degrees. Mirror of SSS_MAX_SCATTER_PER_DEPTH
// (postfx.h).
//
// The units are the whole point. Capping the radius in PIXELS made the delivered
// world width fall as 1/height once the cap engaged, so the same material
// scattered differently on a larger display: measured rho = 0.025 across a
// 500->4000 px sweep, i.e. 2.5% of the low-resolution effect survived at 4K
// (spec 11.14 phase 0). Capping the world radius instead makes projScale cancel
// identically below, so the delivered width is either the authored radius or
// this ceiling times depth, at every resolution, SSAA factor and render scale.
const float MAX_SCATTER_PER_DEPTH = 0.05;

// Jimenez-style separable skin diffusion profile: a sum of three Gaussians per
// channel (sharp core + mid + broad colored tail) gives the characteristic skin
// falloff -- a bright, tight highlight core that bleeds into a long reddish tail
// -- far better than one Gaussian. Per-channel sigma comes from the scatter
// color (red widest). Returns the per-channel weight at pixel distance t.
vec3 profileWeight(float t, vec3 sigma) {
    vec3 s1 = sigma * 0.30; // sharp core
    vec3 s2 = sigma * 1.00; // mid
    vec3 s3 = sigma * TAIL; // broad tail
    float t2 = t * t;
    vec3 g1 = exp(-t2 / (2.0 * s1 * s1));
    vec3 g2 = exp(-t2 / (2.0 * s2 * s2));
    vec3 g3 = exp(-t2 / (2.0 * s3 * s3));
    return 0.35 * g1 + 0.40 * g2 + 0.25 * g3;
}

void main()
{
    // Fold pass (TAA): additive copy of the temporally-accumulated composite delta.
    if (mode == 2) {
        FragColor = texture(srcTex, TexCoords);
        return;
    }

    float centerZ = texture(auxTex, TexCoords).z;
    vec4 center = texture(srcTex, TexCoords);
    vec3 centerSrc = center.rgb;
    float centerA = center.a; // this material's profile index, carried in the alpha

    // Sky / uncovered (z >= 0): no skin here. The composite pass emits 0 (adds
    // nothing to hdr, including alpha); the H pass passes the source through,
    // keeping the alpha so the V pass reads the same profile index.
    if (centerZ >= 0.0) {
        FragColor = mode > 0 ? vec4(0.0) : vec4(centerSrc, centerA);
        return;
    }

    // Decode this pixel's material tag (0 = non-skin, skin = profile index + 1)
    // and select its per-material scatter profile (color + radius).
    int centerTag = int(centerA + 0.5);
    int idx = clamp(centerTag - 1, 0, MAX_SSS_PROFILES - 1);
    vec3 sssColor = sssProfiles[idx].rgb;
    float sssRadius = sssProfiles[idx].w;

    float depth = -centerZ;
    // Cap in world units BEFORE projecting, so the delivered width is
    // max(min(sssRadius, MAX_SCATTER_PER_DEPTH * depth), depth / projScale) and
    // projScale survives only in that last term -- the one-texel floor, which is
    // a genuine "do not blur below a texel" guard rather than a scatter budget.
    // depth > 0 is guaranteed by the centerZ >= 0 early-out above.
    float radPx = max(min(sssRadius / depth, MAX_SCATTER_PER_DEPTH) * projScale, 1.0);
    vec3 sigma = max(sssColor, vec3(1e-3)) * radPx; // per-channel base width
    float sigmaZ = max(sssRadius, 1e-3);            // view-Z bilateral extent (world units)

    vec3 sum = centerSrc;   // center tap; profileWeight(0) = 0.35+0.40+0.25 = 1.0 exactly
    vec3 sumW = vec3(1.0);
    for (int i = 1; i <= HALF_TAPS; i++) {
        float t = float(i) / float(HALF_TAPS) * radPx * TAIL; // reach the broad tail
        vec3 pw = profileWeight(t, sigma); // independent of the ±side below -- hoist it
        vec2 off = dir * t * texelSize;
        for (int s = -1; s <= 1; s += 2) {
            vec2 uv = TexCoords + off * float(s);
            vec4 tap = texture(srcTex, uv);
            vec3 tapSrc = tap.rgb;
            float tapZ = texture(auxTex, uv).z;
            // Reject across the silhouette (sky: tapZ 0 -> big gap; any large depth
            // step) AND across a material boundary (a different tag: another skin
            // profile, or a non-skin/background surface at near-equal depth), so
            // one material's scatter never bleeds into its neighbour.
            float dz = abs(-tapZ - depth);
            float bilat = (tapZ < 0.0 && int(tap.a + 0.5) == centerTag)
                              ? exp(-dz * dz / (2.0 * sigmaZ * sigmaZ))
                              : 0.0;
            vec3 w = pw * bilat;
            sum += tapSrc * w;
            sumW += w;
        }
    }
    vec3 blur = sum / max(sumW, vec3(1e-5));

    // V/composite pass folds the recomposite delta (blur - sharp diffuse), with
    // alpha 0 so the additive fold leaves hdr alpha untouched; the H pass just
    // emits the horizontal blur for the V pass to read.
    FragColor = mode > 0 ? vec4(blur - texture(origTex, TexCoords).rgb, 0.0)
                         : vec4(blur, centerA);
}
