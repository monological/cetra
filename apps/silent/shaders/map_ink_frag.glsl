#version 330 core

// A mark written on the town map (spec 13.43), as tools/make_map.py drew it: the ink in alpha and,
// in red, when the pen reached each texel as a fraction of the mark's writing. uFocus is how far
// through its writing the mark is, so the ink shows where the pen has been, behind a front as soft
// as the pen is wide. The ink's colour is the quad's.
in vec2 vUV;
in vec4 vColor;

out vec4 FragColor;

uniform sampler2D uTex;
uniform float uFocus;

#define FRONT 0.015 // of the mark's writing the pen's front is soft over

void main() {
    vec4 ink = texture(uTex, vUV);
    float written = smoothstep(ink.r - FRONT, ink.r, uFocus);
    float alpha = ink.a * vColor.a * written;
    if (alpha < 0.003)
        discard;
    FragColor = vec4(vColor.rgb, alpha);
}
