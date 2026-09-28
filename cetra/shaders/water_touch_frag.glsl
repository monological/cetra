#version 330 core

/*
 * Touch ripples, one step (spec 13.4, after Clearwater): the rings a body leaves where it moves
 * through the water, as a height field on a small square that follows the camera.
 *
 * The discrete wave equation on a grid -- each texel's velocity pulled toward the mean height of
 * its four neighbours, both lightly damped -- which carries a disturbance outward as a ring and
 * lets it die away. Clearwater's constants, per step, at WATER_TOUCH_STEP_HZ. It is a height
 * field over the sea rather than part of the sea: the spectral and Gerstner waves do not push
 * the rings about, which at a few metres across and a few seconds long is not where the eye
 * looks.
 *
 * .r height, world units; .g velocity. The window moves in whole texels, so the field is read
 * shifted by that many to stay where it was in the world; what enters at the edge is still water.
 * Every drop queued since the last step is pressed in as a cosine dimple.
 */

in vec2 TexCoords;
out vec4 Field;

uniform sampler2D touchPrev;
uniform vec2 touchShift; // the window's move this step, in uv
uniform int touchDropCount;
uniform vec4 touchDrops[4]; // .xy centre in uv, .z radius in uv, .w depth in world units

void main() {
    vec2 px = 1.0 / vec2(textureSize(touchPrev, 0));
    vec2 uv = TexCoords + touchShift;
    vec4 c = texture(touchPrev, uv);
    float avg = 0.25 * (texture(touchPrev, uv + vec2(px.x, 0.0)).r +
                        texture(touchPrev, uv - vec2(px.x, 0.0)).r +
                        texture(touchPrev, uv + vec2(0.0, px.y)).r +
                        texture(touchPrev, uv - vec2(0.0, px.y)).r);
    float v = (c.g + (avg - c.r) * 0.9) * 0.9955;
    float h = (c.r + v) * 0.9985;
    for (int i = 0; i < touchDropCount; i++) {
        float d = length(TexCoords - touchDrops[i].xy);
        if (d < touchDrops[i].z)
            h -= touchDrops[i].w * (0.5 + 0.5 * cos(3.14159265 * d / touchDrops[i].z));
    }
    // Damped toward the edge, so a ring runs out rather than reflecting off the window's wall.
    vec2 e = min(TexCoords, 1.0 - TexCoords);
    float edge = smoothstep(0.0, 0.06, min(e.x, e.y));
    h *= mix(0.9, 1.0, edge);
    v *= mix(0.9, 1.0, edge);
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
        h = 0.0;
        v = 0.0;
    }
    Field = vec4(h, v, 0.0, 1.0);
}
