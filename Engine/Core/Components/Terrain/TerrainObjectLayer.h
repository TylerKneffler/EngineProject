#pragma once

#include "Core/Component.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <vector>
#include <map>
#include <chrono>
#include "Core/Components/Mesh/Mesh.h"
#include "Core/Components/Mesh/Lod/ObjectLodProvider.h"

namespace Engine::Components
{
// Implemented by terrain object renderers so the inspector uses their authored LODs.
// Compatibility bridge for existing terrain-authored providers. New
// components should implement ObjectLodProvider directly.
class TerrainObjectLodProvider : public ObjectLodProvider
{
public:
    virtual std::map<std::string, std::vector<std::string>> GetTerrainObjectLodMeshes() const = 0;
    std::map<std::string, ObjectLodProfileData>
        GetObjectLodProfiles() const override
    {
        std::map<std::string, ObjectLodProfileData> result;
        for (auto& [asset, meshes] : GetTerrainObjectLodMeshes())
            result.emplace(std::move(asset),
                ObjectLodProfileData{std::move(meshes)});
        return result;
    }
};
// Place on a direct child of a Terrain cell. Positions are cell-local; the
// owning TerrainManager saves this data when it streams the cell out.
class TerrainObjectLayer final : public Engine::Core::Component
{
public:
    struct PrefabPrepareTimings
    {
        double placementSignatureMs=0;
        double sourceProbeMs=0;
        double placementAssetsMs=0;
        double lodProfilesMs=0;
        double sourceSetMs=0;
        // Validation includes the signature, profile and source subphases.
        // Build includes synchronous buffer creation; worker work is separate.
        double validationMs=0;
        double sourceLookupMs=0;
        double buildMs=0;
        double mainBufferCreateMs=0;
    };
    struct Placement
    {
        mutable uint64_t id=0;
        std::string asset;
        glm::vec3 position{0.f};
        glm::vec3 normal{0.f, 1.f, 0.f};
        float yawDegrees = 0.f;
        float scale = 1.f;
    };

