#ifndef _TERRAIN_TEX_H_
#define _TERRAIN_TEX_H_

// Procedural ground materials for a layered surface (spec 11.60). Pure CPU, same
// contract as vegetation_tex.h and sand.h: every entry point returns malloc'd
// 8-bit buffers the caller uploads and owns, and nothing here touches GL.
//
// Inherits vegetation_tex.h's seeding hazard with its noise: seed immediately
// before the bake, on one thread.
//
// EVERY FIELD HERE IS PERIODIC. A ground layer is tiled across a kilometre, so a
// map built from unbounded noise prints a hard grid at every tile boundary --
// which is not a filtering problem and cannot be blurred away. The tiled noise
// variants are the whole reason this file can exist at the sizes it uses.

typedef enum TerrainLayerKind {
    TERRAIN_LAYER_GRASS,  // the default ground: fine, matte, slightly clumped
    TERRAIN_LAYER_ROCK,   // scoured bedrock: hard, fractured, low relief contrast
    TERRAIN_LAYER_SILT,   // what the water dropped: smooth, pale, faintly bedded
    TERRAIN_LAYER_GRAVEL, // a stream bed: coarse cells, high relief
} TerrainLayerKind;

// The two maps one layer needs, PACKED for the material texture array:
//
//   albedo   rgb = albedo in stored codes, a = HEIGHT
//   surface  rg  = tangent normal xy, b = roughness, a = ambient occlusion
//
// Packed here rather than by the caller because the packing is a contract with
// layers.glsl, and a second place that knows it is a second place to get it
// wrong. Both buffers are `size * size * 4` bytes.
//
// One height field behind both, for the reason veg_bark_height_field states: the
// albedo's shading, the normal's relief, the roughness and the height that
// decides the blend all describe ONE surface rather than four noises that happen
// to share a texel. The height in the albedo's alpha is that same field, which
// is what makes the interlock between two layers agree with what they look like.
void terrain_layer_maps(TerrainLayerKind kind, int size, unsigned int seed,
                        unsigned char** out_albedo, unsigned char** out_surface);

#endif // _TERRAIN_TEX_H_
