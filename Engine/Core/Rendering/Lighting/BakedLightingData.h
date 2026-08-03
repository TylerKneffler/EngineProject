#pragma once

#include "Core/component.h"
#include <glm/glm.hpp>

class IEditorUi;

// Persistent output written by BakedLightingPipeline. This first bake stage is
// an object-space irradiance probe; future lightmap data can live beside it.
class BakedLightingData final : public Component
{
public:
    BakedLightingData();

    glm::vec3 irradiance{ 0.f };
    glm::vec3 directionalIrradiance{ 0.f };
    glm::vec3 lightDirection{ 0.f, 1.f, 0.f };
    bool valid = false;
    int version = 2;

    void DrawProperties(IEditorUi& ui) override;
};
