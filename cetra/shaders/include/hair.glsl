// Card-hair specular: two shifted anisotropic lobes along the strand
// (Kajiya-Kay with Marschner's cuticle tilt; the real-time reduction Scheuermann
// popularised and Karis's 2016 course notes formalise). Roadmap B8.
//
// A hair fibre is not a surface, it is a cylinder with a tilted cuticle, and the
// light that reaches the eye took one of three routes:
//
//   R    reflects off the outside. Uncoloured -- it never entered the fibre --
//        and shifted toward the ROOT by the cuticle tilt.
//   TT   goes in, through, and out the far side. Only visible with the light
//        BEHIND the hair, which is why it needs its own term and its own
//        strength rather than falling out of the view-side lobes.
//   TRT  in, off the back wall, out again. Carries the fibre's absorption, so it
//        is tinted, and shifted toward the TIP -- the opposite way from R.
//
// The two shifts are what make hair read as hair: one white highlight and one
// coloured one, separated along the strand. Collapse the shift to zero and this
// degenerates to a single anisotropic streak, which is what the isotropic GGX
// highlight already looked like.
//
// GGX is not used here at all. Its microfacet model assumes a surface with a
// normal; a strand has a tangent and an entire circle of valid normals, so the
// half-vector it wants does not exist. The tangent IS the shading frame.

// Decode the strand map: .xy strand direction in TANGENT space, .z coherence,
// .w strand identity. See Material.hair_flow_tex for the encoding and
// tools/gen_hair_flow.py for where the values come from.
//
// This is what stops a card behaving like a card. Every fragment on a quad
// shares one interpolated tangent, so an anisotropic lobe keyed on it fires
// across the WHOLE card at once and reads as a flat sheet. The strands are
// real, but they live in the texture and the geometry knows nothing about
// them; this is the channel through which it finds out.
//
// The angle is HALVED on read because the map stores a doubled angle -- the
// only form that survives the mask array's resample and mip chain, since a
// strand and its reverse are the same strand and averaging raw directions
// would cancel them. Coherence arrives as the vector's LENGTH, so a
// neighbourhood of disagreeing strands reports low confidence by itself and
// the caller can fall back rather than trust an average of nothing.
// Halving puts the angle in [-PI/2, PI/2], so the returned direction always has
// a non-negative first component -- it is the representative that lies in the
// +T half-plane, and a caller inherits the card tangent's sign for free.
vec4 hairStrandData(sampler2DArray masks, vec2 uv, int layer) {
    vec3 texel = texture(masks, vec3(uv, float(layer))).rgb;
    vec2 doubled = texel.rg * 2.0 - 1.0;
    float coherence = min(length(doubled), 1.0);
    float angle = 0.5 * atan(doubled.y, doubled.x);
    return vec4(cos(angle), sin(angle), coherence, texel.b);
}

// Kajiya-Kay: the anisotropic response of a fibre running along T. sin of the
// angle between T and each of L, V, combined so the lobe is a ring around the
// strand rather than a point.
//
// NORMALISED, and that is load-bearing rather than tidiness. This replaces a
// microfacet term that has already been divided by 4*NdotV*NdotL, so it has to
// come out on the same scale or the substitution changes total energy rather
// than its distribution. A raw pow() peaks at 1.0 and, multiplied by radiance,
// delivers several times what GGX would -- which reads as a blown white band
// across the strands and is the first thing this model got wrong.
//
// (e + 2) / (8 pi) is the Blinn-Phong normalisation. Kajiya-Kay is a Phong lobe
// in the sine of the strand angle rather than the half-vector angle, so the
// constant carries over; it is an approximation to the true fibre integral, not
// a derivation from it.
// `TdotV`, `sinTV` and `norm` come from the frame rather than being recomputed:
// none of the three depends on the light, and this runs once per light per
// pixel on geometry that overdraws heavily.
float hairStrandSpec(vec3 T, vec3 L, float TdotV, float sinTV, float exponent, float norm) {
    float TdotL = dot(T, L);
    float sinTL = sqrt(max(1.0 - TdotL * TdotL, 0.0));
    // The Kajiya-Kay bracket, clamped: negative means the lobe is behind the
    // strand for this pair and contributes nothing.
    return pow(max(sinTL * sinTV - TdotL * TdotV, 0.0), exponent) * norm;
}

// The cuticle tilt. Shifting the tangent along the NORMAL is what moves a lobe
// up or down the strand -- it is a rotation of the shading frame, not a change
// of direction along the hair.
vec3 hairShiftTangent(vec3 T, vec3 N, float shift) {
    return normalize(T + shift * N);
}

