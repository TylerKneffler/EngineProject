#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/Serialization/Json.h"
#include "Core/Rendering/RenderJobs.h"
#include "Core/Compoonents/Terrain/TerrainObjectLayer.h"
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
    Engine::Serialization::JsonValue document;
    try
    {
        document = Engine::Rendering::RenderJobs::Get().Submit([path] {
            return Engine::Serialization::SceneSerializer::ReadSceneFile(path);
        }).get();
    }
    catch (const std::exception&) { return false; }
    return LoadFromValue(document);
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
    Engine::Serialization::JsonValue document;
    try
    {
        document = Engine::Rendering::RenderJobs::Get().Submit([source] {
            return Engine::Serialization::SceneSerializer::ParseSceneText(source);
        }).get();
    }
    catch (const std::exception&) { return false; }
    return LoadFromValue(document);
}

bool Scene::RestoreTrustedSnapshot(const Engine::Serialization::JsonValue& snapshot,
    bool publishSceneSnapshot, bool retainPrefabPrototypes)
{
    AssertOwnerThread();
    if (!m_graphicsProvider || !snapshot.IsObject() ||
        snapshot["version"].AsInt() != 1)
        return false;
    std::vector<std::shared_ptr<void>> retainedPrototypes;
    if (retainPrefabPrototypes)
        for (const auto* layer:GetComponents<Engine::Components::TerrainObjectLayer>())
            if (layer) layer->AppendPrefabPrototypeRetention(retainedPrototypes);
    const bool previousSuppression=m_suppressSceneSnapshotPublication;
    m_suppressSceneSnapshotPublication = true;
    struct PublicationGuard
    {
        bool& suppressed;
        bool previous;
        ~PublicationGuard() { suppressed = previous; }
    } publication{m_suppressSceneSnapshotPublication,previousSuppression};
    if (!Engine::Serialization::SceneSerializer::LoadFromValue(
            *this, snapshot, m_graphicsProvider))
        return false;
    m_restorePrefabPrototypes=std::move(retainedPrototypes);
    // Play-mode restoration already has the trusted snapshot as its editor
    // history baseline. Avoid serializing the just-restored scene a second
    // time at this commit boundary. Ordinary scene loads still publish it.
    if (publishSceneSnapshot)
        m_suppressSceneSnapshotPublication = previousSuppression;
    CommitPendingObjectChanges();
    return true;
}

bool Scene::LoadFromValue(const Engine::Serialization::JsonValue& document)
{
    AssertOwnerThread();
    if (!m_graphicsProvider || !document.IsObject() ||
        document["version"].AsInt() != 1)
        return false;
    const auto previous = Engine::Serialization::SceneSerializer::CaptureSceneDocument(*this);
    const bool wasStarted = HasStarted();
    const uint64_t previousRevision = GetStructureRevision();
    if (RestoreTrustedSnapshot(document)) return true;
    if (GetStructureRevision() == previousRevision) return false;
    if (!RestoreTrustedSnapshot(previous))
        throw std::runtime_error("Failed to restore the scene after a failed load");
    if (wasStarted) Start();
    return false;
}

}
