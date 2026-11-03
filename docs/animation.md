# Animation

Skeletal animation, from a clip to the skinning matrices: sampling clips into poses, what plays and
how it blends, root motion and stride matching, the state graph that decides what plays, IK and foot
locking, a head turned toward a point, and ragdolls. [`foot-locking.md`](foot-locking.md) has the
foot-locking method and its measurements; this doc links to it rather than repeating it.

## The pose seam

Each frame an animation state goes through one fixed order (`animation.c`, the update that builds
`bone_matrices`):

1. The animator samples and blends its sources into a pose: per-bone translation, rotation, scale.
2. Locals are built from the pose and globals accumulated parent first. These model-space globals
   are the one place per-bone positions exist.
3. Either the ragdoll writes the globals, or, when no ragdoll is active, spring bones, then the IK,
   then the look-at correct them, in that order.
4. The skinning matrices are built from the globals.

Everything in step 3 writes `global_transforms` and nothing else. A bone no clip drives has
`driven[]` clear, and a rotation written into the pose for it is discarded, so a correction written
there would vanish. Each stage keeps its own place for a reason given in its section below.

## Clips and poses

`animation.c/h` holds clips, skeletons and the pose (spec 12.1).

- `animation_sample_pose` evaluates one clip into per-bone TRS, with its retargeting and hierarchy
  in per-call scratch.
- `pose_blend` mixes two poses.
- `animation_state_apply_pose` turns a pose into `bone_matrices[128]`, and keeps the previous pose
  for TAA motion vectors. That previous pose goes up as three affine rows a bone
  (`uPrevBoneRows` in `include/skin.glsl`): a second `mat4[128]` beside `boneMatrices` does not fit
  the vertex stage's uniform budget and the program fails to link.

A bone no clip drives copies its bind matrix rather than being rebuilt from a decomposed bind, which
keeps the single-clip path bit-identical.

**A retargeted bone takes its position from the bind pose**, whatever the keys hold. The pose that
plays is in place however far the rig the clip was authored on travelled.

### What a clip means: stride speed and root travel

Two functions ask what a clip means rather than what it holds. Both answer in model units and both
refuse rather than guess.

**`animation_stride_speed`** (spec 12.10) is the ground speed the clip's own feet imply, from a loop
sampled at 60 Hz:

- A foot is standing while it is in the lower half of its own vertical range.
- The answer is the median horizontal velocity of the toe over those samples. A median, not a fit
  over a window, because the samples need not be contiguous: on a rig whose hip translation the
  retarget replaced, a real stance splits into several runs, and a window has to be found before it
  can be used, which can be found wrong.
- The two feet must agree in direction and magnitude; the published answer is weighted by how long
  each stood.
- It refuses a clip that does not walk. A straight-leg pendulum has both feet lowest at the same
  instant swinging opposite ways, so their low samples carry both directions and median to nothing.
  That refusal is a result, not a gap.

**`animation_root_travel`** (spec 12.18) asks the other way: not what the feet imply about the
ground but what the root states about the body, read off the clip's root channel at its first tick
and its last. It refuses:

- a clip that states no travel, which is most in-place clips;
- a retargeted root channel. Since the pose that plays takes a retargeted bone's position from the
  bind pose, answering from the keys would hand a character a distance measured on another skeleton
  in another rig's proportions, which no frame of the animation shows.

A stated travel is not proof the clip goes anywhere: `touch_down` and `jump_start` each carry about
9 cm of hip between their ends, a crouch and a rise. Read the answer with the clip in mind.

`animation_pose_pin_root` is root travel's other half: it puts the root back at its bind position
and heading once the travel has been handed to the character, or the character moves twice, once as
a body and once as a mesh sliding off its own capsule.

## What plays: the animator

`animator.c/h` (spec 12.1) is what plays on a skeleton and how one thing becomes another:

