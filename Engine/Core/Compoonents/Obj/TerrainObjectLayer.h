#pragma once

#include "Core/Component.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <vector>
#include <map>
#include "Core/Compoonents/Obj/Mesh.h"

namespace Engine::Components
{
// Place on a direct child of a Terrain cell. Positions are cell-local; the
// owning TerrainManager saves this data when it streams the cell out.
class TerrainObjectLayer final : public Engine::Core::Component
{
public:
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
    float shadowDistance = 8.f;
    std::vector<Placement> placements;

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
    JsonValue Serialize() const override;
    void Deserialize(const JsonValue& value) override;
    bool DrawProperties(::Engine::Editor::IEditorUi& ui) override;
private:
    mutable PreparedTiles m_prepared;
    mutable uint64_t m_preparedRevision=UINT64_MAX,m_preparedSignature=0;
    mutable const Placement* m_preparedData=nullptr;
    mutable size_t m_preparedCount=0;
    mutable float m_preparedSpan=4;
    mutable bool m_loadedPrepared=false;
    void AddPrepared(int x,int z,const PreparedPlacement& record) const;
    std::string SerializePrepared() const;
    void LoadPrepared(const std::string& bytes);

};
}
