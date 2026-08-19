#ifndef _IES_H_
#define _IES_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// IES photometric profiles (IESNA LM-63, spec 11.57): the measured angular
// distribution of a real luminaire, replacing the analytic cone a spot would
// otherwise radiate through.
//
// Read tools/gen_ies_table.py first -- it is the reference implementation of the
// same parse and resample, it explains the file format, and the fixture runs it
// over synthesised files whose candela are known by construction so this reader
// is checked against a painted ground truth rather than against itself.

// The table ceiling. 32 vertical taps is 5.6 degrees, at or above what real
// files carry -- the common grids are 19 or 37 vertical angles, i.e. 10 or 5
// degree steps -- so a finer grid would mostly resample noise upward. Mirrored
// in tools/gen_ies_table.py and in cetra/shaders/include/lights_ubo.glsl; all
// three have to agree or the shader indexes a table it was not handed.
#define IES_MAX_VERT  32
#define IES_MAX_HORIZ 16

// How many distinct profiles one scene may hold. The block is sized for the
// worst case (every profile fully asymmetric) so a scene cannot author its way
// past the guaranteed 16 KB uniform block; a symmetric profile costs a
// sixteenth of that, so a realistic set is far below the cap.
#define IES_MAX_PROFILES 8

// Floats the loaded tables SHARE. The second and independent limit: a profile
// spends only its own v_taps * h_taps, so a symmetric one costs 32 against a
// fully asymmetric one's 512, and sizing a fixed slot for the worst case would
// spend the whole budget on the rare shape. 3968 floats is 992 vec4, which
// holds seven profiles at the ceiling or a hundred-odd symmetric ones.
//
// This file owns the number and ubo.h only lays it out, the relationship
// shore_chain.h has with the shore film block -- so raising it cannot leave the
// block sized for the old one.
#define IES_POOL_FLOATS 3968

// One resampled luminaire.
//
// `table` is V-MAJOR -- h_taps entries per vertical tap -- because that is the
// order the shader walks it, and it is NORMALISED so its peak is exactly 1.
// `peak_cd` carries the candela that normalisation divided out, which is what
// lets a scene omit `intensity` and still get the file's absolute output: the
// loader seeds intensity from it, and normalised x peak IS absolute.
typedef struct IesProfile {
    char* path; // resolved, owned; the cache key
    int v_taps;
    int h_taps; // 1 for a rotationally symmetric file
    int offset; // first float of this profile's table in the library's pool
    float span; // horizontal sweep the file declares: 90, 180 or 360 degrees
    float v_lo; // first and last measured vertical angle, degrees
    float v_hi;
    float peak_cd;
    // The largest vertical angle at which ANY tap is non-zero -- the profile's
    // angular support, in degrees from the axis. The spot shadow frustum is
    // fitted to this rather than to the authored cone, because a real IES skirt
    // routinely emits past the cone and the map would otherwise clip it, leaving
    // the skirt lit and unshadowed.
    float support_deg;
    // The largest v_taps*h_taps a profile can occupy. The POOL is what profiles
    // actually share (ies.c), so this bounds one table rather than sizing the set.
    float table[IES_MAX_VERT * IES_MAX_HORIZ];
} IesProfile;

// The scene's profile set. Cached by resolved path like the texture pool, for
// the same reason: two lights naming one luminaire are one profile, and the
// table is what the UBO is sized against.
typedef struct IesLibrary IesLibrary;

IesLibrary* create_ies_library(void);
void free_ies_library(IesLibrary* lib);

// Load (or return a cached) profile for `path`. Returns the profile's index, or
// -1 when the file is missing, malformed or the library is full -- every one of
// which logs by name rather than failing silently, because a light that quietly
// loses its profile renders as a plausible bare cone.
int ies_library_load(IesLibrary* lib, const char* path);

int ies_library_count(const IesLibrary* lib);
const IesProfile* ies_library_at(const IesLibrary* lib, int index);

// Floats of the shared pool the loaded profiles occupy, for the probe and for
// anything reporting how close a scene sits to the block's budget.
int ies_library_pool_used(const IesLibrary* lib);

// The std140 IesBlock, mirrored by lights_ubo.glsl. Uploaded ONCE when the
// library changes, never by the per-frame cluster path -- which is the whole
// reason it is its own block rather than room in LightsBlock (see ubo.h).
typedef struct GpuIesBlock {
    int32_t ies_counts[4]; // x = profile count, yzw unused
    // TWO vec4 per profile:
    //   [2i+0] pool offset, v_taps, h_taps, horizontal span in degrees
    //   [2i+1] v_lo, v_hi (the file's measured vertical range), unused, unused
    //
    // The measured range is carried rather than resampling every profile onto a
    // fixed 0..180 domain, which would spend half the taps on a hemisphere a
    // downlight never lights. Outside it the profile is ZERO, not the clamped
    // edge value: a file measured 0..90 stopped there because the luminaire
    // emits nothing above it, and clamping would light the ceiling.
    //
    // Floats rather than ints because std140 pads an ivec4 array identically and
    // the shader wants most of them as floats anyway.
    float ies_desc[IES_MAX_PROFILES * 2][4];
    float ies_pool[IES_POOL_FLOATS];
} GpuIesBlock;

// Fill `block` from the library. Returns false when there is nothing to upload,
// so a scene with no profiles allocates and uploads nothing at all.
bool ies_library_pack(const IesLibrary* lib, GpuIesBlock* block);

// Fold a horizontal angle into the measured sweep [0, span].
//
// A partial sweep MIRRORS rather than repeats: a bilateral file measured 0..180
// describes 190 degrees as 170, not as 10. Shared with the shader's iesFold and
// with the tool; the probe's angle-by-angle read is what holds the three
// together.
float ies_fold_horizontal(float angle_deg, float span_deg);

// The profile's value at a direction, in [0,1]. `v_deg` is the angle from the
// luminaire axis, `h_deg` the angle about it. The CPU twin of the shader's
// iesProfile, used by the probe and by the cull-radius solve.
float ies_profile_sample(const IesProfile* p, float v_deg, float h_deg);

// Print every loaded profile and a sweep of its angles, in the --water-fft-probe
// idiom (`ies-probe <tag> k=v k=v`, header first with available=/reason=).
//
// The instrument exists because a profile's correctness is a NUMERIC claim a
// frame cannot make: a table resampled from the wrong plane, folded with a
// modulo instead of a mirror, or scaled by the wrong multiplier still lights a
// room plausibly. What the gate compares these rows against is candela the
// fixture generator KNEW before the render, not values read back off itself.
void ies_library_probe(const IesLibrary* lib);

#endif // _IES_H_
