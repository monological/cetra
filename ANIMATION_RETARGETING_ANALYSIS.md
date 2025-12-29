# Animation Retargeting Analysis

## Executive Summary

The animation retargeting system has a **fundamental flaw**: it computes the rotation delta using **LOCAL** bone rest poses, but the `bone_matrix` formula uses **GLOBAL** inverse bind poses. This mismatch causes incorrect animation direction (backwards walking) and body part distortion.

---

## 1. Data Analysis

### Target Skeleton (raiden_textured_rigged.glb)

```
Armature (0.0°)
  └── root ground (180.0°)  ← quaternion [0, 0.7071, 0.7071, 0]
        └── root hips (180.0°)  ← quaternion [0, -0.6763, 0.7367, 0]
              ├── pelvis (4.9°)
              │     ├── leg left thigh...
              │     └── leg right thigh...
              └── spine lower (178.7°)
                    └── spine middle (10.1°)
                          └── spine upper (13.0°)
                                ├── arm left shoulder 1 (112.1°)
                                └── arm right shoulder 1 (112.1°)
```

**Critical observation**: The bone hierarchy has **multiple consecutive 180° rotations**:
- `root ground`: 180° around axis (0, 0.707, 0.707) - between +Y and +Z
- `root hips`: 180° around axis (0, -0.677, 0.737) - between -Y and +Z
- `spine lower`: 178.7° rotation

### Source Animation (strut_walk.fbx via Assimp)

```
Node hierarchy from Assimp:
RootNode
└── mixamorig:Hips_$AssimpFbx$_Translation
    └── mixamorig:Hips_$AssimpFbx$_PreRotation  (has ~0.74° around X)
        └── mixamorig:Hips_$AssimpFbx$_Rotation  (IDENTITY at rest, animated)
            └── mixamorig:Hips
                └── mixamorig:Spine_$AssimpFbx$_Translation
                    └── mixamorig:Spine_$AssimpFbx$_PreRotation (has ~9.2° around X)
                        └── mixamorig:Spine_$AssimpFbx$_Rotation (animated)
```

**Animation keyframes for Hips_Rotation (first few):**
```
time=0: quaternion [-0.032, 0.013, -0.011, 0.999] ≈ 4.5° rotation
time=1: quaternion [-0.034, 0.015, -0.010, 0.999] ≈ 4.7° rotation
...
```

These are **small deviations** from identity (the Mixamo T-pose rest).

---

## 2. Code Flow Analysis

### 2.1 Skeleton Import (import.c:474-519)

```c
// inverse_bind_pose from Assimp's offset matrix (GLOBAL inverse)
mat4 inverse_bind = GLM_MAT4_IDENTITY_INIT;
copy_aiMatrix_to_mat4(&ai_bone->mOffsetMatrix, inverse_bind);

// local_transform from node hierarchy (LOCAL transform)
mat4 local_transform = GLM_MAT4_IDENTITY_INIT;
copy_aiMatrix_to_mat4(&bone_node->mTransformation, local_transform);

add_bone_to_skeleton(skeleton, name, parent, inverse_bind, local_transform);
```

**Key point**: `inverse_bind_pose` is GLOBAL (includes all parent transforms), but `local_transform` is LOCAL.

### 2.2 Delta Computation (import.c:688-704)

```c
// Source rest pose: assumed identity (Mixamo T-pose)
versor source_rest = {0.0f, 0.0f, 0.0f, 1.0f};

// Target rest pose: extracted from LOCAL transform only!
versor target_rest;
extract_rotation_quat(target_bone->local_transform, target_rest);  // ← LOCAL!

// Delta = target_rest * inv(source_rest) = target_rest (since source is identity)
glm_quat_mul(target_rest, source_inv, channel->rotation_delta);
```

**BUG**: Uses `local_transform` rotation, but should use GLOBAL rest pose rotation.

### 2.3 Bone Matrix Computation (animation.c:656-731)

```c
// Step 1: Apply retargeting to keyframe
if (channel->needs_retargeting) {
    versor corrected_rot;
    glm_quat_mul(channel->rotation_delta, rot, corrected_rot);  // delta * keyframe
    glm_quat_copy(corrected_rot, rot);
}

// Build local transform
glm_quat_mat4(rot, rotation);
// local = T * R * S
glm_mat4_mul(trans, rotation, temp);
glm_mat4_mul(temp, scaling, state->local_transforms[i]);

// Step 2: Propagate global transforms
if (bone->parent_index < 0) {
    global = local;  // Root bone
} else {
    global = parent_global * local;  // Child bone
}

// Step 3: Final bone matrix
bone_matrix = global * inverse_bind_pose;  // ← inverse_bind_pose is GLOBAL!
```

