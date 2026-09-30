#pragma once

#include "Core/component.h"
#include <glm/glm.hpp>

namespace Engine::Rendering
{

// Persistent source/generated asset mapping written by BakedLightingPipeline.
// The source snapshot makes inline and prefab materials safely reversible.
// A valid component also defines a static mixed-lighting receiver: baked
// occlusion is authoritative on that surface, so it does not sample realtime
// shadow maps. It remains a realtime caster so dynamic receivers still see
// its shadow. Objects without valid baked data are dynamic receivers.
class BakedLightingData final : public Engine::Core::Component
{
public:
    BakedLightingData();

    glm::vec3 irradiance{ 0.f };
    glm::vec3 directionalIrradiance{ 0.f };
    glm::vec3 lightDirection{ 0.f, 1.f, 0.f };
    std::string originalMaterialAsset;
    std::string originalMaterialSnapshot;
    std::string bakedMaterialAsset;
    std::string bakedLightmapAsset;
    bool valid = false;
    int version = 3;

    bool DrawProperties(::Engine::Editor::IEditorUi& ui) override;
};
}
