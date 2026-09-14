# Foot locking — notes from Daniel Holden's write-up

Reference notes, in our own words, on the foot-locking technique described at
<https://theorangeduck.com/page/inverse-kinematics-foot-locking> (Daniel Holden,
theorangeduck.com). Written up here because spec 12.4 shipped foot *planting* and hit
exactly the failure modes this method exists to fix. **Spec 12.9 implemented it** and **spec
12.10 gave the demo a travel speed its clips could carry** — the last two sections say what
shipped, where the shipped form deviates and why, and what it took to make the feature
visible at all. Everything between here and there describes the METHOD, and is left as it was
written.

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

> **Was:** `plant_fraction` was a single threshold with no hysteresis, so the decision was
> re-made from scratch every frame and could oscillate. Spec 12.9 added the pair; see the
> last section.

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

> **Was:** `applied_target` eased toward the requested target with a plain position lerp at
> `blend_rate` — continuous in position and discontinuous in velocity. `blend_rate` is gone;
> see the last section for what replaced it and for the one thing the replacement needs.

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

  **This indicted our `max_pelvis_drop` directly**, and 12.9 halved it and left the foot
  short past the cap — but REFUSED the soft clamp, on arithmetic peculiar to a rig that binds
  straight. Both are in the last section.

- **Lock the toe, not the heel.** The toe is the part in contact roughly nine times out of
  ten; heel-only contact is rare and unstable, and leaving the heel free preserves the
  animation. *Our rig had no toe bone* — the chain ended at `LeftFoot`/`RightFoot` — which
  spec 12.9 fixed by generating one, and the lock holds there. Contact is still DETECTED at
  the ankle, for a measured reason the last section gives.

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

## What it cost, and where it deviates (spec 12.9)

**Implemented.** The five gaps below are closed; each entry says how, and where the shipped
form differs from the method above. The numbers are `ik-slide`'s: a stance foot travels
**0.0034 m** across the ground where planting left **0.0783 m**, over the same ticks, with the
body moving at the speed the clip's own feet imply.

1. **The contact signal is derived at RUNTIME, from the pose at the solve seam, and judged at
   the ANKLE.** The method labels contacts offline from toe speed and height. Two deviations,
   both forced:
   - Our clips carry no root motion — the character controller moves the entity — so a stance
     foot's model-space horizontal speed is the walk speed rather than zero, and the horizontal
     term thresholds to nothing. Height and vertical speed say the same thing about a foot set
     down and not yet picked up, and need no authored data and no baking step.
   - Judged at the toe, as the method has it, the label found **6 contacts over 3 cycles**: on a
     clip retargeted onto a rig of other proportions the toe JOINT passes back through its own
     bind clearance in mid-swing (0.0390 against a stance 0.0137), slowly enough that the speed
     test admits it too. The ankle reads 0.12 airborne against 0.085 planted and separates
     cleanly. The toe is still what a lock HOLDS — that is about where the contact is, not how
     it is found.
2. **Hysteresis.** `lock_distance` 0.15 of a leg and `unlock_distance` 0.45, the larger by
   design, and an inverted pair is refused by name rather than behaving as one threshold.
   `ik-hysteresis` asserts one pin per walk cycle — a flapping LOCK; `ik-contact`'s
   `runs == cycles` is the same shape for a flapping LABEL, and the two are different
   quantities. **Both distances are compared in MODEL space**, which they were not at first:
   testing the held contact against the live toe in WORLD space silently multiplies the
   threshold by any scale on the rig's node, and gametest carries `PLAYER_SCALE 2`, so the
   documented 0.45 was behaving as 0.225. No arm could see it — the probe runs at scale 1.
3. **The held contact point.** `contact_world`, frozen in WORLD space — `ik_set_world` is what
   the caller owes for that. Captured AFTER the solve rather than before: taken from the clip's
   pose it names somewhere the foot is not yet, and the lock then spends its first ticks
   dragging the foot onto it, which is travel across the ground and reads as the slide. Taken at
   the end of the frame the decision is made, the pin moves nothing. That also retires the
   method's "height clamped to the ground" step — by then the solve has already put the ankle on
   the ground it was handed.
