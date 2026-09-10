#ifndef _ANIMATOR_COMPONENT_H_
#define _ANIMATOR_COMPONENT_H_

/*
 * The ANIMATOR entity component (spec 12.1): an Animator the entity owns,
 * its pose bound to the entity's node, ticked once per RENDERED frame from
 * the game loop's pre-render hook with the sim clock's delta -- a whole
 * number of fixed steps, 0 on a frame that took none and 0 while paused, so
 * a paused rig holds its pose and reads zero deformation velocity.
 *
 * Once per rendered frame and never per fixed step, because the tick begins
 * with the prev-pose latch (animation_snapshot_prev_pose): two steps in one
 * frame would latch twice and lose a step's motion vector, and a frame that
 * took no step would never latch and keep a stale one.
 */

#include "../animator.h"

struct Entity;
struct EntityManager;

// Takes ownership of `animator` (freed with the component). Binds the pose to
// entity->node if one is set at add time; an entity that gets its node later,
// or re-parents its rig, calls node_set_pose itself. Returns the animator, or
// NULL on refusal.
Animator* entity_add_animator(struct Entity* entity, Animator* animator);
Animator* entity_get_animator(struct Entity* entity);

// Tick every ANIMATOR entity by dt seconds: the game loop's call, once per
// rendered frame.
void update_all_animators(struct EntityManager* em, float dt);

#endif // _ANIMATOR_COMPONENT_H_
