#include "animator_component.h"
#include "entity.h"
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
    if (c->node)
        node_set_pose(c->node, animator->state);
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
    if (c && c->animator)
        animator_update(c->animator, *(const float*)user_data);
}

void update_all_animators(struct EntityManager* em, float dt) {
    if (!em)
        return;
    entity_manager_foreach_with(em, COMPONENT_BIT(COMPONENT_ANIMATOR), animator_tick_cb, &dt);
}
