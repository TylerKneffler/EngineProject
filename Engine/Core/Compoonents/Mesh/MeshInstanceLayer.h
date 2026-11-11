#pragma once

#include "Core/Component.h"
#include "Core/Compoonents/Mesh/MeshInstanceLayerData.h"
#include "Core/Compoonents/Mesh/MeshInstanceSourceSet.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Engine::Components
{
struct ObjectLodProfileData;

// Renders repeated static mesh placements on any scene object. Placement
// ownership and streaming remain with the component that authors those poses.
class MeshInstanceLayer final : public Engine::Core::Component
{
public:
    struct Placement
    {
        uint64_t id = 0;
        glm::mat4 transform{1.f};
    };

    MeshInstanceLayer();

    std::string sourceMeshAsset;
    float groupSpanMeters = 64.f;

    const std::vector<Placement>& GetPlacements() const { return m_placements; }
    void SetSourceMeshAsset(std::string asset);
    // Runtime source providers can supply several geometry/material parts.
    // The source set is not serialized; provider assets remain provider-owned.
    void SetSourceSet(MeshInstanceSourceSet sources,
        IGraphicsProvider* provider);
    void SetPlacements(std::vector<Placement> placements);
    // Derived placements from generators/providers are kept separate from the
    // authored transforms serialized by this component.
    void SetProviderPlacements(std::vector<Placement> placements);
    uint64_t AddPlacement(const glm::mat4& transform);
    bool SetPlacementTransform(uint64_t placementId, const glm::mat4& transform);
    bool RemovePlacement(uint64_t placementId);
    void ClearPlacements();

    void OnAfterDeserialize(IGraphicsProvider* provider) override;
    void PrepareGraphicsResources(IGraphicsProvider* provider) override;
    void OnDestroy() override;
    JsonValue Serialize() const override;
    void Deserialize(const JsonValue& value) override;
    bool DrawProperties(::Engine::Editor::IEditorUi& ui) override;
    bool RequiresUpdate() const override { return false; }

private:
    using TileKey = std::pair<int64_t, int64_t>;
    struct PendingGroup
    {
        TileKey tile;
        uint32_t chunk = 0;
        std::string sourcePartKey;
        std::vector<size_t> placementIndices;
    };
    ObjectLodProfileData ResolveLodProfile() const;
    std::vector<std::string> ResolveLodPaths(
        const ObjectLodProfileData& profile) const;
    uint64_t ConfigurationSignature(
        const ObjectLodProfileData& profile,
        const std::vector<std::string>& paths,
        const std::vector<Placement>& placements,
        IGraphicsProvider* provider) const;
    void QueuePreparation();
    void RemoveGeneratedObjects();
    void RemovePendingGeneratedObjects();
    bool Publish(IGraphicsProvider* provider,
        const ObjectLodProfileData& profile);

    std::vector<Placement> m_placements;
    std::vector<Placement> m_providerPlacements;
    std::vector<Placement> m_pendingPlacements;
    std::vector<std::string> m_loadedPaths;
    std::vector<std::shared_ptr<Mesh>> m_loadedSources;
    std::vector<std::string> m_pendingPaths;
    std::vector<std::shared_ptr<Mesh>> m_pendingSources;
    MeshInstanceSourceSet m_providerSourceSet;
    IGraphicsProvider* m_sourceSetProvider = nullptr;
    MeshInstanceSourceSet m_pendingSourceSet;
    std::vector<uint64_t> m_generatedObjects;
    std::vector<uint64_t> m_pendingGeneratedObjects;
    std::vector<PendingGroup> m_pendingGroups;
    std::map<TileKey, std::vector<size_t>> m_pendingTilePlacements;
    size_t m_nextPlacementToGroup = 0;
    size_t m_nextGroup = 0;
    bool m_pendingGroupsInitialized = false;
    IGraphicsProvider* m_loadedProvider = nullptr;
    size_t m_nextSource = 0;
    uint64_t m_requestedSignature = 0;
    bool m_hasRequestedSignature = false;
    bool m_building = false;
    std::string m_lastError;
};
}