4. **Inertialization.** `blend_rate` became `transition_time`: a cubic Hermite from the captured
   offset, in position AND velocity, to zero. The half worth naming is that it FINISHES — once
   decayed the applied target is the requested one exactly, where an exponential ease against a
   moving target sits a constant distance behind for as long as the motion lasts (0.039 m here,
   which was the whole residual). It needs the standard guard: the velocity term may not carry
   the output further from the target than the position offset already is.

   Two things about it are easy to get wrong and both were, at first. The input velocity must
   be differenced from the CALLER's target and not from the resolved one — differenced across
   the very discontinuity being captured, it is the jump over one tick, the guard binds every
   single time at exactly `transition_time / dt`, and the blend degenerates into a fixed
   reshaping of the position curve carrying no measured velocity at all. And the ease this
   replaced was also filtering the caller's target every frame; nothing does now, so an unlocked
   foot follows a noisy ground query unfiltered and `teleport_distance` is the only remaining
   guard. That is a consequence of exact tracking rather than an oversight, but it is the
   caller's to know about.
5. **The pelvis drop is demoted, not removed**, and the extension soft-clamp is **REFUSED**.
   `max_pelvis_drop` is halved to 0.31 of a leg — set from the content, being the deepest thing
   the demo world asks — and `ik-drop` exercises the cap and the refusal under it, which spec
   12.5 recorded nothing had. The soft clamp loses on arithmetic this rig makes brutal: a band
   has to begin below full extension to smooth anything, and near full extension the knee angle
   goes as the SQUARE ROOT of the shortening, so 1 per cent of band costs 16.2 degrees of
   permanent bend, 2 per cent 23.0 and 5 per cent 36.4. What it buys is continuity in the
   ankle's VELOCITY as a target leaves reach; the position is already continuous. **That is a
   property of a bind pose that is exactly straight, not of the method** — on a rig that binds
   bent the band costs a fraction of it, so re-measure rather than inherit the refusal.

### What the demo could not show, and what fixed it (spec 12.10)

**Was:** `gametest`'s player moved at `PLAYER_SPEED` 10 m/s on a rig whose leg is 0.82 m and whose
clip implies a 0.95 m/s stride. At about ten times its animation's speed the contact label never
fired, no lock ever formed, and the frame was **0 px** against `--no-lock` — the mechanism behaving
correctly, there being no contact to hold, and the demo rather than the feature failing to show it.
Walked at a stick deflection of 0.12 the same frame moved 52,244 px. The note ended: *a game that
wants this benefit has to match its travel speed to its clips' stride, or carry root motion;
neither is in this engine.*

**Spec 12.10 did the first of those.** `animation_stride_speed` measures the ground speed a clip's
own feet imply, `animator_stride_speed` blends it across a space, and a game divides the speed it
wants to travel at by the answer to get a playback rate. `gametest`'s locomotion entries now sit at
the world speeds they imply, its knob is metres per second, and full stick is derived from the
fastest clip instead of written down. On `t_pose.fbx` + `strut_walk` that is 2.67 m/s, and against
`--no-lock` the frame moves **2217 px** at full stick where it was 0.

Three things that spec learned about the feature, which belong here rather than there:

- **A lock inside its `unlock_distance` holds the toe perfectly still whatever the body is doing.**
  So SLIDE alone does not measure whether locking is working: at half the clip's speed the foot does
  not travel at all while the leg is hauled a third of a metre away from the pose. That is the
  T-Rex posture this page records the method warning against, arriving by a different route, and
  what sees it is the distance between the solved ankle and the one the animator asked for.
- **Past the band a clip can carry, the honest answer is to blend to another clip.** `gametest`
  stretches playback between 0.6x and 1.6x and no further; outside that a walk reads as a stagger
  or a sprint, and stretching further trades a sliding foot for a worse-looking one.
- **The contact label opens during late swing on one foot of `strut_walk`**, leaving the right
  ankle a standing correction of about 0.45 of a leg that no playback rate moves — proved by
  walking the body at that foot's own implied speed and watching it stay. Every instrument spec
  12.9 built reads the LEFT foot, which is why nineteen arms never saw it. Filed, not fixed:
  moving the label would move `ik-contact`, `ik-slide` and `ik-hysteresis` with it.

Root motion is still not in this engine, and is the other answer to the same problem.
