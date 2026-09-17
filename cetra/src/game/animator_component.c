#include "animator_component.h"
#include "entity.h"
#include "../anim_graph.h"
#include "../scene.h"
#include "../ext/log.h"

#include <stdlib.h>

// The ANIMATOR component payload: the animator, and the node its pose was
// bound to, so the binding can be undone when the component goes.
typedef struct AnimatorComponent {
    Animator* animator; // owned
    SceneNode* node;    // borrowed; where node_set_pose was called
} AnimatorComponent;

// The node outlives the component -- free_game frees the entity manager
// first and the scene last -- so a pose the node still names would dangle.
static void animator_component_free(void* data) {
    AnimatorComponent* c = (AnimatorComponent*)data;
    if (!c)
        return;
    if (c->node && c->animator && c->node->pose == c->animator->state)
        node_set_pose(c->node, NULL);
    free_animator(c->animator);
    free(c);
}

Animator* entity_add_animator(struct Entity* entity, Animator* animator) {
    if (!entity || !animator)
        return NULL;
    if (entity_has_component(entity, COMPONENT_ANIMATOR)) {
        log_error("Entity '%s' already has an animator", entity->name);
        return NULL;
    }
    AnimatorComponent* c = calloc(1, sizeof(AnimatorComponent));
    if (!c)
        return NULL;
    c->animator = animator;
    c->node = entity->node;
    if (c->node) {
        node_set_pose(c->node, animator->state);
    } else {
        // Not fatal -- an entity may get its node later -- but silence here
        // reads downstream as a rig that animates and never draws.
        log_warn("Entity '%s' has no node yet; its animator poses nothing until "
                 "node_set_pose is called",
                 entity->name);
    }
    entity_add_component(entity, COMPONENT_ANIMATOR, c);
    entity_set_component_free(entity, COMPONENT_ANIMATOR, animator_component_free);
    return animator;
}

Animator* entity_get_animator(struct Entity* entity) {
    if (!entity)
        return NULL;
    AnimatorComponent* c = (AnimatorComponent*)entity_get_component(entity, COMPONENT_ANIMATOR);
    return c ? c->animator : NULL;
}

static void animator_tick_cb(Entity* entity, void* user_data) {
    AnimatorComponent* c = (AnimatorComponent*)entity_get_component(entity, COMPONENT_ANIMATOR);
    if (!c || !c->animator)
        return;
    const float dt = *(const float*)user_data;
    /*
     * The state machine decides, then the animator plays (spec 12.20).
     *
     * Here rather than in each app's own hook so the ordering is settled once,
     * and the order is load-bearing three ways: this is the one cadence the
     * animator's clocks agree with, it reads the finished edge on the frame that
     * produced it, and it sits after the fixed steps have drained root motion and
     * before the next accumulation -- so a switch costs at most one rendered
     * frame of travel, where a switch from an app's own fixed step costs however
     * many steps that frame happened to run.
     *
     * A graph reaches this through the animator it bound to rather than through a
     * field of its own: that pointer already exists so a second graph can stand
     * the first down, and a second copy of the same relationship is a second
     * place for the two to disagree.
     */
    if (c->animator->graph)
        anim_graph_update(c->animator->graph, dt);
    animator_update(c->animator, dt);
}

void update_all_animators(struct EntityManager* em, float dt) {
    if (!em)
        return;
    entity_manager_foreach_with(em, COMPONENT_BIT(COMPONENT_ANIMATOR), animator_tick_cb, &dt);
}
