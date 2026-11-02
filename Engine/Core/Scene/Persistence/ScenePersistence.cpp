#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include <stdexcept>

namespace Engine::Scene
{

bool Scene::Save(const std::string& path) const
{
    return Engine::Serialization::SceneSerializer::Save(*this, path);
}

bool Scene::Load(const std::string& path)
{
    AssertOwnerThread();
    if (!m_graphicsProvider)
        return false;
    // Deserializers clear the graph before constructing objects. Keep a
    // restorable owner-side document until the replacement has fully loaded.
    const std::string previous = SaveSnapshotToString();
    const bool wasStarted = HasStarted();
    const uint64_t previousRevision = GetStructureRevision();
    m_suppressSceneSnapshotPublication = true;
    struct PublicationGuard
    {
        bool& suppressed;
        ~PublicationGuard() { suppressed = false; }
    } publication{m_suppressSceneSnapshotPublication};
    if (Engine::Serialization::SceneSerializer::Load(*this, path, m_graphicsProvider))
    {
        m_suppressSceneSnapshotPublication = false;
        CommitPendingObjectChanges();
        return true;
    }
    if (GetStructureRevision() == previousRevision)
        return false;
    if (!Engine::Serialization::SceneSerializer::LoadFromString(
            *this, previous, m_graphicsProvider))
        throw std::runtime_error("Failed to restore the scene after a failed load");
    if (wasStarted) Start();
    m_suppressSceneSnapshotPublication = false;
    CommitPendingObjectChanges();
    return false;
}

std::string Scene::SaveToString() const
{
    return Engine::Serialization::SceneSerializer::SaveToString(*this);
}

std::string Scene::SaveSnapshotToString() const
{
    return Engine::Serialization::SceneSerializer::SaveSnapshotToString(*this);
}

bool Scene::LoadFromString(const std::string& source)
{
    AssertOwnerThread();
    if (!m_graphicsProvider)
        return false;
    const std::string previous = SaveSnapshotToString();
    const bool wasStarted = HasStarted();
    const uint64_t previousRevision = GetStructureRevision();
    m_suppressSceneSnapshotPublication = true;
    struct PublicationGuard
    {
        bool& suppressed;
        ~PublicationGuard() { suppressed = false; }
    } publication{m_suppressSceneSnapshotPublication};
    if (Engine::Serialization::SceneSerializer::LoadFromString(
            *this, source, m_graphicsProvider))
    {
        m_suppressSceneSnapshotPublication = false;
        CommitPendingObjectChanges();
        return true;
    }
    if (GetStructureRevision() == previousRevision)
        return false;
    if (!Engine::Serialization::SceneSerializer::LoadFromString(
            *this, previous, m_graphicsProvider))
        throw std::runtime_error("Failed to restore the scene after a failed load");
    if (wasStarted) Start();
    m_suppressSceneSnapshotPublication = false;
    CommitPendingObjectChanges();
    return false;
}

}
