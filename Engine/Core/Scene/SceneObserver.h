#pragma once

#include <vector>

namespace Engine::Core { class Object; }

namespace Engine::Scene
{
// Receives structural notifications from a Scene. Systems that keep pointers
// to scene objects (editor selection, renderer caches) register to drop them
// before the memory is released. Callbacks run on the scene owner thread.
class ISceneObserver
{
public:
    virtual ~ISceneObserver() = default;
    // Objects (including descendants) about to be destroyed; still valid.
    virtual void OnObjectsRemoving(const std::vector<Engine::Core::Object*>&) {}
    // Every object is about to be destroyed (ClearObjects / load).
    virtual void OnObjectsClearing() {}
    // An object's component set or membership changed. Null means the whole
    // scene changed (bulk add/clear); the owner may already be removed.
    virtual void OnStructureChanged(Engine::Core::Object*) {}
    // The scene is fencing background work before replacing objects. Jobs
    // that read scene objects must be joined before returning.
    virtual void OnBackgroundWorkFence() {}
    // The scene itself is being destroyed. Unregistering is not required.
    virtual void OnSceneDestroying() {}
};
}
