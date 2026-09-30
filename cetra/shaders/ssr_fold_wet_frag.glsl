#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// The SSR fold for a frame with wet ground (spec 13.9), in place of upsample_tent_frag's plain
// tent, under the same (GL_ONE, GL_ONE_MINUS_SRC_ALPHA) blend.
//
// Wet ground is a real surface, and SSR must REPLACE its share of the environment's reflection
// rather than lerp the whole pixel toward the trace: a lerp dims the pixel's diffuse by a
// Fresnel the lit shader already applied, and keeps the environment's reflection under the one
// that replaced it. So a wet pixel's pair carries the reflection with its Fresnel on the colour
// and its coverage bare -- the fraction of the environment's reflection the trace or the probe
// stands in for -- and here the frame gains the one and loses that fraction of the other,
// read from the ambient specular the scene pass routed to its own buffer. With alpha 0 the
// blend adds, so the diffuse is untouched.
//
// The catcher keeps the lerp, and the tent averages each class only with its own: a wet pair's
// bare coverage folded into the wall beside a puddle would darken the wall by it. A surface
// marked as neither takes nothing -- SSR never traced it, and what the denoise bled into it
// from the wet ground below is not its reflection. Frames with nothing wet never reach this
// program, so the catcher's fold there is the plain tent's to the bit.
//
// The canvas carries the ambient specular after the split composite's occlusion; what comes
// out here is the unoccluded share, which is the same under open sky, where wet ground is.
uniform sampler2D srcTex;     // the SSR buffer, premultiplied pairs (see above)
uniform sampler2D normalsTex; // view normal .xyz + the SSR marker .a: below -1 is wet
uniform sampler2D specTex;    // the ambient specular, working space
uniform vec2 texelSize;       // one SSR-buffer texel

// The SSR marker's class: 2 wet ground (below -1), 1 the shadow catcher (-1 to 0), 0 unmarked.
int classAt(vec2 uv) {
    float a = texture(normalsTex, uv).a;
    return a < -1.0 ? 2 : (a < 0.0 ? 1 : 0);
}

void main()
{
    const float KERNEL[3] = float[3](0.25, 0.5, 0.25);
    int cls = classAt(TexCoords);
    if (cls == 0) {
        FragColor = vec4(0.0);
        return;
    }
    bool wet = cls == 2;
    vec4 sum = vec4(0.0);
    float total = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 uv = TexCoords + vec2(float(x), float(y)) * texelSize;
            if (classAt(uv) != cls)
                continue;
            float k = KERNEL[x + 1] * KERNEL[y + 1];
            sum += texture(srcTex, uv) * k;
            total += k;
        }
    }
    // The centre tap is always its own class, so the total is at least its weight.
    sum /= total;
    FragColor = wet ? vec4(sum.rgb - sum.a * texture(specTex, TexCoords).rgb, 0.0) : sum;
}
