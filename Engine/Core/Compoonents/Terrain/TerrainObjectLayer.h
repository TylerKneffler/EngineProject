#pragma once

#include "Core/Component.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <vector>
#include <map>
#include <chrono>
#include "Core/Compoonents/Mesh/Mesh.h"
#include "Core/Compoonents/Obj/ObjectLodProvider.h"

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
    float screenLod1Pixels = 300.f, screenLod2Pixels = 80.f;
    float screenLodHysteresis = .1f;
    float shadowDistance = 8.f;
    // Prefab shadows fade within the final metres of shadowDistance.
    // GrassField tiles retain their own distanceFadeWidth control.
    float shadowFadeWidth = 1.f;
    // Runtime spatial span used for generated prefab instance groups.
    // Zero retains the engine default. Not serialized; benchmark-only override.
    static float prefabInstanceGroupSpanMeters;
    static void SetPrefabInstanceGroupSpanForBenchmark(float span);
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
    const PrefabPrepareTimings& GetLastPrefabPrepareTimings() const
    { return m_lastPrefabPrepareTimings; }
    bool DrawProperties(::Engine::Editor::IEditorUi& ui) override;
private:
    struct PrefabInstances;
    std::shared_ptr<PrefabInstances> m_prefabInstances;
    uint64_t m_prefabCheckedRevision=UINT64_MAX;
    const Placement* m_prefabCheckedData=nullptr;
    size_t m_prefabCheckedCount=0;
    bool m_hasPrefabPlacements=false;
    bool m_prefabSourcePreparationPending=false;
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
    void AddPrepared(int x,int z,const PreparedPlacement& record) const;
    std::string SerializePrepared() const;
    void LoadPrepared(const std::string& bytes, uint64_t placementSignature);

};
}

