#version 330 core
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;

in vec3 Normal_vs[];
in vec3 WorldPos_vs[];     // World position
in vec3 ViewPos_vs[];      // View position
in vec3 FragPos_vs[];      // Fragment position in clip space
in float ClipDepth_vs[];   // Depth in clip space
in float FragDepth_vs[];
in vec2 TexCoords_vs[];
in mat3 TBN_vs[];

out vec3 Normal;
out vec3 WorldPos;
out vec3 ViewPos;
out vec3 FragPos;
out float ClipDepth;
out float FragDepth;
out vec2 TexCoords;
out mat3 TBN;

const bool useExplodeEffect = true; // Set to true to enable the exploding effect
const float explodeFactor = 20.0; // Exploding effect factor

void main() {
    for (int i = 0; i < 3; i++) {
        // Use gl_Position from vertex shader
        vec4 clipPos = gl_in[i].gl_Position;

        if (useExplodeEffect) {
            // Apply the exploding effect
            vec3 direction = normalize(WorldPos_vs[i] - vec3(0.0, 0.0, 0.0));
            clipPos.xyz += direction * explodeFactor;
        }

        // Pass other vertex attributes
        Normal = Normal_vs[i];
        WorldPos = WorldPos_vs[i];
        ViewPos = ViewPos_vs[i];
        FragPos = FragPos_vs[i];
        ClipDepth = ClipDepth_vs[i];
        FragDepth = FragDepth_vs[i];
        TexCoords = TexCoords_vs[i];
        TBN = TBN_vs[i];

        gl_Position = clipPos;
        EmitVertex();
    }
    EndPrimitive();
}


