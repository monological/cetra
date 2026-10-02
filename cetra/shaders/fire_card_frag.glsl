#version 330 core

// One card of a FLIPBOOK fire (spec 13.14), composited premultiplied onto the HDR canvas after the
// temporal seam (blend ONE, ONE_MINUS_SRC_ALPHA).
//
// Two frames of the sheet either side of the card's place in the loop, mixed. The colour is
// sRGB-encoded against the sheet's peak, decoded by the texture's format; the card fades where it
// meets geometry, so its flat edge never shows where it stands in a hearth; and it is fogged at
// its own depth, as the march is.

in vec2 vUv;
in float vViewDepth;

uniform sampler2D sheet;   // SRGB8_ALPHA8: premultiplied colour over the peak, coverage
uniform ivec4 sheetLayout; // frames, columns, rows, unused
uniform vec2 frameTexels;   // a frame's width and height in pixels
uniform float sheetGutter;  // transparent pixels round each frame in the sheet
uniform float framePos;     // this card's place in the loop, in frames
uniform float peakNits;
uniform float brightness;

uniform mat4 projection; // read by depth.glsl, through froxel.glsl
uniform vec2 viewport;   // post-resolution pixels
uniform sampler2D sceneDepth;
uniform sampler3D fogVolume;
uniform int fogSlices;
uniform float fogNear;
uniform float fogFar;
uniform float fogDepthDist;

#include "view.glsl"
#include "froxel.glsl"

out vec4 FragColor;

// Metres over which a card fades into the surface behind it.
const float FIRE_CARD_SOFT_DEPTH = 0.08;

// A frame's texel in the sheet, held half a texel inside the frame so a neighbour never bleeds in
// at full size; the gutter round each frame is what keeps one out as the sheet is minified.
vec2 sheetUv(int f, vec2 uv) {
    vec2 inset = 0.5 / frameTexels;
    uv = clamp(uv, inset, 1.0 - inset);
    vec2 cell = frameTexels + 2.0 * sheetGutter;
    vec2 tile = vec2(float(f % sheetLayout.y), float(f / sheetLayout.y));
    return (tile * cell + sheetGutter + uv * frameTexels) / (cell * vec2(sheetLayout.yz));
}

void main() {
    int frames = sheetLayout.x;
    float pos = mod(framePos, float(frames));
    int f0 = int(floor(pos));
    int f1 = (f0 + 1) % frames;
    float t = pos - floor(pos);
    vec4 c = mix(texture(sheet, sheetUv(f0, vUv)), texture(sheet, sheetUv(f1, vUv)), t);
    // Most of a sheet is empty, and an empty texel adds nothing under this blend: skip its fog
    // and its write.
    if (c == vec4(0.0))
        discard;

    vec2 screen = gl_FragCoord.xy / viewport;
    float surface = -viewZFromNdcZ(2.0 * texture(sceneDepth, screen).r - 1.0);
    float soft = clamp((surface - vViewDepth) / FIRE_CARD_SOFT_DEPTH, 0.0, 1.0);
    if (soft <= 0.0)
        discard;
    vec4 front =
        froxelSampleMedium(fogVolume, screen, vViewDepth, fogNear, fogFar, fogSlices, fogDepthDist);
    vec3 lit = min(c.rgb * peakNits * brightness * preExposure * front.a, vec3(WS_SCENE_MAX));
    FragColor = vec4((lit + front.rgb * c.a) * soft, c.a * soft);
}
