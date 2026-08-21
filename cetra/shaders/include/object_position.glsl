// Where a vertex ends up, shared by every stage that rasterizes scene geometry:
// the shading passes, the shadow depth pass, and the depth prepass.
//
// The prepass is why this exists. It draws the same triangles as pbr_vert and
// the shading pass then tests against its depth with GL_LEQUAL, so the two must
// agree to the LAST BIT -- not merely be the same formula written twice. Before
// this chunk there were three hand-written copies of the position path
// (pbr_vert, pbr_skinned_vert, shadow_depth_vert) which were meant to agree,
// and shadow_depth_vert's had already drifted: it concatenates
// lightSpaceMatrix * model into one matrix where pbr_vert chains three
// matrix-vector products. Same answer in exact arithmetic, different low bits in
// floating point, and a fourth copy written for the prepass would have
// inherited whichever mistake it was copied from.
//
// Same argument as wind.glsl and skin.glsl, which exist because a shadow that
// displaces differently from its caster detaches from it. This one is stricter:
// there the failure is a visibly wrong shadow, here it is fragments silently
// failing the depth test and the surface disappearing.
//
// Under LEQUAL the exposure is ONE-SIDED, and that is worth knowing before
// writing a new stage: a shading fragment landing a last bit NEARER than the
// depth the prepass wrote still passes, one landing a bit FARTHER is rejected
// and takes its surface with it. So drift is not symmetric noise here -- half of
// it deletes geometry.
#include "wind.glsl" // windOffset, below. Included rather than assumed: this
                     // chunk lands in five programs and the expansion is
                     // include-once, so requiring callers to order it right
                     // would buy nothing but a startup compile error.
#include "terrain_morph.glsl" // cetraMorphOffset, the second displacer.

// The object-space position a vertex actually occupies: posed by `bone`, then
// displaced by wind and by the terrain morph.
//
// The bone matrix is a PARAMETER rather than computed here, because
// pbr_skinned_vert needs that same matrix for the normal and tangent and would
// otherwise blend four bones twice per vertex. `isSkinned` is the caller's
// `skinned` uniform: disabled vertex attributes read as (0,0,0,1), so an
// unskinned mesh drawn by a skinned program must not take the posed branch.
//
// `model` reaches the two displacers for different halves of itself. Wind takes
// its TRANSLATION alone, as the per-object phase -- the OBJECT's position and
// not the vertex's on purpose, since a phase that varied within a mesh would
// tear it. The morph takes the whole matrix, because its factor is a distance
// from the camera and so has to be measured where the vertex actually is.
//
// `eye` is the camera in storage space, and it is a parameter rather than read
// straight off uMorphEye so that a caller evaluating the PREVIOUS frame's
// position can hand over the previous camera. There is no such split for wind,
// which takes its own `t` instead.
//
// `rest` is the UN-skinned bind position, and it is deliberately what wind is
// evaluated against rather than the posed result: the height mask that pins a
// cloth top and frees its hem is authored in rest space, and re-measuring it
// against an animated pose makes the mask slide as the mesh moves.
//
// ANYTHING ADDED HERE NEEDS A BOUND, AND HAS TO REACH ALL FIVE PROGRAMS.
//
// draw_list.c's _item_bounds is the CPU mirror of this function -- it bounds the
// box these displacers can move a vertex into, which is what lets a swaying,
// posed or morphing mesh be frustum-culled instead of exempted. A fourth
// displacer compiles fine, bounds nothing, and culls geometry that is on screen;
// the symptom is a mesh popping at the frame edge, and no golden or gate in the
// corpus watches for it.
//
// The uniform side is the same requirement seen from the other end, and it is
// the sharper of the two: this chunk lands in FIVE programs (pbr, pbr_skinned,
// depth prepass, shadow depth, shadow absorb) and every one of them must be fed
// the same inputs. engine_upload_displacement_uniforms is the one call that does
// it -- a program that misses a uniform displaces its vertices somewhere else,
// and under the prepass's one-sided LEQUAL that DELETES the surface rather than
// shading it wrong. Spec 11.62 shipped exactly that, reaching one program of
// five, and the full gate suite was green.
//
// Split in two only because a caller that already holds a POSED position -- the
// previous-frame skinned one, which no depth stage computes -- needs the
// displacement to add to it rather than a position to replace it with. Both
// halves are here so neither can list a different set of displacers.
vec3 cetra_local_displacement(vec3 rest, vec2 uv0, vec2 uv1, float t, mat4 model, vec3 eye) {
    return windOffset(rest, uv0, uv1, t, model[3].xyz) + cetraMorphOffset(rest, model, eye);
}

vec4 cetra_local_position(vec3 rest, mat4 bone, bool isSkinned, vec2 uv0, vec2 uv1, float t,
                          mat4 model, vec3 eye) {
    vec4 local = isSkinned ? bone * vec4(rest, 1.0) : vec4(rest, 1.0);
    local.xyz += cetra_local_displacement(rest, uv0, uv1, t, model, eye);
    return local;
}


// Object -> world -> view -> clip, as THREE chained matrix-vector products in
// that nesting, and the nesting is the point.
//
// `p * (v * (m * local))` and `(p * v * m) * local` are the same value in exact
// arithmetic and different values in floats: the concatenated form sums four
// products per element before touching the vertex, the chained form rounds at
// each stage. Either is fine on its own; mixing them between two passes that
// compare depths is not.
//
// Returned as a struct rather than three functions so a caller cannot take the
// world position from here and compute view or clip its own way.
struct CetraObjectPos {
    vec4 world;
    vec4 view;
    vec4 clip;
};

CetraObjectPos cetra_object_position(mat4 m, mat4 v, mat4 p, vec4 local) {
    CetraObjectPos o;
    o.world = m * local;
    o.view = v * o.world;
    o.clip = p * o.view;
    return o;
}
