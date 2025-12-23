#ifndef _COMPONENT_H_
#define _COMPONENT_H_

#include <stdint.h>

// Component types - add more as needed for your game
typedef enum {
    COMPONENT_NONE = 0,
    COMPONENT_MESH_RENDERER, // Visual representation (links to SceneNode)
    COMPONENT_RIGID_BODY,    // Physics body (Bullet)
    COMPONENT_CHARACTER,     // Character controller
    COMPONENT_ANIMATOR,      // Skeletal animation
    COMPONENT_AUDIO_SOURCE,  // 3D audio emitter
    COMPONENT_MAX
} ComponentType;

// Component base - wraps typed data
typedef struct Component {
    ComponentType type;
    void* data;
    void (*free_func)(void* data); // Optional custom destructor
} Component;

// Helper to create component mask
#define COMPONENT_BIT(type) (1u << (type))

#endif // _COMPONENT_H_
