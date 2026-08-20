#ifndef _MATERIAL_TEXTURE_ARRAY_H_
#define _MATERIAL_TEXTURE_ARRAY_H_

#include <GL/glew.h>
#include <stdbool.h>

struct Scene;
struct Engine;

// A GL_TEXTURE_2D_ARRAY holding a scene's UNIQUE linear per-texel material
// images as canonical-size layers, so the fragment stage samples ONE array
// instead of that many separate 2D samplers -- the GL 4.1 way to stay under the
// 16 texture-image-unit ceiling and scale to new data by adding a layer. Each
// material records a per-slot layer index (-1 = absent, and the shader falls
// back to the scalar factor), resolved at build time.
//
// Two kinds of tenant live here:
//   - the six per-texel MASKS, which this was built for
//     (roughness/metallic/ao/opacity/microsurface/anisotropy, plus the hair
//     strand map riding the anisotropy slot)
//   - a layered surface's LAYER MAPS and its splat (spec 11.60): albedo with a
//     height in alpha, packed normal/roughness/AO, and the per-texel weights
//
// It was called MaterialMaskArray until spec 11.61, and the rename is worth a
// note because 11.60 argued FOR keeping the old name and was wrong. The argument
// was that `anisotropy_tex` still carries the hair strand map under its old name
// (11.20), so a slot that grows a second tenant should be documented rather than
// renamed. That precedent does not transfer: `anisotropy_tex`'s declared
// contract -- per-texel grain direction -- is STILL TRUE of a strand map, so the
// name merely narrowed. An albedo is not a mask under any reading, so this one
// became untrue, and it took sixteen lines of comment here to explain that a
// name meant something other than what it said.
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
// the assumption. Sources smaller than the canonical size are upsampled (a
// source already AT it copies 1:1, byte-exact); they are uncompressed 8-bit and
// read as data (linear), so RGBA8 is lossless per channel and the per-mask
// channel reads (.g/.b/.r) are preserved.
typedef struct MaterialTextureArray {
    GLuint texture;            // GL_TEXTURE_2D_ARRAY, RGBA8, linear, mipped (never 0 once created)
    int width, height;         // canonical layer size in texels, shared by every layer
    int layer_count;           // populated layers (0 = a scene with no material textures)
    GLuint quad_vao, quad_vbo; // fullscreen quad for the resample pass (created once)
} MaterialTextureArray;

MaterialTextureArray* create_material_texture_array(void);
void free_material_texture_array(MaterialTextureArray* arr);

// (Re)build from scene->materials: dedup unique source textures, resample each
// into a layer at the canonical size (GPU copy through the engine's mask_copy
// program), mip, and write each material's layer indices. Requires the source
// textures to be loaded. Returns 0 on success (incl. the no-texture case).
int material_texture_array_build(MaterialTextureArray* arr, struct Scene* scene,
                                 struct Engine* engine);

// Bind the array to a texture unit. Always valid -- a 1x1 dummy layer exists
// before the first build, so the shader never samples an unbound array.
void material_texture_array_bind(const MaterialTextureArray* arr, int unit);

// Build scene->material_textures if a rebuild is pending and the source textures
// have finished loading (lazily creates it on first need). Idempotent per frame;
// the engine render loop calls this every frame.
void material_texture_array_ensure_built(struct Scene* scene, struct Engine* engine);

#endif // _MATERIAL_TEXTURE_ARRAY_H_
