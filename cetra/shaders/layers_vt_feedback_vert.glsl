#version 330 core

/*
 * The feedback pass's vertex stage (spec 11.67): position through model and
 * view-projection, world position out, nothing else. Deliberately without the
 * displacement chunk (morph, wind): the pass votes on PAGE WANTS at a fraction
 * of render resolution, and a vertex a morph would move sits well inside the
 * 30-unit page its unmorphed position names -- the vote is conservative, not
 * wrong.
 */
layout(location = 0) in vec3 aPos;

uniform mat4 model;
uniform mat4 viewProj;

out vec3 WorldPos;

void main() {
    vec4 world = model * vec4(aPos, 1.0);
    WorldPos = world.xyz;
    gl_Position = viewProj * world;
}
