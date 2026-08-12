#ifndef _VEGETATION_TEX_H_
#define _VEGETATION_TEX_H_

// Procedural bark and foliage texture synthesis. Pure CPU: every entry point
// returns a malloc'd 8-bit buffer that the caller uploads and owns.
//
// Kept out of the GL layer deliberately, so a bake needs no context and an app
// can decide for itself whether the result becomes a Texture, a file, or an
// atlas layer.

// --- 2D noise --------------------------------------------------------------
//
// Separate from noise.h's 3D field, which has no 2D form. That header's own
// comment names consolidating the two as a follow-up; this is the code it was
// referring to, now at least in the library rather than in an app.
//
// HAZARD, carried over unchanged from where this lived before: veg_noise_seed
// uses srand/rand and writes a file-static table. It is neither thread-safe nor
// isolated from any other seeder, and veg_perlin2 lazily seeds itself with a
// default on first use. Bake on one thread, and seed immediately before the
// bake that depends on it. noise.h's NoisePerm is the pattern to move to.
void veg_noise_seed(unsigned int seed);
float veg_perlin2(float x, float y);
float veg_fbm2(float x, float y, int octaves, float persistence);
float veg_worley2(float x, float y, unsigned int seed);
// Uniform in [min, max) from the same global rand(). Same hazard.
float veg_rand_range(float min_val, float max_val);

// --- bark ------------------------------------------------------------------

// The bark relief in [0,1], `width * height` floats the caller allocates: 0 deep
// in a fissure, 1 on a plate face.
//
// Every bark map below derives from this ONE field, so the albedo's dark cracks,
// the normal map's ridges and the POM displacement describe the same surface
// rather than three independently noisy ones that happen to share a texel.
void veg_bark_height_field(float* out, int width, int height);

unsigned char* veg_bark_albedo(int width, int height, const float* field);    // RGB
unsigned char* veg_bark_normal(int width, int height, const float* field);    // RGB
unsigned char* veg_bark_height(int width, int height, const float* field);    // R
unsigned char* veg_bark_roughness(int width, int height, const float* field); // RGB

// --- foliage ---------------------------------------------------------------

// One leaf on transparent ground, RGBA, `size` square. For a billboard, which
// samples the full 0..1 range and would squash the cluster atlas below.
unsigned char* veg_leaf_sprite(int size);

// The leaf-cluster atlas the mesh generator's UVs address: TG_LEAF_VARIANTS
// cells tiling along U ONLY, because the wind shader reads UV0.y as the flutter
// weight and rows would move each card's pivot. Albedo is RGBA with a real alpha
// channel -- the cards are alpha-masked, and without it foliage renders as solid
// quads. All three buffers are allocated here and owned by the caller.
void veg_leaf_cluster_maps(int width, int height, unsigned char** out_albedo,
                           unsigned char** out_normal, unsigned char** out_rough);

#endif // _VEGETATION_TEX_H_
