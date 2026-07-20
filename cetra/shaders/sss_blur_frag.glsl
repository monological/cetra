#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Separable screen-space subsurface scattering blur (§4.12), one axis per pass.
// Blurs the skin diffuse buffer (0 off-skin) with a depth-aware Gaussian whose
// screen-space width comes from a fixed world radius / view depth, per-channel-
// scaled by the scatter color so red bleeds widest. A view-Z bilateral weight
// rejects taps across the skin silhouette so the scatter never pulls in the
// background (and off-skin taps don't dilute the edge). The vertical pass also
// folds the composite: it outputs blur - originalDiffuse, which the C side
// additive-blends into the HDR scene (hdr + blur - D), softening the diffuse
// while FragColor's specular stays sharp. M4 swaps the single Gaussian for the
// Jimenez multi-Gaussian profile.
uniform sampler2D srcTex;   // buffer being blurred (diffuse D in H, H-blur in V)
uniform sampler2D origTex;  // original diffuse D (for the composite subtract; V pass)
uniform sampler2D auxTex;   // .z = linear view-space Z (negative in front; 0 = sky/off-skin)
uniform vec2 texelSize;     // 1 / render resolution
uniform vec2 dir;           // (1,0) horizontal pass, (0,1) vertical pass
uniform float projScale;    // 0.5 * projection[1][1] * renderHeight (world radius -> px)
uniform float sssRadius;    // world-space scatter radius
uniform vec3 sssColor;      // per-channel scatter weight (skin ~(1,0.3,0.2); R widest)
uniform int subtractCenter; // 1 in the V/composite pass -> output blur - origDiffuse

const int HALF_TAPS = 12;   // samples per side (cover the broad tail Gaussian)
const float MAX_PX = 48.0;  // clamp the screen-space blur radius
const float TAIL = 2.2;     // how far (x the base radius) the widest Gaussian reaches

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
    float centerZ = texture(auxTex, TexCoords).z;
    vec3 centerSrc = texture(srcTex, TexCoords).rgb;

    // Sky / uncovered (z >= 0): no skin here. The composite pass emits 0 (adds
    // nothing to hdr); the H pass passes the source through.
    if (centerZ >= 0.0) {
        FragColor = vec4(subtractCenter > 0 ? vec3(0.0) : centerSrc, 1.0);
        return;
    }

    float depth = -centerZ;
    float radPx = clamp(sssRadius * projScale / depth, 1.0, MAX_PX);
    vec3 sigma = max(sssColor, vec3(1e-3)) * radPx; // per-channel base width
    float sigmaZ = max(sssRadius, 1e-3);            // view-Z bilateral extent (world units)

    vec3 sum = centerSrc * profileWeight(0.0, sigma); // center tap
    vec3 sumW = profileWeight(0.0, sigma);
    for (int i = 1; i <= HALF_TAPS; i++) {
        float t = float(i) / float(HALF_TAPS) * radPx * TAIL; // reach the broad tail
        vec2 off = dir * t * texelSize;
        for (int s = -1; s <= 1; s += 2) {
            vec2 uv = TexCoords + off * float(s);
            vec3 tapSrc = texture(srcTex, uv).rgb;
            float tapZ = texture(auxTex, uv).z;
            // Reject across the silhouette: sky (tapZ 0 -> big gap) and any large
            // depth step fall off, so the scatter stays on the skin surface.
            float dz = abs(-tapZ - depth);
            float bilat = (tapZ < 0.0) ? exp(-dz * dz / (2.0 * sigmaZ * sigmaZ)) : 0.0;
            vec3 w = profileWeight(t, sigma) * bilat;
            sum += tapSrc * w;
            sumW += w;
        }
    }
    vec3 blur = sum / max(sumW, vec3(1e-5));

    // V/composite pass folds the recomposite delta (blur - sharp diffuse); the H
    // pass just emits the horizontal blur for the V pass to read.
    FragColor = vec4(subtractCenter > 0 ? blur - texture(origTex, TexCoords).rgb : blur, 1.0);
}
