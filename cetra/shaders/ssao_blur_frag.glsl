#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// 4x4 box blur matched to the SSAO noise tile period: exactly cancels the
// per-pixel rotation noise in one pass. With edgeAware on, the taps are weighted
// by linear-depth similarity (a bilateral / joint blur) so the sphere's
// silhouette occlusion cannot bleed across a depth discontinuity onto the floor
// -- the "dripping" streaks below contact shadows. Within a surface all 16 taps
// share depth, the weights are ~uniform, and it degrades to the plain box so the
// noise tile still cancels.
uniform sampler2D aoTex;
uniform sampler2D auxTex; // Aux G-buffer; .z = linear view-Z (< 0), 0 = sky
uniform vec2 texelSize;
uniform int edgeAware; // 1 = depth-bilateral; 0 = plain box (byte-identical legacy)

void main()
{
    if (edgeAware == 0) {
        // Legacy plain 4x4 box. Kept bit-exact (result / 16.0) so the AO buffer
        // -- and the final frame -- is byte-identical with the edge filter off.
        float result = 0.0;
        for (int x = -2; x < 2; x++) {
            for (int y = -2; y < 2; y++) {
                vec2 offset = vec2(float(x), float(y)) * texelSize;
                result += texture(aoTex, TexCoords + offset).r;
            }
        }
        FragColor = vec4(vec3(result / 16.0), 1.0);
        return;
    }

    // Depth-bilateral 4x4. A tap on a surface a relative depth-step away (the
    // sphere silhouette vs the floor behind it) is down-weighted by a Gaussian
    // on |dz|; renormalise by the accumulated weight (the center tap, w = 1,
    // always contributes). A sky tap (z = 0) against a surface reads as a huge
    // dz and drops out. tol is depth-relative so a gentle gradient across the
    // window stays ~uniform (noise-tile cancellation preserved) while a
    // silhouette cliff is rejected.
    float zc = texture(auxTex, TexCoords).z;
    float tol = max(0.05 * abs(zc), 0.02);
    float result = 0.0;
    float wsum = 0.0;
    for (int x = -2; x < 2; x++) {
        for (int y = -2; y < 2; y++) {
            vec2 offset = vec2(float(x), float(y)) * texelSize;
            float zt = texture(auxTex, TexCoords + offset).z;
            float dz = (zt - zc) / tol;
            float w = exp(-dz * dz);
            result += texture(aoTex, TexCoords + offset).r * w;
            wsum += w;
        }
    }
    FragColor = vec4(vec3(result / wsum), 1.0);
}
