# Foot locking — notes from Daniel Holden's write-up

Reference notes, in our own words, on the foot-locking technique described at
<https://theorangeduck.com/page/inverse-kinematics-foot-locking> (Daniel Holden,
theorangeduck.com). Written up here because spec 12.4 shipped foot *planting* and hit
exactly the failure modes this method exists to fix. Nothing here is implemented yet.

---

## What it is, and why planting is not it

Planting asks, every frame, "where is the ground under this foot right now?" and solves
the leg onto that answer. It has **no memory**. Locking asks a different question: while
a foot is in contact, it holds one **fixed world-space point** for the whole contact
phase, and the leg solves to that.

The memory is the entire difference. Without it a foot chases the animation and slides,
because the point it is solving to moves with the character; with it the point stays put
while the body travels over it, which is what standing on the ground actually looks
like.

The technique was written for a specific cause of sliding — root motion that has been
scaled or otherwise altered so it no longer matches the distance the animation's feet
travel — but the mechanism applies to any mismatch between where a clip puts a foot and
where the world says it should be.

## Per-foot state

More state than we currently carry. Roughly:

| | |
|---|---|
| `position`, `velocity` | the output foot, and its derivative |
| `inputPosition`, `inputVelocity` | what the clip says, this frame |
| `offsetPosition`, `offsetVelocity` | the correction being blended away |
| `contact` | the frozen world point, valid while locked |
| `locked` | the state flag |
| `time` | since the last transition |

The velocity terms are not decoration — they are what makes the blend continuous rather
than merely continuous-in-position.

## Locking and unlocking, with hysteresis

**Lock** when *both* hold: the clip signals a contact, **and** the output foot is within
`lockDistance` of the clip's foot. The second condition stops a foot locking while it is
still far from where the animation wants it.

**Unlock** when *either* holds: the clip's contact ends, **or** the output foot has drifted
further than `unlockDistance` from the clip's foot.

`unlockDistance` is deliberately **larger** than `lockDistance`. That gap is hysteresis,
and its purpose is to stop the state flipping back and forth frame to frame.

> This is the piece our implementation is missing outright. `plant_fraction` is a single
> threshold with no hysteresis, so the decision is re-made from scratch every frame and
> can oscillate — which is what "scissoring in and out very quickly" looks like.

## Holding the lock

The contact point is captured at the moment of locking from the clip's foot position,
with its height clamped to the ground. While locked, that constant world point *is* the
IK target, regardless of what the root and hips are doing. It persists until an unlock
condition fires.

## Blending in and out: inertialization

Transitions are smoothed with a cubic inertialization rather than a plain lerp. At a
transition the offset captures the discontinuity — in **both** position and velocity —
and that offset decays from 1 to 0 over a blend window through cubic basis weights, the
output being the input plus the decaying offset terms.

Locking captures the difference between the animated trajectory and the static contact;
unlocking captures the reverse, easing the foot back onto the animation.

> Ours eases `applied_target` toward the requested target with a plain position lerp at
> `blend_rate`. That is continuous in position and discontinuous in velocity, so a foot
> arrives at a lock with the wrong speed and has to be dragged straight afterwards.

## How the IK solve relates to it

Unchanged, and that is the point: the leg solver simply receives the state's `position`
as its target. It neither knows nor cares whether that came from a live animation frame
or a frozen contact point. Locking is a layer *above* the solve, not a change to it.

Our `solve_two_bone` already has this shape — it takes a target and nothing else — so
locking would sit in front of it without disturbing the solver.

## Failure modes the author calls out

- **Over-extension, and the trap of fixing it with the hips.** Leg extension is soft-clamped
  with an exponential falloff rather than hard-limited. Crucially, dragging the hips down
  to avoid hyper-extension produces "T-Rex" posturing, and the author's judgement is that
  *"a little bit of sliding is better than breaking the source animation."*

  **This indicts our `max_pelvis_drop` directly.** We drop the pelvis whenever a foot
  cannot reach, which is exactly the remedy being warned against.

- **Lock the toe, not the heel.** The toe is the part in contact roughly nine times out of
  ten; heel-only contact is rare and unstable, and leaving the heel free preserves the
  animation. *Our rig has no toe bone* — the chain ends at `LeftFoot`/`RightFoot` — so
  ankle-locking is all that is available to us, and that is a real limitation rather than
  a simplification.

- **Unreachable locks** are handled by the `unlockDistance` threshold: if the animation
  travels too far from the contact point, the lock simply releases rather than straining
  the IK.

- **Contact annotation is itself a source of error.** Contacts are labelled by thresholding
  toe speed (around 0.1–0.5 m/s) and height, then median/Gaussian filtered. Fast
  animations have short contact windows and label badly, and those errors propagate.

## The algorithm, in order

1. **Offline:** label each animation frame as in-contact or not, from toe velocity and
   height, then filter the labels.
2. **Per frame, per foot:**
   - finite-difference the clip's toe velocity;
   - advance the inertialization toward the current target (frozen contact, or live clip);
   - measure the distance between the output foot and the clip's foot;
   - unlocked + contact signalled + distance < `lockDistance` → **lock**, record the
     contact point, start a transition;
   - locked + (contact ended **or** distance > `unlockDistance`) → **unlock**, start a
     transition.
3. **Solve** the leg chain onto the state's position, then apply height clamping and toe
   orientation on top.
4. Render from the corrected rotations.

---

## What this would mean for `cetra/src/ik.c`

Gaps between this and what spec 12.4 shipped, in the order they bite:

1. **No contact signal.** We infer "should this foot be planted" purely from its height
   above the target. The method wants a per-frame contact label from the clip. We are not
   starting from nothing: `add_footsteps` already fires `step_l` / `step_r` events on the
   walk and run clips, so authored contact *onsets* exist — what is missing is the
   duration of each contact, not its start.
2. **No hysteresis.** One threshold where the method uses two. This is the most likely
   cause of the oscillation observed by eye.
3. **No held contact point.** `applied_target` tracks the ray hit every frame, so it moves
   with the character. Nothing is ever frozen, so sliding is not merely possible — it is
   guaranteed.
4. **Position-only blending.** `blend_rate` lerps a position; inertialization carries
   velocity too.
5. **The pelvis drop is the warned-against remedy**, and should probably become a
   soft-clamp on extension with the drop bounded much harder, or removed.

A foot-locking pass would be its own spec. 12.4's planting is a prerequisite for it — the
ground query, the model-space conversion and the two-bone solve are all reusable as they
stand — but locking is a layer above, with its own state, its own thresholds and its own
blend.
