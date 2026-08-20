#ifndef _MASK_ARRAY_H_
#define _MASK_ARRAY_H_

#include <GL/glew.h>
#include <stdbool.h>

struct Scene;
struct Engine;

// A GL_TEXTURE_2D_ARRAY holding a scene's UNIQUE linear per-texel material
// data (roughness/metallic/ao/opacity/microsurface/anisotropy/subsurface, and
// the hair strand map) as canonical-size layers, so the fragment stage samples
// ONE array instead of that many separate 2D samplers -- the GL 4.1 way to stay
// under the 16 texture-image-unit ceiling and scale to new data by adding a
// layer. Each material records a per-slot layer index (Material *_layer;
// -1 = absent, shader falls back to the scalar factor), resolved at build time.
//
// SINCE SPEC 11.60 IT ALSO HOLDS LAYERED-SURFACE MAPS -- albedo, packed
// normal/roughness/AO, and the splat weights -- and the name did not change,
// for the same reason `anisotropy_tex` still carries the hair strand map
// (11.20): a slot that grows a second tenant is documented where it is
// declared, not renamed across every call site. What the array actually holds
// is a scene's unique per-texel material IMAGES, of which the masks were merely
// the first kind.
//
// That an albedo can live here at all is not a loophole. The array's real
// contract is the paragraph below -- linear data that survives being averaged --
// and an albedo stored NON-sRGB, decoded in the shader, satisfies it exactly.
// The alternative was a second sampler2DArray declaration, which `pbr_frag` at
// 16/16 has nowhere to put. This is 11.45's rule (ocean.glsl) applied a second
// time: N textures of one SHAPE cost one declaration, whatever N is and whatever
// the textures mean.
//
// Mostly scalar masks, but not by definition -- a layer may carry a vector
// field. Whatever it carries must AVERAGE meaningfully, because every layer is
// resampled through a linear filter and then mipped: a quantity with a sign
// ambiguity has to be encoded in a form that survives being averaged with its
// own negation, or it vanishes exactly where the surface is minified.
//
// Layers share one width and one height -- two dimensions, not a square. A
// square at the longest side promotes every layer to the largest single
// dimension in the scene, which a non-square atlas (foliage cells laid out along
// u) makes expensive for no detail: halving apps/forest's array cost nothing but
// the assumption. Masks smaller than the canonical size are
// upsampled (a mask already AT the canonical size copies 1:1, byte-exact);
// masks are uncompressed 8-bit and read as data (linear), so RGBA8 is lossless
// per channel and the per-mask channel reads (.g/.b/.r) are preserved.
typedef struct MaterialMaskArray {
    GLuint texture;            // GL_TEXTURE_2D_ARRAY, RGBA8, linear, mipped (never 0 once created)
    int width, height;         // canonical layer size in texels, shared by every layer
    int layer_count;           // populated layers (0 = a scene with no mask textures)
    GLuint quad_vao, quad_vbo; // fullscreen quad for the resample pass (created once)
} MaterialMaskArray;

MaterialMaskArray* create_material_mask_array(void);
void free_material_mask_array(MaterialMaskArray* arr);

// (Re)build from scene->materials: dedup unique source textures, resample each
// into a layer at the canonical size (GPU copy through the engine's mask_copy
// program), mip, and write each material's *_layer indices. Requires the source
// textures to be loaded. Returns 0 on success (incl. the no-texture case).
int mask_array_build(MaterialMaskArray* arr, struct Scene* scene, struct Engine* engine);

// Bind the array to a texture unit. Always valid -- a 1x1 dummy layer exists
// before the first build, so the shader never samples an unbound array.
void mask_array_bind(const MaterialMaskArray* arr, int unit);

// Build scene->mask_array if a rebuild is pending and the source textures have
// finished loading (lazily creates it on first need). Idempotent per frame;
// the engine render loop calls this every frame.
void mask_array_ensure_built(struct Scene* scene, struct Engine* engine);

#endif // _MASK_ARRAY_H_
