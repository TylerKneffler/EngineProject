#pragma once

#include "Core/Component.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <vector>

namespace Engine::Components
{
// Place on a direct child of a Terrain cell. Positions are cell-local; the
// owning TerrainManager saves this data when it streams the cell out.
class TerrainObjectLayer final : public Engine::Core::Component
{
public:
    struct Placement
    {
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

    // Align local Y to the sampled terrain normal, then turn about that axis.
    static glm::quat SurfaceRotation(const glm::vec3& normal,
        float yawDegrees);
    JsonValue Serialize() const override;
    void Deserialize(const JsonValue& value) override;
    bool DrawProperties(::Engine::Editor::IEditorUi& ui) override;
};
}
