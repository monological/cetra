#ifndef _MASK_ARRAY_H_
#define _MASK_ARRAY_H_

#include <GL/glew.h>
#include <stdbool.h>

struct Scene;
struct Engine;

// A GL_TEXTURE_2D_ARRAY holding a scene's UNIQUE linear scalar-mask textures
// (roughness/metallic/ao/opacity/microsurface/anisotropy/subsurface) as
// canonical-size layers, so the fragment stage samples ONE array instead of
// seven separate 2D mask samplers -- the GL 4.1 way to stay under the 16
// texture-image-unit ceiling and scale to new mask types by adding a layer.
// Each material records a per-mask layer index (Material *_layer; -1 = absent,
// shader falls back to the scalar factor), resolved here at build time.
//
// Layers share one size, so masks smaller than the canonical size are
// upsampled (a mask already AT the canonical size copies 1:1, byte-exact);
// masks are uncompressed 8-bit and read as data (linear), so RGBA8 is lossless
// per channel and the per-mask channel reads (.g/.b/.r) are preserved.
typedef struct MaterialMaskArray {
    GLuint texture;            // GL_TEXTURE_2D_ARRAY, RGBA8, linear, mipped (never 0 once created)
    int size;                  // canonical square layer size in texels
    int layer_count;           // populated layers (0 = a scene with no mask textures)
    GLuint quad_vao, quad_vbo; // fullscreen quad for the resample pass (created once)
} MaterialMaskArray;

MaterialMaskArray* create_material_mask_array(void);
void free_material_mask_array(MaterialMaskArray* arr);

// (Re)build from scene->materials: dedup unique mask textures, resample each
// into a layer at the canonical size (GPU copy through the engine's mask_copy
// program), mip, and write each material's *_layer indices. Requires the mask
// source textures to be loaded. Returns 0 on success (incl. the no-mask case).
int mask_array_build(MaterialMaskArray* arr, struct Scene* scene, struct Engine* engine);

// Bind the array to a texture unit. Always valid -- a 1x1 dummy layer exists
// before the first build, so the shader never samples an unbound array.
void mask_array_bind(const MaterialMaskArray* arr, int unit);

// Build scene->mask_array if a rebuild is pending and the source textures have
// finished loading (lazily creates it on first need). Idempotent per frame;
// the engine render loop calls this every frame.
void mask_array_ensure_built(struct Scene* scene, struct Engine* engine);

#endif // _MASK_ARRAY_H_
