#pragma once

#include "Core/component.h"
#include <glm/glm.hpp>

class IEditorUi;

// Persistent source/generated asset mapping written by BakedLightingPipeline.
// The source snapshot makes inline and prefab materials safely reversible.
class BakedLightingData final : public Component
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

    void DrawProperties(IEditorUi& ui) override;
};
