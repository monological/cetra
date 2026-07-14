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
uniform vec3 camPos;

vec3 sample_direction(vec3 d)
{
    if (!groundProjection || gpRadius <= 0.0)
        return d;

    // Fade the projection out smoothly as the camera nears the dome shell,
    // ending at the plain infinite skybox instead of popping at a boundary
    vec3 c = camPos;
    float fade = smoothstep(0.7 * gpRadius, 0.95 * gpRadius, length(c));
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

    // Apply exposure
    envColor *= exposure;

    // Reinhard tone mapping
    envColor = envColor / (envColor + vec3(1.0));

    // Gamma correction
    envColor = pow(envColor, vec3(1.0 / 2.2));

    FragColor = vec4(envColor, 1.0);
}