---

## 3. The Fundamental Bug

### Mathematical Analysis

For `root hips` bone:
- **Local rest pose**: 180° (quaternion [0, -0.6763, 0.7367, 0])
- **Parent (root ground) rest pose**: 180° (quaternion [0, 0.7071, 0.7071, 0])
- **Global rest pose**: `parent_global * local` = complex combined rotation

The `inverse_bind_pose` encodes the **inverse of the GLOBAL rest pose**.

When we compute:
```
bone_matrix = global_animated * inverse_bind_pose
```

The `inverse_bind_pose` expects `global_animated` to be relative to the **global** coordinate system, not local.

### What the current code does:

```
delta = local_rest_rotation (180° for root_hips, ignores root_ground's 180°)
local_animated = delta * keyframe
global_animated = parent_global * local_animated
bone_matrix = global_animated * inv(global_bind)
```

For root_hips:
```
global_bind = root_ground_global * root_hips_local
            = 180°_A * 180°_B  (complex composition)

global_animated = root_ground_global * (delta * keyframe)
                = 180°_A * (180°_B * keyframe)
                = 180°_A * 180°_B * keyframe

bone_matrix = (180°_A * 180°_B * keyframe) * inv(180°_A * 180°_B)
            = keyframe  (if axes align)
```

This looks correct! But the issue is more subtle...

### The Real Problem: Axis Transformation

