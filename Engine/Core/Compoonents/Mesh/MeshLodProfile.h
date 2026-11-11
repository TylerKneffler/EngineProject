#pragma once

#include "Core/Component.h"
#include "Core/Compoonents/Obj/ObjectLodProvider.h"

#include <cstdint>
#include <glm/glm.hpp>
#include <map>
#include <string>
#include <vector>

namespace Engine::Components
{
class Mesh;

// Reusable asset-chain LOD configuration for an ordinary static Mesh. It also
// publishes the same profile to placement systems such as terrain object layers.
class MeshLodProfile final : public Engine::Core::Component,
    public ObjectLodProvider
{
public:
    MeshLodProfile();

    std::string baseMeshAsset;
    std::vector<std::string> alternateMeshes;
    std::vector<ObjectLodPartProfileData> representationParts;
    std::vector<std::string> shadowMeshes;
    std::vector<float> distances;
    std::vector<float> screenThresholdsPixels;
    float hysteresis = .1f;
    float shadowDistance = 0.f;
    bool screenSizeSelection = false;
    bool distanceOverride = false;
    bool ditherTransition = true;
    float transitionWidth = .1f;
    bool cullAfterLastLod = false;
    float finalLodCullDistance = 200.f;

    std::map<std::string, ObjectLodProfileData>
        GetObjectLodProfiles() const override;
    void PrepareGraphicsResources(IGraphicsProvider* provider) override;
    void OnDestroy() override;
    JsonValue Serialize() const override;
    void Deserialize(const JsonValue& value) override;
    bool DrawProperties(::Engine::Editor::IEditorUi& ui) override;
    bool RequiresUpdate() const override { return false; }

private:
    uint64_t GeometrySignature() const;
    void ApplySelectionPolicy(Mesh& mesh, size_t residentLodCount) const;

    uint64_t m_preparedSignature = 0;
    size_t m_nextAlternate = 0;
    size_t m_nextBoundsScan = 0;
    glm::vec3 m_scannedBoundsMin{0.f};
    glm::vec3 m_scannedBoundsMax{0.f};
    bool m_hasScannedBounds = false;
    bool m_hasPreparedSignature = false;
    bool m_preparationFailed = false;
    std::string m_lastError;
};
}