- **A phase-synced 1D blend space.** Every entry runs on one clock, so a foot planted 40% through the
  walk is planted 40% through the run.
- **A crossfade** that drops the outgoing source when it completes, so the settled pose is exactly
  the incoming one.
- **One bone-masked override layer** (`animator_play_layer`, masks from `animator_mask_subtree`),
  with its own fade in and out; a one-shot releases itself at its end.
- **Clip events** (`animation_add_event`), fired from the base's highest-weighted entry and from the
  override clip through one callback, after the frame's pose is applied, so a handler may call
  `animator_play`.

A clip's clock is in its ticks; fades and `dt` are in seconds, unscaled by `speed`.

There is no state machine in the animator. A game decides what plays and when, as code or as data
through the state graph below, which is a layer over the animator and not a change to it.
`graph-identity` holds that: a one-state graph is bit-identical to calling the animator directly.

**Phase sync is a trap for clips of very different length.** A two-tick rest pose beside an 86-tick
walk turns the shared clock 43 times too fast for the walk. A held frame has no phase to lose, so
give it the other entry's period.

## Root motion and stride matching

These are two answers to one problem, a body moving at a speed its feet do not show, from opposite
sides. Stride matching scales playback until the clip keeps up with the body; root motion moves the
body exactly as the clip says. Both live in the animator because only it knows each source's clock,
its wrap and its weights.

### Stride matching

An entry carries a `stride` (spec 12.10): the ground it implies, in the units of the caller's own
parameter axis, measured by `animation_stride_speed` from a clip and a rig. `animator_stride_speed`
blends it.

**The blend is a product, not a mean.** The space keeps one clock, so the blended pose lays down
`sum(w * stride * seconds)` of ground per turn of the loop while the clock turns the loop at
`sum(w / seconds)`. That collapses to the weighted mean of the strides only when every clip is the
same length, which a walk-and-run space is not, and a game using the mean is wrong by an amount that
reads as a tuning problem.

Divide the speed a game wants to travel at by `animator_stride_speed` and write it to `speed`. That
is stride matching, and it is what makes foot locking work at a speed other than the clip's.

**A stride of 0 means the entry lays down no ground.** That is true of a standing clip and is also
what an unmeasured entry carries. The two are deliberately not distinguished: an idle at 0 is what
lets a speed axis reach zero, and only the caller knows which of its entries were meant to move.

### Root motion

The animator extracts root motion (spec 12.18; `animator_root_motion`, `animator_take_root_motion`).
Four things about it are easy to get backwards:

- **It is read per entry, never from the blended pose.** The weights and the fade move too, so
  differencing a blended root reads a knob turn or a crossfade as a stride across the room.
  `anim-root-fade` is the arm that tells the two designs apart.
- **A wrap adds back exactly what that entry stated for one loop**, which is the whole correction;
  the shared clock is what makes one wrap flag enough.
- **A switch rebases rather than differencing, and drops whatever nobody took.** Otherwise a source
  that stopped travelling hands its hoard to the next one in a single step, as a lurch.
- **What a caller takes is a displacement that the take zeroes, not a velocity.** The animator ticks
  once per rendered frame over however many fixed steps that frame ran, and a controller takes a
  velocity per step; handing a distance over exactly once conserves it under any pacing.

**The pose is pinned only where a source states a travel** (`animation_pose_pin_root`). An in-place
clip is free to swing and counter-rotate its hips.

## The state graph

`anim_graph.c/h` (spec 12.20) decides what plays and when, as data. `animator.h` states that the
animator has no state machine; the graph closes that from outside, as a layer over it.

A graph is a table of **states**, each naming a playback source the app registered
(`anim_graph_add_source`) plus the parameters driving its blend axis and its rate, and a table of
**transitions** over a closed condition vocabulary. Transitions are one table, evaluated in one
order, first match wins.

What a graph holds is a parameter block, a current state and how long it has been in it. What is
playing stays the animator's: two records of one fact is how a load restores one and not the other.

