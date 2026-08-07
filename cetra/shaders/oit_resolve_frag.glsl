#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Weighted-blended OIT resolve (McGuire & Bavoil 2013). The accumulate pass summed
// premultiplied weighted color into accum.rgb and the sum of weights into accum.a,
// and multiplied (1 - alpha) into revealage. Recover the weighted-average color and
// emit it with revealage as alpha; the C side folds this over the opaque scene with
// blend (GL_ONE_MINUS_SRC_ALPHA, GL_SRC_ALPHA) -> avgColor*(1-reveal) + scene*reveal.
//
// Under moment weighting (spec 11.17) the same buffers mean something different:
// each layer was already scaled by its own measured transmittance, so the sum is
// the foreground itself rather than an average of candidates for it. The C side
// switches the blend to match -- (GL_ONE, GL_SRC_ALPHA), so only the background
// is attenuated.
uniform sampler2D accumTex;     // rgb = sum(color*alpha*w), a = sum(alpha*w)
uniform sampler2D revealageTex; // r = product(1 - alpha)
uniform int momentWeighted;     // 1 = accum.rgb is the composite's foreground already

void main() {
    vec4 accum = texture(accumTex, TexCoords);
    float reveal = texture(revealageTex, TexCoords).r;
    vec3 avgColor = accum.rgb / max(accum.a, 1e-5);
    FragColor = vec4(momentWeighted == 1 ? accum.rgb : avgColor, reveal);
}
