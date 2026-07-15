#version 330 core
in vec3 TexCoords;
out vec4 FragColor;

uniform samplerCube skyboxTex;
uniform float exposure;

// Ground projection: instead of sampling the environment at infinity,
// project it onto a finite dome (radius gpRadius, centered on the world
// origin at ground level y=0) so camera translation produces parallax and
// the model appears to stand on the environment's floor. gpHeight is the
// height above ground the HDR was captured from.
uniform bool groundProjection;
uniform float gpRadius;
uniform float gpHeight;
uniform float gpFadeStart; // Fraction of gpRadius where the fade begins
uniform vec3 camPos;

vec3 sample_direction(vec3 d)
{
    if (!groundProjection || gpRadius <= 0.0)
        return d;

    // Fade the projection out smoothly as the camera moves away from the
    // capture region, ending at the plain infinite skybox instead of popping.
    // Apps clamp camera zoom to the fade start, so no reachable view shows
    // the blend; the fade is a safety net for unclamped cameras.
    vec3 c = camPos;
    float fade = smoothstep(gpFadeStart * gpRadius, 0.95 * gpRadius, length(c));
    if (fade >= 1.0)
        return d;

    // Far intersection of the view ray with the dome sphere (camera inside)
    float b = dot(c, d);
    float k = dot(c, c) - gpRadius * gpRadius;
    float disc = max(b * b - k, 0.0);
    float t = -b + sqrt(disc);
    vec3 q = c + t * d;

    // Below the horizon the geometry is the ground plane, not the sphere
    if (q.y < 0.0 && d.y < 0.0 && c.y > 0.0) {
        float tg = -c.y / d.y;
        q = c + tg * d;
    }

    // Sample the panorama as seen from its capture point
    vec3 projected = normalize(q - vec3(0.0, gpHeight, 0.0));
    return normalize(mix(projected, d, fade));
}

void main()
{
    vec3 dir = sample_direction(normalize(TexCoords));
    vec3 envColor = texture(skyboxTex, dir).rgb;

    // Environment brightness scalar; output stays linear HDR — tone
    // mapping and gamma happen in the post pass (tonemap_frag.glsl)
    envColor *= exposure;

    FragColor = vec4(envColor, 1.0);
}