When `delta * keyframe` is computed:
- `delta` = 180° around axis B
- `keyframe` = small rotation around axis X (Mixamo's forward tilt axis)

The result `delta * keyframe` rotates around a **transformed** axis. This is pre-multiplication, which applies delta AFTER keyframe. But we want to apply keyframe in the Mixamo coordinate system, then transform to target coordinates.

The correct formula should be conjugation:
```
corrected = delta * keyframe * inv(delta)
```

This applies `keyframe` in its original coordinate system, then transforms the result.

**BUT** - this still doesn't account for the parent chain!

### The Complete Solution

The delta should be based on **global** rest poses:
```c
// Compute global rest pose for target bone (accumulate parent chain)
mat4 global_rest_target;
compute_global_transform(skeleton, bone_index, global_rest_target);
versor global_rot_target;
glm_mat4_quat(global_rest_target, global_rot_target);

// Source global rest is identity (Mixamo root at origin with identity rotation)
versor global_rot_source = {0, 0, 0, 1};

// Global delta
versor global_delta;
glm_quat_mul(global_rot_target, glm_quat_inv(global_rot_source), global_delta);
```

Then the retargeting formula becomes:
```c
// Transform keyframe from source global to target global coordinates
versor corrected = global_delta * keyframe * inv(global_delta);

// OR simpler: just use the keyframe as-is if we compute bone_matrix differently
```

---

## 4. Alternative Approach: No Retargeting Formula Needed

The cleanest solution is to **not modify the keyframe at all**, but instead:

1. Apply the animation keyframe as the LOCAL transform (replacing local_transform, not composing)
2. Propagate globals normally
3. Use a **different inverse_bind_pose** that accounts for the coordinate system difference

This is how most game engines handle it:
```c
// Don't compose with delta - just use keyframe directly
local_animated = T * keyframe * S;

// Globals propagate normally
global_animated = parent_global * local_animated;

// Bone matrix uses identity as bind if animation is for different skeleton
bone_matrix = global_animated * inv(source_global_bind);
```

The `source_global_bind` would be identity for Mixamo (their T-pose is identity).

---

## 5. Proposed Fix

### Option A: Fix Delta to Use Global Rest Pose

```c
// In import.c, compute global rest pose for target bone
static void compute_bone_global_rest(Skeleton* skel, int bone_idx, mat4 out) {
    if (bone_idx < 0) {
        glm_mat4_identity(out);
        return;
    }
    Bone* bone = &skel->bones[bone_idx];
    if (bone->parent_index < 0) {
        glm_mat4_copy(bone->local_transform, out);
    } else {
        mat4 parent_global;
        compute_bone_global_rest(skel, bone->parent_index, parent_global);
        glm_mat4_mul(parent_global, bone->local_transform, out);
    }
}

// Then in delta computation:
mat4 global_rest;
compute_bone_global_rest(skeleton, bone_index, global_rest);
versor global_rot;
glm_mat4_quat(global_rest, global_rot);

// For Mixamo source, global rest is also identity
versor source_global = {0, 0, 0, 1};

// Compute global delta
glm_quat_mul(global_rot, source_global_inv, channel->rotation_delta);
```

### Option B: Use Animation-Relative Inverse Bind

Store a separate `animation_inverse_bind` that's identity for retargeted animations:
```c
if (channel->needs_retargeting) {
    // Use identity as bind reference (Mixamo's coordinate system)
    mat4 identity = GLM_MAT4_IDENTITY;
    glm_mat4_copy(identity, animation_inverse_bind[bone_index]);
} else {
    glm_mat4_copy(inverse_bind_pose, animation_inverse_bind[bone_index]);
}

// Then in bone_matrix:
bone_matrix = global_animated * animation_inverse_bind;
```

### Option C: Pre-bake Coordinate Transform into Animation

Load the animation keyframes and transform them once at load time:
```c
// For each rotation keyframe:
versor source_to_target = compute_axis_remap_quaternion(...);
versor transformed_key;
glm_quat_mul(source_to_target, original_key, transformed_key);
glm_quat_mul(transformed_key, glm_quat_inv(source_to_target), transformed_key);
// Store transformed_key instead of original_key
```

---

## 6. Recommended Solution

**Option A** (Fix Delta to Use Global Rest Pose) is the most correct approach because:

1. It fixes the root cause (local vs global mismatch)
2. It maintains the existing code structure
3. It works with the bone hierarchy properly

### Implementation Steps:

1. Add helper function to compute global rest pose for a bone
2. Modify `process_ai_animations_internal()` to use global rest pose for delta
3. Keep the rest of the bone_matrix computation unchanged
4. Test with the strut_walk animation

---

## 7. Debug Verification

After implementing the fix, verify with this debug output:

```c
// In compute_bone_matrices, after computing bone_matrix:
if (i < 5) {
    versor bm_rot;
    glm_mat4_quat(state->bone_matrices[i], bm_rot);
    float angle = 2 * acosf(fabsf(bm_rot[3])) * 57.2958f;

    // Get original keyframe angle
    AnimationChannel* ch = get_channel_for_bone(anim, i);
    versor key;
    interpolate_rotation(ch->rotation_keys, ch->rotation_key_count, time, key);
    float key_angle = 2 * acosf(fabsf(key[3])) * 57.2958f;

    printf("Bone %zu: keyframe=%.1f°, bone_matrix=%.1f° (should match!)\n",
           i, key_angle, angle);
}
```

**Expected result**: `bone_matrix` angle should approximately equal `keyframe` angle for all bones.

---

## 8. Files to Modify

| File | Change |
|------|--------|
| `cetra/src/import.c` | Add `compute_bone_global_rest()`, modify delta computation to use global rest |
| `cetra/src/animation.c` | Possibly simplify retargeting (just use delta directly, no conjugation) |

---

## 9. Summary

The animation plays backwards because:

1. **Delta uses LOCAL rest pose** (180° for root_hips)
2. **But root_ground also has 180° rotation** that's not accounted for
3. The composition of these 180° rotations creates a complex global transform
4. **inverse_bind_pose is GLOBAL** and expects global coordinates
5. The mismatch between LOCAL delta and GLOBAL inverse_bind corrupts the animation direction

**The fix**: Compute delta from GLOBAL rest poses, not LOCAL.

---

## 10. Implementation Attempts and Failures

This section documents the various approaches tried and why they failed, to prevent repeating the same mistakes.

### Attempt 1: Pre-multiply with LOCAL delta

```c
// delta = local_rest (e.g., 180 deg for root_hips)
corrected = delta * keyframe;
```

**Result**: T-pose looked correct, but animation played BACKWARDS.

**Why it failed**: Pre-multiplication applies delta's rotation AFTER keyframe. This changes the rotation axis incorrectly. For a 180 deg delta and small keyframe, the result is approximately 180 deg - which means the bone ends up at its rest pose but the animation direction is inverted.

### Attempt 2: Post-multiply with LOCAL delta

```c
corrected = keyframe * delta;
```

**Result**: Same as Attempt 1 - backwards animation.

**Why it failed**: Same fundamental issue. Quaternion multiplication order affects axis transformation, but the core problem is that we're composing rotations when we should be transforming coordinate systems.

### Attempt 3: Global delta with pre-multiply

```c
// delta = global_rest (accumulates parent chain)
// For root_hips: global = root_ground(180) * root_hips_local(180) = ~175 deg
corrected = delta * keyframe;
```

**Result**: bone_matrix showed ~178 deg when keyframe was only ~6 deg.

**Why it failed**: The math assumption was wrong. I assumed:
```
bone_matrix = global * inv(bind)
            = (delta * keyframe) * inv(delta)
            = keyframe
```

But this only works for ROOT bones. For child bones, the parent's animated transform is also involved, and the formula doesn't simplify correctly.

### Attempt 4: Global delta with post-multiply

```c
corrected = keyframe * delta;
```

**Result**: Same massive bone_matrix angles (~178 deg).

**Why it failed**: Same issue as Attempt 3. The fundamental assumption about how bone_matrix simplifies was incorrect.

### Attempt 5: Conjugation with LOCAL delta

```c
// Conjugation: transforms rotation axis from source to target coordinate frame
corrected = delta * keyframe * inv(delta);
```

**Result**: (Testing in progress)

**Why this might work**: Conjugation preserves the rotation MAGNITUDE while transforming the AXIS. A 6 deg keyframe becomes 6 deg in the target's local coordinate system.

**Why this might still fail**: Conjugation alone produces a rotation relative to identity. But the target bone's rest pose is NOT identity - it's `local_rest`. So we may need to compose with rest pose after conjugation.

### Attempt 6: Pre-multiply with LOCAL delta (compose with rest)

```c
// Apply keyframe ON TOP OF rest pose
corrected = local_rest * keyframe;
```

**Result**: (Testing in progress)

**Why this might work**: In Mixamo, rest=identity, so keyframe IS the absolute rotation. In target, rest=local_rest, so we apply keyframe relative to that rest pose.

---

## 11. Why The Initial Analysis Was Wrong

The initial analysis correctly identified that:
1. `inverse_bind_pose` is GLOBAL
2. `local_transform` is LOCAL
3. There's a mismatch

But the proposed solution was flawed because it conflated two different problems:

### Problem 1: Coordinate System Mismatch
The animation keyframe defines rotation in Mixamo's local coordinate system. The target bone has a DIFFERENT local coordinate system (rotated by `local_rest`). To apply the same rotation, we need to transform the keyframe's rotation axis.

**Solution**: Conjugation - `rest * keyframe * inv(rest)` transforms the axis.

### Problem 2: Rest Pose Difference
Mixamo's rest pose is identity. Target's rest pose is `local_rest`. The animation keyframe is a rotation FROM rest, not an absolute rotation.

**Solution**: Apply keyframe on top of rest - `rest * keyframe`.

### The Confusion

I confused these two problems. When I used GLOBAL delta, I was trying to solve Problem 2 at the global level, but:
1. The bone_matrix formula already handles global transforms correctly
2. What we need to fix is the LOCAL rotation, not the global one

Using GLOBAL delta essentially "double-counts" the parent contributions that are already handled by the parent-child transform propagation.

### The Correct Approach

For retargeting, we should work entirely in LOCAL space:

1. **Extract LOCAL rest pose** from target bone's `local_transform`
2. **Transform keyframe axis** using LOCAL rest: either conjugation or simple composition
3. **Let the engine propagate** global transforms normally

The global `inverse_bind_pose` is already consistent with how globals are propagated, so we shouldn't try to "fix" it at the retargeting stage.

---

## 12. Key Insights

1. **Quaternion multiplication is NOT commutative**: `a * b != b * a`. Order matters.

2. **Pre-multiply vs Post-multiply**:
   - `delta * keyframe`: applies keyframe first, then rotates by delta
   - `keyframe * delta`: applies delta first, then rotates by keyframe

3. **Conjugation** (`a * b * inv(a)`): transforms b's rotation axis by a's rotation, preserving b's angle.

4. **Animation keyframes are relative to REST**, not absolute. Mixamo rest = identity, so keyframes look absolute, but they're actually "rotate X degrees from rest".

5. **Don't mix coordinate spaces**: If using LOCAL delta, apply it locally. If using GLOBAL delta, you need to restructure the entire bone_matrix computation.

6. **Parent chain is already handled**: The `global = parent * local` propagation already accounts for parent transforms. Trying to "pre-apply" parent rotations in the delta leads to double-counting.

---

## 13. Current Status

Testing LOCAL rest pose with simple composition: `corrected = local_rest * keyframe`

This interprets the animation as "rotate by keyframe from rest pose" and applies that in the target's local coordinate frame.