The game layer's `ANIMATOR` component ticks the graph and then the animator, once per rendered
frame; [`game-framework.md`](game-framework.md) has why that order and cadence matter.

### The rules in the header

Each of these is something a reader would otherwise rediscover by watching a character behave
wrongly:

- **A row into the state already current is never taken.** Otherwise an interrupt that stays true
  re-issues its own play every tick and the clip never leaves its first frame.
- **At most one row a tick, no chaining.** Otherwise a state is reached whose entry conditions were
  never true, and every link after the first freezes what was on screen.
- **`from == NULL` is for interrupts only.** An any-state row's condition is a superset of the
  specific rows' preconditions and eats them on their first frame, so a fallback must name its
  `from`.
- **FINISHED is latched on entry and never read live.** `animator_finished` is a per-frame edge that
  never re-arms, so a row ANDing it with anything else would lose it the first frame the other
  condition failed, and park the graph on a held clip forever.
- **A FINISHED row out of a RETURN state is refused**, since the animator resumes on the very frame
  it fires, and the two would be two writers in one frame.

**RETURN is a kind, not a spelling of EXIT.** `animator_play_space` builds a fresh space, so a row
back into a locomotion state restarts its clock, where the animator's resume restores the outgoing
space including its time. Restarting teleports the walk's phase and every foot lock riding it. A
RETURN state needs no row out and must not be given one; the graph follows the animator back.

### Binding

`anim_graph_bind` resolves every state against the rig actually loaded:

- **A source whose clip is missing refuses to register.** An app finds its clips by name on whatever
  rig it was handed, so a missing one arrives as a NULL. The refusal makes that state unreachable,
  every row into it is pruned, and the set is reported once by name. That is the feature: it
  replaces a clip-presence guard on every transition.
- **A state that can be entered and never left is warned about.**
- **A table is refused whole**, logged by name, for an unresolvable `start`, a row whose `from`
  equals its `to`, a row naming a state or parameter that does not exist, an op a parameter's kind
  cannot answer, or a FINISHED row out of a RETURN state.
- Binding a second graph to one animator stands the first down.

**The graph drives the base layer only.** The override layer's mask is a bone name,
`animator_layer_finished` is a latch where `animator_finished` is an edge, and there is exactly one
layer.

No GL, no physics, no clock and no `game/` type. It names one engine type, `Animator`, because a
machine over playback has to ask what is playing. A graph binds to a NULL animator and a state may
name no source, which lets most of its arms run with no window, no rig and no clip.

## IK and foot locking

`ik.c/h` (specs 12.4 and 12.9) is two-bone IK for foot planting and locking: the law of cosines over
a hip, knee and ankle chain.

### Where it runs

