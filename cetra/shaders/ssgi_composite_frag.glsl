#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// SSGI composite: add one bounce of indirect diffuse to the lit scene. The GTAO
// sweep gathered nearby surfaces' lit radiance into giTex (half-res); here it is
// tinted by the receiver's base color and demoted for metals (metals reflect no
// diffuse), scaled by intensity, and added ADDITIVELY (GL_ONE, GL_ONE) into the
// HDR scene before bloom -- so bounce light blooms like direct light.
uniform sampler2D giTex;     // Half-res gathered irradiance (.rgb), bilinear-upsampled
uniform sampler2D albedoTex; // Full-res base color (.rgb) + metalness (.a)
uniform float intensity;     // User multiplier on the bounce

void main()
{
    vec4 albedo = texture(albedoTex, TexCoords);
    vec3 gi = texture(giTex, TexCoords).rgb;
    float kD = 1.0 - albedo.a; // metals get no indirect diffuse
    FragColor = vec4(albedo.rgb * gi * kD * intensity, 1.0);
}