// Roughness -> Kajiya-Kay exponent. Inverted and squared so the control reads
// like every other roughness in the engine (0 sharp, 1 broad) while the exponent
// it drives spans the range the lobe actually needs.
float hairExponent(float roughness) {
    float r = clamp(roughness, 0.02, 1.0);
    return mix(160.0, 4.0, r * r);
}

// Half-angle at which the lobe falls to half its peak -- the width, in radians,
// that a per-strand shift has to clear before neighbouring strands stop sharing
// one highlight.
//
// This is what hairJitter is denominated in, and the reason it is: the shift is
// a tangent tilt, so the jitter needed to separate strands is set entirely by
// how WIDE the lobe is, which hairRoughness controls. A jitter in raw tilt units
// would therefore have to be re-tuned every time roughness moved, and would fail
// silently -- it does not look wrong when it stops separating, it just goes back
// to being a sheet. Measured on the fixture at roughness 0.30 (half-width 0.097):
// a jitter of one half-width moves 17% of lit texels for no energy loss, two
// move 28% but start costing 5%, and four cost 14%.
float hairLobeHalfWidth(float roughness) {
    return acos(pow(0.5, 1.0 / hairExponent(roughness)));
}

// Everything about a hair fragment that does not depend on which light is being
// integrated: the three lobes' shifted tangents, their exponents, and the
// view-side half of the Kajiya-Kay bracket.
//
// This exists because the surrounding loop runs per light per pixel and hair
// cards overdraw heavily, so recomputing two normalize() and three sqrt per
// light is the difference the file already pays to avoid elsewhere (fragCascade
// and the LTC lookups are hoisted for the same reason).
struct HairFrame {
    vec3 Tr, Ttrt, Ttt;    // R, TRT and TT tangents, cuticle shift applied
    vec3 TdotV;            // per lobe, in the same order
    vec3 sinTV;            //  "
    vec3 exponent;         //  "
    vec3 norm;             // Blinn-Phong normalisation per lobe
};

HairFrame hairMakeFrame(vec3 T, vec3 N, vec3 V, float roughness, float shift) {
    float e = hairExponent(roughness);
    HairFrame f;
    // R sits toward the root and TRT toward the tip -- opposite shifts along the
    // normal. TT is unshifted; it comes through the fibre rather than off it.
    f.Tr = hairShiftTangent(T, N, -shift);
    f.Ttrt = hairShiftTangent(T, N, shift * 2.0);
    f.Ttt = T;
    // TRT is broader than R (two refractions and a bounce spread it) and TT
    // broader still.
    f.exponent = vec3(e, e * 0.5, max(e * 0.25, 1.0));
    f.norm = (f.exponent + 2.0) / (8.0 * PI);
    f.TdotV = vec3(dot(f.Tr, V), dot(f.Ttrt, V), dot(f.Ttt, V));
    f.sinTV = sqrt(max(vec3(1.0) - f.TdotV * f.TdotV, vec3(0.0)));
    return f;
}

// The three lobes, summed. Returns radiance-scale colour to be multiplied by the
// light and its shadow, exactly like the GGX specular it replaces.
//
// tint applies to TRT and TT and NOT to R, which is the whole reason hair reads
// as having two highlights of different colours.
vec3 hairSpecular(HairFrame f, vec3 N, vec3 L, vec3 tint, float backlit, float fresnel) {
    // R: uncoloured, and the sharper of the two -- a first-surface reflection,
    // so nothing has scattered it. Weighted by the SAME Fresnel the microfacet
    // lobe used: this is still light off a dielectric interface, and dropping
    // the term is what makes a face-on strand as bright as a grazing one.
    float r = hairStrandSpec(f.Tr, L, f.TdotV.x, f.sinTV.x, f.exponent.x, f.norm.x) * fresnel;

    float trt = hairStrandSpec(f.Ttrt, L, f.TdotV.y, f.sinTV.y, f.exponent.y, f.norm.y);

    // TT: the light is behind the strand, so this keys on the BACK-facing
    // geometry term. Tinted twice over, which is why it reads deeper than TRT.
    float ttWrap = max(-dot(N, L), 0.0);
    float tt = backlit * ttWrap *
               hairStrandSpec(f.Ttt, -L, f.TdotV.z, f.sinTV.z, f.exponent.z, f.norm.z);

    return vec3(r) + tint * trt + tint * tint * tt;
}
