#pragma once

#include <vector>

namespace Engine::Core { class Object; }

namespace Engine::Scene
{
// Receives structural notifications from a Scene. Systems that keep pointers
// to scene objects (editor selection, caches) register to drop them before the
// memory is released. Callbacks run on the scene owner thread.
class ISceneObserver
{
public:
    virtual ~ISceneObserver() = default;
    // Objects (including descendants) about to be destroyed; still valid.
    virtual void OnObjectsRemoving(const std::vector<Engine::Core::Object*>&) {}
    // Every object is about to be destroyed (ClearObjects / load).
    virtual void OnObjectsClearing() {}
    // The scene itself is being destroyed. Unregistering is not required.
    virtual void OnSceneDestroying() {}
};
}
