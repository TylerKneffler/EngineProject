#pragma once
#include "Core/component.h"
#include "Core/PropertyMacros.h"
#include <glm/glm.hpp>

class IEditorUi;

// Omnidirectional point light. Baked lights are serialized for a future
// lightmap pass but deliberately do not contribute to realtime rendering.
class Light : public Component
{
public:
    Light();
    ~Light() = default;

    PROPERTY(Inspector, EditAnywhere, Category = "Light")
    glm::vec3 color { 1.f, 0.95f, 0.85f };

    PROPERTY(Inspector, EditAnywhere, Category = "Light", ClampMin = "0.0")
    float intensity = 4.f;

    PROPERTY(Inspector, EditAnywhere, Category = "Light", ClampMin = "0.01")
    float range = 8.f;

    PROPERTY(Inspector, EditAnywhere, Category = "Light", Range = "0.1, 8.0")
    float falloff = 2.f;

    // false = Realtime, true = Baked.
    PROPERTY(Inspector, EditAnywhere, Category = "Light")
    bool baked = false;

    void DrawProperties(IEditorUi& ui) override;
};