It runs after the spring bones, between the globals being accumulated and the skinning matrices
being built. After, because the springs' pass re-accumulates every unsimulated bone from its parent
and would erase a solve written before it. It writes globals only, for the reason in
[The pose seam](#the-pose-seam).

### The bend plane

The bend plane comes from a pole carried in the hip's own frame, not from the current triangle: a
bind pose that is exactly straight leaves that triangle degenerate on nearly every frame.

**The pole is taken from the rig first.** A bind knee sits off the hip-ankle line by the amount and
in the direction its author meant it to bend, so the perpendicular part of that offset is the pole.
`ik_add_foot`'s `knee_forward` is the fallback for a rig that binds straight. It cannot be the first
choice because it is in the hip's frame, and hip frames differ between riggers: the `+Z` that suits
the generated puppet points backwards on a humanoid whose thigh binds 178 degrees away from it, and
the knee folds under on every stance frame with every chain resolved and nothing warning.

### Targets and planting

**Two target setters, and the difference matters.** `ik_foot_set_target` takes a point for the
ankle. `ik_foot_set_ground` takes the surface and adds `sole_offset`, the ankle's clearance above its
own sole, derived once from the bind pose at `ik_add_foot`. One setter with the offset folded in is
wrong: most call sites pass arbitrary geometry (a reach target, a full extension, an exact distance
for a closed form), which the offset would displace. A target and a ground are different claims and
only the caller knows which it holds.

**Whether a foot is planted is decided here, never by the caller.** Only here are the globals still
the clip's pose; a caller reading them sees its own last solve, so a planted foot reads as planted
and the feet weld to the ground.

`max_pelvis_drop` is a fraction of the asking foot's leg length, not metres, so it carries to a rig
of another size, in the same units as `plant_fraction`.

### Foot locking

Locking is a layer in front of the solve, not a change to it: the solver receives a point and does
not know whether it came from a live ray or a contact frozen forty frames ago. A contact is labelled
from the ankle's lift and vertical speed, pinned at the toe in world space, and held until the label
ends or the clip has carried the foot past `unlock_distance`, which is larger than `lock_distance`;
that gap is the hysteresis. [`foot-locking.md`](foot-locking.md) has the method, why detection is
at the ankle, the inertialized blend, and where the implementation deviates.

Rules a caller and a maintainer need:

- **`ik_set_world` is what a caller owes** each frame. Without it a held point rides along with the
  character and the lock does nothing.
- **The contact is captured after the solve.** A point taken from the clip's pose is somewhere the
  foot is not yet, and the lock spends its first ticks dragging the foot onto it.
- **A locked foot takes its weight unfaded.** `plant_fraction` answers "is this foot down" from
  height alone; the contact label is better and stricter.
- **Everything is in model space and fractions of the leg.** A scale reaches the world matrix and
  nothing the solver decides, so the same walk on a node scaled 1 and 2 comes out identical;
  `ik-scale` holds that. A world distance compared against a model threshold silently scales the
  threshold by the node's scale, and no arm at scale 1 can see it.
- **A lock inside its unlock distance holds the foot still whatever the body is doing**, so slide
  alone does not show locking works: a body moving at half the clip's speed leaves the foot still
  while the leg is hauled from the pose. The distance between the solved ankle and the one the
  animator asked for shows it; `ik-slide-speeds` reads that.
- **The exponential soft-clamp on extension is refused.** On a bind pose that is exactly straight
  the knee angle near full extension goes as the square root of the shortening, so a small band
  costs a large permanent bend. That is a property of this rig, not the method: re-measure on a rig
  that binds bent.

## Look-at

`look_at.c/h` (spec 13.17) turns a head toward a point. It lives in the animation state's `look_at`
slot and is solved right after the IK, which may drop the pelvis the whole neck hangs from, inside
the non-ragdoll branch, writing globals only.

- **A chain of up to four bones, root first.** Each takes its share of what is left of the turn, read
  off the aim after the bones above have turned, so the last bone lands the aim exactly.
- **The turn is a yaw about the rig's up and then a pitch about the level axis across the new aim,
  never the shortest arc.** The shortest arc rolls the head on any target that differs in both
  heading and elevation; a cocked head is something to author, not a by-product.
- **Limits are measured from the chain parent's forward** (`max_yaw`, `max_pitch`, radians).
- **The gaze is eased as a world direction**, so a body turning under a steady gaze leaves the head
  on its target.
- **Past `give_up` it lets go**, and takes the target back only within `max_yaw`, so a target on the
  boundary does not flick.
- **Nothing runs at weight zero**, so an installed system with no target leaves every pose
  bit-identical.
- **`look_at_set_world` is the caller's to owe each frame**, as `ik_set_world` is.

## Ragdolls

`ragdoll.c/h` and `ragdoll_jolt.cpp/h` (spec 12.16) let a rig fall over: where animation and
physics meet. A ragdoll starts from the live pose, so the heap continues the character's motion, and
writes back at the seam the IK uses.

**It replaces the pose rather than correcting it**, so it is an alternative to the springs and the
IK, not a stage after them. While one is active neither runs: both are stateful integrators, and a
spring building velocity against a pose nothing draws, or a foot lock held on ground a falling body
stopped standing on, hands that state back the moment the ragdoll ends.

### Building the bodies

**One humanoid, described once.** `RAGDOLL_HUMANOID[]` carries each slot's name, its parent, how its
bone is found and the joint limits it bends by.

The capsules are built from the bind pose:

- **Length** runs from a bone to the next simulated joint, not the skeleton's first child, which is
  the wrong answer once a rig carries a `pelvis` on its hips or a `sword` in its shoulder.
- **Radius** comes from `Mesh.bone_aabb`, the per-bone boxes kept for culling, unioned over every
  mesh skinned to the rig and over every bone the body carries: its own plus each descendant down to
  the next simulated joint, which is the set `ragdoll_apply` moves rigidly with it. Both unions
  matter on an imported character: a rig arriving as fifteen meshes measures nothing for most of its
  body if asked about one, and a hips bone holding no weights measures nothing at all. The failure
  is not a refusal but thin uniform sticks and a tiny pelvis, a legal ragdoll that melts.
  `ragdoll-mass` bounds the spread of masses at 100x.

**Two bones are found by walking, not by name.** `categorize_bone` gives one category to a whole
chain (`BCAT_SPINE` covers spine and chest, `BCAT_HEAD` covers `Head` and `HeadTop_End`), and the
semantic matcher answers only when exactly one bone matches, so on a segmented spine or a Mixamo
head it refuses. `resolve_by_category` asks the hierarchy instead, through `skeleton_mark_subtree`.

**The body count is a measurement, not a constant**: twelve on a full humanoid, eleven on a
one-segment spine. `ragdoll-build` asserts structure instead (every bone resolved, exactly one root,
every parent earlier in build order than its child, one constraint per body but the root, with the
parent column read back from the built system) and reports the count.

### Frames and scale

**Model space, scaled once.** The node scale has to come back out of the basis on the way back:
`to_model` carries 1/scale and a Jolt body carries none, so mixing them skins a character at half
size, which reads as an explosion rather than a scale bug.

**The capsule basis must be right-handed.** A capsule is radially symmetric, so the shape cannot say
which way its X and Z point, but Jolt keeps orientation as a quaternion: a left-handed basis is a
reflection, no quaternion is one, and `GetQuaternion` returns the nearest rotation instead.
`glm_mat4_inv` inverts a reflection fine, so the round trip closes on the C side and only what comes
back from Jolt is wrong: a leg end for end, every limb offset by its own length. Nothing that
measures one body sees it, which is why `ragdoll-roundtrip` and `ragdoll-rigid` measure the
relationship between two.

**The constraint anchor belongs at the limb's head**, half a limb back along the twist axis from
`world`, which places the capsule's middle.

### The Jolt half

`ragdoll_jolt.cpp` is a one-TU C++ escape like `cluster_build.cpp` and `physics_cook.cpp`, in
`cetra/src/` root because the build globs C++ only there.

- **It names nothing from the game layer.** `ragdoll_start` takes the opaque `JPC_PhysicsSystem*`
  and the object layer the caller already holds, not a `PhysicsWorld`: an engine file including
  `game/` would be the only such include in the tree, and a layer number restated in C++ would be a
  second copy of a decision the broadphase owns.
- **It refuses rather than asserts.** Parents must precede children (`CreateRagdoll` indexes the
  parent's body while building the child), only part 0 may be a root, and a degenerate capsule is
  named, since `CapsuleShape` asserts its dimensions and Jolt's default trace is a breakpoint.
- **`SwingTwistConstraintSettings` is not a free choice**: it is the only type
  `DriveToPoseUsingMotors` accepts, which keeps a getting-up feature possible.
- **Removal precedes release.** `~Ragdoll` destroys its bodies without removing them, so dropping the
  last reference while they are still added takes the process down.
