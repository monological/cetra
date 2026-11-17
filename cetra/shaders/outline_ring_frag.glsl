#version 330 core

// A view's outlines (spec 14.39): from its outline mask, a ring outside each outlined silhouette,
// premultiplied, which the view's copy into the window lays over its picture. A pixel the mask
// covers is the surface itself and takes nothing; one outside takes the colour of the nearest
// covered pixel -- the strongest among the nearest, so where a seen silhouette and a hidden one
// meet the seen one rings -- and as much of it as its distance leaves inside the ring's width, so
// the outer edge is antialiased by that distance.

in vec2 TexCoords;
out vec4 FragColor;

uniform sampler2D maskTex;
uniform float uRadius; // the ring's width, framebuffer pixels

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 size = textureSize(maskTex, 0);
    if (texelFetch(maskTex, p, 0).a > 0.0) {
        FragColor = vec4(0.0);
        return;
    }
    int r = int(ceil(uRadius));
    float best = 1e9;
    vec4 near = vec4(0.0);
    for (int y = -r; y <= r; y++) {
        for (int x = -r; x <= r; x++) {
            ivec2 q = p + ivec2(x, y);
            if (any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, size)))
                continue;
            vec4 m = texelFetch(maskTex, q, 0);
            if (m.a <= 0.0)
                continue;
            float d = length(vec2(float(x), float(y)));
            if (d < best - 1e-4 || (abs(d - best) <= 1e-4 && m.a > near.a)) {
                best = d;
                near = m;
            }
        }
    }
    float cover = clamp(uRadius + 0.5 - best, 0.0, 1.0) * near.a;
    FragColor = vec4(near.rgb * cover, cover);
}