    TerrainObjectLayer();
    // How a layer draws (distances, LOD bands, shadows), apart from what it
    // holds: TerrainManager keeps one per layer name, so every cell's layer
    // of that name draws the same, layers made by painting included.
    struct Settings
    {
        float viewDistance = 18.f, lod1Distance = 8.f, lod2Distance = 13.f, lodHysteresis = .75f;
        bool screenLodSelection = false;
        float screenLod1Pixels = 300.f, screenLod2Pixels = 80.f;
        float shadowDistance = 8.f, farShadowDistance = 0.f, shadowFadeWidth = 1.f;
        int shadowLod = -1;
        bool farObjects = true;
        // Follow the ground (-1: leave the layer's own, 0 off, 1 on).
        int snapToGround = -1, conformToGround = -1;
        // Project detail-budget layer of the layer's object (empty: the one
        // its components prefer, e.g. "Grass" for ground cover).
        std::string detailLayer;
        // Components the layer's object carries, each {"type", "value"} (its
        // serialized fields), e.g. the GrassField that grows a ground-cover
        // layer: added when missing and configured whenever this changes.
        JsonValue components = JsonValue::MakeArray();
        static Settings From(const TerrainObjectLayer& layer);
        // Applies them (and marks the layer dirty) when they differ.
        void ApplyTo(TerrainObjectLayer& layer) const;
        // Text of components, for comparisons.
        std::string ComponentsKey() const;
        JsonValue Serialize() const;
        static Settings Deserialize(const JsonValue& value);
        bool operator==(const Settings& o) const
        {
            return viewDistance == o.viewDistance && lod1Distance == o.lod1Distance &&
                lod2Distance == o.lod2Distance && lodHysteresis == o.lodHysteresis &&
                screenLodSelection == o.screenLodSelection && screenLod1Pixels == o.screenLod1Pixels &&
                screenLod2Pixels == o.screenLod2Pixels && shadowDistance == o.shadowDistance &&
                farShadowDistance == o.farShadowDistance && shadowFadeWidth == o.shadowFadeWidth &&
                shadowLod == o.shadowLod && farObjects == o.farObjects &&
                snapToGround == o.snapToGround && conformToGround == o.conformToGround &&
                detailLayer == o.detailLayer && ComponentsKey() == o.ComponentsKey();
        }
    };
    std::string layerName = "Objects";
    // Distances are measured from the viewer to each object tile's center.
    // LOD 0 ends at lod1Distance; LOD 1 ends at lod2Distance; LOD 2
    // remains until viewDistance. A zero shadowDistance disables casting.
    float viewDistance = 18.f;
    float lod1Distance = 8.f;
    float lod2Distance = 13.f;
    float lodHysteresis = .75f;
    bool screenLodSelection = false;
    bool distanceLodOverride = false;
    // Warp instance vertices across the sampled ground plane while retaining
    // a vertical local Y axis (useful for upright grass on uneven terrain).
    bool conformToGround = false;
    // Ground cover (grass): sculpting always moves placements onto the new
    // surface, even ones that were not resting on the old one, and the
    // object eraser leaves the layer alone unless a prefab is chosen.
    // Layers saved before the flag existed get it from their name.
    bool snapToGround = false;
    // Whether TerrainManager's far blocks draw this layer's placements (as
    // their last detail level) beyond the loaded cells. Off for small things
    // that only matter near (a forest edge's shrubs, under the far trees).
    bool farObjects = true;
    float screenLod1Pixels = 300.f, screenLod2Pixels = 80.f;
    float screenLodHysteresis = .1f;
    float shadowDistance = 8.f;
    // Beyond shadowDistance, the last LOD of each prefab (a tree's impostor)
    // keeps casting out to this distance, into the far shadow cascade
    // (RealtimeShadowSettings::directionalFarDistance). It takes over where
    // the regular casters fade out. 0 (or <= shadowDistance) disables.
    float farShadowDistance = 0.f;
    // Runtime-generated layers (TerrainManager far blocks) whose placements
    // change only by replacement plus MarkConfigurationDirty. Their prefab
    // runtime skips the periodic placement re-hash that catches in-place
    // edits, which for thousands of far trees per block cost milliseconds
    // per frame across all blocks. Not serialized.
    bool placementsChangeOnlyWhenMarked = false;
    // Near object blocks (TerrainManager::nearObjectBlockCells): a streamed
    // cell's layer keeps its placements and colliders but leaves the instanced
    // batches to a block layer that holds the placements of several cells, and
    // that block layer makes no colliders of its own. Not serialized.
    bool suppressPrefabRendering = false;
    bool suppressPrefabColliders = false;
    // True when a placement names a prefab (cached per placement change).
    bool HasPrefabPlacements() const;
    // Prefab shadows fade within the final metres of shadowDistance.
    // GrassField tiles retain their own distanceFadeWidth control.
    float shadowFadeWidth = 1.f;
    // Runtime spatial span used for generated prefab instance groups.
    // Zero retains the engine default. Not serialized; benchmark-only override.
    static float prefabInstanceGroupSpanMeters;
    static void SetPrefabInstanceGroupSpanForBenchmark(float span);
    // Re-stats every prefab source the layers use in one background job a
    // second and advances a shared epoch when any file changes, moves or
    // appears. Settled layers then skip their own source re-checks while the
    // epoch holds (once per frame, from scene preparation).
    static void PollPrefabSources();
    void RebuildPrefabInstancesForBenchmark();
    float GetShadowFadeWidth() const;
    // -1 follows distance bands; 0..2 pins the prefab shadow proxy only.
    int shadowLod = -1;
    std::vector<Placement> placements;
    static constexpr size_t MaximumObjectLods = 16;
    using ObjectLods = ObjectLodProfileData;
    // Keyed by the placement's base asset. Resolved N-level chains use N-1 distances.
    std::map<std::string, ObjectLods> objectLods;
    ObjectLodProfileData GetObjectLodProfile(const std::string& asset) const;
    std::map<std::string, std::vector<std::string>> GetDefinedObjectLods(
        const std::vector<std::string>* placementAssets = nullptr) const;
    // Placement-only assets always have one level, so a summary need only
    // inspect authored profiles rather than every placement in a dense layer.
    size_t GetMaximumDefinedObjectLodCount() const;
    std::vector<std::string> GetObjectShadowLodMeshes(
        const std::string& asset, const std::vector<std::string>& visibleMeshes) const;
    std::vector<float> GetObjectLodDistances(const std::string& asset, size_t lodCount) const;

    struct PreparedPlacement { uint32_t index=0; Mesh::InstanceTransform transform{}; };
    struct PreparedTile
    {
        std::map<std::string,std::vector<PreparedPlacement>> groups;
        glm::vec3 minimum{0},maximum{0}; float maximumScale=0;
        bool initialized=false;
    };
    using PreparedTiles=std::map<std::pair<int,int>,PreparedTile>;
    const PreparedTiles& GetPreparedTiles(float span) const;
    uint64_t GetPlacementSignature() const;
    uint64_t GetPreparedSignature(float span) const { GetPreparedTiles(span); return m_preparedSignature; }
    bool HasLoadedPreparedData() const { return m_loadedPrepared; }

    // Align local Y to the sampled terrain normal, then turn about that axis.
    static glm::quat SurfaceRotation(const glm::vec3& normal,
        float yawDegrees);
    glm::mat4 PlacementTransform(const Placement& placement) const;
    // Placement data only: scene preparation drives it (no frame update).
    bool RequiresUpdate() const override { return false; }
    JsonValue Serialize() const override;
    void Deserialize(const JsonValue& value) override;
    void InstallPrepared(TerrainObjectLayer&& source);
    // Static prefab placements share source geometry/material definitions.
    // Derived render/physics objects are never saved into the authored layer.
    void PreparePrefabInstances(IGraphicsProvider* provider, bool* uploadAvailable = nullptr,
        std::chrono::steady_clock::time_point deadline = {});
    bool IsPrefabPreparationPending() const;
    // Retain shared prototypes and prepared buffers across a trusted scene
    // restore. Cache keys validate source paths and timestamps before reuse.
    void AppendPrefabPrototypeRetention(std::vector<std::shared_ptr<void>>& output) const;
    bool IsPrefabPreparationComplete(IGraphicsProvider* provider) const;
    // False while the layer is idle: placements unchanged and its next
    // re-check not yet due. Scene preparation skips the call entirely then
    // (775 idle visits a frame cost ~0.2 ms in call overhead alone).
    bool IsPrefabPreparationDue(std::chrono::steady_clock::time_point now) const
    {
        return m_prefabCheckedRevision!=GetConfigurationRevision() ||
            m_prefabCheckedData!=placements.data() || m_prefabCheckedCount!=placements.size() ||
            now>=m_prefabIdleUntil;
    }
    const PrefabPrepareTimings& GetLastPrefabPrepareTimings() const
    { return m_lastPrefabPrepareTimings; }
    bool DrawProperties(Engine::Core::IInspectorUi& ui) override;
private:
    // Settings::ApplyTo: each attached component type's value as last applied.
    std::map<std::string, std::string> m_appliedComponents;
    struct PrefabInstances;
    struct PrefabSourceWatch;
    static PrefabSourceWatch& GetPrefabSourceWatch();
    std::shared_ptr<PrefabInstances> m_prefabInstances;
    uint64_t m_prefabCheckedRevision=UINT64_MAX;
    const Placement* m_prefabCheckedData=nullptr;
    size_t m_prefabCheckedCount=0;
    bool m_hasPrefabPlacements=false;
    bool m_prefabSourcePreparationPending=false;
    // Set only on PreparePrefabInstances' idle exits (see IsPrefabPreparationDue).
    std::chrono::steady_clock::time_point m_prefabIdleUntil{};
    bool m_prefabPlacementAssetsValid=false;
    uint64_t m_prefabPlacementAssetsSignature=0;
    std::vector<std::string> m_prefabPlacementAssets;
    // Configuration revisions provide immediate invalidation. The periodic
    // refresh also catches direct field edits that bypass revision tracking.
    const void* m_prefabProfileCacheOwner=nullptr;
    std::vector<std::pair<const void*, uint64_t>> m_prefabProfileComponents;
    uint64_t m_prefabProfilePlacementSignature=0;
    bool m_prefabProfilesValid=false;
    std::chrono::steady_clock::time_point m_prefabProfilesChecked{};
    std::map<std::string,ObjectLods> m_prefabLodProfiles;
    std::vector<std::string> m_prefabRequiredSources;
    PrefabPrepareTimings m_lastPrefabPrepareTimings{};
    mutable PreparedTiles m_prepared;
    mutable uint64_t m_preparedRevision=UINT64_MAX,m_preparedSignature=0;
    mutable const Placement* m_preparedData=nullptr;
    mutable size_t m_preparedCount=0;
    mutable float m_preparedSpan=4;
    mutable bool m_loadedPrepared=false;
    mutable uint64_t m_hasPrefabRevision=UINT64_MAX;
    mutable const Placement* m_hasPrefabData=nullptr;
    mutable size_t m_hasPrefabCount=0;
    mutable bool m_hasPrefab=false;
    void AddPrepared(int x,int z,const PreparedPlacement& record) const;
    std::string SerializePrepared() const;
    void LoadPrepared(const std::string& bytes, uint64_t placementSignature);

};
}

